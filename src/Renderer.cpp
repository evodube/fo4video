#include "Renderer.h"

#include "Manager.h"

namespace fv::Renderer
{
	namespace
	{
		using Present_t = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);

		Present_t                   g_originalPresent{ nullptr };
		ComPtr<ID3D11Device>        g_device;
		ComPtr<ID3D11DeviceContext> g_context;
		IDXGISwapChain*             g_swapChain{ nullptr };
		bool                        g_installed{ false };

		HRESULT STDMETHODCALLTYPE HookedPresent(IDXGISwapChain* a_swapChain, UINT a_syncInterval, UINT a_flags)
		{
			if (a_swapChain == g_swapChain && g_device && g_context) {
				Manager::Get().OnPresent(g_device.Get(), g_context.Get());
			}
			return g_originalPresent(a_swapChain, a_syncInterval, a_flags);
		}
	}

	bool Install()
	{
		if (g_installed) {
			return true;
		}

		auto* data = RE::BSGraphics::GetRendererData();
		if (!data) {
			log::warn("renderer not ready yet");
			return false;
		}
		auto* swapChain = reinterpret_cast<IDXGISwapChain*>(data->renderWindow[0].swapChain);
		if (!swapChain) {
			log::warn("swap chain not ready yet");
			return false;
		}

		// take the device from the swap chain itself rather than trusting struct offsets
		if (FAILED(swapChain->GetDevice(__uuidof(ID3D11Device), reinterpret_cast<void**>(g_device.ReleaseAndGetAddressOf())))) {
			log::error("swap chain has no D3D11 device");
			return false;
		}
		g_device->GetImmediateContext(g_context.ReleaseAndGetAddressOf());

		log::info("renderer: swapChain={} device={} (RendererData device={}) context={}", static_cast<void*>(swapChain),
			static_cast<void*>(g_device.Get()), static_cast<void*>(data->device), static_cast<void*>(g_context.Get()));

		auto**      vtable = *reinterpret_cast<void***>(swapChain);
		DWORD       oldProtect = 0;
		constexpr int kPresent = 8;
		if (!::VirtualProtect(&vtable[kPresent], sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProtect)) {
			log::error("VirtualProtect failed");
			return false;
		}
		g_originalPresent = reinterpret_cast<Present_t>(vtable[kPresent]);
		vtable[kPresent] = reinterpret_cast<void*>(&HookedPresent);
		::VirtualProtect(&vtable[kPresent], sizeof(void*), oldProtect, &oldProtect);

		g_swapChain = swapChain;
		g_installed = true;
		log::info("Present hook installed");
		return true;
	}
}
