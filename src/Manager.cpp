#include "Manager.h"

#include "Compositor.h"
#include "Remote.h"
#include "Settings.h"

namespace fv
{
	namespace
	{
		constexpr std::uint32_t kPatternSize = 256;

		// HUD messages are shown from a game task, never on the spot: callers often hold the manager's lock, or run on
		// the remote's UI thread, and the HUD can wait on the UI - showing it right there could freeze the game.
		void Notify(const std::string& a_message)
		{
			log::info("{}", a_message);
			if (auto* tasks = F4SE::GetTaskInterface()) {
				tasks->AddTask([m = a_message]() { RE::SendHUDMessage::ShowHUDMessage(m.c_str(), nullptr, false, false); });
			}
		}

		// everyday messages (playing, paused, off): can be switched off; problems always show
		void NotifyInfo(const std::string& a_message)
		{
			if (Settings::Get().showMessages) {
				Notify(a_message);
			} else {
				log::info("{}", a_message);
			}
		}

		// Opening a video (Media Foundation source readers, the hardware decoder, seeking to where it was) can take
		// a good part of a second. It used to happen on the game's main thread, so loading a save with several screens
		// playing froze the game while they all opened at once. Now one background thread opens them one after another
		// and each screen starts as soon as its own video is ready.
		class Opener
		{
		public:
			static Opener& Get()
			{
				static Opener o;
				return o;
			}

			void Add(std::function<void()> a_job)
			{
				{
					std::scoped_lock lock(_m);
					_jobs.push_back(std::move(a_job));
					if (!_thread.joinable()) {
						_thread = std::thread([this] { Run(); });
						_thread.detach();
					}
				}
				_cv.notify_one();
			}

		private:
			void Run()
			{
				for (;;) {
					std::function<void()> job;
					{
						std::unique_lock lock(_m);
						_cv.wait(lock, [this] { return !_jobs.empty(); });
						job = std::move(_jobs.front());
						_jobs.pop_front();
					}
					job();
				}
			}

			std::mutex                        _m;
			std::condition_variable           _cv;
			std::deque<std::function<void()>> _jobs;
			std::thread                       _thread;
		};

		void NotifyLater(const std::string& a_message) { Notify(a_message); }

		// The settlement whose build area holds a_pos (looked for around a_near), or nullptr when it's outside any.
		// Works for modded settlements too: it asks the game's own workshop system.
		RE::TESObjectREFR* SettlementAt(const RE::TESObjectREFR& a_near, const RE::NiPoint3& a_pos)
		{
			auto* workshop = RE::Workshop::FindNearestValidWorkshop(a_near);
			return workshop && RE::Workshop::IsLocationWithinBuildableArea(*workshop, a_pos) ? workshop : nullptr;
		}

		std::string SettlementName(RE::TESObjectREFR* a_workshop)
		{
			if (!a_workshop) {
				return "outside any settlement";
			}
			if (auto* loc = a_workshop->GetCurrentLocation()) {
				if (const auto name = RE::TESFullName::GetFullName(*loc); !name.empty()) {
					return std::format("settlement '{}'", name);
				}
			}
			return std::format("settlement (workbench {:08X})", a_workshop->GetFormID());
		}

		bool SameVideo(const std::wstring& a_a, const std::wstring& a_b)
		{
			return !a_a.empty() && _wcsicmp(std::filesystem::path(a_a).filename().c_str(), std::filesystem::path(a_b).filename().c_str()) == 0;
		}

		std::string Narrow(const std::wstring& a_str)
		{
			if (a_str.empty()) {
				return {};
			}
			const int   len = ::WideCharToMultiByte(CP_UTF8, 0, a_str.c_str(), static_cast<int>(a_str.size()), nullptr, 0, nullptr, nullptr);
			std::string out(static_cast<std::size_t>(len), '\0');
			::WideCharToMultiByte(CP_UTF8, 0, a_str.c_str(), static_cast<int>(a_str.size()), out.data(), len, nullptr, nullptr);
			return out;
		}

		// A player belongs to the object it was started on and to the object whose screen it plays on
		// (the drive-in started from its switch is the same video as the one started on the screen).
		bool Is(const Player& a_player, std::uint32_t a_formID)
		{
			return a_formID != 0 && (a_player.formID == a_formID || a_player.screenID.load() == a_formID);
		}

		std::uint32_t PrefKey(const Player& a_player)
		{
			const auto screen = a_player.screenID.load();
			return screen != 0 ? screen : a_player.formID;
		}

		std::uint32_t ScreenOf(const ScreenBinding& a_binding)
		{
			auto owner = a_binding.owner.get();
			return owner ? owner->GetFormID() : 0;
		}

		RE::NiPointer<RE::TESObjectREFR> PickedRef()
		{
			return Manager::Get().Target();
		}

		// Animated test card: scrolling hue gradient, a sweeping white bar,
		// red square top-left and blue square bottom-right (shows orientation/UV flips).
		void MakePattern(std::vector<std::uint8_t>& a_px, std::uint32_t a_w, std::uint32_t a_h, double a_t)
		{
			a_px.resize(static_cast<std::size_t>(a_w) * a_h * 4);
			const auto bar = static_cast<int>(std::fmod(a_t * 0.5, 1.0) * a_w);
			for (std::uint32_t y = 0; y < a_h; ++y) {
				for (std::uint32_t x = 0; x < a_w; ++x) {
					float hue = static_cast<float>(std::fmod(static_cast<double>(x) / a_w + a_t * 0.1, 1.0)) * 6.0f;
					const int   sector = static_cast<int>(hue);
					const float f = hue - sector;
					float       r = 0, g = 0, b = 0;
					switch (sector % 6) {
					case 0: r = 1; g = f; break;
					case 1: r = 1 - f; g = 1; break;
					case 2: g = 1; b = f; break;
					case 3: g = 1 - f; b = 1; break;
					case 4: r = f; b = 1; break;
					default: r = 1; b = 1 - f; break;
					}
					const float shade = 0.35f + 0.65f * (static_cast<float>(y) / a_h);
					r *= shade;
					g *= shade;
					b *= shade;
					if (std::abs(static_cast<int>(x) - bar) < 4) {
						r = g = b = 1.0f;
					}
					if (x < a_w / 6 && y < a_h / 6) {
						r = 1; g = 0; b = 0;
					} else if (x > a_w * 5 / 6 && y > a_h * 5 / 6) {
						r = 0; g = 0; b = 1;
					}
					auto* p = a_px.data() + (static_cast<std::size_t>(y) * a_w + x) * 4;
					p[0] = static_cast<std::uint8_t>(b * 255);
					p[1] = static_cast<std::uint8_t>(g * 255);
					p[2] = static_cast<std::uint8_t>(r * 255);
					p[3] = 255;
				}
			}
		}

		// Full volume inside a_min, silent at a_max, fading in between (a_curve 1 = straight line, 2 = the
		// default smooth fade that drops quicker near the screen, higher = quieter sooner).
		float Attenuate(float a_distance, float a_min, float a_max, float a_curve)
		{
			if (a_distance <= a_min) {
				return 1.0f;
			}
			if (a_distance >= a_max) {
				return 0.0f;
			}
			const float t = 1.0f - (a_distance - a_min) / (a_max - a_min);
			return std::pow(t, a_curve);
		}

		HWND GameWindow()
		{
			auto* data = RE::BSGraphics::GetRendererData();
			return data ? reinterpret_cast<HWND>(data->renderWindow[0].hwnd) : nullptr;
		}
	}

	namespace
	{
		// Texture size that holds a w x h frame at the screen's aspect ratio (black bars instead of stretching).
		std::pair<std::uint32_t, std::uint32_t> FittedSize(std::uint32_t a_w, std::uint32_t a_h, float a_screenAspect)
		{
			if (Settings::Get().aspectMode == 0 || a_screenAspect <= 0.0f || a_w == 0 || a_h == 0) {
				return { a_w, a_h };
			}
			const float video = static_cast<float>(a_w) / static_cast<float>(a_h);
			if (std::abs(video / a_screenAspect - 1.0f) < 0.03f) {
				return { a_w, a_h };
			}
			std::uint32_t w = a_w, h = a_h;
			if (video > a_screenAspect) {
				h = static_cast<std::uint32_t>(std::lround(a_w / a_screenAspect));
			} else {
				w = static_cast<std::uint32_t>(std::lround(a_h * a_screenAspect));
			}
			w = std::min<std::uint32_t>((w + 1) & ~1u, 8192);
			h = std::min<std::uint32_t>((h + 1) & ~1u, 8192);
			return { std::max(w, a_w), std::max(h, a_h) };
		}
	}

	namespace
	{
		// Pixel size of the texture the screen originally used (what the frame/border parts sample).
		bool BaseSize(ID3D11ShaderResourceView* a_srv, std::uint32_t& a_w, std::uint32_t& a_h)
		{
			if (!a_srv) {
				return false;
			}
			ComPtr<ID3D11Resource> res;
			a_srv->GetResource(&res);
			ComPtr<ID3D11Texture2D> tex;
			if (!res || FAILED(res.As(&tex))) {
				return false;
			}
			D3D11_TEXTURE2D_DESC desc{};
			tex->GetDesc(&desc);
			a_w = desc.Width;
			a_h = desc.Height;
			return a_w > 0 && a_h > 0;
		}

		// How the video lands on one screen face: orientation (flip/mirror/rotate) and black bars to keep its shape.
		Compositor::Region FaceRegion(const std::array<float, 4>& a_face, std::uint32_t a_w, std::uint32_t a_h, float a_faceAspect,
			std::uint32_t a_videoW, std::uint32_t a_videoH, int a_orient)
		{
			Compositor::Region r{ a_face[0] * a_w, a_face[1] * a_h, a_face[2] * a_w, a_face[3] * a_h };
			const bool flip = (a_orient & 1) != 0, mirror = (a_orient & 2) != 0, rotated = (a_orient & 4) != 0;
			// tx runs along texture u, ty along texture v
			if (!rotated) {
				r.ou = mirror ? 1.0f : 0.0f; r.au = mirror ? -1.0f : 1.0f; r.bu = 0.0f;
				r.ov = flip ? 1.0f : 0.0f;   r.av = 0.0f;                  r.bv = flip ? -1.0f : 1.0f;
			} else {
				r.ou = mirror ? 1.0f : 0.0f; r.au = 0.0f;                  r.bu = mirror ? -1.0f : 1.0f;
				r.ov = flip ? 1.0f : 0.0f;   r.av = flip ? -1.0f : 1.0f;  r.bv = 0.0f;
			}
			if (Settings::Get().aspectMode == 1 && a_faceAspect > 0.0f && a_videoW && a_videoH) {
				const float video = static_cast<float>(a_videoW) / static_cast<float>(a_videoH);
				float       ku = 1.0f, kv = 1.0f;
				if (video > a_faceAspect * 1.03f) {
					kv = video / a_faceAspect;  // wider video: bars top and bottom
				} else if (video < a_faceAspect * 0.97f) {
					ku = a_faceAspect / video;  // narrower video: bars left and right
				}
				r.ou = 0.5f + (r.ou - 0.5f) * ku; r.au *= ku; r.bu *= ku;
				r.ov = 0.5f + (r.ov - 0.5f) * kv; r.av *= kv; r.bv *= kv;
			}
			return r;
		}
	}

	// ---------------- Slot ----------------

	bool Slot::Ensure(ID3D11Device* a_device, std::uint32_t a_width, std::uint32_t a_height, bool a_srgb, bool a_gpuTarget)
	{
		if (texture && width == a_width && height == a_height && srgb == a_srgb && gpuTarget == a_gpuTarget) {
			return true;
		}
		Unpatch();
		srv.Reset();
		rtv.Reset();
		texture.Reset();

		D3D11_TEXTURE2D_DESC desc{};
		desc.Width = a_width;
		desc.Height = a_height;
		desc.MipLevels = 1;
		desc.ArraySize = 1;
		desc.SampleDesc.Count = 1;
		if (a_gpuTarget) {
			// GPU decoding: the frame is drawn in on the GPU (render target)
			desc.Format = DXGI_FORMAT_B8G8R8A8_TYPELESS;
			desc.Usage = D3D11_USAGE_DEFAULT;
			desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
		} else {
			desc.Format = a_srgb ? DXGI_FORMAT_B8G8R8X8_UNORM_SRGB : DXGI_FORMAT_B8G8R8X8_UNORM;
			desc.Usage = D3D11_USAGE_DYNAMIC;
			desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
			desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
		}

		HRESULT hr = a_device->CreateTexture2D(&desc, nullptr, &texture);
		if (FAILED(hr)) {
			log::error("CreateTexture2D {}x{} failed (0x{:08X})", a_width, a_height, static_cast<std::uint32_t>(hr));
			return false;
		}
		const auto viewFormat = a_srgb ? DXGI_FORMAT_B8G8R8A8_UNORM_SRGB : DXGI_FORMAT_B8G8R8A8_UNORM;
		if (a_gpuTarget) {
			D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
			sd.Format = viewFormat;
			sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
			sd.Texture2D.MipLevels = 1;
			D3D11_RENDER_TARGET_VIEW_DESC rd{};
			rd.Format = viewFormat;
			rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
			hr = a_device->CreateShaderResourceView(texture.Get(), &sd, &srv);
			if (SUCCEEDED(hr)) {
				hr = a_device->CreateRenderTargetView(texture.Get(), &rd, &rtv);
			}
		} else {
			hr = a_device->CreateShaderResourceView(texture.Get(), nullptr, &srv);
		}
		if (FAILED(hr)) {
			log::error("CreateShaderResourceView failed (0x{:08X})", static_cast<std::uint32_t>(hr));
			srv.Reset();
			rtv.Reset();
			texture.Reset();
			return false;
		}
		width = a_width;
		height = a_height;
		srgb = a_srgb;
		gpuTarget = a_gpuTarget;
		log::info("slot texture {}x{} created (sRGB={}{})", a_width, a_height, a_srgb, a_gpuTarget ? ", GPU decoding" : "");
		return true;
	}

	// Copies a frame into the slot texture, centred, with black bars filling any extra space
	// (the texture is sized to the screen's shape when keeping the aspect ratio).
	void Slot::Upload(ID3D11DeviceContext* a_context, const std::uint8_t* a_pixels, std::uint32_t a_width, std::uint32_t a_height)
	{
		if (!texture || a_width > width || a_height > height) {
			return;
		}
		D3D11_MAPPED_SUBRESOURCE mapped{};
		if (FAILED(a_context->Map(texture.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
			return;
		}
		const std::size_t rowBytes = static_cast<std::size_t>(a_width) * 4;
		const std::size_t texRowBytes = static_cast<std::size_t>(width) * 4;
		const std::uint32_t offX = (width - a_width) / 2;
		const std::uint32_t offY = (height - a_height) / 2;
		auto*             dst = static_cast<std::uint8_t*>(mapped.pData);

		if (offX == 0 && offY == 0 && mapped.RowPitch == rowBytes) {
			std::memcpy(dst, a_pixels, rowBytes * a_height);
		} else {
			for (std::uint32_t y = 0; y < height; ++y) {
				auto* row = dst + static_cast<std::size_t>(y) * mapped.RowPitch;
				if (y < offY || y >= offY + a_height) {
					std::memset(row, 0, texRowBytes);
					continue;
				}
				if (offX) {
					std::memset(row, 0, static_cast<std::size_t>(offX) * 4);
					std::memset(row + (static_cast<std::size_t>(offX) + a_width) * 4, 0, texRowBytes - (static_cast<std::size_t>(offX) + a_width) * 4);
				}
				std::memcpy(row + static_cast<std::size_t>(offX) * 4, a_pixels + static_cast<std::size_t>(y - offY) * rowBytes, rowBytes);
			}
		}
		a_context->Unmap(texture.Get(), 0);
	}

	bool Slot::EnsureComposite(ID3D11Device* a_device, std::uint32_t a_width, std::uint32_t a_height, bool a_srgb)
	{
		if (compTexture && compWidth == a_width && compHeight == a_height && compSRGB == a_srgb) {
			return true;
		}
		Unpatch();
		compSRV.Reset();
		compRTV.Reset();
		compTexture.Reset();

		D3D11_TEXTURE2D_DESC desc{};
		desc.Width = a_width;
		desc.Height = a_height;
		desc.MipLevels = 1;
		desc.ArraySize = 1;
		desc.Format = DXGI_FORMAT_R8G8B8A8_TYPELESS;
		desc.SampleDesc.Count = 1;
		desc.Usage = D3D11_USAGE_DEFAULT;
		desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
		if (FAILED(a_device->CreateTexture2D(&desc, nullptr, &compTexture))) {
			log::error("composite texture {}x{} failed", a_width, a_height);
			return false;
		}
		const auto format = a_srgb ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB : DXGI_FORMAT_R8G8B8A8_UNORM;
		D3D11_RENDER_TARGET_VIEW_DESC rtv{};
		rtv.Format = format;
		rtv.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
		srvDesc.Format = format;
		srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
		srvDesc.Texture2D.MipLevels = 1;
		if (FAILED(a_device->CreateRenderTargetView(compTexture.Get(), &rtv, &compRTV)) ||
			FAILED(a_device->CreateShaderResourceView(compTexture.Get(), &srvDesc, &compSRV))) {
			compTexture.Reset();
			compRTV.Reset();
			compSRV.Reset();
			return false;
		}
		compWidth = a_width;
		compHeight = a_height;
		compSRGB = a_srgb;
		compNeedsDraw = true;
		log::info("composite texture {}x{} created (sRGB={})", a_width, a_height, a_srgb);
		return true;
	}

	void Slot::Patch(int a_index, ID3D11ShaderResourceView* a_want)
	{
		auto* tex = niTexture.get();
		if (!tex || !a_want) {
			return;
		}
		auto* rt = tex->rendererTexture;
		if (!rt) {
			return;
		}
		auto* cur = reinterpret_cast<ID3D11ShaderResourceView*>(rt->srView);
		auto* want = a_want;

		if (rt != patchedRT) {
			// never touch a texture unless it is verifiably our placeholder
			if (!Screen::IsPlaceholderTexture(tex)) {
				if (!refusedLogged) {
					log::error("slot {}: texture '{}' is {}x{}, not our 4x8 placeholder; refusing to patch it", a_index, tex->GetName(),
						rt->header.width, rt->header.height);
					refusedLogged = true;
				}
				return;
			}
			patchedRT = rt;
			originalSRV = cur;
			installedSRV = nullptr;
			log::info("slot {}: patched placeholder texture (renderer {})", a_index, static_cast<void*>(rt));
		}

		if (cur == want) {
			return;
		}
		if (cur && cur == installedSRV) {
			installedSRV->Release();  // drop the ref we gave the engine for our previous view
		} else {
			originalSRV = cur;
		}
		want->AddRef();  // the engine now holds this pointer and may release it
		rt->srView = reinterpret_cast<REX::W32::ID3D11ShaderResourceView*>(want);
		installedSRV = want;
	}

	void Slot::Unpatch()
	{
		if (patchedRT && installedSRV && niTexture && niTexture->rendererTexture == patchedRT) {
			if (reinterpret_cast<ID3D11ShaderResourceView*>(patchedRT->srView) == installedSRV) {
				patchedRT->srView = reinterpret_cast<REX::W32::ID3D11ShaderResourceView*>(originalSRV);
				installedSRV->Release();
			}
		}
		patchedRT = nullptr;
		originalSRV = nullptr;
		installedSRV = nullptr;
	}

	void Slot::Release()
	{
		Unpatch();
		rtv.Reset();
		srv.Reset();
		texture.Reset();
		compSRV.Reset();
		compRTV.Reset();
		compTexture.Reset();
		width = height = compWidth = compHeight = 0;
	}

	// ---------------- Manager ----------------

	Manager& Manager::Get()
	{
		static Manager manager;
		return manager;
	}

	std::wstring Manager::ResolveVideoPath(const std::wstring& a_name) const
	{
		namespace fs = std::filesystem;
		fs::path p(a_name);
		if (!p.is_absolute()) {
			p = fs::path(Settings::Get().videoFolder) / p;
		}
		if (!p.has_extension()) {
			p += L".mp4";
		}
		std::error_code ec;
		auto            abs = fs::absolute(p, ec);
		return ec ? p.wstring() : abs.wstring();
	}

	std::vector<std::wstring> Manager::ListVideos() const
	{
		namespace fs = std::filesystem;
		static constexpr std::array<std::wstring_view, 8> exts{ L".mp4", L".m4v", L".mov", L".wmv", L".avi", L".mkv", L".webm", L".asf" };

		// the video folder and its subfolders (each subfolder is a playlist); paths relative to the video folder
		std::vector<std::wstring> out;
		std::error_code           ec;
		const fs::path            root(Settings::Get().videoFolder);
		for (fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end;
			 it.increment(ec)) {
			if (it.depth() > 4) {
				it.disable_recursion_pending();
			}
			if (!it->is_regular_file(ec)) {
				continue;
			}
			auto ext = it->path().extension().wstring();
			for (auto& c : ext) {
				c = static_cast<wchar_t>(::towlower(c));
			}
			if (std::ranges::find(exts, std::wstring_view(ext)) != exts.end()) {
				out.push_back(it->path().lexically_relative(root).wstring());
			}
		}
		// videos in a folder first, then its subfolders, everything by name
		std::ranges::sort(out, [](const std::wstring& a_a, const std::wstring& a_b) {
			const fs::path pa(a_a), pb(a_b);
			const auto     fa = pa.parent_path().wstring(), fb = pb.parent_path().wstring();
			if (fa != fb) {
				return _wcsicmp(fa.c_str(), fb.c_str()) < 0;
			}
			return _wcsicmp(pa.filename().c_str(), pb.filename().c_str()) < 0;
		});
		return out;
	}

	int Manager::FreeSlot() const
	{
		const int max = Settings::Get().maxScreens;
		for (int i = 0; i < max; ++i) {
			if (!_slots[i].inUse) {
				return i;
			}
		}
		return -1;
	}

	void Manager::StopLocked(Player& a_player)
	{
		if (a_player.stopping) {
			return;
		}
		Screen::Unbind(a_player.binding);
		a_player.stopping = true;  // render thread finishes the teardown
	}

	bool Manager::Start(RE::TESObjectREFR* a_ref, SourceKind a_kind, const std::wstring& a_file, bool a_loop, std::int64_t a_startAt,
		bool a_quiet)
	{
		if (!a_ref) {
			Notify("Real Videos: no screen selected");
			return false;
		}
		const auto refName = Screen::RefName(a_ref);
		if (!a_ref->Get3D()) {
			Notify(std::format("Real Videos: {} is not loaded", refName));
			return false;
		}

		int slot = -1;
		{
			std::scoped_lock lock(_lock);
			for (auto& p : _players) {
				if (Is(*p, a_ref->GetFormID())) {
					StopLocked(*p);
				}
			}
			// something new on a screen that was asleep: it doesn't come back with the old video
			std::erase_if(_sleepers, [&](const Sleeper& a_s) { return a_s.formID == a_ref->GetFormID(); });
			slot = FreeSlot();
			if (slot < 0) {
				Notify("Real Videos: all screen slots are busy (raise iMaxScreens)");
				return false;
			}
			_slots[slot].inUse = true;
		}

		auto releaseSlot = [&]() {
			std::scoped_lock lock(_lock);
			_slots[slot].inUse = false;
		};

		auto slotTexture = Screen::LoadSlotTexture(slot);
		auto glowTexture = Screen::LoadSlotTexture(slot + kMaxSlots);  // glow map (optional; missing = glowing shader)
		if (!slotTexture) {
			Notify("Real Videos: placeholder textures missing (Data\\Textures\\RealVideos)");
			releaseSlot();
			return false;
		}

		auto player = std::make_unique<Player>();
		player->ref = a_ref->GetHandle();
		player->formID = a_ref->GetFormID();
		player->name = refName;
		player->slot = slot;
		player->kind = a_kind;
		player->file = a_file;
		player->loop = a_loop;
		player->seekDelta = a_startAt;  // carry on from there (woken up after you walked away)

		if (a_kind == SourceKind::kVideo && !std::filesystem::exists(a_file)) {
			Notify(std::format("Real Videos: file not found: {}", Narrow(a_file)));
			releaseSlot();
			return false;
		}

		if (!Screen::Bind(a_ref, slotTexture.get(), player->binding, -1, glowTexture.get())) {
			Notify(std::format("Real Videos: no screen found on {} (see RealVideos.log)", refName));
			releaseSlot();
			return false;
		}
		// Another video already on this very mesh (e.g. the drive-in started from its switch, then from the
		// screen): that one stops. Undo our bind first so its original look comes back cleanly, then bind again.
		{
			auto* geom = player->binding.geometry.get();
			bool  clash = false;
			{
				std::scoped_lock lock(_lock);
				for (auto& p : _players) {
					clash |= !p->stopping && p->binding.bound && p->binding.geometry.get() == geom;
				}
				if (clash) {
					Screen::Unbind(player->binding);
					for (auto& p : _players) {
						if (!p->stopping && p->binding.bound && p->binding.geometry.get() == geom) {
							log::info("{}: '{}' is already playing on that screen; replacing it", refName, p->name);
							StopLocked(*p);
						}
					}
				}
			}
			if (clash && !Screen::Bind(a_ref, slotTexture.get(), player->binding, -1, glowTexture.get())) {
				Notify(std::format("Real Videos: no screen found on {} (see RealVideos.log)", refName));
				releaseSlot();
				return false;
			}
		}
		player->screenID = ScreenOf(player->binding);
		player->srgb = player->binding.originalWasSRGB;
		player->screenAspect = player->binding.screenAspect;

		// opened once the screen is known: TVs, projectors and the cinema each have their own size limit
		std::function<void()> openJob;
		if (a_kind == SourceKind::kVideo) {
			const auto& settings = Settings::Get();
			const int   maxH = player->binding.classic ? settings.maxHeightTV :
			                   player->binding.bigScreen ? settings.maxHeightCinema :
			                                               settings.maxHeightProjector;
			const bool gpu = settings.gpuDecode && !Compositor::Get().Failed();
			const bool audio = settings.enableAudio;
			const auto height = static_cast<std::uint32_t>(std::max(0, maxH));
			const auto serial = player->serial = _nextSerial++;
			Player*    target = player.get();
			target->openHeight = height;
			// opened in the background (see Opener); the screen shows its dark placeholder until the video is ready
			openJob = [this, target, serial, audio, gpu]() { OpenMedia(target, serial, audio, gpu); };
			// another screen already playing this video: start in step with it
			if (settings.syncSameVideo && a_startAt == 0) {
				std::scoped_lock lock(_lock);
				for (auto& o : _players) {
					if (!o->stopping && o->media && o->kind == SourceKind::kVideo && SameVideo(o->file, a_file)) {
						player->seekDelta = o->media->Clock();
						player->userPaused = o->userPaused.load();
						log::info("{}: in step with {} ({:.0f}s in)", refName, o->name, static_cast<double>(player->seekDelta.load()) / 1e7);
						break;
					}
				}
			}
		}

		{
			std::scoped_lock lock(_lock);
			_slots[slot].niTexture = slotTexture;
			_slots[slot].refusedLogged = false;
			_glowSlots[slot].niTexture = glowTexture;
			_glowSlots[slot].refusedLogged = false;
			_players.push_back(std::move(player));
		}
		if (openJob) {
			Opener::Get().Add(std::move(openJob));  // only once the player is in the list it looks for
		}
		// what this screen has played, for shuffle (random picks avoid recent ones; previous goes back through it)
		if (a_kind == SourceKind::kVideo) {
			std::error_code ec;
			const auto      root = std::filesystem::absolute(std::filesystem::path(Settings::Get().videoFolder), ec);
			const auto      rel = std::filesystem::path(a_file).lexically_relative(root).wstring();  // as ListVideos names it
			auto&           history = _shuffleHistory[a_ref->GetFormID()];
			if (history.empty() || _wcsicmp(history.back().c_str(), rel.c_str()) != 0) {
				history.push_back(rel);
				if (history.size() > 64) {
					history.erase(history.begin());
				}
			}
		}

		if (a_quiet) {
			log::info("{}: back on ({}), carrying on at {:.0f}s", refName, Narrow(std::filesystem::path(a_file).filename().wstring()),
				static_cast<double>(a_startAt) / 1e7);
		} else if (a_kind == SourceKind::kVideo) {
			NotifyInfo(std::format("Real Videos: playing {} on {}", Narrow(std::filesystem::path(a_file).filename().wstring()), refName));
		} else {
			NotifyInfo(std::format("Real Videos: test pattern on {}", refName));
		}
		return true;
	}

	// Runs on the Opener thread. a_target may be gone by then (stopped, replaced): it is only used while it is still
	// in _players with the same serial.
	void Manager::OpenMedia(Player* a_target, std::uint64_t a_serial, bool a_audio, bool a_gpu)
	{
		const auto alive = [&]() {
			return std::ranges::any_of(_players, [&](const auto& p) { return p.get() == a_target && p->serial == a_serial && !p->stopping; });
		};
		std::wstring  file;
		std::string   name;
		std::uint32_t height = 0;
		{
			std::scoped_lock lock(_lock);
			if (!alive()) {
				return;  // stopped before its turn came
			}
			file = a_target->file;
			name = a_target->name;
			height = a_target->openHeight;
		}
		auto media = std::make_unique<MediaSource>();
		if (!media->Open(file, a_audio, height, a_gpu)) {
			NotifyLater(std::format("Real Videos: could not decode {}", Narrow(std::filesystem::path(file).filename().wstring())));
			std::scoped_lock lock(_lock);
			if (alive()) {
				StopLocked(*a_target);
			}
			return;
		}
		// carry on from the saved spot here too: seeking costs as much as opening
		std::int64_t startAt = 0;
		bool         paused = false;
		{
			std::scoped_lock lock(_lock);
			if (!alive()) {
				return;
			}
			startAt = a_target->seekDelta.exchange(0);
			paused = a_target->userPaused.load();
		}
		media->SetPaused(paused);
		if (startAt > 0) {
			media->Seek(startAt);  // starts it too
		} else {
			media->Start();
		}
		std::scoped_lock lock(_lock);
		if (alive()) {
			a_target->media = std::move(media);
			a_target->mediaSince = std::chrono::steady_clock::now();
			a_target->gpuMisses = 0;
			a_target->gpuMissLogged = false;
			a_target->noPictureLogged = false;
			log::info("{}: video ready ({})", name, a_target->media->OnGpu() ? "GPU decoding" : "CPU decoding");
		} else {
			std::thread([m = std::move(media)]() mutable { m.reset(); }).detach();
		}
	}

	// The graphics card path doesn't work on this PC (some overlays / wrappers / drivers can't share the decoder's
	// pictures with the game). Called with _lock held: this video, and every one after it, decodes on the CPU instead.
	void Manager::FallBackToCpuLocked(Player& a_player, const std::string& a_why)
	{
		if (!a_player.media || !a_player.media->OnGpu()) {
			return;
		}
		log::warn("{}: GPU decoding isn't working here ({}); switching to CPU decoding for this session", a_player.name, a_why);
		MediaSource::DisableGpu();
		a_player.seekDelta = a_player.media->Clock();  // carry on from the same spot
		std::thread([m = std::move(a_player.media)]() mutable { m.reset(); }).detach();
		a_player.gpuIn.clear();
		Opener::Get().Add([this, target = &a_player, serial = a_player.serial, audio = Settings::Get().enableAudio]() {
			OpenMedia(target, serial, audio, false);
		});
	}

	// a video in Data\Videos\Autoplay (or a folder inside it)
	bool Manager::IsAutoplay(const std::wstring& a_file) const
	{
		std::error_code ec;
		const auto      root = std::filesystem::absolute(std::filesystem::path(Settings::Get().videoFolder), ec);
		const auto      rel = std::filesystem::path(a_file).lexically_relative(root);
		const auto      first = rel.begin() != rel.end() ? rel.begin()->wstring() : std::wstring{};
		return _wcsicmp(first.c_str(), L"Autoplay") == 0 && std::next(rel.begin()) != rel.end();
	}

	// a random other video from the same Autoplay folder as a_file (the same one if it's the only one); full path
	std::wstring Manager::PickAutoplay(const std::wstring& a_file)
	{
		std::error_code ec;
		const auto      root = std::filesystem::absolute(std::filesystem::path(Settings::Get().videoFolder), ec);
		const auto      folder = std::filesystem::path(a_file).lexically_relative(root).parent_path().wstring();
		std::vector<std::wstring> pool, all;
		for (const auto& v : ListVideos()) {
			if (_wcsicmp(std::filesystem::path(v).parent_path().wstring().c_str(), folder.c_str()) != 0) {
				continue;
			}
			const auto full = ResolveVideoPath(v);
			all.push_back(full);
			if (_wcsicmp(full.c_str(), a_file.c_str()) != 0) {
				pool.push_back(full);
			}
		}
		auto& from = pool.empty() ? all : pool;
		if (from.empty()) {
			return {};
		}
		return from[std::uniform_int_distribution<std::size_t>(0, from.size() - 1)(_rng)];
	}

	bool Manager::Play(RE::TESObjectREFR* a_ref, const std::wstring& a_file, bool a_loop)
	{
		return Start(a_ref, SourceKind::kVideo, ResolveVideoPath(a_file), a_loop);
	}

	bool Manager::PlayTestPattern(RE::TESObjectREFR* a_ref)
	{
		return Start(a_ref, SourceKind::kTestPattern, {}, true);
	}

	void Manager::Stop(RE::TESObjectREFR* a_ref)
	{
		if (!a_ref) {
			return;
		}
		std::scoped_lock lock(_lock);
		for (auto& p : _players) {
			if (Is(*p, a_ref->GetFormID())) {
				StopLocked(*p);
			}
		}
	}

	void Manager::StopAll()
	{
		std::scoped_lock lock(_lock);
		for (auto& p : _players) {
			StopLocked(*p);
		}
	}

	bool Manager::IsPlaying(RE::TESObjectREFR* a_ref)
	{
		if (!a_ref) {
			return false;
		}
		std::scoped_lock lock(_lock);
		for (auto& p : _players) {
			if (Is(*p, a_ref->GetFormID()) && !p->stopping) {
				return true;
			}
		}
		return false;
	}

	void Manager::SetVolume(RE::TESObjectREFR* a_ref, float a_volume)
	{
		if (!a_ref) {
			return;
		}
		std::scoped_lock lock(_lock);
		for (auto& p : _players) {
			if (Is(*p, a_ref->GetFormID())) {
				p->userVolume = std::clamp(a_volume, 0.0f, 4.0f);
			}
		}
	}

	void Manager::Maintain()
	{
		auto*            player = RE::PlayerCharacter::GetSingleton();
		const auto       playerPos = player ? player->GetPosition() : RE::NiPoint3{};
		const auto&      settings = Settings::Get();
		std::scoped_lock lock(_lock);

		// Build mode: screens go dark (video paused) until it's closed
		static const RE::BSFixedString workshopMenu("WorkshopMenu");
		auto*                          ui = RE::UI::GetSingleton();
		const bool                     buildMode = ui && ui->GetMenuOpen(workshopMenu);
		if (buildMode != _inBuildMode) {
			log::info("build mode {}: {} the videos", buildMode ? "opened" : "closed", buildMode ? "hiding" : "showing");
			_inBuildMode = buildMode;
			for (auto& p : _players) {
				if (p->stopping) {
					continue;
				}
				if (buildMode) {
					// the moment build mode opens, before the workshop touches anything: every screen goes back to
					// exactly how the game made it (own shader and material, back in its group), so the workshop's
					// highlighting and moving only ever see the original
					if (p->binding.bound) {
						Screen::Unbind(p->binding);
						log::info("{}: back to its original look for build mode", p->name);
					}
				} else {
					p->rebindCooldown = 6;  // put the video back about a second after build mode closes
				}
			}
		}

		// How lit the world is right now, for the back / frame / pole of glowing projector screens: they glow with
		// the picture, so they're dimmed to what the game's lighting would give them. Outside: the current weather's
		// ambient + sunlight/moonlight colours (follows the time of day, storms, dusk...). Inside: the cell's own.
		{
			auto lum = [](float r, float g, float b) { return 0.2126f * r + 0.7152f * g + 0.0722f * b; };
			auto lumPacked = [&](std::uint32_t c) {
				return lum(static_cast<float>(c & 0xFF) / 255.0f, static_cast<float>((c >> 8) & 0xFF) / 255.0f, static_cast<float>((c >> 16) & 0xFF) / 255.0f);
			};
			float ambient = 0.25f, direct = 0.25f;
			auto* cell = player ? player->parentCell : nullptr;
			if (cell && cell->IsInterior()) {
				if (auto* data = cell->cellData.interior) {
					ambient = lumPacked(data->ambient);
					direct = lumPacked(data->directional);
				}
			} else if (auto* sky = RE::Sky::GetSingleton()) {
				const auto& a = sky->skyColor[RE::TESWeather::ColorTypes::kAmbient];
				const auto& d = sky->skyColor[RE::TESWeather::ColorTypes::kSunlight];
				ambient = lum(a.r, a.g, a.b);
				direct = lum(d.r, d.g, d.b);
			}
			// a surface gets the ambient plus, on average, about half the sun (it isn't always facing it)
			// brighter light counts a little extra (the eye adapts: at 1x the frame looked right at night but too dark
			// in the morning), so one slider fits day and night
			const float measured = ambient + 0.5f * direct;
			const float boost = settings.frameLight * (1.0f + 2.0f * std::clamp(measured, 0.0f, 1.0f));
			const float target = std::clamp(measured * boost, 0.0f, 1.0f);
			// ease towards it so lightning flashes and weather changes don't make the frame flicker
			static float smooth = -1.0f;
			smooth = smooth < 0.0f ? target : smooth + (target - smooth) * 0.05f;
			_frameLight = smooth;
			static float logged = -1.0f;
			static auto  lastLog = std::chrono::steady_clock::time_point{};
			const auto   now = std::chrono::steady_clock::now();
			if (std::abs(smooth - logged) > 0.05f && now - lastLog > std::chrono::seconds(10)) {
				logged = smooth;
				lastLog = now;
				log::info("world light {:.2f} (ambient {:.2f}, sun/moon {:.2f}, {}): projector frames at {:.0f}%", smooth, ambient, direct,
					cell && cell->IsInterior() ? "inside" : "outside", smooth * 100.0f);
			}
		}

		// where the camera is and where it looks, for "out of view"
		RE::NiPoint3 camPos = playerPos, camDir{ 0.0f, 1.0f, 0.0f };
		bool         haveCam = false;
		if (auto* camera = RE::PlayerCamera::GetSingleton(); camera && camera->cameraRoot && player) {
			camPos = camera->cameraRoot->GetWorldTranslate();
			const float pitch = player->data.angle.x, yaw = player->data.angle.z;
			camDir = { std::sin(yaw) * std::cos(pitch), std::cos(yaw) * std::cos(pitch), -std::sin(pitch) };
			haveCam = true;
		}

		// which settlement you're in (about once a second)
		{
			static int every = 0;
			if (player && settings.offInOtherSettlement && (++every % 6 == 0 || !_playerWorkshopKnown)) {
				auto*      ws = SettlementAt(*player, playerPos);
				const auto handle = ws ? ws->GetHandle() : RE::ObjectRefHandle{};
				if (!_playerWorkshopKnown || handle.get().get() != _playerWorkshop.get().get()) {
					log::info("you're now {}", SettlementName(ws));
				}
				_playerWorkshop = handle;
				_playerWorkshopKnown = true;
			}
		}

		for (auto& p : _players) {
			if (p->stopping) {
				continue;
			}
			if (buildMode) {
				// Leave the scene graph alone while the workshop works on it (highlighting, moving, scrapping):
				// changing materials under it crashed the game. The render thread shows the screens dark instead.
				continue;
			}
			auto ref = p->ref.get();
			if (!ref) {
				log::info("{} no longer exists; stopping", p->name);
				StopLocked(*p);
				continue;
			}
			if (ref->IsDeleted() || ref->IsDisabled()) {
				// scrapped or stored in build mode: stop, otherwise the build menu shows it still playing
				log::info("{} was removed (scrapped/stored/disabled); stopping", p->name);
				StopLocked(*p);
				continue;
			}

			auto* root = ref->Get3D();
			const bool valid = Screen::StillValid(p->binding);
			if (!valid) {
				// 3D unloaded/rebuilt, screen part re-attached (projector screens), or another mod swapped
				// the material: rebind. When unbound, retry about once a second.
				const bool wasBound = p->binding.bound;
				if (wasBound || p->rebindCooldown-- <= 0) {
					if (wasBound) {
						log::info("{}: screen mesh changed, rebinding", p->name);
					}
					Screen::Unbind(p->binding);
					if (root && _slots[p->slot].niTexture) {
						if (Screen::Bind(ref.get(), _slots[p->slot].niTexture.get(), p->binding, -1, _glowSlots[p->slot].niTexture.get())) {
							auto* geom = p->binding.geometry.get();
							const bool taken = std::ranges::any_of(_players, [&](const auto& o) {
								return o.get() != p.get() && !o->stopping && o->binding.bound && o->binding.geometry.get() == geom;
							});
							if (taken) {
								// another video owns that screen now: don't fight over it
								log::info("{}: its screen is used by another video; stopping", p->name);
								StopLocked(*p);
								continue;
							}
							p->screenID = ScreenOf(p->binding);
							_slots[p->slot].compNeedsDraw = true;
							p->srgb = p->binding.originalWasSRGB;
							p->screenAspect = p->binding.screenAspect;
						}
					}
					p->rebindCooldown = 6;
				}
			}

			Screen::KeepOverlaysHidden(p->binding);
			{
				LoadBrightnessLocked();
				const auto key = PrefKey(*p);
				const auto it = _prefs.find(key);
				p->brightness = it != _prefs.end() ? it->second.brightness : 1.0f;
				p->brightenable = p->binding.bound && !p->binding.classic;
			}

			// far away or out of view for a while: stop updating the picture (the sound carries on). Comes back
			// the moment you look at it or come closer.
			if (p->binding.bound) {
				p->cinema = p->binding.bigScreen;
			}
			const bool  cinema = p->cinema;
			const bool  freezeOn = cinema ? settings.cinemaFreeze : settings.freeze;
			const float freezeDistance = cinema ? settings.cinemaFreezeDistance : settings.freezeDistance;
			const float freezeSeconds = cinema ? settings.cinemaFreezeSeconds : settings.freezeSeconds;
			const bool  offOn = cinema ? settings.cinemaAutoOff : settings.autoOff;
			const float offDistance = cinema ? settings.cinemaAutoOffDistance : settings.autoOffDistance;
			const float offSeconds = cinema ? settings.cinemaAutoOffSeconds : settings.autoOffSeconds;
			if (freezeOn && player && p->binding.bound) {
				auto*       g = p->binding.geometry.get();
				const auto  c = g ? g->worldBound.center : ref->GetPosition();
				const float r = g ? std::max(g->worldBound.fRadius, 1.0f) : 100.0f;
				const auto  v = c - camPos;
				const float d = v.Length();
				bool        inView = true;
				if (haveCam && d > r) {
					// inside a generous cone around where you look (wider than the screen's field of view)
					const float cosAngle = v.Dot(camDir) / d;
					const float angle = std::acos(std::clamp(cosAngle, -1.0f, 1.0f));
					inView = angle < 1.15f + std::asin(std::min(1.0f, r / d));  // ~66 degrees + the screen's size
				}
				const bool unseen = d > freezeDistance || !inView;
				const auto now = std::chrono::steady_clock::now();
				if (!unseen) {
					p->unseen = false;
					p->frozen = false;
				} else if (!p->unseen) {
					p->unseen = true;
					p->unseenSince = now;
				} else if (!p->frozen && now - p->unseenSince >= std::chrono::duration<float>(freezeSeconds)) {
					p->frozen = true;
					log::info("{}: paused ({})", p->name, inView ? "far away" : "out of view");
				}
			} else {
				p->frozen = false;
			}

			// the settlement this screen is in (once, while it's loaded)
			if (!p->workshopChecked && p->binding.bound && settings.offInOtherSettlement) {
				p->workshopChecked = true;
				auto  owner = p->binding.owner.get();
				auto* screen = owner ? owner.get() : ref.get();
				auto* g = p->binding.geometry.get();
				auto* ws = screen ? SettlementAt(*screen, g ? g->worldBound.center : screen->GetPosition()) : nullptr;
				p->workshop = ws ? ws->GetHandle() : RE::ObjectRefHandle{};
				log::info("{}: in {}", p->name, SettlementName(ws));
			}
			// you're in a different settlement: turn it off after a few seconds (back on when you return)
			bool goOff = false;
			if (settings.offInOtherSettlement && _playerWorkshopKnown) {
				auto mine = p->workshop.get();
				auto here = _playerWorkshop.get();
				const auto now = std::chrono::steady_clock::now();
				if (mine && here && mine.get() != here.get()) {
					if (!p->elsewhere) {
						p->elsewhere = true;
						p->elsewhereSince = now;
					} else if (now - p->elsewhereSince >= std::chrono::duration<float>(settings.otherSettlementSeconds)) {
						goOff = true;
					}
				} else {
					p->elsewhere = false;
				}
			}

			// walked away: turn the screen off after a while (frees its decoder, textures and slot) and remember
			// where it was, to carry on when you come back
			if ((offOn || goOff) && player) {
				auto*      g = p->binding.bound ? p->binding.geometry.get() : nullptr;
				const auto where = g ? g->worldBound.center : ref->GetPosition();
				const float limit = offDistance;
				const float ax = where.x - playerPos.x, ay = where.y - playerPos.y, az = where.z - playerPos.z;
				const auto  now = std::chrono::steady_clock::now();
				const bool farAway = offOn && std::sqrt(ax * ax + ay * ay + az * az) > limit;
				if (farAway || goOff) {
					if (!p->away) {
						p->away = true;
						p->awaySince = now;
					}
					if (goOff || now - p->awaySince >= std::chrono::duration<float>(offSeconds)) {
						Sleeper s;
						s.ref = p->ref;
						s.formID = p->formID;
						s.kind = p->kind;
						s.file = p->file;
						s.loop = p->loop;
						s.position = p->media ? p->media->Clock() : p->seekDelta.load();
						s.where = where;
						s.wakeDistance = limit * 0.9f;  // a little closer than where it turned off, so it doesn't flicker on/off
						s.name = p->name;
						s.workshop = p->workshop;
						s.paused = p->userPaused.load();
						s.cinema = cinema;
						if (goOff) {
							log::info("{}: you're in another settlement; turned off until you're back (at {:.0f}s)", p->name,
								static_cast<double>(s.position) / 1e7);
						} else {
							log::info("{}: you've been away {:.0f}s; turned off until you come back (at {:.0f}s)", p->name, offSeconds,
								static_cast<double>(s.position) / 1e7);
						}
						_sleepers.push_back(std::move(s));
						StopLocked(*p);
						continue;
					}
				} else {
					p->away = false;
				}
			}

			float volume = 0.0f;
			if (p->binding.bound && player) {
				// big screens (the drive-in) carry much further, measured from the screen itself
				auto*      geom = p->binding.geometry.get();
				const bool big = geom && p->binding.bigScreen;
				const auto pos = big ? geom->worldBound.center : ref->GetPosition();
				const float dx = pos.x - playerPos.x;
				const float dy = pos.y - playerPos.y;
				const float dz = pos.z - playerPos.z;
				const float dist = std::sqrt(dx * dx + dy * dy + dz * dz);
				const float gain = big ? Attenuate(dist, settings.bigMinDistance, settings.bigMaxDistance, settings.falloffCurve)
				                       : Attenuate(dist, settings.minDistance, settings.maxDistance, settings.falloffCurve);
				const auto& pr = _prefs[PrefKey(*p)];
				const float screenVolume = pr.volume < 0.0f ? settings.startVolume : pr.volume;
				volume = pr.muted ? 0.0f : settings.volume * p->userVolume * screenVolume * gain;
				// which side the screen is on, seen from the camera (from where you stand when you're right at it)
				float pan = 0.0f;
				if (settings.directionalSound && haveCam) {
					const float hx = pos.x - camPos.x, hy = pos.y - camPos.y;
					const float hl = std::sqrt(hx * hx + hy * hy);
					if (hl > 70.0f) {
						const float yaw = player->data.angle.z;
						pan = (hx * std::cos(yaw) - hy * std::sin(yaw)) / hl;  // camera right = (cos yaw, -sin yaw)
						pan *= std::clamp((hl - 70.0f) / 140.0f, 0.0f, 1.0f);  // fades in over the first few metres
					}
				}
				p->pan = pan;
			}
			p->volume = volume;
		}

		// screens playing the same video stay in step: anyone more than a second and a half off the first is moved
		if (settings.syncSameVideo && !buildMode) {
			for (std::size_t i = 0; i < _players.size(); ++i) {
				auto& lead = *_players[i];
				if (lead.stopping || !lead.media || lead.kind != SourceKind::kVideo || lead.frozen.load()) {
					continue;
				}
				bool first = true;
				for (std::size_t k = 0; k < i; ++k) {
					auto& o = *_players[k];
					if (!o.stopping && o.media && SameVideo(o.file, lead.file)) {
						first = false;
						break;
					}
				}
				if (!first) {
					continue;
				}
				const auto leadClock = lead.media->Clock();
				for (std::size_t k = i + 1; k < _players.size(); ++k) {
					auto& f = *_players[k];
					if (f.stopping || !f.media || !SameVideo(f.file, lead.file) || f.seekDelta.load() != 0 || f.frozen.load()) {
						continue;
					}
					f.userPaused = lead.userPaused.load();
					const auto off = leadClock - f.media->Clock();
					if (std::abs(off) > 15'000'000) {
						f.seekDelta = off;
						log::info("{}: back in step with {}", f.name, lead.name);
					}
				}
			}
		}

		// screens that turned off while you were away (or saved playing): back on, where they were, once you're close
		// again. Only one every 0.3 s, nearest first, so loading a save with lots of screens doesn't hit the game at once.
		if (!buildMode && player) {
			const auto now = std::chrono::steady_clock::now();
			auto       best = _sleepers.end();
			float      bestDist = FLT_MAX;
			for (auto it = _sleepers.begin(); it != _sleepers.end();) {
				auto ref = it->ref.get();
				if (!ref || ref->IsDeleted() || ref->IsDisabled()) {
					log::info("{}: gone while it was off; forgetting it", it->name);
					it = _sleepers.erase(it);
					best = _sleepers.end();  // iterators moved; look again next time
					bestDist = FLT_MAX;
					continue;
				}
				const float dx = it->where.x - playerPos.x, dy = it->where.y - playerPos.y, dz = it->where.z - playerPos.z;
				const float dist = std::sqrt(dx * dx + dy * dy + dz * dz);
				const bool  isNear = dist < it->wakeDistance;
				// a screen in a settlement comes back when you're in that settlement, or close by outside any
				// settlement; while you're in another settlement it stays off even if that one is next door
				auto       mine = it->workshop.get();
				auto       here = _playerWorkshopKnown ? _playerWorkshop.get() : RE::NiPointer<RE::TESObjectREFR>{};
				const bool elsewhere = settings.offInOtherSettlement && mine && here && here.get() != mine.get();
				const bool wake = (!(it->cinema ? settings.cinemaAutoOff : settings.autoOff) || isNear) && !elsewhere;
				if (wake && ref->Get3D() && dist < bestDist) {
					best = it;
					bestDist = dist;
				}
				++it;
			}
			if (best != _sleepers.end() && now - _lastWake >= std::chrono::milliseconds(300)) {
				_lastWake = now;
				if (auto* tasks = F4SE::GetTaskInterface()) {
					tasks->AddTask([this, s = *best]() {
						auto r = s.ref.get();
						if (!r) {
							return;
						}
						// Videos\Autoplay: nothing is remembered - coming back starts a different video from that folder
						if (s.kind == SourceKind::kVideo && IsAutoplay(s.file)) {
							if (const auto next = PickAutoplay(s.file); !next.empty()) {
								log::info("{}: Autoplay folder, a new video instead of carrying on", s.name);
								Start(r.get(), SourceKind::kVideo, next, s.loop, 0, true);
								return;
							}
						}
						if (Start(r.get(), s.kind, s.file, s.loop, s.position, true) && s.paused) {
							std::scoped_lock lock(_lock);
							for (auto& p : _players) {
								if (!p->stopping && Is(*p, s.formID)) {
									p->userPaused = true;
								}
							}
						}
					});
				}
				_sleepers.erase(best);
			}
		}
	}

	void Manager::ScheduleMaintain()
	{
		if (_maintainPending.exchange(true)) {
			return;
		}
		auto* tasks = F4SE::GetTaskInterface();
		if (!tasks) {
			_maintainPending = false;
			return;
		}
		tasks->AddTask([this]() {
			Maintain();
			_maintainPending = false;
		});
	}

	bool Manager::RefuseUnsupported(RE::TESObjectREFR* a_ref)
	{
		if (!Settings::Get().onlySupportedScreens) {
			return false;
		}
		std::string what;
		if (Screen::IsSupported(a_ref, &what)) {
			return false;
		}
		log::info("{}: {}; ignored (bOnlySupportedScreens=1)", Screen::RefName(a_ref), what);
		Notify("Real Videos: that isn't a TV, projector or cinema screen");
		return true;
	}

	void Manager::HotkeyPlayNext()
	{
		auto ref = PickedRef();
		if (!ref) {
			Notify("Real Videos: aim at a TV, projector or cinema screen first");
			return;
		}
		if (RefuseUnsupported(ref.get())) {
			return;
		}
		const auto videos = ListVideos();
		if (videos.empty()) {
			Notify(std::format("Real Videos: no videos in {}", Narrow(Settings::Get().videoFolder)));
			return;
		}
		const auto& name = videos[_nextVideo % videos.size()];
		++_nextVideo;
		Play(ref.get(), name, Settings::Get().loop);
	}

	void Manager::HotkeyTestPattern()
	{
		auto ref = PickedRef();
		if (!ref) {
			Notify("Real Videos: aim at a TV, projector or cinema screen first");
			return;
		}
		if (RefuseUnsupported(ref.get())) {
			return;
		}
		PlayTestPattern(ref.get());
	}

	void Manager::HotkeyStop()
	{
		if (auto ref = PickedRef(); ref && IsPlaying(ref.get())) {
			Stop(ref.get());
			NotifyInfo(std::format("Real Videos: stopped {}", Screen::RefName(ref.get())));
		} else {
			StopAll();
			NotifyInfo("Real Videos: stopped all screens");
		}
	}

	void Manager::HotkeyDump()
	{
		auto ref = PickedRef();
		if (!ref) {
			Notify("Real Videos: aim at a TV, projector or cinema screen first");
			return;
		}
		DumpScreen(ref.get());
	}

	void Manager::DumpScreen(RE::TESObjectREFR* a_ref)
	{
		if (!a_ref) {
			return;
		}
		Screen::Dump(a_ref);
		NotifyInfo(std::format("Real Videos: details of {} written to RealVideos.log", Screen::RefName(a_ref)));
	}

	RE::NiPointer<RE::TESObjectREFR> Manager::Target()
	{
		const auto& s = Settings::Get();
		if (s.aimToSelect) {
			if (auto* aimed = Screen::PickAimed(s.aimDistance)) {
				return RE::NiPointer<RE::TESObjectREFR>(aimed);
			}
		}
		return RE::Console::GetPickRef().get();
	}

	void Manager::RemotePlayPause(RE::TESObjectREFR* a_ref)
	{
		if (!a_ref) {
			return;
		}
		{
			std::scoped_lock lock(_lock);
			for (auto& p : _players) {
				if (Is(*p, a_ref->GetFormID()) && !p->stopping) {
					p->userPaused = !p->userPaused;
					if (Settings::Get().syncSameVideo) {  // screens in step pause together
						for (auto& o : _players) {
							if (o.get() != p.get() && !o->stopping && SameVideo(o->file, p->file)) {
								o->userPaused = p->userPaused.load();
							}
						}
					}
					NotifyInfo(std::format("Real Videos: {} {}", p->userPaused ? "paused" : "playing", p->name));
					return;
				}
			}
		}
		RemoteStep(a_ref, 0);  // off: switch on with the current video
	}

	void Manager::RemoteStep(RE::TESObjectREFR* a_ref, int a_dir)
	{
		if (!a_ref || RefuseUnsupported(a_ref)) {
			return;
		}
		auto videos = ListVideos();
		if (videos.empty()) {
			Notify(std::format("Real Videos: no videos in {}", Narrow(Settings::Get().videoFolder)));
			return;
		}
		// what's playing on this screen: next / previous stay inside its folder (its playlist)
		const auto current = CurrentVideo(a_ref);
		const auto inFolder = [&](const std::wstring& a_folder) {
			std::erase_if(videos, [&](const std::wstring& a_v) { return _wcsicmp(std::filesystem::path(a_v).parent_path().wstring().c_str(), a_folder.c_str()) != 0; });
		};
		if (!current.empty()) {
			inFolder(std::filesystem::path(current).parent_path().wstring());
		} else if (Settings::Get().shuffle) {
			// shuffle never mixes folders: turning a screen on picks from the folder it last played from, or else from the
			// videos directly in the video folder (not its subfolders) - unless there are none there
			std::wstring folder;
			if (auto it = _shuffleHistory.find(a_ref->GetFormID()); it != _shuffleHistory.end() && !it->second.empty()) {
				folder = std::filesystem::path(it->second.back()).parent_path().wstring();
			}
			auto kept = videos;
			inFolder(folder);
			if (videos.empty()) {
				videos = std::move(kept);
				inFolder(L"");
				if (videos.empty()) {
					videos = ListVideos();
				}
			}
		}
		std::ptrdiff_t index = static_cast<std::ptrdiff_t>(_nextVideo % std::max<std::size_t>(videos.size(), 1));
		bool           playing = false;
		for (std::size_t i = 0; i < videos.size(); ++i) {
			if (_wcsicmp(videos[i].c_str(), current.c_str()) == 0) {
				index = static_cast<std::ptrdiff_t>(i);
				playing = true;
			}
		}
		if (videos.empty()) {
			return;
		}
		if (Settings::Get().shuffle) {
			const auto id = a_ref->GetFormID();
			auto&      history = _shuffleHistory[id];
			// previous: back to the video played before this one (if it's still in this folder)
			if (a_dir < 0 && playing && history.size() >= 2) {
				history.pop_back();  // the current one
				const auto back = history.back();
				history.pop_back();  // starting it adds it again
				if (std::ranges::any_of(videos, [&](const std::wstring& a_v) { return _wcsicmp(a_v.c_str(), back.c_str()) == 0; })) {
					Play(a_ref, back, Settings::Get().loop);
					return;
				}
			}
			// a random one, not the current one and not one of the last few played (about half the folder)
			std::vector<std::size_t> pool;
			const std::size_t        avoid = std::min(history.size(), videos.size() / 2);
			for (std::size_t i = 0; i < videos.size(); ++i) {
				const bool recent = std::any_of(history.end() - static_cast<std::ptrdiff_t>(avoid), history.end(),
					[&](const std::wstring& a_h) { return _wcsicmp(a_h.c_str(), videos[i].c_str()) == 0; });
				const bool isCurrent = playing && static_cast<std::ptrdiff_t>(i) == index;
				if (!recent && (!isCurrent || videos.size() == 1)) {
					pool.push_back(i);
				}
			}
			if (pool.empty()) {
				for (std::size_t i = 0; i < videos.size(); ++i) {
					if (!playing || static_cast<std::ptrdiff_t>(i) != index || videos.size() == 1) {
						pool.push_back(i);
					}
				}
			}
			const auto pick = videos[pool[std::uniform_int_distribution<std::size_t>(0, pool.size() - 1)(_rng)]];
			Play(a_ref, pick, Settings::Get().loop);  // with shuffle on, the end of a video moves on instead of repeating
			return;
		}
		if (playing) {
			const auto n = static_cast<std::ptrdiff_t>(videos.size());
			index = ((index + a_dir) % n + n) % n;
		} else {
			++_nextVideo;
		}
		Play(a_ref, videos[static_cast<std::size_t>(index)], Settings::Get().loop);
	}

	void Manager::RemotePlayFile(RE::TESObjectREFR* a_ref, const std::wstring& a_name)
	{
		if (!a_ref || RefuseUnsupported(a_ref)) {
			return;
		}
		Play(a_ref, a_name, Settings::Get().loop);
	}

	void Manager::RemoteFirst(RE::TESObjectREFR* a_ref)
	{
		if (!a_ref || RefuseUnsupported(a_ref)) {
			return;
		}
		auto videos = ListVideos();
		if (videos.empty()) {
			Notify(std::format("Real Videos: no videos in {}", Narrow(Settings::Get().videoFolder)));
			return;
		}
		// home: the first video of the folder (playlist) this screen is playing from
		if (const auto current = CurrentVideo(a_ref); !current.empty()) {
			const auto folder = std::filesystem::path(current).parent_path().wstring();
			for (const auto& v : videos) {
				if (_wcsicmp(std::filesystem::path(v).parent_path().wstring().c_str(), folder.c_str()) == 0) {
					Play(a_ref, v, Settings::Get().loop);
					return;
				}
			}
		}
		Play(a_ref, videos.front(), Settings::Get().loop);
	}

	std::wstring Manager::CurrentVideo(RE::TESObjectREFR* a_ref)
	{
		std::scoped_lock lock(_lock);
		for (auto& p : _players) {
			if (a_ref && Is(*p, a_ref->GetFormID()) && !p->stopping && p->kind == SourceKind::kVideo) {
				std::error_code ec;
				const auto      root = std::filesystem::absolute(std::filesystem::path(Settings::Get().videoFolder), ec);
				return std::filesystem::path(p->file).lexically_relative(root).wstring();
			}
		}
		return {};
	}

	void Manager::RemotePower(RE::TESObjectREFR* a_ref)
	{
		if (!a_ref) {
			return;
		}
		if (IsPlaying(a_ref)) {
			Stop(a_ref);
			NotifyInfo(std::format("Real Videos: {} off", Screen::RefName(a_ref)));
		} else {
			RemoteStep(a_ref, 0);
		}
	}

	void Manager::RemoteSeek(std::uint32_t a_formID, float a_seconds)
	{
		std::scoped_lock lock(_lock);
		std::wstring file;
		for (auto& p : _players) {
			if (Is(*p, a_formID) && !p->stopping) {
				p->seekDelta += static_cast<std::int64_t>(a_seconds * 10'000'000.0f);
				file = p->file;
			}
		}
		if (Settings::Get().syncSameVideo && !file.empty()) {  // screens in step move together
			for (auto& p : _players) {
				if (!Is(*p, a_formID) && !p->stopping && SameVideo(p->file, file)) {
					p->seekDelta += static_cast<std::int64_t>(a_seconds * 10'000'000.0f);
				}
			}
		}
	}

	void Manager::RemoteSeekTo(std::uint32_t a_formID, float a_seconds)
	{
		std::scoped_lock lock(_lock);
		for (auto& p : _players) {
			if (Is(*p, a_formID) && !p->stopping && p->media) {
				const auto target = static_cast<std::int64_t>(std::max(0.0f, a_seconds) * 10'000'000.0f);
				p->seekDelta = target - p->media->Clock();
				if (Settings::Get().syncSameVideo) {  // screens in step move together
					for (auto& o : _players) {
						if (o.get() != p.get() && !o->stopping && o->media && SameVideo(o->file, p->file)) {
							o->seekDelta = target - o->media->Clock();
						}
					}
				}
				return;
			}
		}
	}

	namespace
	{
		RE::NiPoint3 PlayerPos()
		{
			auto* pc = RE::PlayerCharacter::GetSingleton();
			return pc ? pc->GetPosition() : RE::NiPoint3{};
		}
	}

	void Manager::AllScreens(RE::TESObjectREFR* a_from, bool a_on)
	{
		const auto& settings = Settings::Get();
		const auto  center = PlayerPos();
		if (!a_on) {
			int stopped = 0;
			{
				std::scoped_lock lock(_lock);
				for (auto& p : _players) {
					auto ref = p->ref.get();
					auto* g = p->binding.bound ? p->binding.geometry.get() : nullptr;
					const auto where = g ? g->worldBound.center : ref ? ref->GetPosition() : center;
					if (!p->stopping && where.GetDistance(center) <= settings.allRadius) {
						StopLocked(*p);
						++stopped;
					}
				}
				_sleepers.clear();
			}
			NotifyInfo(std::format("Real Videos: {} screen{} here off", stopped, stopped == 1 ? "" : "s"));
			return;
		}

		// the video on the screen you're aiming at, else the first one
		std::wstring file;
		{
			std::scoped_lock lock(_lock);
			for (auto& p : _players) {
				if (a_from && Is(*p, a_from->GetFormID()) && !p->stopping && p->kind == SourceKind::kVideo) {
					file = p->file;
				}
			}
		}
		if (file.empty()) {
			const auto videos = ListVideos();
			if (videos.empty()) {
				Notify(std::format("Real Videos: no videos in {}", Narrow(settings.videoFolder)));
				return;
			}
			file = ResolveVideoPath(videos.front());
		}
		int started = 0;
		for (auto& ref : Screen::FindNearby(center, settings.allRadius)) {
			if (IsPlaying(ref.get())) {
				continue;
			}
			{
				std::scoped_lock lock(_lock);
				if (FreeSlot() < 0) {
					Notify("Real Videos: all screen slots are busy (raise 'Screens playing at once')");
					break;
				}
			}
			if (Start(ref.get(), SourceKind::kVideo, file, settings.loop, 0, true)) {
				++started;
			}
		}
		NotifyInfo(std::format("Real Videos: {} screen{} here on", started, started == 1 ? "" : "s"));
	}

	void Manager::AllScreensToggle(RE::TESObjectREFR* a_from)
	{
		const auto center = PlayerPos();
		bool       any = false;
		{
			std::scoped_lock lock(_lock);
			for (auto& p : _players) {
				auto ref = p->ref.get();
				if (!p->stopping && ref && ref->GetPosition().GetDistance(center) <= Settings::Get().allRadius) {
					any = true;
				}
			}
		}
		AllScreens(a_from, !any);
	}

	void Manager::RebindAll()
	{
		std::scoped_lock lock(_lock);
		for (auto& p : _players) {
			if (!p->stopping && p->binding.bound) {
				Screen::Unbind(p->binding);  // Maintain binds it again with the current settings
				p->rebindCooldown = 0;
			}
		}
		Notify("Real Videos: settings applied to the playing screens");
	}

	std::uint32_t Manager::PrefKeyLocked(std::uint32_t a_formID) const
	{
		for (auto& p : _players) {
			if (!p->stopping && Is(*p, a_formID)) {
				return PrefKey(*p);
			}
		}
		return a_formID;
	}

	void Manager::RemoteSetVolume(std::uint32_t a_formID, float a_volume)
	{
		std::scoped_lock lock(_lock);
		_prefs[PrefKeyLocked(a_formID)].volume = std::clamp(a_volume, 0.0f, 2.0f);
	}

	void Manager::RemoteSetMuted(std::uint32_t a_formID, bool a_muted)
	{
		std::scoped_lock lock(_lock);
		_prefs[PrefKeyLocked(a_formID)].muted = a_muted;
	}

	namespace
	{
		constexpr auto kBrightnessIni = "Data\\F4SE\\Plugins\\RealVideos_Screens.ini";
	}

	void Manager::LoadBrightnessLocked()
	{
		if (_brightnessLoaded) {
			return;
		}
		_brightnessLoaded = true;
		std::vector<char> buf(32 * 1024);
		const auto        len = ::GetPrivateProfileSectionA("Brightness", buf.data(), static_cast<DWORD>(buf.size()), kBrightnessIni);
		for (std::size_t i = 0; i < len && buf[i];) {
			const std::string line(&buf[i]);
			i += line.size() + 1;
			const auto eq = line.find('=');
			if (eq == std::string::npos) {
				continue;
			}
			try {
				const auto key = static_cast<std::uint32_t>(std::stoul(line.substr(0, eq), nullptr, 16));
				const float value = std::stof(line.substr(eq + 1));
				if (key) {
					_prefs[key].brightness = std::clamp(value, 0.25f, 1.0f);
				}
			} catch (...) {
			}
		}
	}

	void Manager::RemoteSetBrightness(std::uint32_t a_formID, float a_brightness)
	{
		std::scoped_lock lock(_lock);
		LoadBrightnessLocked();
		const auto  key = PrefKeyLocked(a_formID);
		const float value = std::clamp(a_brightness, 0.25f, 1.0f);
		_prefs[key].brightness = value;
		for (auto& p : _players) {
			if (Is(*p, a_formID)) {
				p->brightness = value;
			}
		}
		if (key) {
			const auto name = std::format("{:08X}", key);
			::WritePrivateProfileStringA("Brightness", name.c_str(), std::abs(value - 1.0f) < 0.001f ? nullptr : std::format("{:.2f}", value).c_str(),
				kBrightnessIni);
		}
	}

	// The remote draws on the game's UI thread every frame. It must never wait for _lock: the main thread can hold it
	// while it does something that waits for the UI (that froze the game when touching the remote). If the lock is
	// busy, the remote shows what it showed a frame ago.
	RemoteView Manager::View(std::uint32_t a_formID)
	{
		std::unique_lock lock(_lock, std::try_to_lock);
		if (!lock.owns_lock()) {
			std::scoped_lock cache(_viewLock);
			if (auto it = _viewCache.find(a_formID); it != _viewCache.end()) {
				return it->second;
			}
			RemoteView v;
			v.formID = a_formID;
			return v;
		}
		auto v = ViewLocked(a_formID);
		std::scoped_lock cache(_viewLock);
		_viewCache[a_formID] = v;
		return v;
	}

	RemoteView Manager::ViewLocked(std::uint32_t a_formID)
	{
		RemoteView v;
		v.formID = a_formID;
		v.volume = Settings::Get().startVolume;
		if (auto it = _prefs.find(PrefKeyLocked(a_formID)); it != _prefs.end()) {
			if (it->second.volume >= 0.0f) {
				v.volume = it->second.volume;
			}
			v.muted = it->second.muted;
			v.brightness = it->second.brightness;
		}
		for (auto& p : _players) {
			if (Is(*p, a_formID) && !p->stopping) {
				v.name = p->name;
				v.playing = true;
				v.paused = p->userPaused;
				v.brightenable = p->brightenable;
				if (p->media) {
					v.duration = static_cast<float>(p->media->Duration() / 1e7);
					v.position = std::clamp(static_cast<float>(p->media->Clock() / 1e7), 0.0f, std::max(v.duration, 0.0f));
				}
				v.testPattern = p->kind == SourceKind::kTestPattern;
				v.file = Narrow(std::filesystem::path(p->file).filename().wstring());
				{
					std::error_code ec;
					const auto      root = std::filesystem::absolute(std::filesystem::path(Settings::Get().videoFolder), ec);
					v.folder = Narrow(std::filesystem::path(p->file).lexically_relative(root).parent_path().wstring());
				}
			}
		}
		return v;
	}

	std::vector<RemoteView> Manager::Views()
	{
		std::unique_lock lock(_lock, std::try_to_lock);
		if (!lock.owns_lock()) {
			std::scoped_lock cache(_viewLock);
			return _viewsCache;
		}
		std::vector<RemoteView> out;
		for (auto& p : _players) {
			if (!p->stopping) {
				out.push_back(ViewLocked(p->formID));
			}
		}
		std::scoped_lock cache(_viewLock);
		_viewsCache = out;
		return out;
	}

	bool Manager::IsGamePaused() const
	{
		const auto& s = Settings::Get();
		if (s.pauseUnfocused) {
			const HWND window = GameWindow();
			if (window && ::GetForegroundWindow() != window) {
				return true;
			}
		}
		if (!s.pauseInMenus) {
			return false;
		}
		auto* ui = RE::UI::GetSingleton();
		if (!ui) {
			return false;
		}
		if (ui->menuMode != 0 || ui->freezeFramePause != 0) {
			return true;  // any menu that pauses the game (Esc, Pip-Boy, console, containers...)
		}
		static const std::array<RE::BSFixedString, 5> menus{
			RE::BSFixedString("PauseMenu"), RE::BSFixedString("PipboyMenu"), RE::BSFixedString("LoadingMenu"), RE::BSFixedString("MainMenu"),
			RE::BSFixedString("WorkshopMenu")  // build mode: the video is hidden there too (see Maintain)
		};
		for (const auto& name : menus) {
			if (ui->GetMenuOpen(name)) {
				return true;
			}
		}
		return false;
	}

	// a_mode: 0 = next mesh of this object, 1 = next orientation, 2 = next screen of a nearby object, 3 = other side
	void Manager::HotkeyNextMesh(int a_mode)
	{
		auto ref = PickedRef();
		if (!ref) {
			Notify("Real Videos: aim at a screen first");
			return;
		}
		FixScreen(ref.get(), a_mode);
	}

	void Manager::FixScreen(RE::TESObjectREFR* a_target, int a_mode)
	{
		const bool a_flip = a_mode == 1 || a_mode == 3;
		RE::NiPointer<RE::TESObjectREFR> ref(a_target);
		if (!ref) {
			return;
		}

		std::string message;
		{
			std::scoped_lock lock(_lock);
			Player*          player = nullptr;
			for (auto& p : _players) {
				if (Is(*p, ref->GetFormID()) && !p->stopping) {
					player = p.get();
				}
			}
			if (!player || !_slots[player->slot].niTexture) {
				message = "Real Videos: start a video on it first";
			} else if (a_mode == 2 && Screen::CountCandidates(ref.get()) <= Screen::CountOwnCandidates(ref.get())) {
				message = "Real Videos: no other screens nearby";
			} else {
				auto&     b = player->binding;
				const int total = std::max(1, Screen::CountCandidates(ref.get()));
				const int own = std::clamp(Screen::CountOwnCandidates(ref.get()), 0, total);
				const int current = b.bound ? b.candidateIndex : -1;
				int       index = std::max(current, 0);
				if (a_mode == 0) {
					// own meshes only: wrap around inside [0, own)
					index = own > 0 ? (current >= 0 && current < own ? (current + 1) % own : 0) : 0;
				} else if (a_mode == 2) {
					// nearby screens only: [own, total)
					if (total > own) {
						index = current >= own && current + 1 < total ? current + 1 : own;
					}
				}
				int flip = b.flip;
				if (a_mode == 1) {
					flip = (b.flip & 8) | ((b.flip + 1) & 7);
				} else if (a_mode == 3) {
					flip = b.flip ^ 8;
				}

				Screen::SaveChoice(ref.get(), { index, {}, flip, {} });
				Screen::Unbind(b);
				if (Screen::Bind(ref.get(), _slots[player->slot].niTexture.get(), b, std::max(index, 0), _glowSlots[player->slot].niTexture.get())) {
					_slots[player->slot].compNeedsDraw = true;
					player->srgb = b.originalWasSRGB;
					player->screenAspect = b.screenAspect;
					Screen::SaveChoice(ref.get(), { b.candidateIndex, b.geometryName, b.flip, b.ownerIsNearby ? b.ownerKey : std::string() });
					static constexpr std::array<const char*, 8> turns{ "as detected", "flipped vertically", "mirrored", "rotated 180",
						"turned 90", "turned 90 + flipped", "turned 90 + mirrored", "turned 270" };
					const std::string flipText = std::format("{}{}", turns[b.flip & 7], (b.flip & 8) ? ", other side" : "");
					auto owner = b.owner.get();
					if (b.ownerIsNearby) {
						message = std::format("Real Videos: screen {}/{} nearby: {} '{}', {}", b.candidateIndex - own + 1, total - own,
							Screen::RefName(owner.get()), b.geometryName, flipText);
					} else {
						message = std::format("Real Videos: mesh {}/{} '{}', {}", b.candidateIndex + 1, own, b.geometryName, flipText);
					}
				} else {
					message = "Real Videos: could not use that mesh";
				}
			}
		}
		Notify(message);
	}

	void Manager::PollHotkeys()
	{
		const auto& s = Settings::Get();
		const HWND  window = GameWindow();
		if (window && ::GetForegroundWindow() != window) {
			return;
		}

		// only the remote has a key now; everything else is on the remote and the "Now playing" page
		const std::array<int, 6> keys{ 0, 0, 0, 0, 0, s.keyRemote };
		for (std::size_t i = 0; i < keys.size(); ++i) {
			if (keys[i] <= 0) {
				continue;
			}
			const bool down = (::GetAsyncKeyState(keys[i]) & 0x8000) != 0;
			if (down && !_keyWasDown[i]) {
				auto*      tasks = F4SE::GetTaskInterface();
				const bool shift = (::GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
				const bool ctrl = (::GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
				if (tasks) {
					tasks->AddTask([this, i, shift, ctrl]() {
						switch (i) {
						case 0: HotkeyPlayNext(); break;
						case 1: HotkeyTestPattern(); break;
						case 2: HotkeyStop(); break;
						case 3: HotkeyDump(); break;
						case 4: HotkeyNextMesh(ctrl && shift ? 3 : ctrl ? 2 : shift ? 1 : 0); break;
						default: Remote::Toggle(); break;
						}
					});
				}
			}
			_keyWasDown[i] = down;
		}
	}

	void Manager::OnPresent(ID3D11Device* a_device, ID3D11DeviceContext* a_context)
	{
		++_frame;
		const auto& settings = Settings::Get();
		if (settings.enableHotkeys) {
			PollHotkeys();
		}
		Remote::Tick();
		if (_frame % 10 == 0) {
			ScheduleMaintain();
		}

		std::scoped_lock lock(_lock);
		if (_players.empty()) {
			return;
		}

		const bool paused = IsGamePaused();
		if (paused != _wasPaused) {
			log::info("game {} -> {} screens", paused ? "paused" : "resumed", paused ? "pausing" : "resuming");
			_wasPaused = paused;
		}
		const auto seconds = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();

		for (auto it = _players.begin(); it != _players.end();) {
			auto& p = **it;
			auto& slot = _slots[p.slot];

			if (p.stopping) {
				slot.Unpatch();
				_glowSlots[p.slot].Unpatch();
				if (p.media) {
					// tear down decoder threads / audio voice off the render thread
					std::thread([media = std::move(p.media)]() mutable { media.reset(); }).detach();
				}
				slot.inUse = false;
				log::info("stopped {} (slot {}, {} frames shown)", p.name, p.slot, p.framesShown);
				it = _players.erase(it);
				continue;
			}
			++it;

			if (!slot.niTexture) {
				continue;
			}
			if (_inBuildMode) {
				// build mode: no video anywhere (the screen shows our dark placeholder), and nothing plays
				slot.Unpatch();
				_glowSlots[p.slot].Unpatch();
				if (p.media) {
					p.media->SetPaused(true);
				}
				continue;
			}
			const bool srgb = settings.srgb < 0 ? p.srgb.load() : settings.srgb > 0;
			const auto& b = p.binding;
			const bool  composite = b.bound && b.composite && Compositor::Get().Ready(a_device);
			const float letterboxAspect = composite ? 0.0f : p.screenAspect.load();

			// 1) get the newest picture into the video texture
			bool          fresh = false;
			std::uint32_t vw = 0, vh = 0;
			const bool held = paused || p.userPaused;  // game pause or remote pause
			if (p.kind == SourceKind::kTestPattern) {
				if (!held || p.pattern.empty()) {
					MakePattern(p.pattern, kPatternSize, kPatternSize, seconds);
					const auto [tw, th] = FittedSize(kPatternSize, kPatternSize, letterboxAspect);
					if (slot.Ensure(a_device, tw, th, srgb)) {
						slot.Upload(a_context, p.pattern.data(), kPatternSize, kPatternSize);
						++p.framesShown;
						fresh = true;
						vw = vh = kPatternSize;
					}
				}
			} else if (p.media) {
				if (const auto jump = p.seekDelta.exchange(0); jump != 0) {
					p.media->Seek(p.media->Clock() + jump);
				}
				// far away / out of view: paused like the remote's pause (picture and sound), carries on from the same spot
				const bool frozen = p.frozen.load();
				p.wasFrozen = frozen;
				p.media->SetPaused(held || frozen);
				p.media->SetVolume(p.volume);
				p.media->SetPan(p.pan.load());
				if (!held && !frozen && p.media->Finished()) {
					if (settings.shuffle && p.kind == SourceKind::kVideo) {
						// shuffle: another random video from the same folder
						if (!p.advancePending) {
							p.advancePending = true;
							if (auto* tasks = F4SE::GetTaskInterface()) {
								tasks->AddTask([this, id = p.formID]() {
									if (auto* ref = RE::TESForm::GetFormByID<RE::TESObjectREFR>(id); ref && ref->Get3D()) {
										RemoteStep(ref, +1);
									}
								});
							}
						}
					} else if (p.loop) {
						p.media->Restart();
					}
				}
				VideoFrame frame;
				if (!frozen && p.media->AcquireFrame(p.media->Clock(), frame) && frame.gpuTexture) {
					// GPU decoding: draw the decoder's shared texture into the slot texture (black bars around it)
					const auto [tw, th] = FittedSize(frame.width, frame.height, letterboxAspect);
					auto&      in = p.gpuIn[frame.gpuShared];
					if (!in.texture && SUCCEEDED(a_device->OpenSharedResource(frame.gpuShared, IID_PPV_ARGS(&in.texture)))) {
						in.texture.As(&in.mutex);
					}
					auto& view = in.srv[srgb ? 1 : 0];
					if (in.texture && !view) {
						D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
						sd.Format = srgb ? DXGI_FORMAT_B8G8R8A8_UNORM_SRGB : DXGI_FORMAT_B8G8R8A8_UNORM;
						sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
						sd.Texture2D.MipLevels = 1;
						a_device->CreateShaderResourceView(in.texture.Get(), &sd, &view);
					}
					// why a GPU frame couldn't be drawn (logged once; after a run of failures: CPU decoding instead)
					const char* miss = nullptr;
					HRESULT     syncHr = S_OK;
					if (!in.texture) {
						miss = "the game can't open the decoder's shared texture";
					} else if (!in.mutex) {
						miss = "the shared texture has no keyed mutex";
					} else if (!view) {
						miss = "couldn't make a view of the shared texture";
					} else if (!Compositor::Get().Ready(a_device)) {
						miss = "the compositor isn't available";
					} else if (!slot.Ensure(a_device, tw, th, srgb, true)) {
						miss = "couldn't make the screen's texture";
					} else if ((syncHr = in.mutex->AcquireSync(1, 0)) != S_OK) {
						miss = "the decoder's picture wasn't ready to read";
					}
					if (miss) {
						++p.gpuMisses;
						if (!p.gpuMissLogged) {
							p.gpuMissLogged = true;
							log::warn("{}: GPU frame not drawn: {} (0x{:08X})", p.name, miss, static_cast<std::uint32_t>(syncHr));
						}
						if (p.gpuMisses >= 30) {
							p.media->RecycleFrame(std::move(frame));
							FallBackToCpuLocked(p, miss);
							continue;
						}
					}
					if (!miss) {
						p.gpuMisses = 0;
						const float offX = static_cast<float>((tw - frame.width) / 2), offY = static_cast<float>((th - frame.height) / 2);
						const Compositor::Region where{ offX, offY, offX + static_cast<float>(frame.width), offY + static_cast<float>(frame.height) };
						Compositor::Get().Compose(a_context, slot.rtv.Get(), slot.width, slot.height, nullptr, view.Get(),
							std::span<const Compositor::Region>(&where, 1));
						in.mutex->ReleaseSync(0);
						frame.gpuKey = 0;
						if (p.framesShown++ == 0) {
							log::info("{}: first video frame on screen ({}x{}, GPU decoding)", p.name, frame.width, frame.height);
						}
						fresh = true;
						vw = frame.width;
						vh = frame.height;
					}
					p.media->RecycleFrame(std::move(frame));
				} else if (frame.width && !frame.gpuTexture) {
					const auto [tw, th] = FittedSize(frame.width, frame.height, letterboxAspect);
					if (slot.Ensure(a_device, tw, th, srgb)) {
						slot.Upload(a_context, frame.pixels.data(), frame.width, frame.height);
						if (p.framesShown++ == 0) {
							log::info("{}: first video frame on screen ({}x{})", p.name, frame.width, frame.height);
						}
						fresh = true;
						vw = frame.width;
						vh = frame.height;
					}
					p.media->RecycleFrame(std::move(frame));
				}
				// nothing on screen 5 s after the video was ready (and it isn't paused): say why we know of, and if the
				// graphics card path is in use, switch this and later videos to CPU decoding
				if (p.media && p.framesShown == 0 && !held && !frozen && !p.noPictureLogged &&
					std::chrono::steady_clock::now() - p.mediaSince > std::chrono::seconds(5)) {
					p.noPictureLogged = true;
					log::warn("{}: no picture 5 s after the video was ready ({} decoding, {} GPU frames not drawn)", p.name,
						p.media->OnGpu() ? "GPU" : "CPU", p.gpuMisses);
					if (p.media->OnGpu()) {
						FallBackToCpuLocked(p, p.gpuMisses ? "its frames couldn't be drawn" : "the decoder sent no frames");
					}
				}
			}
			if (vw) {
				p.lastVideoWidth = vw;
				p.lastVideoHeight = vh;
			}

			// brightness: the screen's own (remote, up to 100%) times the cinema's, painted into the picture on every
			// screen. (Changing a glowing shader's strength on the fly didn't take on the projectors.) The glow keeps
			// the global "Projector glow brightness".
			const float screenBright = p.brightenable ? p.brightness.load() : 1.0f;
			const bool  glowing = b.bound && b.selfLit;
			if (glowing || (b.bound && b.glowMap)) {
				Screen::SetGlow(b, 1.0f);
			}
			const float pictureBright = (b.bigScreen ? settings.bigScreenBrightness : 1.0f) * screenBright;
			const float pictureFx[4]{ pictureBright, 0.0f, static_cast<float>(std::fmod(seconds, 3600.0)), 0.0f };
			// old film look: the cinema's own, projectors their own, TVs none
			const Settings::Film* filmSet = b.bound && !b.classic ? (b.bigScreen ? &settings.cinemaFilm : &settings.projectorFilm) : nullptr;
			float                 film[8]{};
			const bool            filmOn = filmSet && filmSet->Any();
			if (filmOn) {
				filmSet->Pack(film);
			}
			// the glowing projector's own texture (back, frame, pole) dimmed to the world's light
			const float frameScale = glowing && !b.glowOverlay ? _frameLight.load() : 1.0f;
			const bool  brightChanged = std::abs(pictureBright - p.lastPictureBrightness) > 0.0001f || std::abs(frameScale - p.lastFrameScale) > 0.01f;
			p.lastPictureBrightness = pictureBright;
			if (std::abs(frameScale - p.lastFrameScale) > 0.01f) {
				static auto lastLog = std::chrono::steady_clock::time_point{};
				if (std::chrono::steady_clock::now() - lastLog > std::chrono::seconds(3)) {
					lastLog = std::chrono::steady_clock::now();
					log::info("{}: glowing={} copy={} composite={} frame brightness {:.0f}% -> {:.0f}%", p.name, glowing, b.glowOverlay, composite,
						p.lastFrameScale * 100.0f, frameScale * 100.0f);
				}
				p.lastFrameScale = frameScale;
			}

			// 2) shared texture: paint the screen's own texture, then the video only inside the screen face
			ID3D11ShaderResourceView* want = slot.srv.Get();
			if (composite) {
				auto*                     base = b.originalBase.get();
				auto*                     baseSRV = base && base->rendererTexture ? reinterpret_cast<ID3D11ShaderResourceView*>(base->rendererTexture->srView) : nullptr;
				std::uint32_t             bw = 1024, bh = 1024;
				BaseSize(baseSRV, bw, bh);
				// enough resolution that the face holds the video at roughly its own size
				const float faceW = std::max(b.faceRects[0][2] - b.faceRects[0][0], 0.01f) * bw;
				std::uint32_t scale = 1;
				while (scale < 4 && faceW * scale < p.lastVideoWidth * 0.75f && bw * scale * 2 <= 4096 && bh * scale * 2 <= 4096) {
					scale *= 2;
				}
				if (slot.srv && slot.EnsureComposite(a_device, bw * scale, bh * scale, srgb)) {
					if (fresh || slot.compNeedsDraw || brightChanged || (filmOn && !held && !p.frozen.load())) {
						slot.compNeedsDraw = false;
						std::array<Compositor::Region, 2> regions{};
						for (int i = 0; i < b.faceCount && i < 2; ++i) {
							regions[i] = FaceRegion(b.faceRects[i], slot.compWidth, slot.compHeight, b.faceAspect[i], p.lastVideoWidth, p.lastVideoHeight, b.faceOrient[i]);
						}
						const std::span<const Compositor::Region> faces(regions.data(), static_cast<std::size_t>(std::clamp(b.faceCount, 0, 2)));
						if (b.glowMap) {
							// glow map: the lit texture gets a black picture (the light comes from the glow texture only),
							// the glow texture is black everywhere but the picture
							const float darkFx[4]{ 0.0f, 0.0f, 0.0f, 0.0f };
							Compositor::Get().Compose(a_context, slot.compRTV.Get(), slot.compWidth, slot.compHeight, baseSRV, slot.srv.Get(), faces, darkFx);
							auto& glow = _glowSlots[p.slot];
							if (glow.niTexture && glow.EnsureComposite(a_device, slot.compWidth, slot.compHeight, srgb)) {
								Compositor::Get().Compose(a_context, glow.compRTV.Get(), glow.compWidth, glow.compHeight, nullptr, slot.srv.Get(), faces,
									pictureFx, false, 1.0f, filmOn ? film : nullptr);
							}
						} else {
							Compositor::Get().Compose(a_context, slot.compRTV.Get(), slot.compWidth, slot.compHeight, baseSRV, slot.srv.Get(), faces,
								pictureFx, b.glowOverlay, frameScale, filmOn ? film : nullptr);
						}
					}
					if (b.glowMap) {
						auto& glow = _glowSlots[p.slot];
						if (glow.compSRV) {
							glow.Patch(p.slot + kMaxSlots, glow.compSRV.Get());
						}
					}
					want = slot.compSRV.Get();
					if (!p.compositeLogged) {
						p.compositeLogged = true;
						log::info("{}: drawing video only inside the screen face ({}x{} texture, base {}; glowing={} copy={} frame brightness {:.0f}%)", p.name,
							slot.compWidth, slot.compHeight, baseSRV ? "original" : "black", glowing, b.glowOverlay, frameScale * 100.0f);
					}
				}
			}

			// 3) big screens (the drive-in): brighter, with the VHS tape look, redrawn every frame (the tape noise moves)
			//    and lit projector screens given their own brightness from the remote
			const bool filtered = !composite && b.bound && !b.classic && (pictureBright != 1.0f || filmOn) && Compositor::Get().Ready(a_device);
			if (filtered && p.frozen.load() && slot.compSRV) {
				want = slot.compSRV.Get();  // frozen: keep the last filtered picture, nothing redrawn
			} else if (filtered && slot.srv && slot.width && slot.height && slot.EnsureComposite(a_device, slot.width, slot.height, srgb)) {
				const float fx[4]{ pictureBright, 0.0f, static_cast<float>(std::fmod(seconds, 3600.0)), 0.0f };
				const Compositor::Region whole{ 0.0f, 0.0f, static_cast<float>(slot.compWidth), static_cast<float>(slot.compHeight) };
				Compositor::Get().Compose(a_context, slot.compRTV.Get(), slot.compWidth, slot.compHeight, nullptr, slot.srv.Get(),
					std::span<const Compositor::Region>(&whole, 1), fx, false, 1.0f, filmOn ? film : nullptr);
				want = slot.compSRV.Get();
			}

			slot.Patch(p.slot, want);
			Screen::ReapplyUV(p.binding);
		}
	}

	// ---------------- save games ----------------
	//
	// Record 'SCRN' v1: count, then per screen: formID, workshop formID, kind, loop, paused, position, where (x, y, z),
	// file (length + UTF-16). Record 'PREF' v1: count, then per screen: formID, volume, muted.
	// Loaded screens start out "asleep" and come on by the same rules as screens you walked away from.

	namespace
	{
		constexpr std::uint32_t kScreensRecord = 'SCRN';
		constexpr std::uint32_t kPrefsRecord = 'PREF';

		void WriteString(const F4SE::SerializationInterface* a_intfc, const std::wstring& a_s)
		{
			const auto n = static_cast<std::uint32_t>(a_s.size());
			a_intfc->WriteRecordData(n);
			if (n) {
				a_intfc->WriteRecordData(a_s.data(), n * sizeof(wchar_t));
			}
		}

		bool ReadString(const F4SE::SerializationInterface* a_intfc, std::wstring& a_s)
		{
			std::uint32_t n = 0;
			if (a_intfc->ReadRecordData(n) != sizeof(n) || n > 4096) {
				return false;
			}
			a_s.resize(n);
			return n == 0 || a_intfc->ReadRecordData(a_s.data(), n * sizeof(wchar_t)) == n * sizeof(wchar_t);
		}
	}

	void Manager::SaveScreens(const F4SE::SerializationInterface* a_intfc)
	{
		std::scoped_lock lock(_lock);
		struct Entry
		{
			std::uint32_t formID, workshop;
			std::uint8_t  kind, loop, paused;
			std::int64_t  position;
			RE::NiPoint3  where;
			std::wstring  file;
		};
		std::vector<Entry> entries;
		for (auto& p : _players) {
			if (p->stopping) {
				continue;
			}
			auto  ref = p->ref.get();
			auto* g = p->binding.bound ? p->binding.geometry.get() : nullptr;
			auto  ws = p->workshop.get();
			entries.push_back({ p->formID, ws ? ws->GetFormID() : 0u, static_cast<std::uint8_t>(p->kind), p->loop, p->userPaused.load(),
				p->media ? p->media->Clock() : 0, g ? g->worldBound.center : ref ? ref->GetPosition() : RE::NiPoint3{}, p->file });
		}
		for (auto& s : _sleepers) {
			auto ws = s.workshop.get();
			entries.push_back({ s.formID, ws ? ws->GetFormID() : 0u, static_cast<std::uint8_t>(s.kind), s.loop, s.paused, s.position, s.where, s.file });
		}
		if (a_intfc->OpenRecord(kScreensRecord, 1)) {
			a_intfc->WriteRecordData(static_cast<std::uint32_t>(entries.size()));
			for (const auto& e : entries) {
				a_intfc->WriteRecordData(e.formID);
				a_intfc->WriteRecordData(e.workshop);
				a_intfc->WriteRecordData(e.kind);
				a_intfc->WriteRecordData(e.loop);
				a_intfc->WriteRecordData(e.paused);
				a_intfc->WriteRecordData(e.position);
				a_intfc->WriteRecordData(e.where.x);
				a_intfc->WriteRecordData(e.where.y);
				a_intfc->WriteRecordData(e.where.z);
				WriteString(a_intfc, e.file);
			}
		}
		if (a_intfc->OpenRecord(kPrefsRecord, 1)) {
			a_intfc->WriteRecordData(static_cast<std::uint32_t>(_prefs.size()));
			for (const auto& [key, pr] : _prefs) {
				a_intfc->WriteRecordData(key);
				a_intfc->WriteRecordData(pr.volume);
				a_intfc->WriteRecordData(static_cast<std::uint8_t>(pr.muted));
			}
		}
		log::info("saved {} screen{} with the game", entries.size(), entries.size() == 1 ? "" : "s");
	}

	void Manager::LoadScreens(const F4SE::SerializationInterface* a_intfc)
	{
		std::scoped_lock lock(_lock);
		std::uint32_t type = 0, version = 0, length = 0;
		std::size_t   restored = 0;
		while (a_intfc->GetNextRecordInfo(type, version, length)) {
			if (type == kScreensRecord && version == 1) {
				std::uint32_t count = 0;
				a_intfc->ReadRecordData(count);
				for (std::uint32_t i = 0; i < count && i < 256; ++i) {
					std::uint32_t formID = 0, workshop = 0;
					std::uint8_t  kind = 0, loop = 1, paused = 0;
					std::int64_t  position = 0;
					RE::NiPoint3  where{};
					std::wstring  file;
					a_intfc->ReadRecordData(formID);
					a_intfc->ReadRecordData(workshop);
					a_intfc->ReadRecordData(kind);
					a_intfc->ReadRecordData(loop);
					a_intfc->ReadRecordData(paused);
					a_intfc->ReadRecordData(position);
					a_intfc->ReadRecordData(where.x);
					a_intfc->ReadRecordData(where.y);
					a_intfc->ReadRecordData(where.z);
					if (!ReadString(a_intfc, file)) {
						break;
					}
					const auto id = a_intfc->ResolveFormID(formID);
					auto*      ref = id ? RE::TESForm::GetFormByID<RE::TESObjectREFR>(*id) : nullptr;
					if (!ref) {
						continue;  // the object is gone (scrapped, or its mod removed)
					}
					Sleeper s;
					s.ref = ref->GetHandle();
					s.formID = ref->GetFormID();
					s.kind = kind == static_cast<std::uint8_t>(SourceKind::kTestPattern) ? SourceKind::kTestPattern : SourceKind::kVideo;
					s.file = file;
					s.loop = loop != 0;
					s.paused = paused != 0;
					s.position = position;
					s.where = where;
					s.wakeDistance = std::max(Settings::Get().autoOffDistance, Settings::Get().cinemaAutoOffDistance) * 0.9f;
					s.name = Screen::RefName(ref);
					if (workshop) {
						if (const auto ws = a_intfc->ResolveFormID(workshop)) {
							if (auto* wsRef = RE::TESForm::GetFormByID<RE::TESObjectREFR>(*ws)) {
								s.workshop = wsRef->GetHandle();
							}
						}
					}
					_sleepers.push_back(std::move(s));
					++restored;
				}
			} else if (type == kPrefsRecord && version == 1) {
				std::uint32_t count = 0;
				a_intfc->ReadRecordData(count);
				for (std::uint32_t i = 0; i < count && i < 4096; ++i) {
					std::uint32_t key = 0;
					float         volume = -1.0f;
					std::uint8_t  muted = 0;
					a_intfc->ReadRecordData(key);
					a_intfc->ReadRecordData(volume);
					a_intfc->ReadRecordData(muted);
					if (const auto id = a_intfc->ResolveFormID(key)) {
						auto& pr = _prefs[*id];
						pr.volume = volume;
						pr.muted = muted != 0;
					}
				}
			}
		}
		log::info("save game: {} screen{} will come back on as you get near them", restored, restored == 1 ? "" : "s");
	}

	void Manager::ForgetScreens()
	{
		std::scoped_lock lock(_lock);
		_sleepers.clear();
		for (auto& [key, pr] : _prefs) {
			pr.volume = -1.0f;
			pr.muted = false;
		}
	}
}

