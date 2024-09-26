/*
 * This source file is part of RmlUi, the HTML/CSS Interface Middleware
 *
 * For the latest information, see http://github.com/mikke89/RmlUi
 *
 * Copyright (c) 2008-2010 CodePoint Ltd, Shift Technology Ltd
 * Copyright (c) 2019-2023 The RmlUi Team, and contributors
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 *
 */

#include <RmlUi/Core/Platform.h>

#if defined RMLUI_PLATFORM_WIN32

    #include "RmlUi_Renderer_DX11.h"
    #include <RmlUi/Core/Core.h>
    #include <RmlUi/Core/DecorationTypes.h>
    #include <RmlUi/Core/FileInterface.h>
    #include <RmlUi/Core/Geometry.h>
    #include <RmlUi/Core/Log.h>
    #include <RmlUi/Core/MeshUtilities.h>
    #include <RmlUi/Core/SystemInterface.h>

    #include "RmlUi_Include_Windows.h"

    #include "d3dcompiler.h"

    #ifdef RMLUI_DX_DEBUG
        #include <dxgidebug.h>
    #endif

// Shader source code

#define MAX_NUM_STOPS 16
#define BLUR_SIZE 7
#define BLUR_NUM_WEIGHTS ((BLUR_SIZE + 1) / 2)

#define RMLUI_STRINGIFY_IMPL(x) #x
#define RMLUI_STRINGIFY(x) RMLUI_STRINGIFY_IMPL(x)

#define RMLUI_SHADER_HEADER \
        "#define MAX_NUM_STOPS " RMLUI_STRINGIFY(MAX_NUM_STOPS) "\n#line " RMLUI_STRINGIFY(__LINE__) "\n"

constexpr const char shader_vert_main[] = RMLUI_SHADER_HEADER R"(
struct VS_Input
{
    float2 inPosition : POSITION;
    float4 inColor : COLOR;
    float2 inTexCoord : TEXCOORD;
};

struct PS_INPUT
{
    float4 outPosition : SV_Position;
    float4 outColor : COLOR;
    float2 outUV : TEXCOORD;
};

cbuffer SharedConstantBuffer : register(b0)
{
    float4x4 m_transform;
    float2 m_translate;
    float2 _padding;
    float4 _padding2[21]; // Padding so that cbuffer aligns with the largest one (gradient)
};

PS_INPUT VSMain(const VS_Input IN)
{
    PS_INPUT result = (PS_INPUT)0;

    float2 translatedPos = IN.inPosition + m_translate;
    float4 resPos = mul(m_transform, float4(translatedPos.x, translatedPos.y, 0.0, 1.0));

    result.outPosition = resPos;
    result.outColor = IN.inColor;
    result.outUV = IN.inTexCoord;

#if defined(RMLUI_PREMULTIPLIED_ALPHA)
    // Pre-multiply vertex colors with their alpha.
    result.outColor.rgb = result.outColor.rgb * result.outColor.a;
#endif

    return result;
};
)";

constexpr const char shader_frag_texture[] = RMLUI_SHADER_HEADER R"(
struct PS_Input
{
    float4 inputPos : SV_Position;
    float4 inputColor : COLOR;
    float2 inputUV : TEXCOORD;
};

Texture2D g_InputTexture : register(t0);
SamplerState g_SamplerLinear : register(s0);

float4 PSMain(const PS_Input IN) : SV_TARGET 
{
    return IN.inputColor * g_InputTexture.Sample(g_SamplerLinear, IN.inputUV); 
};
)";

constexpr const char shader_frag_color[] = RMLUI_SHADER_HEADER R"(
struct PS_Input
{
    float4 inputPos : SV_Position;
    float4 inputColor : COLOR;
    float2 inputUV : TEXCOORD;
};

float4 PSMain(const PS_Input IN) : SV_TARGET 
{ 
    return IN.inputColor; 
};
)";

enum class ShaderGradientFunction { Linear, Radial, Conic, RepeatingLinear, RepeatingRadial, RepeatingConic }; // Must match shader definitions below.

// We need to round up at compile-time so that we can embed the 
#define CEILING(x, y) (((x) + (y)-1) / (y))
static const char shader_frag_gradient[] = RMLUI_SHADER_HEADER "#define MAX_NUM_STOPS_PACKED (uint)" RMLUI_STRINGIFY(CEILING(MAX_NUM_STOPS, 4)) R"(
#define LINEAR 0
#define RADIAL 1
#define CONIC 2
#define REPEATING_LINEAR 3
#define REPEATING_RADIAL 4
#define REPEATING_CONIC 5
#define PI 3.14159265

cbuffer SharedConstantBuffer : register(b0)
{
    float4x4 m_transform;
    float2 m_translate;

    // One to one translation of the OpenGL uniforms results in a LOT of wasted space due to CBuffer alignment rules.
    // Changes from GL3:
    // - Moved m_num_stops below m_func (saved 4 bytes of padding).
    // - Packed m_stop_positions into a float4[MAX_NUM_STOPS / 4] array, as each array element starts a new 16-byte row.
    // The below layout has 0 bytes of padding.

    int m_func;   // one of the above definitions
    int m_num_stops;
    float2 m_p;   // linear: starting point,         radial: center,                        conic: center
    float2 m_v;   // linear: vector to ending point, radial: 2d curvature (inverse radius), conic: angled unit vector
    float4 m_stop_colors[MAX_NUM_STOPS];
    float4 m_stop_positions[MAX_NUM_STOPS_PACKED]; // normalized, 0 -> starting point, 1 -> ending point
};

// Hide the way the data is packed in the cbuffer through a macro
// @NOTE: Hardcoded for MAX_NUM_STOPS 16.
//        i >> 2 => i >> sqrt(MAX_NUM_STOPS)
#define GET_STOP_POS(i) (m_stop_positions[i >> 2][i & 3])

struct PS_Input
{
    float4 inputPos : SV_Position;
    float4 inputColor : COLOR;
    float2 inputUV : TEXCOORD;
};

#define glsl_mod(x,y) (((x)-(y)*floor((x)/(y))))

float4 lerp_stop_colors(float t) {
    float4 color = m_stop_colors[0];

    for (int i = 1; i < m_num_stops; i++)
        color = lerp(color, m_stop_colors[i], smoothstep(GET_STOP_POS(i-1), GET_STOP_POS(i), t));

    return color;
};

float4 PSMain(const PS_Input IN) : SV_TARGET
{
    float t = 0.0;

    if (m_func == LINEAR || m_func == REPEATING_LINEAR) {
        float dist_square = dot(m_v, m_v);
        float2 V = IN.inputPos.xy - m_p;
        t = dot(m_v, V) / dist_square;
    }
    else if (m_func == RADIAL || m_func == REPEATING_RADIAL) {
        float2 V = IN.inputPos.xy - m_p;
        t = length(m_v * V);
    }
    else if (m_func == CONIC || m_func == REPEATING_CONIC) {
        float2x2 R = float2x2(m_v.x, -m_v.y, m_v.y, m_v.x);
        float2 V = mul(R, (IN.inputPos.xy - m_p));
        t = 0.5 + atan2(V.y, -V.x) / (2.0 * PI);
    }

    if (m_func == REPEATING_LINEAR || m_func == REPEATING_RADIAL || m_func == REPEATING_CONIC) {
        float t0 = GET_STOP_POS(0);
        float t1 = GET_STOP_POS(m_num_stops - 1);
        t = t0 + glsl_mod(t - t0, t1 - t0);
    }

    return IN.inputColor * lerp_stop_colors(t);
};
)";

// "Creation" by Danilo Guanabara, based on: https://www.shadertoy.com/view/XsXXDn
static const char shader_frag_creation[] = RMLUI_SHADER_HEADER R"(
struct PS_Input
{
    float4 inputPos : SV_Position;
    float4 inputColor : COLOR;
    float2 inputUV : TEXCOORD;
};

cbuffer SharedConstantBuffer : register(b0)
{
    float4x4 m_transform;
    float2 m_translate;
    float2 m_dimensions;
    float m_value;
    float3 _padding;
    float4 _padding2[20]; // Padding so that cbuffer aligns with the largest one (gradient)
};

#define glsl_mod(x,y) (((x)-(y)*floor((x)/(y))))

float4 PSMain(const PS_Input IN) : SV_TARGET 
{
    float t = m_value;
    float3 c;
    float l;
    for (int i = 0; i < 3; i++) {
        float2 p = IN.inputUV;
        float2 uv = p;
        p -= .5;
        p.x *= m_dimensions.x / m_dimensions.y;
        float z = t + ((float)i) * .07;
        l = length(p);
        uv += p / l * (sin(z) + 1.) * abs(sin(l * 9. - z - z));
        c[i] = .01 / length(glsl_mod(uv, 1.) - .5);
    }
    return float4(c / l, IN.inputColor.a);
};
)";

constexpr const char shader_vert_passthrough[] = RMLUI_SHADER_HEADER R"(
struct VS_Input 
{
    float2 inPosition : POSITION;
    float4 inColor : COLOR;
    float2 inTexCoord : TEXCOORD;
};

struct PS_Input
{
    float4 outPosition : SV_Position;
    float4 outColor : COLOR;
    float2 outUV : TEXCOORD;
};

cbuffer ConstantBuffer : register(b0)
{
    float4x4 m_transform;
    float2 m_translate;
    float2 _padding;
    float4 _padding2[21]; // Padding so that cbuffer aligns with the largest one (gradient)
};

PS_Input VSMain(const VS_Input IN)
{
    PS_Input result = (PS_Input)0;

    result.outPosition = float4(IN.inPosition.xy, 0.0f, 1.0f);
    result.outUV = float2(IN.inTexCoord.x, 1.0f - IN.inTexCoord.y);

    return result;
};
)";

constexpr const char shader_frag_passthrough[] = RMLUI_SHADER_HEADER R"(
struct PS_Input
{
    float4 inputPos : SV_Position;
    float4 inputColor : COLOR;
    float2 inputUV : TEXCOORD;
};

Texture2D g_InputTexture : register(t0);
SamplerState g_SamplerLinear : register(s0);

float4 PSMain(const PS_Input IN) : SV_TARGET 
{
    return g_InputTexture.Sample(g_SamplerLinear, IN.inputUV); 
};
)";

static const char shader_frag_color_matrix[] = RMLUI_SHADER_HEADER R"(

Texture2D g_InputTexture : register(t0);
SamplerState g_SamplerLinear : register(s0);

cbuffer ConstantBuffer : register(b0)
{
    float4x4 m_color_matrix;
    float4 _padding[22]; // Padding so that cbuffer aligns with the largest one (gradient)
};

struct PS_Input
{
    float4 inputPos : SV_Position;
    float4 inputColor : COLOR;
    float2 inputUV : TEXCOORD;
};

float4 PSMain(const PS_Input IN) : SV_TARGET
{
    // The general case uses a 4x5 color matrix for full rgba transformation, plus a constant term with the last column.
    // However, we only consider the case of rgb transformations. Thus, we could in principle use a 3x4 matrix, but we
    // keep the alpha row for simplicity.
    // In the general case we should do the matrix transformation in non-premultiplied space. However, without alpha
    // transformations, we can do it directly in premultiplied space to avoid the extra division and multiplication
    // steps. In this space, the constant term needs to be multiplied by the alpha value, instead of unity.
    float4 texColor = g_InputTexture.Sample(g_SamplerLinear, IN.inputUV); 
    float3 transformedColor = mul(m_color_matrix, texColor).rgb;
    return float4(transformedColor, texColor.a);
};
)";

static const char shader_frag_blend_mask[] = RMLUI_SHADER_HEADER R"(
Texture2D g_InputTexture : register(t0);
SamplerState g_SamplerLinear : register(s0);
Texture2D g_MaskTexture : register(t1);

struct PS_Input
{
    float4 inputPos : SV_Position;
    float4 inputColor : COLOR;
    float2 inputUV : TEXCOORD;
};

float4 PSMain(const PS_Input IN) : SV_TARGET
{
    float4 texColor = g_InputTexture.Sample(g_SamplerLinear, IN.inputUV);
    float maskAlpha = g_MaskTexture.Sample(g_SamplerLinear, IN.inputUV).a;
    return texColor * maskAlpha;
};
)";

#define RMLUI_SHADER_BLUR_HEADER \
    RMLUI_SHADER_HEADER "\n#define BLUR_SIZE " RMLUI_STRINGIFY(BLUR_SIZE) "\n#define BLUR_NUM_WEIGHTS " RMLUI_STRINGIFY(BLUR_NUM_WEIGHTS)

static const char shader_vert_blur[] = RMLUI_SHADER_BLUR_HEADER R"(
struct VS_Input
{
    float2 inPosition : POSITION;
    float4 inColor : COLOR;
    float2 inTexCoord : TEXCOORD;
};

struct PS_INPUT
{
    float4 outPosition : SV_Position;
    float4 outColor : COLOR;
    float2 outUV : TEXCOORD;
};

cbuffer SharedConstantBuffer : register(b0)
{
    float4x4 m_transform;
    float2 m_translate;
    float2 _padding;
    float4 _padding2[21]; // Padding so that cbuffer aligns with the largest one (gradient)
};

PS_INPUT VSMain(const VS_Input IN)
{
    PS_INPUT result = (PS_INPUT)0;

    float2 translatedPos = IN.inPosition + m_translate;
    float4 resPos = mul(m_transform, float4(translatedPos.x, translatedPos.y, 0.0, 1.0));

    result.outPosition = resPos;
    result.outColor = IN.inColor;
    result.outUV = IN.inTexCoord;

#if defined(RMLUI_PREMULTIPLIED_ALPHA)
    // Pre-multiply vertex colors with their alpha.
    result.outColor.rgb = result.outColor.rgb * result.outColor.a;
#endif

    return result;
};
)";

static const char shader_frag_blur[] = RMLUI_SHADER_BLUR_HEADER R"(
Texture2D g_InputTexture : register(t0);
SamplerState g_SamplerLinear : register(s0);

cbuffer ConstantBuffer : register(b0)
{
    float4x4 m_transform;
    float2 m_translate;
    float4 m_weights;
    float2 m_texelOffset;
    float2 m_texCoordMin;
    float2 m_texCoordMax;
    float4 _padding[19]; // Padding so that cbuffer aligns with the largest one (gradient)
};

struct PS_Input
{
    float4 inputPos : SV_Position;
    float2 inputUV[BLUR_SIZE] : TEXCOORD;
};

float4 PSMain(const PS_Input IN) : SV_TARGET
{
    float4 color = float4(0.0, 0.0, 0.0, 0.0);
    for(int i = 0; i < BLUR_SIZE; i++)
    {
        float2 in_region = step(m_texCoordMin, IN.inputUV[i]) * step(IN.inputUV[i], m_texCoordMax);
        color += g_InputTexture.Sample(g_SamplerLinear, IN.inputUV[i]) * in_region.x * in_region.y * m_weights[abs(i - BLUR_NUM_WEIGHTS + 1)];
    }
    return color;
};
)";

static const char shader_frag_drop_shadow[] = RMLUI_SHADER_HEADER R"(
Texture2D g_InputTexture : register(t0);
SamplerState g_SamplerLinear : register(s0);

cbuffer ConstantBuffer : register(b0)
{
    float4x4 m_transform;
    float2 m_translate;
    float2 m_texCoordMin;
    float2 m_texCoordMax;
    float4 m_color;
    float2 _padding;
    float4 _padding2[19]; // Padding so that cbuffer aligns with the largest one (gradient)
};

struct PS_Input
{
    float4 inputPos : SV_Position;
    float4 inputColor : COLOR;
    float2 inputUV : TEXCOORD;
};

float4 PSMain(const PS_Input IN) : SV_TARGET
{
    float2 in_region = step(m_texCoordMin, IN.inputUV) * step(IN.inputUV, m_texCoordMax);
    return g_InputTexture.Sample(g_SamplerLinear, IN.inputUV).a * in_region.x * in_region.y * m_color;
};
)";

enum class ProgramId {
    None,
    Color,
    Texture,
    Gradient,
    Creation,
    Passthrough,
    ColorMatrix,
    BlendMask,
    Blur,
    DropShadow,
    Count,
};
enum class VertShaderId {
    Main,
    Passthrough,
    Blur,
    Count,
};
enum class FragShaderId {
    Color,
    Texture,
    Gradient,
    Creation,
    Passthrough,
    ColorMatrix,
    BlendMask,
    Blur,
    DropShadow,
    Count,
};
enum class ShaderType {
    Vertex,
    Fragment,
};

namespace Gfx {

struct VertShaderDefinition {
    VertShaderId id;
    const char* name_str;
    const char* code_str;
    const size_t code_size;
};
struct FragShaderDefinition {
    FragShaderId id;
    const char* name_str;
    const char* code_str;
    const size_t code_size;
};
struct ProgramDefinition {
    ProgramId id;
    const char* name_str;
    VertShaderId vert_shader;
    FragShaderId frag_shader;
};

// clang-format off
static const VertShaderDefinition vert_shader_definitions[] = {
    {VertShaderId::Main,        "main",         shader_vert_main,           sizeof(shader_vert_main)},
    {VertShaderId::Passthrough, "passthrough",  shader_vert_passthrough,    sizeof(shader_vert_passthrough)},
    {VertShaderId::Blur,        "blur",         shader_vert_blur,           sizeof(shader_vert_blur)},
};
static const FragShaderDefinition frag_shader_definitions[] = {
    {FragShaderId::Color,       "color",        shader_frag_color,          sizeof(shader_frag_color)},
    {FragShaderId::Texture,     "texture",      shader_frag_texture,        sizeof(shader_frag_texture)},
    {FragShaderId::Gradient,    "gradient",     shader_frag_gradient,       sizeof(shader_frag_gradient)},
    {FragShaderId::Creation,    "creation",     shader_frag_creation,       sizeof(shader_frag_creation)},
    {FragShaderId::Passthrough, "passthrough",  shader_frag_passthrough,    sizeof(shader_frag_passthrough)},
    {FragShaderId::ColorMatrix, "color_matrix", shader_frag_color_matrix,   sizeof(shader_frag_color_matrix)},
    {FragShaderId::BlendMask,   "blend_mask",   shader_frag_blend_mask,     sizeof(shader_frag_blend_mask)},
    {FragShaderId::Blur,        "blur",         shader_frag_blur,           sizeof(shader_frag_blur)},
    {FragShaderId::DropShadow,  "drop_shadow",  shader_frag_drop_shadow,    sizeof(shader_frag_drop_shadow)},
};
static const ProgramDefinition program_definitions[] = {
    {ProgramId::Color,       "color",        VertShaderId::Main,        FragShaderId::Color},
    {ProgramId::Texture,     "texture",      VertShaderId::Main,        FragShaderId::Texture},
    {ProgramId::Gradient,    "gradient",     VertShaderId::Main,        FragShaderId::Gradient},
    {ProgramId::Creation,    "creation",     VertShaderId::Main,        FragShaderId::Creation},
    {ProgramId::Passthrough, "passthrough",  VertShaderId::Passthrough, FragShaderId::Passthrough},
    {ProgramId::ColorMatrix, "color_matrix", VertShaderId::Passthrough, FragShaderId::ColorMatrix},
    {ProgramId::BlendMask,   "blend_mask",   VertShaderId::Passthrough, FragShaderId::BlendMask},
    {ProgramId::Blur,        "blur",         VertShaderId::Blur,        FragShaderId::Blur},
    {ProgramId::DropShadow,  "drop_shadow",  VertShaderId::Passthrough, FragShaderId::DropShadow},
};
// clang-format on

template <typename T, typename Enum>
class EnumArray {
public:
    const T& operator[](Enum id) const
    {
        RMLUI_ASSERT((size_t)id < (size_t)Enum::Count);
        return ids[size_t(id)];
    }
    T& operator[](Enum id)
    {
        RMLUI_ASSERT((size_t)id < (size_t)Enum::Count);
        return ids[size_t(id)];
    }
    auto begin() const { return ids.begin(); }
    auto end() const { return ids.end(); }

private:
    Rml::Array<T, (size_t)Enum::Count> ids = {};
};

struct ShaderProgram {
public:
    ID3D11VertexShader* vertex_shader;
    ID3D11PixelShader* pixel_shader;
};

using Programs = EnumArray<ShaderProgram, ProgramId>;
using VertShaders = EnumArray<ID3DBlob*, VertShaderId>;
using FragShaders = EnumArray<ID3DBlob*, FragShaderId>;

struct ProgramData {
    Programs programs;
    VertShaders vert_shaders;
    FragShaders frag_shaders;
};

// Create the shader, 'shader_type' is either GL_VERTEX_SHADER or GL_FRAGMENT_SHADER.
static bool CreateShader(ID3D11Device* p_device, ID3DBlob*& out_shader_dxil, ShaderType shader_type, const char* code_string, const size_t code_length)
{
    RMLUI_ASSERT(shader_type == ShaderType::Vertex || shader_type == ShaderType::Fragment);

    #ifdef RMLUI_DX_DEBUG
    static const UINT default_shader_flags = D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
    #else
    static const UINT default_shader_flags = 0;
    #endif

    // Compile shader
    const D3D_SHADER_MACRO macros[] = {"RMLUI_PREMULTIPLIED_ALPHA", nullptr, nullptr, nullptr};
    ID3DBlob* p_error_buff = nullptr;

    HRESULT result = D3DCompile(code_string, code_length, nullptr, macros, nullptr, shader_type == ShaderType::Vertex ? "VSMain" : "PSMain",
        shader_type == ShaderType::Vertex ? "vs_5_0" : "ps_5_0", default_shader_flags, 0, &out_shader_dxil, &p_error_buff);
    RMLUI_DX_ASSERTMSG(result, "failed to D3DCompile");
    #ifdef RMLUI_DX_DEBUG
    if (FAILED(result))
    {
        Rml::Log::Message(Rml::Log::Type::LT_ERROR, "failed to compile shader: %s", (char*)p_error_buff->GetBufferPointer());
        return false;
    }
    #endif

    DX_CLEANUP_RESOURCE_IF_CREATED(p_error_buff);

    return true;
}

static bool CreateProgram(ID3D11Device* p_device, ShaderProgram& out_program, ProgramId program_id, ID3DBlob* vertex_shader, ID3DBlob* fragment_shader)
{
    RMLUI_ASSERT(vertex_shader);
    RMLUI_ASSERT(fragment_shader);

    HRESULT result =
        p_device->CreateVertexShader(vertex_shader->GetBufferPointer(), vertex_shader->GetBufferSize(), nullptr, &out_program.vertex_shader);
    RMLUI_DX_ASSERTMSG(result, "failed to CreateVertexShader");
    if (FAILED(result))
    {
    #ifdef RMLUI_DX_DEBUG
        Rml::Log::Message(Rml::Log::Type::LT_ERROR, "failed to create vertex shader: %d", result);
    #endif
        return false;
    }

    result = p_device->CreatePixelShader(fragment_shader->GetBufferPointer(), fragment_shader->GetBufferSize(), nullptr, &out_program.pixel_shader);
    RMLUI_DX_ASSERTMSG(result, "failed to CreatePixelShader");
    if (FAILED(result))
    {
    #ifdef RMLUI_DX_DEBUG
        Rml::Log::Message(Rml::Log::Type::LT_ERROR, "failed to create pixel shader: %d", result);
    #endif
        return false;
    }

    return true;
}

struct RenderTargetData {
    int width = 0, height = 0;
    ID3D11RenderTargetView* render_target_view = nullptr; // To write to colour attachment buffer
    ID3D11DepthStencilView* depth_stencil_view = nullptr; // To write to stencil buffer
    ID3D11Texture2D* render_target_texture = nullptr; // For MSAA resolve
    ID3D11ShaderResourceView* render_target_shader_resource_view = nullptr; // To blit
    bool owns_depth_stencil_buffer = false;
};

enum class RenderTargetAttachment { None, Depth, DepthStencil };

static bool CreateRenderTarget(ID3D11Device* p_device, RenderTargetData& out_rt, int width, int height, int samples,
    RenderTargetAttachment attachment, ID3D11DepthStencilView* shared_depth_stencil_buffer)
{
    // Generate render target
    D3D11_TEXTURE2D_DESC texture_desc = {};
    ZeroMemory(&texture_desc, sizeof(D3D11_TEXTURE2D_DESC));
    texture_desc.Width = width;
    texture_desc.Height = height;
    texture_desc.MipLevels = 1;
    texture_desc.ArraySize = 1;
    texture_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    texture_desc.SampleDesc.Count = (samples > 0) ? samples : 1; // MSAA
    texture_desc.SampleDesc.Quality = 0;
    texture_desc.Usage = D3D11_USAGE_DEFAULT;
    texture_desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    texture_desc.CPUAccessFlags = 0;
    texture_desc.MiscFlags = 0;

    ID3D11Texture2D* rt_texture = nullptr;
    HRESULT result = p_device->CreateTexture2D(&texture_desc, nullptr, &rt_texture);
    RMLUI_DX_ASSERTMSG(result, "failed to CreateTexture2D");
    if (FAILED(result))
    {
        Rml::Log::Message(Rml::Log::LT_ERROR, "ID3D11Device::CreateTexture2D (%d)", result);
        return false;
    }

    D3D11_RENDER_TARGET_VIEW_DESC render_target_view_desc = {};
    render_target_view_desc.Format = texture_desc.Format;
    render_target_view_desc.ViewDimension = samples > 0 ? D3D11_RTV_DIMENSION_TEXTURE2DMS : D3D11_RTV_DIMENSION_TEXTURE2D;
    render_target_view_desc.Texture2D.MipSlice = 0;

    ID3D11RenderTargetView* render_target_view = nullptr;
    result = p_device->CreateRenderTargetView(rt_texture, &render_target_view_desc, &render_target_view);
    RMLUI_DX_ASSERTMSG(result, "failed to CreateRenderTargetView");
    if (FAILED(result))
    {
        Rml::Log::Message(Rml::Log::LT_ERROR, "ID3D11Device::CreateRenderTargetView (%d)", result);
        return false;
    }

    D3D11_SHADER_RESOURCE_VIEW_DESC render_target_shader_resource_view_desc = {};
    render_target_shader_resource_view_desc.Format = texture_desc.Format;
    render_target_shader_resource_view_desc.ViewDimension = samples > 0 ? D3D11_SRV_DIMENSION_TEXTURE2DMS : D3D11_SRV_DIMENSION_TEXTURE2D;
    render_target_shader_resource_view_desc.Texture2D.MostDetailedMip = 0;
    render_target_shader_resource_view_desc.Texture2D.MipLevels = 1;

    ID3D11ShaderResourceView* render_target_shader_resource_view = nullptr;
    result = p_device->CreateShaderResourceView(rt_texture, &render_target_shader_resource_view_desc, &render_target_shader_resource_view);
    RMLUI_DX_ASSERTMSG(result, "failed to CreateShaderResourceView");
    if (FAILED(result))
    {
        Rml::Log::Message(Rml::Log::LT_ERROR, "ID3D11Device::CreateShaderResourceView (%d)", result);
        return false;
    }

    // Generate stencil buffer if necessary
    ID3D11DepthStencilView* depth_stencil_view = nullptr;

    if (attachment != RenderTargetAttachment::None)
    {
        if (shared_depth_stencil_buffer)
        {
            // Share the depth/stencil buffer
            depth_stencil_view = shared_depth_stencil_buffer;
            depth_stencil_view->AddRef(); // Increase reference count since we're sharing it
        }
        else
        {
            // Create a new depth/stencil buffer
            D3D11_TEXTURE2D_DESC depthDesc = {};
            depthDesc.Width = width;
            depthDesc.Height = height;
            depthDesc.MipLevels = 1;
            depthDesc.ArraySize = 1;
            depthDesc.Format =
                (attachment == RenderTargetAttachment::DepthStencil) ? DXGI_FORMAT_D32_FLOAT_S8X24_UINT : DXGI_FORMAT_D24_UNORM_S8_UINT;
            depthDesc.SampleDesc.Count = (samples > 0) ? samples : 1;
            depthDesc.SampleDesc.Quality = 0;
            depthDesc.Usage = D3D11_USAGE_DEFAULT;
            depthDesc.BindFlags = D3D11_BIND_DEPTH_STENCIL;
            depthDesc.CPUAccessFlags = 0;
            depthDesc.MiscFlags = 0;

            ID3D11Texture2D* depth_stencil_texture = nullptr;
            result = p_device->CreateTexture2D(&depthDesc, nullptr, &depth_stencil_texture);
            RMLUI_DX_ASSERTMSG(result, "failed to CreateTexture2D");
            if (FAILED(result))
            {
                Rml::Log::Message(Rml::Log::LT_ERROR, "ID3D11Device::CreateTexture2D (%d)", result);
                return false;
            }

            D3D11_DEPTH_STENCIL_VIEW_DESC dsvDesc = {};
            dsvDesc.Format = depthDesc.Format;
            dsvDesc.ViewDimension = (samples > 0) ? D3D11_DSV_DIMENSION_TEXTURE2DMS : D3D11_DSV_DIMENSION_TEXTURE2D;
            dsvDesc.Texture2D.MipSlice = 0;

            result = p_device->CreateDepthStencilView(depth_stencil_texture, &dsvDesc, &depth_stencil_view);
            RMLUI_DX_ASSERTMSG(result, "failed to CreateDepthStencilView");
            if (FAILED(result))
            {
                depth_stencil_texture->Release();
                Rml::Log::Message(Rml::Log::LT_ERROR, "ID3D11Device::CreateDepthStencilView (%d)", result);
                return false;
            }

            depth_stencil_texture->Release();
        }
    }

    out_rt = {};
    out_rt.width = width;
    out_rt.height = height;
    out_rt.render_target_view = render_target_view;
    out_rt.render_target_texture = rt_texture;
    out_rt.render_target_shader_resource_view = render_target_shader_resource_view;
    out_rt.depth_stencil_view = depth_stencil_view;
    out_rt.owns_depth_stencil_buffer = shared_depth_stencil_buffer != nullptr;

    return true;
}

static void DestroyRenderTarget(RenderTargetData& rt)
{
    DX_CLEANUP_RESOURCE_IF_CREATED(rt.render_target_view);
    DX_CLEANUP_RESOURCE_IF_CREATED(rt.render_target_texture);
    DX_CLEANUP_RESOURCE_IF_CREATED(rt.render_target_shader_resource_view);
    DX_CLEANUP_RESOURCE_IF_CREATED(rt.depth_stencil_view);
    rt = {};
}

static void BindTexture(ID3D11DeviceContext* context, const RenderTargetData& rt, uint32_t slot = 1)
{
    context->PSSetShaderResources(0, slot, &rt.render_target_shader_resource_view);
}

static bool CreateShaders(ID3D11Device* p_device, ProgramData& data)
{
    RMLUI_ASSERT(std::all_of(data.vert_shaders.begin(), data.vert_shaders.end(), [](auto&& value) { return value == nullptr; }));
    RMLUI_ASSERT(std::all_of(data.frag_shaders.begin(), data.frag_shaders.end(), [](auto&& value) { return value == nullptr; }));
    RMLUI_ASSERT(std::all_of(data.programs.begin(), data.programs.end(), [](const ShaderProgram& value) { return value.vertex_shader == nullptr || value.pixel_shader == nullptr; }));
    auto ReportError = [](const char* type, const char* name) {
        Rml::Log::Message(Rml::Log::LT_ERROR, "Could not create shader %s: '%s'.", type, name);
        return false;
    };

    for (const VertShaderDefinition& def : vert_shader_definitions)
    {
        if (!CreateShader(p_device, data.vert_shaders[def.id], ShaderType::Vertex, def.code_str, def.code_size))
            return ReportError("vertex shader", def.name_str);
    }

    for (const FragShaderDefinition& def : frag_shader_definitions)
    {
        if (!CreateShader(p_device, data.frag_shaders[def.id], ShaderType::Fragment, def.code_str, def.code_size))
            return ReportError("fragment shader", def.name_str);
    }

    for (const ProgramDefinition& def : program_definitions)
    {
        if (!CreateProgram(p_device, data.programs[def.id], def.id, data.vert_shaders[def.vert_shader], data.frag_shaders[def.frag_shader]))
            return ReportError("program", def.name_str);
    }

    return true;
}

static void DestroyShaders(const ProgramData& data)
{
    for (ShaderProgram programs : data.programs)
    {
        DX_CLEANUP_RESOURCE_IF_CREATED(programs.vertex_shader);
        DX_CLEANUP_RESOURCE_IF_CREATED(programs.pixel_shader);
    }

    for (ID3DBlob* shader_blob : data.vert_shaders)
    {
        DX_CLEANUP_RESOURCE_IF_CREATED(shader_blob);
    }

    for (ID3DBlob* shader_blob : data.frag_shaders)
    {
        DX_CLEANUP_RESOURCE_IF_CREATED(shader_blob);
    }
}

} // namespace Gfx

static uintptr_t HashPointer(uintptr_t inPtr)
{
    uintptr_t Value = (uintptr_t)inPtr;
    Value = ~Value + (Value << 15);
    Value = Value ^ (Value >> 12);
    Value = Value + (Value << 2);
    Value = Value ^ (Value >> 4);
    Value = Value * 2057;
    Value = Value ^ (Value >> 16);
    return Value;
}

RenderInterface_DX11::RenderInterface_DX11() {}
RenderInterface_DX11::~RenderInterface_DX11() {}

void RenderInterface_DX11::Init(ID3D11Device* p_d3d_device, ID3D11DeviceContext* p_d3d_device_context)
{
    RMLUI_ASSERTMSG(p_d3d_device, "p_d3d_device cannot be nullptr!");
    RMLUI_ASSERTMSG(p_d3d_device_context, "p_d3d_device_context cannot be nullptr!");

    // Assign D3D resources
    m_d3d_device = p_d3d_device;
    m_d3d_context = p_d3d_device_context;
    m_render_layers.SetD3DResources(m_d3d_device);

    // Pre-cache quad for blitting
    Rml::Mesh mesh;
    Rml::MeshUtilities::GenerateQuad(mesh, Rml::Vector2f(-1), Rml::Vector2f(2), {});
    m_fullscreen_quad_geometry = RenderInterface_DX11::CompileGeometry(mesh.vertices, mesh.indices);

    // RmlUi serves vertex colors and textures with premultiplied alpha, set the blend mode accordingly.
    // Equivalent to glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA).
    if (!m_blend_state_enable)
    {
        D3D11_BLEND_DESC blend_desc{};
        ZeroMemory(&blend_desc, sizeof(blend_desc));
        blend_desc.AlphaToCoverageEnable = false;
        blend_desc.IndependentBlendEnable = false;
        blend_desc.RenderTarget[0].BlendEnable = false;
        HRESULT result = m_d3d_device->CreateBlendState(&blend_desc, &m_blend_state_disable);
        RMLUI_DX_ASSERTMSG(result, "failed to CreateBlendState");
    #ifdef RMLUI_DX_DEBUG
        if (FAILED(result))
        {
            Rml::Log::Message(Rml::Log::LT_ERROR, "ID3D11Device::CreateBlendState (%d)", result);
            return;
        }
    #endif

        blend_desc.RenderTarget[0].BlendEnable = true;
        blend_desc.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
        blend_desc.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
        blend_desc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
        blend_desc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
        blend_desc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
        blend_desc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
        blend_desc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        result = m_d3d_device->CreateBlendState(&blend_desc, &m_blend_state_enable);
        RMLUI_DX_ASSERTMSG(result, "failed to CreateBlendState");
    #ifdef RMLUI_DX_DEBUG
        if (FAILED(result))
        {
            Rml::Log::Message(Rml::Log::LT_ERROR, "ID3D11Device::CreateBlendState (%d)", result);
            return;
        }
    #endif

        ZeroMemory(&blend_desc, sizeof(blend_desc));
        blend_desc.RenderTarget[0].BlendEnable = false;
        blend_desc.RenderTarget[0].RenderTargetWriteMask = 0;
        result = m_d3d_device->CreateBlendState(&blend_desc, &m_blend_state_disable_color);
        RMLUI_DX_ASSERTMSG(result, "failed to CreateBlendState");
    #ifdef RMLUI_DX_DEBUG
        if (FAILED(result))
        {
            Rml::Log::Message(Rml::Log::LT_ERROR, "ID3D11Device::CreateBlendState (%d)", result);
            return;
        }
    #endif
    }

    // Scissor regions require a rasterizer state. Cache one for scissor on and off
    {
        D3D11_RASTERIZER_DESC rasterizerDesc{};
        rasterizerDesc.FillMode = D3D11_FILL_SOLID;
        rasterizerDesc.CullMode = D3D11_CULL_NONE;
        rasterizerDesc.FrontCounterClockwise = false;
        rasterizerDesc.DepthBias = D3D11_DEFAULT_DEPTH_BIAS;
        rasterizerDesc.SlopeScaledDepthBias = D3D11_DEFAULT_SLOPE_SCALED_DEPTH_BIAS;
        rasterizerDesc.DepthBiasClamp = D3D11_DEFAULT_DEPTH_BIAS_CLAMP;
        rasterizerDesc.DepthClipEnable = false;
        rasterizerDesc.ScissorEnable = true;
        rasterizerDesc.MultisampleEnable = NUM_MSAA_SAMPLES > 1;
        rasterizerDesc.AntialiasedLineEnable = NUM_MSAA_SAMPLES > 1;

        HRESULT result = m_d3d_device->CreateRasterizerState(&rasterizerDesc, &m_rasterizer_state_scissor_enabled);
        RMLUI_DX_ASSERTMSG(result, "failed to CreateRasterizerState");
    #ifdef RMLUI_DX_DEBUG
        if (FAILED(result))
        {
            Rml::Log::Message(Rml::Log::LT_ERROR, "ID3D11Device::CreateRasterizerState (scissor: enabled) (%d)", result);
            return;
        }
    #endif

        rasterizerDesc.ScissorEnable = false;

        result = m_d3d_device->CreateRasterizerState(&rasterizerDesc, &m_rasterizer_state_scissor_disabled);
        RMLUI_DX_ASSERTMSG(result, "failed to CreateRasterizerState");
    #ifdef RMLUI_DX_DEBUG
        if (FAILED(result))
        {
            Rml::Log::Message(Rml::Log::LT_ERROR, "ID3D11Device::CreateRasterizerState (scissor: disabled) (%d)", result);
            return;
        }
    #endif
    }

    // Compile and load shaders
    auto mut_program_data = Rml::MakeUnique<Gfx::ProgramData>();
    if (Gfx::CreateShaders(m_d3d_device, *mut_program_data))
    {
        program_data = std::move(mut_program_data);
    }

    // Create vertex layout. This will be constant to avoid copying to an intermediate struct.
    {
        D3D11_INPUT_ELEMENT_DESC polygonLayout[] = {
            {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_VERTEX_DATA, 0},
            {"COLOR", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_VERTEX_DATA, 0},
            {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_VERTEX_DATA, 0},
        };
        uint32_t numElements = sizeof(polygonLayout) / sizeof(polygonLayout[0]);

        HRESULT result =
            m_d3d_device->CreateInputLayout(polygonLayout, numElements, program_data->vert_shaders[VertShaderId::Main]->GetBufferPointer(),
                program_data->vert_shaders[VertShaderId::Main]->GetBufferSize(), &m_vertex_layout);
        RMLUI_DX_ASSERTMSG(result, "failed to CreateInputLayout");
        if (FAILED(result))
        {
            return;
        }
    }

    // Create constant buffers. This is so that we can bind uniforms such as translation and color to the shaders.
    {
        D3D11_BUFFER_DESC cbufferDesc{};
        cbufferDesc.Usage = D3D11_USAGE_DYNAMIC;
        cbufferDesc.ByteWidth = sizeof(ShaderCbuffer);
        cbufferDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        cbufferDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        cbufferDesc.MiscFlags = 0;
        cbufferDesc.StructureByteStride = 0;

        HRESULT result = m_d3d_device->CreateBuffer(&cbufferDesc, nullptr, &m_shader_buffer);
        RMLUI_DX_ASSERTMSG(result, "failed to CreateBuffer");
        if (FAILED(result))
        {
            return;
        }
    }

    // Create sampler state for textures
    {
        D3D11_SAMPLER_DESC samplerDesc{};
        samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        samplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_WRAP;
        samplerDesc.AddressV = D3D11_TEXTURE_ADDRESS_WRAP;
        samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
        samplerDesc.MipLODBias = 0.0f;
        samplerDesc.MaxAnisotropy = 1;
        samplerDesc.ComparisonFunc = D3D11_COMPARISON_ALWAYS;
        samplerDesc.BorderColor[0] = 0;
        samplerDesc.BorderColor[1] = 0;
        samplerDesc.BorderColor[2] = 0;
        samplerDesc.BorderColor[3] = 0;
        samplerDesc.MinLOD = 0;
        samplerDesc.MaxLOD = D3D11_FLOAT32_MAX;

        HRESULT result = m_d3d_device->CreateSamplerState(&samplerDesc, &m_samplerState);
        RMLUI_DX_ASSERTMSG(result, "failed to CreateSamplerState");
        if (FAILED(result))
        {
            return;
        }
    }

    // Create depth stencil states
    {
        D3D11_DEPTH_STENCIL_DESC desc;
        ZeroMemory(&desc, sizeof(desc));
        desc.DepthEnable = false;
        desc.FrontFace.StencilFunc = D3D11_COMPARISON_EQUAL;
        desc.FrontFace.StencilPassOp = D3D11_STENCIL_OP_KEEP;
        desc.FrontFace.StencilFailOp = D3D11_STENCIL_OP_KEEP;
        desc.FrontFace.StencilDepthFailOp = D3D11_STENCIL_OP_KEEP;
        desc.BackFace = desc.FrontFace;
        desc.StencilEnable = false;
        // Disabled
        m_d3d_device->CreateDepthStencilState(&desc, &m_depth_stencil_state_disable);

        desc.StencilEnable = true;
        desc.StencilReadMask = D3D11_DEFAULT_STENCIL_READ_MASK;
        desc.StencilWriteMask = D3D11_DEFAULT_STENCIL_WRITE_MASK;
        desc.FrontFace.StencilFunc = D3D11_COMPARISON_ALWAYS;
        desc.FrontFace.StencilPassOp = D3D11_STENCIL_OP_REPLACE;
        desc.FrontFace.StencilFailOp = D3D11_STENCIL_OP_KEEP;
        desc.FrontFace.StencilDepthFailOp = D3D11_STENCIL_OP_KEEP;
        desc.BackFace = desc.FrontFace;
        // Set and SetInverse
        m_d3d_device->CreateDepthStencilState(&desc, &m_depth_stencil_state_stencil_set);

        desc.FrontFace.StencilFunc = D3D11_COMPARISON_ALWAYS;
        desc.FrontFace.StencilPassOp = D3D11_STENCIL_OP_INCR;
        desc.FrontFace.StencilFailOp = D3D11_STENCIL_OP_KEEP;
        desc.FrontFace.StencilDepthFailOp = D3D11_STENCIL_OP_KEEP;
        desc.BackFace = desc.FrontFace;
        // Intersect
        m_d3d_device->CreateDepthStencilState(&desc, &m_depth_stencil_state_stencil_intersect);
    }
}

void RenderInterface_DX11::Cleanup()
{
    if (program_data)
    {
        Gfx::DestroyShaders(*program_data);
        program_data.reset();
    }

    // Loop through geometry cache and free all resources
    for (auto& it : m_geometry_cache)
    {
        DX_CLEANUP_RESOURCE_IF_CREATED(it.second.vertex_buffer);
        DX_CLEANUP_RESOURCE_IF_CREATED(it.second.index_buffer);
    }

    // Cleans up all general resources
    DX_CLEANUP_RESOURCE_IF_CREATED(m_samplerState);
    DX_CLEANUP_RESOURCE_IF_CREATED(m_blend_state_enable);
    DX_CLEANUP_RESOURCE_IF_CREATED(m_blend_state_disable);
    DX_CLEANUP_RESOURCE_IF_CREATED(m_blend_state_disable_color);
    DX_CLEANUP_RESOURCE_IF_CREATED(m_depth_stencil_state_disable);
    DX_CLEANUP_RESOURCE_IF_CREATED(m_depth_stencil_state_stencil_intersect);
    DX_CLEANUP_RESOURCE_IF_CREATED(m_depth_stencil_state_stencil_set);
    DX_CLEANUP_RESOURCE_IF_CREATED(m_rasterizer_state_scissor_disabled);
    DX_CLEANUP_RESOURCE_IF_CREATED(m_rasterizer_state_scissor_enabled);
    DX_CLEANUP_RESOURCE_IF_CREATED(m_shader_buffer);
    DX_CLEANUP_RESOURCE_IF_CREATED(m_vertex_layout);
}

void RenderInterface_DX11::BeginFrame(IDXGISwapChain* p_swapchain, ID3D11RenderTargetView* p_render_target_view)
{
    RMLUI_ASSERT(m_viewport_width >= 1 && m_viewport_height >= 1);

    RMLUI_ASSERTMSG(p_swapchain, "p_swapchain cannot be nullptr!");
    RMLUI_ASSERTMSG(p_render_target_view, "p_render_target_view cannot be nullptr!");
    RMLUI_ASSERTMSG(m_d3d_context, "d3d_context cannot be nullptr!");
    RMLUI_ASSERTMSG(m_d3d_device, "d3d_device cannot be nullptr!");

    // Backup DX11 state
    {
        m_previous_d3d_state.scissor_rects_count = m_previous_d3d_state.viewports_count = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
        m_d3d_context->RSGetScissorRects(&m_previous_d3d_state.scissor_rects_count, m_previous_d3d_state.scissor_rects);
        m_d3d_context->RSGetViewports(&m_previous_d3d_state.viewports_count, m_previous_d3d_state.viewports);
        m_d3d_context->RSGetState(&m_previous_d3d_state.rastizer_state);
        m_d3d_context->OMGetBlendState(&m_previous_d3d_state.blend_state, m_previous_d3d_state.blend_factor, &m_previous_d3d_state.sample_mask);
        m_d3d_context->OMGetDepthStencilState(&m_previous_d3d_state.depth_stencil_state, &m_previous_d3d_state.stencil_ref);
        m_d3d_context->PSGetShaderResources(0, 1, &m_previous_d3d_state.pixel_shader_shader_resource);
        m_d3d_context->PSGetSamplers(0, 1, &m_previous_d3d_state.pixel_shader_sampler);
        m_previous_d3d_state.pixel_shader_instances_count = m_previous_d3d_state.vertex_shader_instances_count =
            m_previous_d3d_state.geometry_shader_instances_count = 256;
        m_d3d_context->PSGetShader(&m_previous_d3d_state.pixel_shader, m_previous_d3d_state.pixel_shader_instances,
            &m_previous_d3d_state.pixel_shader_instances_count);
        m_d3d_context->VSGetShader(&m_previous_d3d_state.vertex_shader, m_previous_d3d_state.vertex_shader_instances,
            &m_previous_d3d_state.vertex_shader_instances_count);
        m_d3d_context->VSGetConstantBuffers(0, 1, &m_previous_d3d_state.vertex_shader_constant_buffer);
        m_d3d_context->GSGetShader(&m_previous_d3d_state.geometry_shader, m_previous_d3d_state.geometry_shader_instances,
            &m_previous_d3d_state.geometry_shader_instances_count);

        m_d3d_context->IAGetPrimitiveTopology(&m_previous_d3d_state.primitive_topology);
        m_d3d_context->IAGetIndexBuffer(&m_previous_d3d_state.index_buffer, &m_previous_d3d_state.index_buffer_format,
            &m_previous_d3d_state.index_buffer_offset);
        m_d3d_context->IAGetVertexBuffers(0, 1, &m_previous_d3d_state.vertex_buffer, &m_previous_d3d_state.vertex_buffer_stride,
            &m_previous_d3d_state.vertex_buffer_offset);
        m_d3d_context->IAGetInputLayout(&m_previous_d3d_state.input_layout);
    }

    m_bound_render_target = p_render_target_view;
    m_bound_swapchain = p_swapchain;

    // Initialise DX11 state for RmlUi
    D3D11_VIEWPORT d3dviewport;
    d3dviewport.TopLeftX = 0;
    d3dviewport.TopLeftY = 0;
    d3dviewport.Width = m_viewport_width;
    d3dviewport.Height = m_viewport_height;
    d3dviewport.MinDepth = 0.0f;
    d3dviewport.MaxDepth = 1.0f;
    m_d3d_context->RSSetViewports(1, &d3dviewport);
    SetBlendState(m_blend_state_enable);
    m_d3d_context->RSSetState(m_rasterizer_state_scissor_disabled); // Disable scissor
    m_d3d_context->OMSetDepthStencilState(m_depth_stencil_state_disable, 0);
    Clear();
    
    SetTransform(nullptr);

    m_render_layers.BeginFrame(m_viewport_width, m_viewport_height);
    m_d3d_context->OMSetRenderTargets(1, &m_render_layers.GetTopLayer().render_target_view, m_render_layers.GetTopLayer().depth_stencil_view);
    float clearColor[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    m_d3d_context->ClearRenderTargetView(m_render_layers.GetTopLayer().render_target_view, clearColor);

    UseProgram(ProgramId::None);
    // program_transform_dirty.set();
    m_scissor_state = Rml::Rectanglei::MakeInvalid();
}

void RenderInterface_DX11::EndFrame()
{
    RMLUI_ASSERTMSG(m_bound_render_target, "m_bound_render_target cannot be nullptr!");

    const Gfx::RenderTargetData& rt_active = m_render_layers.GetTopLayer();
    const Gfx::RenderTargetData& rt_postprocess = m_render_layers.GetPostprocessPrimary();

    // Resolve MSAA to the post-process framebuffer.
    m_d3d_context->ResolveSubresource(rt_postprocess.render_target_texture, 0, rt_active.render_target_texture, 0, DXGI_FORMAT_R8G8B8A8_UNORM);

    // Draw to bound_render_target (usually the swapchain RTV)
    m_d3d_context->OMSetRenderTargets(1, &m_bound_render_target, nullptr);

    // Assuming we have an opaque background, we can just write to it with the premultiplied alpha blend mode and we'll get the correct result.
    // Instead, if we had a transparent destination that didn't use premultiplied alpha, we would need to perform a manual un-premultiplication step.
    Gfx::BindTexture(m_d3d_context, rt_postprocess);

    // @TODO: Find a pattern for flipped textures
    UseProgram(ProgramId::Passthrough);

    DrawFullscreenQuad();

    m_render_layers.EndFrame();

    // Reset internal state
    m_bound_swapchain = nullptr;
    m_bound_render_target = nullptr;
    m_current_blend_state = nullptr;

    // Restore modified DX state
    {
        m_d3d_context->RSSetScissorRects(m_previous_d3d_state.scissor_rects_count, m_previous_d3d_state.scissor_rects);
        m_d3d_context->RSSetViewports(m_previous_d3d_state.viewports_count, m_previous_d3d_state.viewports);
        m_d3d_context->RSSetState(m_previous_d3d_state.rastizer_state);
        if (m_previous_d3d_state.rastizer_state)
        {
            m_previous_d3d_state.rastizer_state->Release();
        }
        m_d3d_context->OMSetBlendState(m_previous_d3d_state.blend_state, m_previous_d3d_state.blend_factor, m_previous_d3d_state.sample_mask);
        if (m_previous_d3d_state.blend_state)
        {
            m_previous_d3d_state.blend_state->Release();
        }
        m_d3d_context->OMSetDepthStencilState(m_previous_d3d_state.depth_stencil_state, m_previous_d3d_state.stencil_ref);
        if (m_previous_d3d_state.depth_stencil_state)
        {
            m_previous_d3d_state.depth_stencil_state->Release();
        }
        m_d3d_context->PSSetShaderResources(0, 1, &m_previous_d3d_state.pixel_shader_shader_resource);
        if (m_previous_d3d_state.pixel_shader_shader_resource)
        {
            m_previous_d3d_state.pixel_shader_shader_resource->Release();
        }
        m_d3d_context->PSSetSamplers(0, 1, &m_previous_d3d_state.pixel_shader_sampler);
        if (m_previous_d3d_state.pixel_shader_sampler)
        {
            m_previous_d3d_state.pixel_shader_sampler->Release();
        }
        m_d3d_context->PSSetShader(m_previous_d3d_state.pixel_shader, m_previous_d3d_state.pixel_shader_instances,
            m_previous_d3d_state.pixel_shader_instances_count);
        if (m_previous_d3d_state.pixel_shader)
        {
            m_previous_d3d_state.pixel_shader->Release();
        }
        for (UINT i = 0; i < m_previous_d3d_state.pixel_shader_instances_count; i++)
        {
            if (m_previous_d3d_state.pixel_shader_instances[i])
            {
                m_previous_d3d_state.pixel_shader_instances[i]->Release();
            }
        }
        m_d3d_context->VSSetShader(m_previous_d3d_state.vertex_shader, m_previous_d3d_state.vertex_shader_instances,
            m_previous_d3d_state.vertex_shader_instances_count);
        if (m_previous_d3d_state.vertex_shader)
        {
            m_previous_d3d_state.vertex_shader->Release();
        }
        m_d3d_context->VSSetConstantBuffers(0, 1, &m_previous_d3d_state.vertex_shader_constant_buffer);
        if (m_previous_d3d_state.vertex_shader_constant_buffer)
        {
            m_previous_d3d_state.vertex_shader_constant_buffer->Release();
        }
        m_d3d_context->GSSetShader(m_previous_d3d_state.geometry_shader, m_previous_d3d_state.geometry_shader_instances,
            m_previous_d3d_state.geometry_shader_instances_count);
        if (m_previous_d3d_state.geometry_shader)
        {
            m_previous_d3d_state.geometry_shader->Release();
        }
        for (UINT i = 0; i < m_previous_d3d_state.vertex_shader_instances_count; i++)
        {
            if (m_previous_d3d_state.vertex_shader_instances[i])
            {
                m_previous_d3d_state.vertex_shader_instances[i]->Release();
            }
        }
        m_d3d_context->IASetPrimitiveTopology(m_previous_d3d_state.primitive_topology);
        m_d3d_context->IASetIndexBuffer(m_previous_d3d_state.index_buffer, m_previous_d3d_state.index_buffer_format,
            m_previous_d3d_state.index_buffer_offset);
        if (m_previous_d3d_state.index_buffer)
        {
            m_previous_d3d_state.index_buffer->Release();
        }
        m_d3d_context->IASetVertexBuffers(0, 1, &m_previous_d3d_state.vertex_buffer, &m_previous_d3d_state.vertex_buffer_stride,
            &m_previous_d3d_state.vertex_buffer_offset);
        if (m_previous_d3d_state.vertex_buffer)
        {
            m_previous_d3d_state.vertex_buffer->Release();
        }
        m_d3d_context->IASetInputLayout(m_previous_d3d_state.input_layout);
        if (m_previous_d3d_state.input_layout)
        {
            m_previous_d3d_state.input_layout->Release();
        }
    }
}

Rml::CompiledGeometryHandle RenderInterface_DX11::CompileGeometry(Rml::Span<const Rml::Vertex> vertices, Rml::Span<const int> indices)
{
    ID3D11Buffer* vertex_buffer = nullptr;
    ID3D11Buffer* index_buffer = nullptr;

    // Bind vertex buffer
    {
        D3D11_BUFFER_DESC vertexBufferDesc{};
        vertexBufferDesc.Usage = D3D11_USAGE_DEFAULT;
        vertexBufferDesc.ByteWidth = sizeof(Rml::Vertex) * vertices.size();
        vertexBufferDesc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        vertexBufferDesc.CPUAccessFlags = 0;
        vertexBufferDesc.MiscFlags = 0;
        vertexBufferDesc.StructureByteStride = 0;

        D3D11_SUBRESOURCE_DATA vertexData{};
        vertexData.pSysMem = vertices.data();
        vertexData.SysMemPitch = 0;
        vertexData.SysMemSlicePitch = 0;

        HRESULT result = m_d3d_device->CreateBuffer(&vertexBufferDesc, &vertexData, &vertex_buffer);
        RMLUI_DX_ASSERTMSG(result, "failed to CreateBuffer");
        if (FAILED(result))
        {
            return Rml::CompiledGeometryHandle(0);
        }
    }

    // Bind index buffer
    {
        D3D11_BUFFER_DESC indexBufferDesc{};
        indexBufferDesc.Usage = D3D11_USAGE_DEFAULT;
        indexBufferDesc.ByteWidth = sizeof(int) * indices.size();
        indexBufferDesc.BindFlags = D3D11_BIND_INDEX_BUFFER;
        indexBufferDesc.CPUAccessFlags = 0;
        indexBufferDesc.MiscFlags = 0;
        indexBufferDesc.StructureByteStride = 0;

        D3D11_SUBRESOURCE_DATA indexData{};
        indexData.pSysMem = indices.data();
        indexData.SysMemPitch = 0;
        indexData.SysMemSlicePitch = 0;

        HRESULT result = m_d3d_device->CreateBuffer(&indexBufferDesc, &indexData, &index_buffer);
        RMLUI_DX_ASSERTMSG(result, "failed to CreateBuffer");
        if (FAILED(result))
        {
            DX_CLEANUP_RESOURCE_IF_CREATED(vertex_buffer);
            return Rml::CompiledGeometryHandle(0);
        }
    }
    uintptr_t handleId = HashPointer((uintptr_t)index_buffer);

    DX11_GeometryData geometryData{};

    geometryData.vertex_buffer = vertex_buffer;
    geometryData.index_buffer = index_buffer;
    geometryData.index_count = indices.size();

    m_geometry_cache.emplace(handleId, geometryData);
    return Rml::CompiledGeometryHandle(handleId);
}

void RenderInterface_DX11::ReleaseGeometry(Rml::CompiledGeometryHandle handle)
{
    if (m_geometry_cache.find(handle) != m_geometry_cache.end())
    {
        DX11_GeometryData geometryData = m_geometry_cache[handle];

        DX_CLEANUP_RESOURCE_IF_CREATED(geometryData.vertex_buffer);
        DX_CLEANUP_RESOURCE_IF_CREATED(geometryData.index_buffer);

        m_geometry_cache.erase(handle);
    }
    else
    {
        Rml::Log::Message(Rml::Log::LT_WARNING, "Geometry Handle %d does not exist!", handle);
    }
}

void RenderInterface_DX11::RenderGeometry(Rml::CompiledGeometryHandle handle, Rml::Vector2f translation, Rml::TextureHandle texture)
{
    if (m_geometry_cache.find(handle) != m_geometry_cache.end())
    {
        DX11_GeometryData geometryData = m_geometry_cache[handle];

        if (m_translation != translation)
        {
            m_translation = translation;
            m_cbuffer_dirty = true;
        }

        if (texture == TexturePostprocess)
        {

        }
        else if (texture)
        {
            // Texture available
            UseProgram(ProgramId::Texture);
            if (texture != TextureEnableWithoutBinding)
            {
                ID3D11ShaderResourceView* texture_view = (ID3D11ShaderResourceView*)texture;
                m_d3d_context->PSSetShaderResources(0, 1, &texture_view);
            }
            m_d3d_context->PSSetSamplers(0, 1, &m_samplerState);
        }
        else
        {
            // No texture, use color
            UseProgram(ProgramId::Color);
        }

        UpdateConstantBuffer();

        m_d3d_context->IASetInputLayout(m_vertex_layout);
        m_d3d_context->VSSetConstantBuffers(0, 1, &m_shader_buffer);
        m_d3d_context->PSSetConstantBuffers(0, 1, &m_shader_buffer);

        // Bind vertex and index buffers, issue draw call
        uint32_t stride = sizeof(Rml::Vertex);
        uint32_t offset = 0;
        m_d3d_context->IASetVertexBuffers(0, 1, &geometryData.vertex_buffer, &stride, &offset);
        m_d3d_context->IASetIndexBuffer(geometryData.index_buffer, DXGI_FORMAT_R32_UINT, 0);
        m_d3d_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        m_d3d_context->DrawIndexed(geometryData.index_count, 0, 0);
    }
    else
    {
        Rml::Log::Message(Rml::Log::LT_WARNING, "Geometry Handle %d does not exist!", handle);
    }
}

// Set to byte packing, or the compiler will expand our struct, which means it won't read correctly from file
#pragma pack(1)
struct TGAHeader {
    char idLength;
    char colourMapType;
    char dataType;
    short int colourMapOrigin;
    short int colourMapLength;
    char colourMapDepth;
    short int xOrigin;
    short int yOrigin;
    short int width;
    short int height;
    char bitsPerPixel;
    char imageDescriptor;
};
// Restore packing
#pragma pack()


Rml::TextureHandle RenderInterface_DX11::LoadTexture(Rml::Vector2i& texture_dimensions, const Rml::String& source)
{
    // Use the user provided image loading function if it's provided, else fallback to the included TGA one
    if (LoadTextureFromFileRaw != nullptr && FreeTextureFromFileRaw != nullptr)
    {
        int texture_width = 0, texture_height = 0;
        size_t image_size_bytes = 0;
        uint8_t* texture_data = nullptr;
        LoadTextureFromFileRaw(source, &texture_width, &texture_height, &texture_data, &image_size_bytes);

        if (texture_data != nullptr)
        {
            texture_dimensions.x = texture_width;
            texture_dimensions.y = texture_height;

            Rml::TextureHandle handle = GenerateTexture({texture_data, image_size_bytes}, texture_dimensions);

            FreeTextureFromFileRaw(texture_data);
            return handle;
        }

        // Image must be invalid if the file failed to load. Fallback to the default loader.
    }

    Rml::FileInterface* file_interface = Rml::GetFileInterface();
    Rml::FileHandle file_handle = file_interface->Open(source);
    if (!file_handle)
    {
        return false;
    }

    file_interface->Seek(file_handle, 0, SEEK_END);
    size_t buffer_size = file_interface->Tell(file_handle);
    file_interface->Seek(file_handle, 0, SEEK_SET);

    if (buffer_size <= sizeof(TGAHeader))
    {
        Rml::Log::Message(Rml::Log::LT_ERROR, "Texture file size is smaller than TGAHeader, file is not a valid TGA image.");
        file_interface->Close(file_handle);
        return false;
    }

    using Rml::byte;
    Rml::UniquePtr<byte[]> buffer(new byte[buffer_size]);
    file_interface->Read(buffer.get(), buffer_size, file_handle);
    file_interface->Close(file_handle);

    TGAHeader header;
    memcpy(&header, buffer.get(), sizeof(TGAHeader));

    int color_mode = header.bitsPerPixel / 8;
    const size_t image_size = header.width * header.height * 4; // We always make 32bit textures

    if (header.dataType != 2)
    {
        Rml::Log::Message(Rml::Log::LT_ERROR, "Only 24/32bit uncompressed TGAs are supported.");
        return false;
    }

    // Ensure we have at least 3 colors
    if (color_mode < 3)
    {
        Rml::Log::Message(Rml::Log::LT_ERROR, "Only 24 and 32bit textures are supported.");
        return false;
    }

    const byte* image_src = buffer.get() + sizeof(TGAHeader);
    Rml::UniquePtr<byte[]> image_dest_buffer(new byte[image_size]);
    byte* image_dest = image_dest_buffer.get();

    // Targa is BGR, swap to RGB, flip Y axis, and convert to premultiplied alpha.
    for (long y = 0; y < header.height; y++)
    {
        long read_index = y * header.width * color_mode;
        long write_index = ((header.imageDescriptor & 32) != 0) ? read_index : (header.height - y - 1) * header.width * 4;
        for (long x = 0; x < header.width; x++)
        {
            image_dest[write_index] = image_src[read_index + 2];
            image_dest[write_index + 1] = image_src[read_index + 1];
            image_dest[write_index + 2] = image_src[read_index];
            if (color_mode == 4)
            {
                const byte alpha = image_src[read_index + 3];
                for (size_t j = 0; j < 3; j++)
                    image_dest[write_index + j] = byte((image_dest[write_index + j] * alpha) / 255);
                image_dest[write_index + 3] = alpha;
            }
            else
                image_dest[write_index + 3] = 255;

            write_index += 4;
            read_index += color_mode;
        }
    }

    texture_dimensions.x = header.width;
    texture_dimensions.y = header.height;

    return GenerateTexture({image_dest, image_size}, texture_dimensions);
}

Rml::TextureHandle RenderInterface_DX11::GenerateTexture(Rml::Span<const Rml::byte> source, Rml::Vector2i source_dimensions)
{
    ID3D11Texture2D* gpu_texture = nullptr;
    ID3D11ShaderResourceView* gpu_texture_view = nullptr;

    D3D11_TEXTURE2D_DESC textureDesc{};
    textureDesc.Width = source_dimensions.x;
    textureDesc.Height = source_dimensions.y;
    textureDesc.MipLevels = 0;
    textureDesc.ArraySize = 1;
    textureDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    textureDesc.SampleDesc.Count = 1;
    textureDesc.SampleDesc.Quality = 0;
    textureDesc.Usage = D3D11_USAGE_DEFAULT;
    textureDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    textureDesc.CPUAccessFlags = 0;
    textureDesc.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;

    HRESULT result = m_d3d_device->CreateTexture2D(&textureDesc, nullptr, &gpu_texture);
    RMLUI_DX_ASSERTMSG(result, "failed to CreateTexture2D");
    if (FAILED(result))
    {
        return Rml::TextureHandle(0);
    }

    // Set the row pitch of the raw image data
    uint32_t rowPitch = (source_dimensions.x * 4) * sizeof(unsigned char);

    // Copy the raw image data into the texture
    if (source.size() > 0)
    {
        m_d3d_context->UpdateSubresource(gpu_texture, 0, nullptr, source.data(), rowPitch, 0);
    }

    // Setup the shader resource view description.
    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
    srvDesc.Format = textureDesc.Format;
    srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MostDetailedMip = 0;
    srvDesc.Texture2D.MipLevels = -1;

    // Create the shader resource view for the texture.
    result = m_d3d_device->CreateShaderResourceView(gpu_texture, &srvDesc, &gpu_texture_view);
    RMLUI_DX_ASSERTMSG(result, "failed to CreateShaderResourceView");
    if (FAILED(result))
    {
        DX_CLEANUP_RESOURCE_IF_CREATED(gpu_texture);
        return Rml::TextureHandle(0);
    }
    DX_CLEANUP_RESOURCE_IF_CREATED(gpu_texture);

    // Generate mipmaps for this texture.
    if (source.size() > 0)
    {
        m_d3d_context->GenerateMips(gpu_texture_view);
    }

    uintptr_t handleId = HashPointer((uintptr_t)gpu_texture_view);

    return Rml::TextureHandle(gpu_texture_view);
}

void RenderInterface_DX11::ReleaseTexture(Rml::TextureHandle texture_handle)
{
    ID3D11ShaderResourceView* texture_view = (ID3D11ShaderResourceView*)texture_handle;
    DX_CLEANUP_RESOURCE_IF_CREATED(texture_view);
}

static Rml::Colourf ConvertToColorf(Rml::ColourbPremultiplied c0)
{
    Rml::Colourf result;
    for (int i = 0; i < 4; i++)
        result[i] = (1.f / 255.f) * float(c0[i]);
    return result;
}

void RenderInterface_DX11::DrawFullscreenQuad()
{
    RenderGeometry(m_fullscreen_quad_geometry, {}, RenderInterface_DX11::TexturePostprocess);
}

void RenderInterface_DX11::DrawFullscreenQuad(Rml::Vector2f uv_offset, Rml::Vector2f uv_scaling)
{
    Rml::Mesh mesh;
    Rml::MeshUtilities::GenerateQuad(mesh, Rml::Vector2f(-1), Rml::Vector2f(2), {});
    if (uv_offset != Rml::Vector2f() || uv_scaling != Rml::Vector2f(1.f))
    {
        for (Rml::Vertex& vertex : mesh.vertices)
            vertex.tex_coord = (vertex.tex_coord * uv_scaling) + uv_offset;
    }
    const Rml::CompiledGeometryHandle geometry = CompileGeometry(mesh.vertices, mesh.indices);
    RenderGeometry(geometry, {}, RenderInterface_DX11::TexturePostprocess);
    ReleaseGeometry(geometry);
}

/// Flip vertical axis of the rectangle, and move its origin to the vertically opposite side of the viewport.
/// @note Changes coordinate system from RmlUi to OpenGL, or equivalently in reverse.
/// @note The Rectangle::Top and Rectangle::Bottom members will have reverse meaning in the returned rectangle.
static Rml::Rectanglei VerticallyFlipped(Rml::Rectanglei rect, int viewport_height)
{
    RMLUI_ASSERT(rect.Valid());
    Rml::Rectanglei flipped_rect = rect;
    flipped_rect.p0.y = viewport_height - rect.p1.y;
    flipped_rect.p1.y = viewport_height - rect.p0.y;
    return flipped_rect;
}


void RenderInterface_DX11::SetScissor(Rml::Rectanglei region, bool vertically_flip)
{
    if (region.Valid() != m_scissor_state.Valid())
    {
        if (region.Valid())
            m_d3d_context->RSSetState(m_rasterizer_state_scissor_enabled);
        else
            m_d3d_context->RSSetState(m_rasterizer_state_scissor_disabled);
    }

    if (region.Valid() && vertically_flip)
        region = VerticallyFlipped(region, m_viewport_height);

    if (region.Valid() && region != m_scissor_state)
    {
        // Some render APIs don't like offscreen positions (WebGL in particular), so clamp them to the viewport.
        const int x = Rml::Math::Clamp(region.Left(), 0, m_viewport_width);
        const int y = Rml::Math::Clamp(m_viewport_height - region.Bottom(), 0, m_viewport_height);

        D3D11_RECT rect_scissor = {};
        rect_scissor.left = x;
        rect_scissor.top = y;
        rect_scissor.right = x + region.Width();
        rect_scissor.bottom = y + region.Height();

        m_d3d_context->RSSetScissorRects(1, &rect_scissor);
    }

    m_scissor_state = region;
}

void RenderInterface_DX11::EnableScissorRegion(bool enable)
{
    // Assume enable is immediately followed by a SetScissorRegion() call, and ignore it here.
    if (!enable)
        SetScissor(Rml::Rectanglei::MakeInvalid(), false);
}

void RenderInterface_DX11::SetScissorRegion(Rml::Rectanglei region)
{
    SetScissor(region);
}

void RenderInterface_DX11::SetViewport(const int width, const int height)
{
    m_viewport_width = width;
    m_viewport_height = height;
    m_projection = Rml::Matrix4f::ProjectOrtho(0, (float)m_viewport_width, (float)m_viewport_height, 0, -10000, 10000);
}

void RenderInterface_DX11::Clear()
{
    float clearColor[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    m_d3d_context->ClearRenderTargetView(m_bound_render_target, clearColor);
}

void RenderInterface_DX11::SetBlendState(ID3D11BlendState* blend_state)
{
    if (blend_state != m_current_blend_state)
    {
        const float blend_factor[4] = {0.f, 0.f, 0.f, 0.f};
        m_d3d_context->OMSetBlendState(blend_state, blend_factor, 0xFFFFFFFF);
        m_current_blend_state = blend_state;
    }
}

void RenderInterface_DX11::SetTransform(const Rml::Matrix4f* new_transform)
{
    m_transform = (new_transform ? (m_projection * (*new_transform)) : m_projection);
    m_cbuffer_dirty = true;
}

void RenderInterface_DX11::BlitLayerToPostprocessPrimary(Rml::LayerHandle layer_handle)
{
    const Gfx::RenderTargetData& source = m_render_layers.GetLayer(layer_handle);
    const Gfx::RenderTargetData& destination = m_render_layers.GetPostprocessPrimary();

    // Blit and resolve MSAA. Any active scissor state will restrict the size of the blit region.
    m_d3d_context->ResolveSubresource(destination.render_target_texture, 0, source.render_target_texture, 0, DXGI_FORMAT_R8G8B8A8_UNORM);
}

Rml::LayerHandle RenderInterface_DX11::PushLayer()
{
    const Rml::LayerHandle layer_handle = m_render_layers.PushLayer();

    m_d3d_context->OMSetRenderTargets(1, &m_render_layers.GetTopLayer().render_target_view, m_render_layers.GetTopLayer().depth_stencil_view);
    float colors[4] = {0, 0, 0, 0};
    m_d3d_context->ClearRenderTargetView(m_render_layers.GetTopLayer().render_target_view, colors);

    return layer_handle;
}

void RenderInterface_DX11::CompositeLayers(Rml::LayerHandle source_handle, Rml::LayerHandle destination_handle, Rml::BlendMode blend_mode,
    Rml::Span<const Rml::CompiledFilterHandle> filters)
{
    using Rml::BlendMode;

    // Blit source layer to postprocessing buffer. Do this regardless of whether we actually have any filters to be
    // applied, because we need to resolve the multi-sampled framebuffer in any case.
    // @performance If we have BlendMode::Replace and no filters or mask then we can just blit directly to the destination.
    BlitLayerToPostprocessPrimary(source_handle);

    // Render the filters, the PostprocessPrimary framebuffer is used for both input and output.
    // @TODO: RenderFilters implementation
    // RenderFilters(filters);

    // Render to the destination layer.
    m_d3d_context->OMSetRenderTargets(1, &m_render_layers.GetLayer(destination_handle).render_target_view,
        m_render_layers.GetLayer(destination_handle).depth_stencil_view);
    Gfx::BindTexture(m_d3d_context, m_render_layers.GetPostprocessPrimary());

    UseProgram(ProgramId::Passthrough);

    if (blend_mode == BlendMode::Replace)
        SetBlendState(m_blend_state_disable);

    DrawFullscreenQuad();

    if (blend_mode == BlendMode::Replace)
        SetBlendState(m_blend_state_enable);

    if (destination_handle != m_render_layers.GetTopLayerHandle())
        m_d3d_context->OMSetRenderTargets(1, &m_render_layers.GetTopLayer().render_target_view, m_render_layers.GetTopLayer().depth_stencil_view);
}

void RenderInterface_DX11::PopLayer()
{
    m_render_layers.PopLayer();
    m_d3d_context->OMSetRenderTargets(1, &m_render_layers.GetTopLayer().render_target_view, m_render_layers.GetTopLayer().depth_stencil_view);
}

Rml::TextureHandle RenderInterface_DX11::SaveLayerAsTexture()
{
    RMLUI_ASSERT(m_scissor_state.Valid());
    const Rml::Rectanglei bounds = m_scissor_state;

    Rml::TextureHandle render_texture = GenerateTexture({}, bounds.Size());
    if (!render_texture)
        return {};

    BlitLayerToPostprocessPrimary(m_render_layers.GetTopLayerHandle());

    EnableScissorRegion(false);

    const Gfx::RenderTargetData& source = m_render_layers.GetPostprocessPrimary();
    const Gfx::RenderTargetData& destination = m_render_layers.GetPostprocessSecondary();
    
    // Flip the image vertically, as that convention is used for textures, and move to origin.
    D3D11_BOX source_box{};
    source_box.left = bounds.Left();
    source_box.right = bounds.Right();
    source_box.top = source.height - bounds.Bottom();
    source_box.bottom = source.height - bounds.Top();
    source_box.front = 0;
    source_box.back = 1;

    // Perform a copy from the source render target to the destination render target
    m_d3d_context->CopySubresourceRegion(destination.render_target_texture, 0, 0, 0, 0, source.render_target_texture, 0, &source_box);

    // Now we need to copy the destination texture to the final texture for rendering
    // Bind the destination texture as the source for copying to the final texture (render_texture)
    // after extracting the associated resource with it
    ID3D11ShaderResourceView* texture_view = (ID3D11ShaderResourceView*)render_texture;
    ID3D11Resource* texture_resource = nullptr;
    texture_view->GetResource(&texture_resource);

    // Copy the destination texture to the final texture resource
    D3D11_BOX copy_box{};
    copy_box.left = 0;
    copy_box.right = bounds.Width();
    copy_box.top = 0;
    copy_box.bottom = bounds.Height();
    copy_box.front = 0;
    copy_box.back = 1;

    // Copy the blitted content from the destination to the final texture
    m_d3d_context->CopySubresourceRegion(texture_resource, 0, 0, 0, 0, destination.render_target_texture, 0, &copy_box);

    // Restore state and free memory
    SetScissor(bounds);
    m_d3d_context->OMSetRenderTargets(1, &m_render_layers.GetTopLayer().render_target_view, m_render_layers.GetTopLayer().depth_stencil_view);
    texture_resource->Release();

    return render_texture;
}

enum class FilterType { Invalid = 0, Passthrough, Blur, DropShadow, ColorMatrix, MaskImage };
struct CompiledFilter {
    FilterType type;

    // Passthrough
    float blend_factor;

    // Blur
    float sigma;

    // Drop shadow
    Rml::Vector2f offset;
    Rml::ColourbPremultiplied color;

    // ColorMatrix
    Rml::Matrix4f color_matrix;
};

Rml::CompiledFilterHandle RenderInterface_DX11::SaveLayerAsMaskImage()
{
    BlitLayerToPostprocessPrimary(m_render_layers.GetTopLayerHandle());

    const Gfx::RenderTargetData& source = m_render_layers.GetPostprocessPrimary();
    const Gfx::RenderTargetData& destination = m_render_layers.GetBlendMask();

    m_d3d_context->OMSetRenderTargets(1, &destination.render_target_view, nullptr);
    Gfx::BindTexture(m_d3d_context, source);
    UseProgram(ProgramId::Passthrough);
    ID3D11BlendState* blend_state_backup = m_current_blend_state;
    SetBlendState(m_blend_state_disable);

    DrawFullscreenQuad();

    SetBlendState(blend_state_backup);
    m_d3d_context->OMSetRenderTargets(1, &m_render_layers.GetTopLayer().render_target_view, m_render_layers.GetTopLayer().depth_stencil_view);

    CompiledFilter filter = {};
    filter.type = FilterType::MaskImage;
    return reinterpret_cast<Rml::CompiledFilterHandle>(new CompiledFilter(std::move(filter)));
}

Rml::CompiledFilterHandle RenderInterface_DX11::CompileFilter(const Rml::String& name, const Rml::Dictionary& parameters)
{
    CompiledFilter filter = {};

    if (name == "opacity")
    {
        filter.type = FilterType::Passthrough;
        filter.blend_factor = Rml::Get(parameters, "value", 1.0f);
    }
    else if (name == "blur")
    {
        filter.type = FilterType::Blur;
        filter.sigma = Rml::Get(parameters, "sigma", 1.0f);
    }
    else if (name == "drop-shadow")
    {
        filter.type = FilterType::DropShadow;
        filter.sigma = Rml::Get(parameters, "sigma", 0.f);
        filter.color = Rml::Get(parameters, "color", Rml::Colourb()).ToPremultiplied();
        filter.offset = Rml::Get(parameters, "offset", Rml::Vector2f(0.f));
    }
    else if (name == "brightness")
    {
        filter.type = FilterType::ColorMatrix;
        const float value = Rml::Get(parameters, "value", 1.0f);
        filter.color_matrix = Rml::Matrix4f::Diag(value, value, value, 1.f);
    }
    else if (name == "contrast")
    {
        filter.type = FilterType::ColorMatrix;
        const float value = Rml::Get(parameters, "value", 1.0f);
        const float grayness = 0.5f - 0.5f * value;
        filter.color_matrix = Rml::Matrix4f::Diag(value, value, value, 1.f);
        filter.color_matrix.SetColumn(3, Rml::Vector4f(grayness, grayness, grayness, 1.f));
    }
    else if (name == "invert")
    {
        filter.type = FilterType::ColorMatrix;
        const float value = Rml::Math::Clamp(Rml::Get(parameters, "value", 1.0f), 0.f, 1.f);
        const float inverted = 1.f - 2.f * value;
        filter.color_matrix = Rml::Matrix4f::Diag(inverted, inverted, inverted, 1.f);
        filter.color_matrix.SetColumn(3, Rml::Vector4f(value, value, value, 1.f));
    }
    else if (name == "grayscale")
    {
        filter.type = FilterType::ColorMatrix;
        const float value = Rml::Get(parameters, "value", 1.0f);
        const float rev_value = 1.f - value;
        const Rml::Vector3f gray = value * Rml::Vector3f(0.2126f, 0.7152f, 0.0722f);
        // clang-format off
        filter.color_matrix = Rml::Matrix4f::FromRows(
            {gray.x + rev_value, gray.y,             gray.z,             0.f},
            {gray.x,             gray.y + rev_value, gray.z,             0.f},
            {gray.x,             gray.y,             gray.z + rev_value, 0.f},
            {0.f,                0.f,                0.f,                1.f}
        );
        // clang-format on
    }
    else if (name == "sepia")
    {
        filter.type = FilterType::ColorMatrix;
        const float value = Rml::Get(parameters, "value", 1.0f);
        const float rev_value = 1.f - value;
        const Rml::Vector3f r_mix = value * Rml::Vector3f(0.393f, 0.769f, 0.189f);
        const Rml::Vector3f g_mix = value * Rml::Vector3f(0.349f, 0.686f, 0.168f);
        const Rml::Vector3f b_mix = value * Rml::Vector3f(0.272f, 0.534f, 0.131f);
        // clang-format off
        filter.color_matrix = Rml::Matrix4f::FromRows(
            {r_mix.x + rev_value, r_mix.y,             r_mix.z,             0.f},
            {g_mix.x,             g_mix.y + rev_value, g_mix.z,             0.f},
            {b_mix.x,             b_mix.y,             b_mix.z + rev_value, 0.f},
            {0.f,                 0.f,                 0.f,                 1.f}
        );
        // clang-format on
    }
    else if (name == "hue-rotate")
    {
        // Hue-rotation and saturation values based on: https://www.w3.org/TR/filter-effects-1/#attr-valuedef-type-huerotate
        filter.type = FilterType::ColorMatrix;
        const float value = Rml::Get(parameters, "value", 1.0f);
        const float s = Rml::Math::Sin(value);
        const float c = Rml::Math::Cos(value);
        // clang-format off
        filter.color_matrix = Rml::Matrix4f::FromRows(
            {0.213f + 0.787f * c - 0.213f * s,  0.715f - 0.715f * c - 0.715f * s,  0.072f - 0.072f * c + 0.928f * s,  0.f},
            {0.213f - 0.213f * c + 0.143f * s,  0.715f + 0.285f * c + 0.140f * s,  0.072f - 0.072f * c - 0.283f * s,  0.f},
            {0.213f - 0.213f * c - 0.787f * s,  0.715f - 0.715f * c + 0.715f * s,  0.072f + 0.928f * c + 0.072f * s,  0.f},
            {0.f,                               0.f,                               0.f,                               1.f}
        );
        // clang-format on
    }
    else if (name == "saturate")
    {
        filter.type = FilterType::ColorMatrix;
        const float value = Rml::Get(parameters, "value", 1.0f);
        // clang-format off
        filter.color_matrix = Rml::Matrix4f::FromRows(
            {0.213f + 0.787f * value,  0.715f - 0.715f * value,  0.072f - 0.072f * value,  0.f},
            {0.213f - 0.213f * value,  0.715f + 0.285f * value,  0.072f - 0.072f * value,  0.f},
            {0.213f - 0.213f * value,  0.715f - 0.715f * value,  0.072f + 0.928f * value,  0.f},
            {0.f,                      0.f,                      0.f,                      1.f}
        );
        // clang-format on
    }

    if (filter.type != FilterType::Invalid)
        return reinterpret_cast<Rml::CompiledFilterHandle>(new CompiledFilter(std::move(filter)));

    Rml::Log::Message(Rml::Log::LT_WARNING, "Unsupported filter type '%s'.", name.c_str());
    return {};
}

void RenderInterface_DX11::ReleaseFilter(Rml::CompiledFilterHandle filter)
{
    delete reinterpret_cast<CompiledFilter*>(filter);
}

enum class CompiledShaderType { Invalid = 0, Gradient, Creation };
struct CompiledShader {
    CompiledShaderType type;

    // Gradient
    ShaderGradientFunction gradient_function;
    Rml::Vector2f p;
    Rml::Vector2f v;
    Rml::Vector<float> stop_positions;
    Rml::Vector<Rml::Colourf> stop_colors;

    // Shader
    Rml::Vector2f dimensions;
};

Rml::CompiledShaderHandle RenderInterface_DX11::CompileShader(const Rml::String& name, const Rml::Dictionary& parameters)
{
    auto ApplyColorStopList = [](CompiledShader& shader, const Rml::Dictionary& shader_parameters) {
        auto it = shader_parameters.find("color_stop_list");
        RMLUI_ASSERT(it != shader_parameters.end() && it->second.GetType() == Rml::Variant::COLORSTOPLIST);
        const Rml::ColorStopList& color_stop_list = it->second.GetReference<Rml::ColorStopList>();
        const int num_stops = Rml::Math::Min((int)color_stop_list.size(), MAX_NUM_STOPS);

        shader.stop_positions.resize(num_stops);
        shader.stop_colors.resize(num_stops);
        for (int i = 0; i < num_stops; i++)
        {
            const Rml::ColorStop& stop = color_stop_list[i];
            RMLUI_ASSERT(stop.position.unit == Rml::Unit::NUMBER);
            shader.stop_positions[i] = stop.position.number;
            shader.stop_colors[i] = ConvertToColorf(stop.color);
        }
    };

    CompiledShader shader = {};

    if (name == "linear-gradient")
    {
        shader.type = CompiledShaderType::Gradient;
        const bool repeating = Rml::Get(parameters, "repeating", false);
        shader.gradient_function = (repeating ? ShaderGradientFunction::RepeatingLinear : ShaderGradientFunction::Linear);
        shader.p = Rml::Get(parameters, "p0", Rml::Vector2f(0.f));
        shader.v = Rml::Get(parameters, "p1", Rml::Vector2f(0.f)) - shader.p;
        ApplyColorStopList(shader, parameters);
    }
    else if (name == "radial-gradient")
    {
        shader.type = CompiledShaderType::Gradient;
        const bool repeating = Rml::Get(parameters, "repeating", false);
        shader.gradient_function = (repeating ? ShaderGradientFunction::RepeatingRadial : ShaderGradientFunction::Radial);
        shader.p = Rml::Get(parameters, "center", Rml::Vector2f(0.f));
        shader.v = Rml::Vector2f(1.f) / Rml::Get(parameters, "radius", Rml::Vector2f(1.f));
        ApplyColorStopList(shader, parameters);
    }
    else if (name == "conic-gradient")
    {
        shader.type = CompiledShaderType::Gradient;
        const bool repeating = Rml::Get(parameters, "repeating", false);
        shader.gradient_function = (repeating ? ShaderGradientFunction::RepeatingConic : ShaderGradientFunction::Conic);
        shader.p = Rml::Get(parameters, "center", Rml::Vector2f(0.f));
        const float angle = Rml::Get(parameters, "angle", 0.f);
        shader.v = {Rml::Math::Cos(angle), Rml::Math::Sin(angle)};
        ApplyColorStopList(shader, parameters);
    }
    else if (name == "shader")
    {
        const Rml::String value = Rml::Get(parameters, "value", Rml::String());
        if (value == "creation")
        {
            shader.type = CompiledShaderType::Creation;
            shader.dimensions = Rml::Get(parameters, "dimensions", Rml::Vector2f(0.f));
        }
    }

    if (shader.type != CompiledShaderType::Invalid)
        return reinterpret_cast<Rml::CompiledShaderHandle>(new CompiledShader(std::move(shader)));

    Rml::Log::Message(Rml::Log::LT_WARNING, "Unsupported shader type '%s'.", name.c_str());
    return {};
}

void RenderInterface_DX11::RenderShader(Rml::CompiledShaderHandle shader_handle, Rml::CompiledGeometryHandle geometry_handle,
    Rml::Vector2f translation, Rml::TextureHandle /*texture*/)
{
    RMLUI_ASSERT(shader_handle && geometry_handle);
    const CompiledShader& shader = *reinterpret_cast<CompiledShader*>(shader_handle);
    const CompiledShaderType type = shader.type;

    DX11_GeometryData geometry{};
    if (m_geometry_cache.find(geometry_handle) != m_geometry_cache.end())
    {
        geometry = m_geometry_cache[geometry_handle];
    }

    switch (type)
    {
    case CompiledShaderType::Gradient:
    {
        RMLUI_ASSERT(shader.stop_positions.size() == shader.stop_colors.size());
        const int num_stops = (int)shader.stop_positions.size();

        UseProgram(ProgramId::Gradient);

        D3D11_MAPPED_SUBRESOURCE mappedResource{};
        // Lock the constant buffer so it can be written to.
        HRESULT result = m_d3d_context->Map(m_shader_buffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mappedResource);
        if (FAILED(result))
        {
            return;
        }

        // Get a pointer to the data in the constant buffer.
        ShaderCbuffer* dataPtr = (ShaderCbuffer*)mappedResource.pData;

        // Copy the data to the GPU
        dataPtr->transform = m_transform;
        dataPtr->translation = translation;
        dataPtr->gradient.func = static_cast<int>(shader.gradient_function);
        dataPtr->gradient.p = shader.p;
        dataPtr->gradient.v = shader.v;
        dataPtr->gradient.num_stops = num_stops;
        // Reset stop positions and colours to 0
        memset(dataPtr->gradient.stop_positions, 0, sizeof(dataPtr->gradient.stop_positions));
        memset(dataPtr->gradient.stop_colors, 0, sizeof(dataPtr->gradient.stop_colors));
        // Copy to stop position and colours
        memcpy_s(&dataPtr->gradient.stop_positions, num_stops * sizeof(float), shader.stop_positions.data(), num_stops * sizeof(float));
        memcpy_s(&dataPtr->gradient.stop_colors, num_stops * sizeof(Rml::Vector4f), shader.stop_colors.data(), num_stops * sizeof(Rml::Vector4f));

        // Upload to the GPU.
        m_d3d_context->Unmap(m_shader_buffer, 0);

        // Issue draw call
        m_d3d_context->IASetInputLayout(m_vertex_layout);
        m_d3d_context->VSSetConstantBuffers(0, 1, &m_shader_buffer);
        m_d3d_context->PSSetConstantBuffers(0, 1, &m_shader_buffer);
        uint32_t stride = sizeof(Rml::Vertex);
        uint32_t offset = 0;
        m_d3d_context->IASetVertexBuffers(0, 1, &geometry.vertex_buffer, &stride, &offset);
        m_d3d_context->IASetIndexBuffer(geometry.index_buffer, DXGI_FORMAT_R32_UINT, 0);
        m_d3d_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        m_d3d_context->DrawIndexed(geometry.index_count, 0, 0);
    }
    break;
    case CompiledShaderType::Creation:
    {
        const double time = Rml::GetSystemInterface()->GetElapsedTime();

        UseProgram(ProgramId::Creation);

        D3D11_MAPPED_SUBRESOURCE mappedResource{};
        // Lock the constant buffer so it can be written to.
        HRESULT result = m_d3d_context->Map(m_shader_buffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mappedResource);
        if (FAILED(result))
        {
            return;
        }

        // Get a pointer to the data in the constant buffer.
        ShaderCbuffer* dataPtr = (ShaderCbuffer*)mappedResource.pData;

        // Copy the data to the GPU
        dataPtr->transform = m_transform;
        dataPtr->translation = translation;
        dataPtr->creation.value = (float)time;
        dataPtr->creation.dimensions = shader.dimensions;

        // Upload to the GPU.
        m_d3d_context->Unmap(m_shader_buffer, 0);

        // Issue draw call
        m_d3d_context->IASetInputLayout(m_vertex_layout);
        m_d3d_context->VSSetConstantBuffers(0, 1, &m_shader_buffer);
        m_d3d_context->PSSetConstantBuffers(0, 1, &m_shader_buffer);
        uint32_t stride = sizeof(Rml::Vertex);
        uint32_t offset = 0;
        m_d3d_context->IASetVertexBuffers(0, 1, &geometry.vertex_buffer, &stride, &offset);
        m_d3d_context->IASetIndexBuffer(geometry.index_buffer, DXGI_FORMAT_R32_UINT, 0);
        m_d3d_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        m_d3d_context->DrawIndexed(geometry.index_count, 0, 0);
    }
    break;
    case CompiledShaderType::Invalid:
    {
        Rml::Log::Message(Rml::Log::LT_WARNING, "Unhandled render shader %d.", (int)type);
    }
    break;
    }
}

void RenderInterface_DX11::ReleaseShader(Rml::CompiledShaderHandle shader_handle)
{
    delete reinterpret_cast<CompiledShader*>(shader_handle);
}

void RenderInterface_DX11::UpdateConstantBuffer()
{
    if (m_cbuffer_dirty)
    {
        D3D11_MAPPED_SUBRESOURCE mappedResource{};
        // Lock the constant buffer so it can be written to.
        HRESULT result = m_d3d_context->Map(m_shader_buffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mappedResource);
        if (FAILED(result))
        {
            return;
        }

        // Get a pointer to the data in the constant buffer.
        ShaderCbuffer* dataPtr = (ShaderCbuffer*)mappedResource.pData;

        // Copy the data to the GPU
        dataPtr->transform = m_transform;
        dataPtr->translation = m_translation;

        // Upload to the GPU.
        m_d3d_context->Unmap(m_shader_buffer, 0);
        m_cbuffer_dirty = false;
    }
}

void RenderInterface_DX11::UseProgram(ProgramId program_id) {
    RMLUI_ASSERT(program_data);
    if (active_program != program_id)
    {
        if (program_id != ProgramId::None)
        {
            m_d3d_context->VSSetShader(program_data->programs[program_id].vertex_shader, nullptr, 0);
            m_d3d_context->PSSetShader(program_data->programs[program_id].pixel_shader, nullptr, 0);
        }
        active_program = program_id;
    }
}

void RenderInterface_DX11::EnableClipMask(bool enable)
{
    if (enable != m_is_stencil_enabled)
    {
        m_is_stencil_enabled = enable;
        if (!enable)
        {
            m_d3d_context->OMSetDepthStencilState(m_depth_stencil_state_disable, 0);
        }
    }
}

void RenderInterface_DX11::RenderToClipMask(Rml::ClipMaskOperation operation, Rml::CompiledGeometryHandle geometry, Rml::Vector2f translation)
{
    RMLUI_ASSERT(m_is_stencil_enabled);
    using Rml::ClipMaskOperation;

    ID3D11DepthStencilState* stencil_state = m_depth_stencil_state_disable;

    UINT stencil_test_value = 0;
    switch (operation)
    {
    case ClipMaskOperation::Set:
    {
        stencil_test_value = 1;
        stencil_state = m_depth_stencil_state_stencil_set;
    }
    break;
    case ClipMaskOperation::SetInverse:
    {
        stencil_test_value = 0;
        stencil_state = m_depth_stencil_state_stencil_set;
    }
    break;
    case ClipMaskOperation::Intersect:
    {
        stencil_test_value += 1;
        stencil_state = m_depth_stencil_state_stencil_intersect;
    }
    break;
    }
    
    // Disable writing to the color of the render target
    float blendFactor[4] = {0.f, 0.f, 0.f, 0.f};
    m_d3d_context->OMSetBlendState(m_blend_state_disable_color, blendFactor, 0xffffffff);

    m_d3d_context->OMSetRenderTargets(1, &m_render_layers.GetTopLayer().render_target_view, m_render_layers.GetTopLayer().depth_stencil_view);
    m_d3d_context->OMSetDepthStencilState(stencil_state, stencil_test_value);

    bool clear_stencil = (operation == ClipMaskOperation::Set || operation == ClipMaskOperation::SetInverse);
    if (clear_stencil)
    {
        // Clear stencil buffer
        Rml::LayerHandle layer_handle = m_render_layers.GetTopLayerHandle();
        const Gfx::RenderTargetData& rtData = m_render_layers.GetLayer(layer_handle);
        m_d3d_context->ClearDepthStencilView(rtData.depth_stencil_view, D3D11_CLEAR_STENCIL, 1.0f, 0);
    }

    // @TODO: Stencil buffer is inverted?
    RenderGeometry(geometry, translation, {});

    // Restore state
    // @performance Is this even necessary?
    m_d3d_context->OMSetDepthStencilState(m_depth_stencil_state_disable, 0);
    // Restore blend state
    m_d3d_context->OMSetBlendState(m_current_blend_state, blendFactor, 0xffffffff);
}

RenderInterface_DX11::RenderLayerStack::RenderLayerStack()
{
    rt_postprocess.resize(4);
}

RenderInterface_DX11::RenderLayerStack::~RenderLayerStack()
{
    DestroyRenderTargets();
}

void RenderInterface_DX11::RenderLayerStack::SetD3DResources(ID3D11Device* device)
{
    RMLUI_ASSERTMSG(!m_d3d_device, "D3D11Device has already been set!");
    m_d3d_device = device;
}

Rml::LayerHandle RenderInterface_DX11::RenderLayerStack::PushLayer()
{
    RMLUI_ASSERT(layers_size <= (int)rt_layers.size());

    if (layers_size == (int)rt_layers.size())
    {
        // All framebuffers should share a single stencil buffer.
        ID3D11DepthStencilView* shared_depth_stencil = (rt_layers.empty() ? nullptr : rt_layers.front().depth_stencil_view);

        rt_layers.push_back(Gfx::RenderTargetData{});
        Gfx::CreateRenderTarget(m_d3d_device, rt_layers.back(), width, height, NUM_MSAA_SAMPLES, Gfx::RenderTargetAttachment::DepthStencil, shared_depth_stencil);
    }

    layers_size += 1;
    return GetTopLayerHandle();
}


void RenderInterface_DX11::RenderLayerStack::PopLayer()
{
    RMLUI_ASSERT(layers_size > 0);
    layers_size -= 1;
}

const Gfx::RenderTargetData& RenderInterface_DX11::RenderLayerStack::GetLayer(Rml::LayerHandle layer) const
{
    RMLUI_ASSERT((size_t)layer < (size_t)layers_size);
    return rt_layers[layer];
}

const Gfx::RenderTargetData& RenderInterface_DX11::RenderLayerStack::GetTopLayer() const
{
    return GetLayer(GetTopLayerHandle());
}

Rml::LayerHandle RenderInterface_DX11::RenderLayerStack::GetTopLayerHandle() const
{
    RMLUI_ASSERT(layers_size > 0);
    return static_cast<Rml::LayerHandle>(layers_size - 1);
}

void RenderInterface_DX11::RenderLayerStack::SwapPostprocessPrimarySecondary()
{
    std::swap(rt_postprocess[0], rt_postprocess[1]);
}

void RenderInterface_DX11::RenderLayerStack::BeginFrame(int new_width, int new_height)
{
    RMLUI_ASSERT(layers_size == 0);

    if (new_width != width || new_height != height)
    {
        width = new_width;
        height = new_height;

        DestroyRenderTargets();
    }

    PushLayer();
}

void RenderInterface_DX11::RenderLayerStack::EndFrame()
{
    RMLUI_ASSERT(layers_size == 1);
    PopLayer();
}

void RenderInterface_DX11::RenderLayerStack::DestroyRenderTargets()
{
    RMLUI_ASSERTMSG(layers_size == 0, "Do not call this during frame rendering, that is, between BeginFrame() and EndFrame().");

    for (Gfx::RenderTargetData& fb : rt_layers)
        Gfx::DestroyRenderTarget(fb);

    rt_layers.clear();

    for (Gfx::RenderTargetData& fb : rt_postprocess)
        Gfx::DestroyRenderTarget(fb);
}

const Gfx::RenderTargetData& RenderInterface_DX11::RenderLayerStack::EnsureRenderTargetPostprocess(int index)
{
    RMLUI_ASSERT(index < (int)rt_postprocess.size())
    Gfx::RenderTargetData& rt = rt_postprocess[index];
    if (!rt.render_target_view)
        Gfx::CreateRenderTarget(m_d3d_device, rt, width, height, 0, Gfx::RenderTargetAttachment::None, 0);
    return rt;
}

#endif // RMLUI_PLATFORM_WIN32

