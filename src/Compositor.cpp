#include "Compositor.h"

#include <d3dcompiler.h>

namespace fv
{
	namespace
	{
		constexpr char kShader[] = R"(
cbuffer Params : register(b0)
{
	float4 dst;    // x0, y0, x1, y1 in clip space
	float4 uvs;    // u0, v0, u1, v1
};
Texture2D    tex : register(t0);
SamplerState smp : register(s0);

struct VSOut
{
	float4 pos : SV_Position;
	float2 uv  : TEXCOORD0;
};

VSOut VSMain(uint id : SV_VertexID)
{
	float2 t = float2(id & 1, id >> 1);
	VSOut o;
	o.pos = float4(lerp(dst.x, dst.z, t.x), lerp(dst.y, dst.w, t.y), 0.0, 1.0);
	o.uv  = float2(lerp(uvs.x, uvs.z, t.x), lerp(uvs.y, uvs.w, t.y));
	return o;
}

float4 PSMain(VSOut i) : SV_Target
{
	return float4(tex.Sample(smp, i.uv).rgb, 1.0);
}
)";

		struct Params
		{
			float dst[4];
			float uvs[4];
		};

		// Everything Compose() changes, captured and put back afterwards (the game caches its D3D state).
		struct StateBackup
		{
			UINT                      viewportCount{ D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE };
			D3D11_VIEWPORT            viewports[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
			UINT                      scissorCount{ D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE };
			D3D11_RECT                scissors[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
			ID3D11RasterizerState*    raster{};
			ID3D11BlendState*         blend{};
			FLOAT                     blendFactor[4]{};
			UINT                      sampleMask{};
			ID3D11DepthStencilState*  depth{};
			UINT                      stencilRef{};
			ID3D11RenderTargetView*   rtvs[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
			ID3D11DepthStencilView*   dsv{};
			ID3D11ShaderResourceView* psSRV{};
			ID3D11SamplerState*       psSampler{};
			ID3D11PixelShader*        ps{};
			ID3D11VertexShader*       vs{};
			ID3D11GeometryShader*     gs{};
			ID3D11HullShader*         hs{};
			ID3D11DomainShader*       ds{};
			ID3D11ClassInstance*      psInst[256]{};
			ID3D11ClassInstance*      vsInst[256]{};
			ID3D11ClassInstance*      gsInst[256]{};
			UINT                      psInstCount{ 256 }, vsInstCount{ 256 }, gsInstCount{ 256 };
			ID3D11Buffer*             vsCB{};
			D3D11_PRIMITIVE_TOPOLOGY  topology{};
			ID3D11InputLayout*        layout{};
			ID3D11Buffer*             vb{};
			UINT                      vbStride{}, vbOffset{};
			ID3D11Buffer*             ib{};
			DXGI_FORMAT               ibFormat{};
			UINT                      ibOffset{};

			void Save(ID3D11DeviceContext* c)
			{
				c->RSGetViewports(&viewportCount, viewports);
				c->RSGetScissorRects(&scissorCount, scissors);
				c->RSGetState(&raster);
				c->OMGetBlendState(&blend, blendFactor, &sampleMask);
				c->OMGetDepthStencilState(&depth, &stencilRef);
				c->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtvs, &dsv);
				c->PSGetShaderResources(0, 1, &psSRV);
				c->PSGetSamplers(0, 1, &psSampler);
				c->PSGetShader(&ps, psInst, &psInstCount);
				c->VSGetShader(&vs, vsInst, &vsInstCount);
				c->GSGetShader(&gs, gsInst, &gsInstCount);
				c->HSGetShader(&hs, nullptr, nullptr);
				c->DSGetShader(&ds, nullptr, nullptr);
				c->VSGetConstantBuffers(0, 1, &vsCB);
				c->IAGetPrimitiveTopology(&topology);
				c->IAGetInputLayout(&layout);
				c->IAGetVertexBuffers(0, 1, &vb, &vbStride, &vbOffset);
				c->IAGetIndexBuffer(&ib, &ibFormat, &ibOffset);
			}

			template <class T>
			static void Rel(T*& p)
			{
				if (p) {
					p->Release();
					p = nullptr;
				}
			}

			void Restore(ID3D11DeviceContext* c)
			{
				c->RSSetViewports(viewportCount, viewports);
				c->RSSetScissorRects(scissorCount, scissors);
				c->RSSetState(raster);
				c->OMSetBlendState(blend, blendFactor, sampleMask);
				c->OMSetDepthStencilState(depth, stencilRef);
				c->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtvs, dsv);
				c->PSSetShaderResources(0, 1, &psSRV);
				c->PSSetSamplers(0, 1, &psSampler);
				c->PSSetShader(ps, psInst, psInstCount);
				c->VSSetShader(vs, vsInst, vsInstCount);
				c->GSSetShader(gs, gsInst, gsInstCount);
				c->HSSetShader(hs, nullptr, 0);
				c->DSSetShader(ds, nullptr, 0);
				c->VSSetConstantBuffers(0, 1, &vsCB);
				c->IASetPrimitiveTopology(topology);
				c->IASetInputLayout(layout);
				c->IASetVertexBuffers(0, 1, &vb, &vbStride, &vbOffset);
				c->IASetIndexBuffer(ib, ibFormat, ibOffset);

				Rel(raster);
				Rel(blend);
				Rel(depth);
				for (auto& r : rtvs) {
					Rel(r);
				}
				Rel(dsv);
				Rel(psSRV);
				Rel(psSampler);
				Rel(ps);
				Rel(vs);
				Rel(gs);
				Rel(hs);
				Rel(ds);
				for (UINT i = 0; i < psInstCount; ++i) {
					Rel(psInst[i]);
				}
				for (UINT i = 0; i < vsInstCount; ++i) {
					Rel(vsInst[i]);
				}
				for (UINT i = 0; i < gsInstCount; ++i) {
					Rel(gsInst[i]);
				}
				Rel(vsCB);
				Rel(layout);
				Rel(vb);
				Rel(ib);
			}
		};

		ComPtr<ID3DBlob> Compile(const char* a_entry, const char* a_target)
		{
			ComPtr<ID3DBlob> code, errors;
			const HRESULT    hr = ::D3DCompile(kShader, sizeof(kShader) - 1, "RealVideosComposite", nullptr, nullptr, a_entry, a_target,
				   D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);
			if (FAILED(hr)) {
				log::error("shader {} failed to compile: {}", a_entry,
					errors ? std::string(static_cast<const char*>(errors->GetBufferPointer()), errors->GetBufferSize()) : std::string("?"));
				return {};
			}
			return code;
		}
	}

	Compositor& Compositor::Get()
	{
		static Compositor c;
		return c;
	}

	bool Compositor::Ready(ID3D11Device* a_device)
	{
		if (!_tried) {
			_tried = true;
			_ok = Init(a_device);
			log::info("compositor {}", _ok ? "ready" : "unavailable (video will cover the whole mesh)");
		}
		return _ok;
	}

	bool Compositor::Init(ID3D11Device* a_device)
	{
		auto vsCode = Compile("VSMain", "vs_4_0");
		auto psCode = Compile("PSMain", "ps_4_0");
		if (!vsCode || !psCode) {
			return false;
		}
		if (FAILED(a_device->CreateVertexShader(vsCode->GetBufferPointer(), vsCode->GetBufferSize(), nullptr, &_vs)) ||
			FAILED(a_device->CreatePixelShader(psCode->GetBufferPointer(), psCode->GetBufferSize(), nullptr, &_ps))) {
			return false;
		}

		D3D11_BUFFER_DESC cb{};
		cb.ByteWidth = sizeof(Params);
		cb.Usage = D3D11_USAGE_DYNAMIC;
		cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
		cb.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
		if (FAILED(a_device->CreateBuffer(&cb, nullptr, &_cb))) {
			return false;
		}

		D3D11_SAMPLER_DESC s{};
		s.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
		s.AddressU = s.AddressV = s.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
		s.MaxLOD = D3D11_FLOAT32_MAX;
		if (FAILED(a_device->CreateSamplerState(&s, &_clamp))) {
			return false;
		}
		s.AddressU = s.AddressV = s.AddressW = D3D11_TEXTURE_ADDRESS_BORDER;
		s.BorderColor[0] = s.BorderColor[1] = s.BorderColor[2] = 0.0f;
		s.BorderColor[3] = 1.0f;
		if (FAILED(a_device->CreateSamplerState(&s, &_border))) {
			return false;
		}

		D3D11_BLEND_DESC b{};
		b.RenderTarget[0].BlendEnable = FALSE;
		b.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
		D3D11_DEPTH_STENCIL_DESC d{};
		d.DepthEnable = FALSE;
		d.StencilEnable = FALSE;
		D3D11_RASTERIZER_DESC r{};
		r.FillMode = D3D11_FILL_SOLID;
		r.CullMode = D3D11_CULL_NONE;
		r.DepthClipEnable = TRUE;
		return SUCCEEDED(a_device->CreateBlendState(&b, &_blend)) && SUCCEEDED(a_device->CreateDepthStencilState(&d, &_depth)) &&
		       SUCCEEDED(a_device->CreateRasterizerState(&r, &_raster));
	}

	void Compositor::Draw(ID3D11DeviceContext* a_context, std::uint32_t a_width, std::uint32_t a_height, const Region& a_region,
		ID3D11ShaderResourceView* a_srv, ID3D11SamplerState* a_sampler)
	{
		D3D11_MAPPED_SUBRESOURCE mapped{};
		if (FAILED(a_context->Map(_cb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
			return;
		}
		auto*       p = static_cast<Params*>(mapped.pData);
		const float w = static_cast<float>(a_width), h = static_cast<float>(a_height);
		p->dst[0] = a_region.x0 / w * 2.0f - 1.0f;
		p->dst[1] = 1.0f - a_region.y0 / h * 2.0f;
		p->dst[2] = a_region.x1 / w * 2.0f - 1.0f;
		p->dst[3] = 1.0f - a_region.y1 / h * 2.0f;
		p->uvs[0] = a_region.u0;
		p->uvs[1] = a_region.v0;
		p->uvs[2] = a_region.u1;
		p->uvs[3] = a_region.v1;
		a_context->Unmap(_cb.Get(), 0);

		a_context->PSSetShaderResources(0, 1, &a_srv);
		a_context->PSSetSamplers(0, 1, &a_sampler);
		a_context->Draw(4, 0);
	}

	void Compositor::Compose(ID3D11DeviceContext* a_context, ID3D11RenderTargetView* a_target, std::uint32_t a_width, std::uint32_t a_height,
		ID3D11ShaderResourceView* a_base, ID3D11ShaderResourceView* a_video, std::span<const Region> a_regions)
	{
		if (!_ok || !a_target || !a_video) {
			return;
		}

		StateBackup backup;
		backup.Save(a_context);

		D3D11_VIEWPORT vp{ 0.0f, 0.0f, static_cast<float>(a_width), static_cast<float>(a_height), 0.0f, 1.0f };
		a_context->RSSetViewports(1, &vp);
		a_context->RSSetScissorRects(0, nullptr);
		a_context->RSSetState(_raster.Get());
		const float factor[4]{};
		a_context->OMSetBlendState(_blend.Get(), factor, 0xFFFFFFFF);
		a_context->OMSetDepthStencilState(_depth.Get(), 0);
		ID3D11RenderTargetView* rtv = a_target;
		a_context->OMSetRenderTargets(1, &rtv, nullptr);
		a_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
		a_context->IASetInputLayout(nullptr);
		ID3D11Buffer* noBuffer = nullptr;
		UINT          zero = 0;
		a_context->IASetVertexBuffers(0, 1, &noBuffer, &zero, &zero);
		a_context->IASetIndexBuffer(nullptr, DXGI_FORMAT_UNKNOWN, 0);
		a_context->VSSetShader(_vs.Get(), nullptr, 0);
		a_context->PSSetShader(_ps.Get(), nullptr, 0);
		a_context->GSSetShader(nullptr, nullptr, 0);
		a_context->HSSetShader(nullptr, nullptr, 0);
		a_context->DSSetShader(nullptr, nullptr, 0);
		ID3D11Buffer* cb = _cb.Get();
		a_context->VSSetConstantBuffers(0, 1, &cb);

		if (a_base) {
			Draw(a_context, a_width, a_height, { 0, 0, static_cast<float>(a_width), static_cast<float>(a_height), 0, 0, 1, 1 }, a_base, _clamp.Get());
		} else {
			const float black[4]{ 0, 0, 0, 1 };
			a_context->ClearRenderTargetView(a_target, black);
		}
		for (const auto& region : a_regions) {
			Draw(a_context, a_width, a_height, region, a_video, _border.Get());
		}

		ID3D11ShaderResourceView* nullSRV = nullptr;
		a_context->PSSetShaderResources(0, 1, &nullSRV);
		backup.Restore(a_context);
	}
}
