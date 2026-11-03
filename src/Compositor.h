#pragma once

namespace fv
{
	// Draws the screen's original texture, then the video into one or more rectangles of it,
	// into a render target. Used when a mesh shares one texture between its screen and its frame.
	// Render thread only. Saves and restores every bit of D3D state it touches.
	class Compositor
	{
	public:
		struct Region
		{
			float x0, y0, x1, y1;  // destination, in texture pixels (y down)
			// video coordinate at local position (tx, ty) in 0..1 across the region (outside 0..1 = black bar):
			//   u = ou + au*tx + bu*ty,   v = ov + av*tx + bv*ty
			float ou{ 0 }, au{ 1 }, bu{ 0 };
			float ov{ 0 }, av{ 0 }, bv{ 1 };
		};

		static Compositor& Get();

		bool Ready(ID3D11Device* a_device);
		// true once it has been tried and failed (GPU decoding then stays off: it needs the compositor)
		bool Failed() const { return _tried && !_ok; }

		void Compose(ID3D11DeviceContext* a_context, ID3D11RenderTargetView* a_target, std::uint32_t a_width, std::uint32_t a_height,
			ID3D11ShaderResourceView* a_base, ID3D11ShaderResourceView* a_video, std::span<const Region> a_regions,
			const float* a_fx = nullptr,  // a_fx: brightness, VHS strength, time (seconds) for the video regions
			bool a_baseSeeThrough = false,  // alpha 0 outside the video regions (face-only glow)
			float a_baseScale = 1.0f,       // brightness of everything outside the video regions
			const float* a_film = nullptr); // old film look for the video regions: 8 values, see Settings::Film

	private:
		bool Init(ID3D11Device* a_device);
		void Draw(ID3D11DeviceContext* a_context, std::uint32_t a_width, std::uint32_t a_height, const Region& a_region,
			ID3D11ShaderResourceView* a_srv, ID3D11SamplerState* a_sampler, const float* a_fx, const float* a_film);

		ComPtr<ID3D11VertexShader>      _vs;
		ComPtr<ID3D11PixelShader>       _ps;
		ComPtr<ID3D11Buffer>            _cb;
		ComPtr<ID3D11SamplerState>      _clamp;
		ComPtr<ID3D11SamplerState>      _border;
		ComPtr<ID3D11BlendState>        _blend;
		ComPtr<ID3D11DepthStencilState> _depth;
		ComPtr<ID3D11RasterizerState>   _raster;
		bool                            _tried{ false };
		bool                            _ok{ false };
	};
}
