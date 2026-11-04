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

		// a_ptr really is a D3D11 device (or context)? Guards against reading the game's renderer data at the wrong spot
		// on a game version we haven't seen: a bad pointer is caught instead of crashing.
		bool QueryDevice(void* a_ptr, ID3D11Device** a_out)
		{
			__try {
				return a_ptr && SUCCEEDED(static_cast<IUnknown*>(a_ptr)->QueryInterface(__uuidof(ID3D11Device), reinterpret_cast<void**>(a_out)));
			} __except (EXCEPTION_EXECUTE_HANDLER) {
				return false;
			}
		}

		bool QueryContext(void* a_ptr, ID3D11DeviceContext** a_out)
		{
			__try {
				return a_ptr && SUCCEEDED(static_cast<IUnknown*>(a_ptr)->QueryInterface(__uuidof(ID3D11DeviceContext), reinterpret_cast<void**>(a_out)));
			} __except (EXCEPTION_EXECUTE_HANDLER) {
				return false;
			}
		}

		// the graphics card a device runs on (to check two devices are on the same one)
		bool AdapterOf(ID3D11Device* a_device, LUID& a_out)
		{
			ComPtr<IDXGIDevice>  dxgi;
			ComPtr<IDXGIAdapter> adapter;
			DXGI_ADAPTER_DESC    desc{};
			if (!a_device || FAILED(a_device->QueryInterface(IID_PPV_ARGS(&dxgi))) || FAILED(dxgi->GetAdapter(&adapter)) ||
				FAILED(adapter->GetDesc(&desc))) {
				return false;
			}
			a_out = desc.AdapterLuid;
			return true;
		}

		// ReShade, ENB and other graphics wrappers / overlays in the game folder (for problem reports)
		void LogGraphicsMods()
		{
			namespace fs = std::filesystem;
			static constexpr std::array<const char*, 12> kFiles{ "dxgi.dll", "d3d11.dll", "d3d12.dll", "dinput8.dll", "d3dcompiler_46e.dll",
				"enbseries.ini", "enblocal.ini", "ReShade.ini", "ReShade.log", "dxgi.log", "d3d11.log", "reshade-shaders" };
			std::string     found;
			std::error_code ec;
			for (const auto* f : kFiles) {
				if (fs::exists(f, ec)) {
					found += found.empty() ? "" : ", ";
					found += f;
				}
			}
			log::info("graphics mods in the game folder: {}", found.empty() ? "none found" : found);
		}

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

		ComPtr<ID3D11Device> chainDevice;
		if (FAILED(swapChain->GetDevice(__uuidof(ID3D11Device), reinterpret_cast<void**>(chainDevice.GetAddressOf())))) {
			log::error("swap chain has no D3D11 device");
			return false;
		}
		ComPtr<ID3D11DeviceContext> chainContext;
		chainDevice->GetImmediateContext(chainContext.GetAddressOf());

		// Use the device the game itself draws with: our video textures go into the game's own textures, so they must
		// come from the same device. Normally the swap chain's device is that very device. With ReShade / ENB / other
		// wrappers they can differ, and textures from one device used by the other can break or hang the graphics driver.
		g_device = chainDevice;
		g_context = chainContext;
		ComPtr<ID3D11Device>        gameDevice;
		ComPtr<ID3D11DeviceContext> gameContext;
		const bool                  haveGame = QueryDevice(data->device, gameDevice.GetAddressOf()) &&
		                      QueryContext(data->context, gameContext.GetAddressOf());
		if (haveGame && gameDevice.Get() != chainDevice.Get()) {
			ComPtr<ID3D11Device> contextDevice;
			gameContext->GetDevice(contextDevice.GetAddressOf());
			LUID       a{}, b{};
			const bool sameCard = AdapterOf(gameDevice.Get(), a) && AdapterOf(chainDevice.Get(), b) && a.LowPart == b.LowPart &&
			                      a.HighPart == b.HighPart;
			if (contextDevice.Get() == gameDevice.Get() && sameCard) {
				g_device = gameDevice;
				g_context = gameContext;
				log::warn("renderer: the game and the swap chain use different devices (ReShade / ENB / overlay?); using the game's");
			} else {
				log::warn("renderer: the game's device doesn't check out (context {}, same graphics card {}); using the swap chain's",
					contextDevice.Get() == gameDevice.Get(), sameCard);
			}
		} else if (!haveGame) {
			log::warn("renderer: couldn't read the game's own device; using the swap chain's");
		}
		MediaSource::SetGameDevice(g_device.Get());  // GPU decoding uses the same graphics card

		log::info("renderer: swapChain={} device={} (swap chain device={}, RendererData device={}) context={}", static_cast<void*>(swapChain),
			static_cast<void*>(g_device.Get()), static_cast<void*>(chainDevice.Get()), static_cast<void*>(data->device),
			static_cast<void*>(g_context.Get()));
		LogGraphicsMods();

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
