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

#ifndef RMLUI_BACKENDS_RENDERER_DX11_H
#define RMLUI_BACKENDS_RENDERER_DX11_H

    #include <RmlUi/Core/Platform.h>

    #if defined RMLUI_PLATFORM_WIN32
        #include <RmlUi/Core/RenderInterface.h>

        #include <d3d11.h>
        #include <dxgi1_3.h>

class RenderInterface_DX11 : public Rml::RenderInterface {
public:
	void Init(ID3D11Device* pd3dDevice, ID3D11DeviceContext* pd3dDeviceContext, IDXGISwapChain* pSwapChain, ID3D11RenderTargetView* mainRenderTargetView);
    RenderInterface_DX11();
    
    // Resource initialisation and cleanup
    void Cleanup ();

	void BeginFrame();
    // Sets up DirectX11 states for taking rendering commands from RmlUi.
    void EndFrame();

    // -- Inherited from Rml::RenderInterface --

    Rml::CompiledGeometryHandle CompileGeometry(Rml::Span<const Rml::Vertex> vertices, Rml::Span<const int> indices) override;
    void ReleaseGeometry(Rml::CompiledGeometryHandle geometry) override;
    void RenderGeometry(Rml::CompiledGeometryHandle handle, Rml::Vector2f translation, Rml::TextureHandle texture) override;

    Rml::TextureHandle LoadTexture(Rml::Vector2i& texture_dimensions, const Rml::String& source) override;
    Rml::TextureHandle GenerateTexture(Rml::Span<const Rml::byte> source, Rml::Vector2i source_dimensions) override;
    void ReleaseTexture(Rml::TextureHandle texture_handle) override;

    void EnableScissorRegion(bool enable) override;
    void SetScissorRegion(Rml::Rectanglei region) override;

    void SetViewport(const int width, const int height);
    void Clear();

private:
    void SetBlendState(ID3D11BlendState* blendState);

private:
	// @TODO: Replace with vertex / index buffer pairs
	struct GeometryView {
		Rml::Span<const Rml::Vertex> vertices;
		Rml::Span<const int> indices;
	};

	// D3D11 core resources
	ID3D11Device* m_d3dDevice = nullptr;
	ID3D11DeviceContext* m_d3dContext = nullptr;
	IDXGISwapChain* m_swapChain = nullptr;
	ID3D11RenderTargetView* m_mainRenderTargetView = nullptr;

	// Viewport dimensions
	int m_width = 0;
	int m_height = 0;

	// SDL_Renderer* renderer;
	ID3D11BlendState* m_blendState = nullptr;
	// SDL_Rect rect_scissor = {};
	bool scissor_region_enabled = false;

	// D3D11 state
	ID3D11BlendState* m_currentBlendState = nullptr;
	
	struct D3D11State {

		ID3D11BlendState* m_previousBlendState = nullptr;
	};

	D3D11State m_previousD3DState;
};

#endif // RMLUI_PLATFORM_WIN32

#endif // RMLUI_BACKENDS_RENDERER_DX11_H
