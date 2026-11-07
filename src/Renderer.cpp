#include "Renderer.h"

#include "Manager.h"

#include <intrin.h>

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
		// Set when the swap chain isn't a D3D11 one (upscalers with frame generation present through D3D12): then our
		// drawing only runs on this thread, the game's own, never on a frame generator's present thread, because the
		// game's D3D11 immediate context must not be used from two threads at once. 0 = no restriction (normal case).
		std::atomic<DWORD>          g_drawThread{ 0 };
		std::atomic<bool>           g_otherThreadLogged{ false };

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

		std::string Utf8(const wchar_t* a_text)
		{
			const int len = ::WideCharToMultiByte(CP_UTF8, 0, a_text, -1, nullptr, 0, nullptr, nullptr);
			std::string out;
			if (len > 1) {
				out.assign(static_cast<std::size_t>(len - 1), '\0');
				::WideCharToMultiByte(CP_UTF8, 0, a_text, -1, out.data(), len, nullptr, nullptr);
			}
			return out;
		}

		// The PC, for problem reports: CPU, memory, Windows, which graphics card the game draws on and every graphics
		// adapter Windows has (a game running on the CPU's built-in graphics is a common cause of slow or missing video).
		void LogSystem(ID3D11Device* a_gameDevice)
		{
			// CPU name from the processor itself
			int  regs[4]{};
			char brand[49]{};
			__cpuid(regs, 0x80000000);
			if (static_cast<unsigned>(regs[0]) >= 0x80000004) {
				for (int i = 0; i < 3; ++i) {
					__cpuid(regs, 0x80000002 + i);
					std::memcpy(brand + i * 16, regs, 16);
				}
			}
			std::string cpu = brand;
			cpu.erase(0, cpu.find_first_not_of(' '));
			MEMORYSTATUSEX mem{ sizeof(mem) };
			::GlobalMemoryStatusEx(&mem);
			std::string windows = "?";
			using RtlGetVersion_t = LONG(WINAPI*)(OSVERSIONINFOW*);
			if (auto* ntdll = ::GetModuleHandleW(L"ntdll.dll")) {
				if (auto rtl = reinterpret_cast<RtlGetVersion_t>(::GetProcAddress(ntdll, "RtlGetVersion"))) {
					OSVERSIONINFOW v{ sizeof(v) };
					if (rtl(&v) == 0) {
						windows = std::format("{} (build {})", v.dwBuildNumber >= 22000 ? "11" : std::to_string(v.dwMajorVersion), v.dwBuildNumber);
					}
				}
			}
			log::info("system: CPU {} ({} threads), {} GB memory, Windows {}", cpu.empty() ? "?" : cpu, std::thread::hardware_concurrency(),
				(mem.ullTotalPhys + (512ull << 20)) >> 30, windows);

			// the card the game draws on
			ComPtr<IDXGIDevice>  dxgi;
			ComPtr<IDXGIAdapter> adapter;
			DXGI_ADAPTER_DESC    desc{};
			if (a_gameDevice && SUCCEEDED(a_gameDevice->QueryInterface(IID_PPV_ARGS(&dxgi))) && SUCCEEDED(dxgi->GetAdapter(&adapter)) &&
				SUCCEEDED(adapter->GetDesc(&desc))) {
				log::info("system: the game draws on {} ({} MB video memory)", Utf8(desc.Description), desc.DedicatedVideoMemory >> 20);
			}

			// every adapter
			ComPtr<IDXGIFactory1> factory;
			if (SUCCEEDED(::CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
				std::string list;
				ComPtr<IDXGIAdapter1> a;
				for (UINT i = 0; factory->EnumAdapters1(i, a.ReleaseAndGetAddressOf()) != DXGI_ERROR_NOT_FOUND; ++i) {
					DXGI_ADAPTER_DESC1 d{};
					if (SUCCEEDED(a->GetDesc1(&d)) && !(d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) {
						list += std::format("{}{} ({} MB)", list.empty() ? "" : "; ", Utf8(d.Description), d.DedicatedVideoMemory >> 20);
					}
				}
				log::info("system: graphics adapters: {}", list.empty() ? "none found" : list);
			}
		}

		HRESULT STDMETHODCALLTYPE HookedPresent(IDXGISwapChain* a_swapChain, UINT a_syncInterval, UINT a_flags)
		{
			if (a_swapChain == g_swapChain && g_device && g_context) {
				const DWORD only = g_drawThread.load(std::memory_order_relaxed);
				const DWORD self = ::GetCurrentThreadId();
				if (only == 0 || only == self) {
					Manager::Get().OnPresent(g_device.Get(), g_context.Get());
				} else if (!g_otherThreadLogged.exchange(true)) {
					log::warn("renderer: a frame was presented from thread {} (the game's is {}); frame generation? videos are only drawn on the game's own frames",
						self, only);
				}
			}
			return g_originalPresent(a_swapChain, a_syncInterval, a_flags);
		}

		// IDXGISwapChain::Present is slot 8 for D3D11 and D3D12 swap chains alike
		bool HookPresent(IDXGISwapChain* a_swapChain)
		{
			auto**        vtable = *reinterpret_cast<void***>(a_swapChain);
			DWORD         oldProtect = 0;
			constexpr int kPresent = 8;
			if (!::VirtualProtect(&vtable[kPresent], sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProtect)) {
				log::error("VirtualProtect failed");
				return false;
			}
			g_originalPresent = reinterpret_cast<Present_t>(vtable[kPresent]);
			vtable[kPresent] = reinterpret_cast<void*>(&HookedPresent);
			::VirtualProtect(&vtable[kPresent], sizeof(void*), oldProtect, &oldProtect);

			g_swapChain = a_swapChain;
			g_installed = true;
			log::info("Present hook installed");
			return true;
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

		ComPtr<ID3D11Device>        gameDevice;
		ComPtr<ID3D11DeviceContext> gameContext;
		const bool                  haveGame = QueryDevice(data->device, gameDevice.GetAddressOf()) &&
		                      QueryContext(data->context, gameContext.GetAddressOf());

		ComPtr<ID3D11Device> chainDevice;
		if (FAILED(swapChain->GetDevice(__uuidof(ID3D11Device), reinterpret_cast<void**>(chainDevice.GetAddressOf())))) {
			// Upscaling / frame generation mods (DLSS-G, FSR 3 frame generation) replace the game's swap chain with a
			// D3D12 one, which has no D3D11 device. The game still draws with its own D3D11 device, and that's the one
			// our textures must come from anyway, so use it.
			ComPtr<ID3D11Device> contextDevice;
			if (haveGame) {
				gameContext->GetDevice(contextDevice.GetAddressOf());
			}
			if (!haveGame || contextDevice.Get() != gameDevice.Get()) {
				log::error("swap chain has no D3D11 device, and the game's own device couldn't be read; videos can't be shown");
				return false;
			}
			const auto* gameMain = RE::Main::GetSingleton();
			const DWORD gameThread = gameMain && gameMain->threadID ? gameMain->threadID : ::GetCurrentThreadId();
			g_drawThread = gameThread;
			g_device = gameDevice;
			g_context = gameContext;
			MediaSource::SetGameDevice(g_device.Get());
			log::warn("renderer: the swap chain isn't D3D11 (upscaler / frame generation?); using the game's own device, drawing on the game's thread {}",
				gameThread);
			log::info("renderer: swapChain={} device={} context={}", static_cast<void*>(swapChain), static_cast<void*>(g_device.Get()),
				static_cast<void*>(g_context.Get()));
			LogGraphicsMods();
			LogSystem(g_device.Get());
			return HookPresent(swapChain);
		}
		ComPtr<ID3D11DeviceContext> chainContext;
		chainDevice->GetImmediateContext(chainContext.GetAddressOf());

		// Use the device the game itself draws with: our video textures go into the game's own textures, so they must
		// come from the same device. Normally the swap chain's device is that very device. With ReShade / ENB / other
		// wrappers they can differ, and textures from one device used by the other can break or hang the graphics driver.
		g_device = chainDevice;
		g_context = chainContext;
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
		LogSystem(g_device.Get());
		return HookPresent(swapChain);
	}
}
