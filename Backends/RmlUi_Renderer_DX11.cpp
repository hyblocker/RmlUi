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
    #include <RmlUi/Core/Geometry.h>
    #include <RmlUi/Core/MeshUtilities.h>
    #include <RmlUi/Core/FileInterface.h>
    #include <RmlUi/Core/Log.h>

    #include "RmlUi_Include_Windows.h"

    #include "d3dcompiler.h"

    #ifdef RMLUI_DX_DEBUG
        #include <dxgidebug.h>
    #endif

// Shader source code
constexpr const char shader_source_text_color[] = R"(
struct sInputData
{
    float4 inputPos : SV_Position;
    float4 inputColor : COLOR;
    float2 inputUV : TEXCOORD;
};

float4 main(const sInputData inputArgs) : SV_TARGET 
{ 
    return inputArgs.inputColor; 
}
)";

constexpr const char shader_source_text_vertex[] = R"(
struct sInputData 
{
    float2 inPosition : POSITION;
    float4 inColor : COLOR;
    float2 inTexCoord : TEXCOORD;
};

struct sOutputData
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
};

sOutputData main(const sInputData inArgs)
{
    sOutputData result;

    float2 translatedPos = inArgs.inPosition + m_translate;
    float4 resPos = mul(m_transform, float4(translatedPos.x, translatedPos.y, 0.0, 1.0));

    result.outPosition = resPos;
    result.outColor = inArgs.inColor;
    result.outUV = inArgs.inTexCoord;

#if defined(RMLUI_PREMULTIPLIED_ALPHA)
    // Pre-multiply vertex colors with their alpha.
    result.outColor.rgb = result.outColor.rgb * result.outColor.a;
#endif

    return result;
};
)";

constexpr const char shader_source_text_texture[] = R"(
struct sInputData
{
    float4 inputPos : SV_Position;
    float4 inputColor : COLOR;
    float2 inputUV : TEXCOORD;
};

Texture2D g_InputTexture : register(t0);
SamplerState g_SamplerLinear : register(s0);


float4 main(const sInputData inputArgs) : SV_TARGET 
{
    return inputArgs.inputColor * g_InputTexture.Sample(g_SamplerLinear, inputArgs.inputUV); 
}
)";

constexpr const char shader_passthrough_source_vertex[] = R"(
struct sInputData 
{
    float2 inPosition : POSITION;
    float4 inColor : COLOR;
    float2 inTexCoord : TEXCOORD;
};

struct sOutputData
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
};

sOutputData main(const sInputData inArgs)
{
    sOutputData result;

    result.outPosition = float4(inArgs.inPosition, 0.0, 1.0);
    result.outColor = inArgs.inColor;
    result.outUV = inArgs.inTexCoord;

    return result;
};
)";

constexpr const char shader_passthrough_source_fragment[] = R"(
struct sInputData
{
    float4 inputPos : SV_Position;
    float4 inputColor : COLOR;
    float2 inputUV : TEXCOORD;
};

Texture2D g_InputTexture : register(t0);
SamplerState g_SamplerLinear : register(s0);


float4 main(const sInputData inputArgs) : SV_TARGET 
{
    return g_InputTexture.Sample(g_SamplerLinear, inputArgs.inputUV); 
}
)";

namespace Gfx {
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

RenderInterface_DX11::RenderInterface_DX11()
{
    #ifdef RMLUI_DX_DEBUG
    m_default_shader_flags = D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
    #else
    m_default_shader_flags = 0;
    #endif
}

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
    if (!m_blend_state)
    {
        D3D11_BLEND_DESC blendDesc{};
        ZeroMemory(&blendDesc, sizeof(blendDesc));
        blendDesc.AlphaToCoverageEnable = FALSE;
        blendDesc.IndependentBlendEnable = FALSE;
        blendDesc.RenderTarget[0].BlendEnable = TRUE;
        blendDesc.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
        blendDesc.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
        blendDesc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
        blendDesc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
        blendDesc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
        blendDesc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
        blendDesc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        HRESULT result = m_d3d_device->CreateBlendState(&blendDesc, &m_blend_state);
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

    // Buffer shall be cleared up later, as it's required to create the input layout
    ID3DBlob* p_shader_vertex_common{};
    {
        const D3D_SHADER_MACRO macros[] = {"RMLUI_PREMULTIPLIED_ALPHA", nullptr, nullptr, nullptr};
        ID3DBlob* p_error_buff{};

        // Common vertex shader
        HRESULT result = D3DCompile(shader_source_text_vertex, sizeof(shader_source_text_vertex), nullptr, macros, nullptr, "main", "vs_5_0",
            this->m_default_shader_flags, 0, &p_shader_vertex_common, &p_error_buff);
        RMLUI_DX_ASSERTMSG(result, "failed to D3DCompile");
    #ifdef RMLUI_DX_DEBUG
        if (FAILED(result))
        {
            Rml::Log::Message(Rml::Log::Type::LT_ERROR, "failed to compile shader: %s", (char*)p_error_buff->GetBufferPointer());
        }
    #endif

        DX_CLEANUP_RESOURCE_IF_CREATED(p_error_buff);

        // Color fragment shader
        ID3DBlob* p_shader_color_pixel{};

        result = D3DCompile(shader_source_text_color, sizeof(shader_source_text_color), nullptr, macros, nullptr, "main", "ps_5_0",
            this->m_default_shader_flags, 0, &p_shader_color_pixel, &p_error_buff);
        RMLUI_DX_ASSERTMSG(result, "failed to D3DCompile");
    #ifdef RMLUI_DX_DEBUG
        if (FAILED(result))
        {
            Rml::Log::Message(Rml::Log::Type::LT_ERROR, "failed to compile shader: %s", (char*)p_error_buff->GetBufferPointer());
        }
    #endif

        DX_CLEANUP_RESOURCE_IF_CREATED(p_error_buff);

        // Texture fragment shader
        ID3DBlob* p_shader_color_texture{};

        result = D3DCompile(shader_source_text_texture, sizeof(shader_source_text_texture), nullptr, macros, nullptr, "main", "ps_5_0",
            this->m_default_shader_flags, 0, &p_shader_color_texture, &p_error_buff);
        RMLUI_DX_ASSERTMSG(result, "failed to D3DCompile");
    #ifdef RMLUI_DX_DEBUG
        if (FAILED(result))
        {
            Rml::Log::Message(Rml::Log::Type::LT_ERROR, "failed to compile shader: %s", (char*)p_error_buff->GetBufferPointer());
        }
    #endif

        DX_CLEANUP_RESOURCE_IF_CREATED(p_error_buff);

        // Passthrough vertex shader
        ID3DBlob* p_shader_passthrough_vertex{};

        result = D3DCompile(shader_passthrough_source_vertex, sizeof(shader_passthrough_source_vertex), nullptr, macros, nullptr, "main",
            "vs_5_0", this->m_default_shader_flags, 0, &p_shader_passthrough_vertex, &p_error_buff);
        RMLUI_DX_ASSERTMSG(result, "failed to D3DCompile");
    #ifdef RMLUI_DX_DEBUG
        if (FAILED(result))
        {
            Rml::Log::Message(Rml::Log::Type::LT_ERROR, "failed to compile shader: %s", (char*)p_error_buff->GetBufferPointer());
        }
    #endif

        DX_CLEANUP_RESOURCE_IF_CREATED(p_error_buff);

        // Passthrough fragment shader
        ID3DBlob* p_shader_passthrough_pixel{};

        result = D3DCompile(shader_passthrough_source_fragment, sizeof(shader_passthrough_source_fragment), nullptr, macros, nullptr, "main",
            "ps_5_0", this->m_default_shader_flags, 0, &p_shader_passthrough_pixel, &p_error_buff);
        RMLUI_DX_ASSERTMSG(result, "failed to D3DCompile");
    #ifdef RMLUI_DX_DEBUG
        if (FAILED(result))
        {
            Rml::Log::Message(Rml::Log::Type::LT_ERROR, "failed to compile shader: %s", (char*)p_error_buff->GetBufferPointer());
        }
    #endif

        DX_CLEANUP_RESOURCE_IF_CREATED(p_error_buff);

        // Create the shader objects
        result = m_d3d_device->CreateVertexShader(p_shader_vertex_common->GetBufferPointer(), p_shader_vertex_common->GetBufferSize(), nullptr,
            &m_shader_vertex_common);
        RMLUI_DX_ASSERTMSG(result, "failed to CreateVertexShader");
        if (FAILED(result))
        {
    #ifdef RMLUI_DX_DEBUG
            Rml::Log::Message(Rml::Log::Type::LT_ERROR, "failed to create vertex shader: %d", result);
    #endif
            DX_CLEANUP_RESOURCE_IF_CREATED(p_shader_color_pixel);
            DX_CLEANUP_RESOURCE_IF_CREATED(p_shader_color_texture);
            DX_CLEANUP_RESOURCE_IF_CREATED(p_shader_passthrough_vertex);
            DX_CLEANUP_RESOURCE_IF_CREATED(p_shader_passthrough_pixel);
            goto cleanup;
            return;
        }

        result = m_d3d_device->CreatePixelShader(p_shader_color_pixel->GetBufferPointer(), p_shader_color_pixel->GetBufferSize(), nullptr,
            &m_shader_pixel_color);
        RMLUI_DX_ASSERTMSG(result, "failed to CreatePixelShader");
        if (FAILED(result))
        {
    #ifdef RMLUI_DX_DEBUG
            Rml::Log::Message(Rml::Log::Type::LT_ERROR, "failed to create pixel shader: %d", result);
    #endif
            DX_CLEANUP_RESOURCE_IF_CREATED(p_shader_color_pixel);
            DX_CLEANUP_RESOURCE_IF_CREATED(p_shader_color_texture);
            DX_CLEANUP_RESOURCE_IF_CREATED(p_shader_passthrough_vertex);
            DX_CLEANUP_RESOURCE_IF_CREATED(p_shader_passthrough_pixel);
            goto cleanup;
            return;
        }

        result = m_d3d_device->CreatePixelShader(p_shader_color_texture->GetBufferPointer(), p_shader_color_texture->GetBufferSize(), nullptr,
            &m_shader_pixel_texture);
        RMLUI_DX_ASSERTMSG(result, "failed to CreatePixelShader");
        if (FAILED(result))
        {
    #ifdef RMLUI_DX_DEBUG
            Rml::Log::Message(Rml::Log::Type::LT_ERROR, "failed to create pixel shader: %d", result);
    #endif
            DX_CLEANUP_RESOURCE_IF_CREATED(p_shader_color_pixel);
            DX_CLEANUP_RESOURCE_IF_CREATED(p_shader_color_texture);
            DX_CLEANUP_RESOURCE_IF_CREATED(p_shader_passthrough_vertex);
            DX_CLEANUP_RESOURCE_IF_CREATED(p_shader_passthrough_pixel);
            goto cleanup;
            return;
        }

        // Passthrough
        result = m_d3d_device->CreateVertexShader(p_shader_passthrough_vertex->GetBufferPointer(), p_shader_passthrough_vertex->GetBufferSize(),
            nullptr, &m_shader_passthrough_vertex);
        RMLUI_DX_ASSERTMSG(result, "failed to CreateVertexShader");
        if (FAILED(result))
        {
    #ifdef RMLUI_DX_DEBUG
            Rml::Log::Message(Rml::Log::Type::LT_ERROR, "failed to create vertex shader: %d", result);
    #endif
            DX_CLEANUP_RESOURCE_IF_CREATED(p_shader_color_pixel);
            DX_CLEANUP_RESOURCE_IF_CREATED(p_shader_color_texture);
            DX_CLEANUP_RESOURCE_IF_CREATED(p_shader_passthrough_vertex);
            DX_CLEANUP_RESOURCE_IF_CREATED(p_shader_passthrough_pixel);
            goto cleanup;
            return;
        }

        result = m_d3d_device->CreatePixelShader(p_shader_passthrough_pixel->GetBufferPointer(), p_shader_passthrough_pixel->GetBufferSize(), nullptr,
            &m_shader_passthrough_fragment);
        RMLUI_DX_ASSERTMSG(result, "failed to CreatePixelShader");
        if (FAILED(result))
        {
    #ifdef RMLUI_DX_DEBUG
            Rml::Log::Message(Rml::Log::Type::LT_ERROR, "failed to create pixel shader: %d", result);
    #endif
            DX_CLEANUP_RESOURCE_IF_CREATED(p_shader_color_pixel);
            DX_CLEANUP_RESOURCE_IF_CREATED(p_shader_color_texture);
            DX_CLEANUP_RESOURCE_IF_CREATED(p_shader_passthrough_vertex);
            DX_CLEANUP_RESOURCE_IF_CREATED(p_shader_passthrough_pixel);
            goto cleanup;
            return;
        }

        DX_CLEANUP_RESOURCE_IF_CREATED(p_shader_color_pixel);
        DX_CLEANUP_RESOURCE_IF_CREATED(p_shader_color_texture);
        DX_CLEANUP_RESOURCE_IF_CREATED(p_shader_passthrough_vertex);
        DX_CLEANUP_RESOURCE_IF_CREATED(p_shader_passthrough_pixel);
    }

    // Create vertex layout. This will be constant to avoid copying to an intermediate struct.
    {
        D3D11_INPUT_ELEMENT_DESC polygonLayout[] = {
            {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_VERTEX_DATA, 0},
            {"COLOR", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_VERTEX_DATA, 0},
            {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_VERTEX_DATA, 0},
        };
        uint32_t numElements = sizeof(polygonLayout) / sizeof(polygonLayout[0]);

        HRESULT result = m_d3d_device->CreateInputLayout(polygonLayout, numElements, p_shader_vertex_common->GetBufferPointer(),
            p_shader_vertex_common->GetBufferSize(), &m_vertex_layout);
        RMLUI_DX_ASSERTMSG(result, "failed to CreateInputLayout");
        if (FAILED(result))
        {
            goto cleanup;
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
            goto cleanup;
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
            goto cleanup;
            return;
        }
    }

    // Create depth stencil states
    {
        D3D11_DEPTH_STENCIL_DESC desc;
        ZeroMemory(&desc, sizeof(desc));
        desc.DepthEnable = false;
        desc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
        desc.DepthFunc = D3D11_COMPARISON_ALWAYS;
        desc.FrontFace.StencilFailOp = desc.FrontFace.StencilDepthFailOp = desc.FrontFace.StencilPassOp = D3D11_STENCIL_OP_KEEP;
        desc.FrontFace.StencilFunc = D3D11_COMPARISON_ALWAYS;
        desc.BackFace = desc.FrontFace;
        desc.StencilEnable = false;
        // Disabled
        m_d3d_device->CreateDepthStencilState(&desc, &m_depth_stencil_state_disable);

        desc.StencilEnable = true;
        desc.StencilReadMask = 0xFF;
        desc.StencilWriteMask = 0xFF;
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

cleanup:

    // Release the vertex shader buffer and pixel shader buffer since they are no longer needed.
    DX_CLEANUP_RESOURCE_IF_CREATED(p_shader_vertex_common);
}

void RenderInterface_DX11::Cleanup()
{
    // Loop through geometry cache and free all resources
    for (auto& it : m_geometry_cache)
    {
        DX_CLEANUP_RESOURCE_IF_CREATED(it.second.vertex_buffer);
        DX_CLEANUP_RESOURCE_IF_CREATED(it.second.index_buffer);
    }

    // Cleans up all general resources
    DX_CLEANUP_RESOURCE_IF_CREATED(m_samplerState);
    DX_CLEANUP_RESOURCE_IF_CREATED(m_blend_state);
    DX_CLEANUP_RESOURCE_IF_CREATED(m_depth_stencil_state_disable);
    DX_CLEANUP_RESOURCE_IF_CREATED(m_rasterizer_state_scissor_disabled);
    DX_CLEANUP_RESOURCE_IF_CREATED(m_rasterizer_state_scissor_enabled);
    DX_CLEANUP_RESOURCE_IF_CREATED(m_shader_passthrough_fragment);
    DX_CLEANUP_RESOURCE_IF_CREATED(m_shader_passthrough_vertex);
    DX_CLEANUP_RESOURCE_IF_CREATED(m_shader_pixel_color);
    DX_CLEANUP_RESOURCE_IF_CREATED(m_shader_pixel_texture);
    DX_CLEANUP_RESOURCE_IF_CREATED(m_shader_buffer);
    DX_CLEANUP_RESOURCE_IF_CREATED(m_vertex_layout);
    DX_CLEANUP_RESOURCE_IF_CREATED(m_shader_vertex_common);
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
    SetBlendState(m_blend_state);
    m_d3d_context->RSSetState(m_rasterizer_state_scissor_disabled); // Disable scissor
    m_d3d_context->OMSetDepthStencilState(m_depth_stencil_state_disable, 0);
    m_d3d_context->OMSetRenderTargets(1, &m_bound_render_target, nullptr);
    Clear();
    
    SetTransform(nullptr);

    m_render_layers.BeginFrame(m_viewport_width, m_viewport_height);
    m_d3d_context->OMSetRenderTargets(1, &m_render_layers.GetTopLayer().render_target_view, nullptr);
    float clearColor[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    m_d3d_context->ClearRenderTargetView(m_render_layers.GetTopLayer().render_target_view, clearColor);

    // UseProgram(ProgramId::None);
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

    // @TODO: UseProgram
    // @TODO: Find a pattern for flipped textures
    // UseProgram(ProgramId::Passthrough);
    m_d3d_context->VSSetShader(this->m_shader_passthrough_vertex, nullptr, 0);
    m_d3d_context->PSSetShader(this->m_shader_passthrough_fragment, nullptr, 0);

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
            m_d3d_context->VSSetShader(this->m_shader_vertex_common, nullptr, 0);
            m_d3d_context->PSSetShader(this->m_shader_pixel_texture, nullptr, 0);
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
            m_d3d_context->VSSetShader(this->m_shader_vertex_common, nullptr, 0);
            m_d3d_context->PSSetShader(this->m_shader_pixel_color, nullptr, 0);
        }

        UpdateConstantBuffer();

        m_d3d_context->IASetInputLayout(m_vertex_layout);
        m_d3d_context->VSSetConstantBuffers(0, 1, &m_shader_buffer);

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
    m_d3d_context->UpdateSubresource(gpu_texture, 0, nullptr, source.data(), rowPitch, 0);

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
    m_d3d_context->GenerateMips(gpu_texture_view);

    uintptr_t handleId = HashPointer((uintptr_t)gpu_texture_view);

    return Rml::TextureHandle(gpu_texture_view);
}

void RenderInterface_DX11::ReleaseTexture(Rml::TextureHandle texture_handle)
{
    ID3D11ShaderResourceView* texture_view = (ID3D11ShaderResourceView*)texture_handle;
    DX_CLEANUP_RESOURCE_IF_CREATED(texture_view);
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

void RenderInterface_DX11::SetBlendState(ID3D11BlendState* blendState)
{
    if (blendState != m_current_blend_state)
    {
        const float blend_factor[4] = {0.f, 0.f, 0.f, 0.f};
        m_d3d_context->OMSetBlendState(blendState, blend_factor, 0xFFFFFFFF);
        m_current_blend_state = blendState;
    }
}

void RenderInterface_DX11::SetTransform(const Rml::Matrix4f* new_transform)
{
    m_transform = (new_transform ? (m_projection * (*new_transform)) : m_projection);
    m_cbuffer_dirty = true;
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

