#include "Manager.h"

#include "Compositor.h"
#include "Remote.h"
#include "Settings.h"

namespace fv
{
	namespace
	{
		constexpr std::uint32_t kPatternSize = 256;

		void Notify(const std::string& a_message)
		{
			log::info("{}", a_message);
			RE::SendHUDMessage::ShowHUDMessage(a_message.c_str(), nullptr, false, false);
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

	bool Slot::Ensure(ID3D11Device* a_device, std::uint32_t a_width, std::uint32_t a_height, bool a_srgb)
	{
		if (texture && width == a_width && height == a_height && srgb == a_srgb) {
			return true;
		}
		Unpatch();
		srv.Reset();
		texture.Reset();

		D3D11_TEXTURE2D_DESC desc{};
		desc.Width = a_width;
		desc.Height = a_height;
		desc.MipLevels = 1;
		desc.ArraySize = 1;
		desc.Format = a_srgb ? DXGI_FORMAT_B8G8R8X8_UNORM_SRGB : DXGI_FORMAT_B8G8R8X8_UNORM;
		desc.SampleDesc.Count = 1;
		desc.Usage = D3D11_USAGE_DYNAMIC;
		desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

		HRESULT hr = a_device->CreateTexture2D(&desc, nullptr, &texture);
		if (FAILED(hr)) {
			log::error("CreateTexture2D {}x{} failed (0x{:08X})", a_width, a_height, static_cast<std::uint32_t>(hr));
			return false;
		}
		hr = a_device->CreateShaderResourceView(texture.Get(), nullptr, &srv);
		if (FAILED(hr)) {
			log::error("CreateShaderResourceView failed (0x{:08X})", static_cast<std::uint32_t>(hr));
			texture.Reset();
			return false;
		}
		width = a_width;
		height = a_height;
		srgb = a_srgb;
		log::info("slot texture {}x{} created (sRGB={})", a_width, a_height, a_srgb);
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

		std::vector<std::wstring> out;
		std::error_code           ec;
		for (const auto& entry : fs::directory_iterator(fs::path(Settings::Get().videoFolder), ec)) {
			if (!entry.is_regular_file(ec)) {
				continue;
			}
			auto ext = entry.path().extension().wstring();
			for (auto& c : ext) {
				c = static_cast<wchar_t>(::towlower(c));
			}
			if (std::ranges::find(exts, std::wstring_view(ext)) != exts.end()) {
				out.push_back(entry.path().filename().wstring());
			}
		}
		std::ranges::sort(out);
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

	bool Manager::Start(RE::TESObjectREFR* a_ref, SourceKind a_kind, const std::wstring& a_file, bool a_loop)
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

		if (a_kind == SourceKind::kVideo) {
			if (!std::filesystem::exists(a_file)) {
				Notify(std::format("Real Videos: file not found: {}", Narrow(a_file)));
				releaseSlot();
				return false;
			}
			player->media = std::make_unique<MediaSource>();
			if (!player->media->Open(a_file, Settings::Get().enableAudio)) {
				Notify(std::format("Real Videos: could not decode {}", Narrow(std::filesystem::path(a_file).filename().wstring())));
				releaseSlot();
				return false;
			}
		}

		if (!Screen::Bind(a_ref, slotTexture.get(), player->binding)) {
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
			if (clash && !Screen::Bind(a_ref, slotTexture.get(), player->binding)) {
				Notify(std::format("Real Videos: no screen found on {} (see RealVideos.log)", refName));
				releaseSlot();
				return false;
			}
		}
		player->screenID = ScreenOf(player->binding);
		player->srgb = player->binding.originalWasSRGB;
		player->screenAspect = player->binding.screenAspect;

		Player* raw = player.get();
		{
			std::scoped_lock lock(_lock);
			_slots[slot].niTexture = slotTexture;
			_slots[slot].refusedLogged = false;
			_players.push_back(std::move(player));
		}
		if (raw->media) {
			raw->media->Start();
		}

		if (a_kind == SourceKind::kVideo) {
			Notify(std::format("Real Videos: playing {} on {}", Narrow(std::filesystem::path(a_file).filename().wstring()), refName));
		} else {
			Notify(std::format("Real Videos: test pattern on {}", refName));
		}
		return true;
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
			const float target = std::clamp((ambient + 0.5f * direct) * settings.frameLight, 0.0f, 1.0f);
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
						if (Screen::Bind(ref.get(), _slots[p->slot].niTexture.get(), p->binding)) {
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

			float volume = 0.0f;
			if (p->binding.bound && player) {
				// big screens (the drive-in) carry much further, measured from the screen itself
				auto*      geom = p->binding.geometry.get();
				const bool big = geom && settings.bigScreenRadius > 0.0f && geom->worldBound.fRadius >= settings.bigScreenRadius;
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
			}
			p->volume = volume;
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
		log::info("{}: {}; F9/F10 ignored (bOnlySupportedScreens=1)", Screen::RefName(a_ref), what);
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
			Notify(std::format("Real Videos: stopped {}", Screen::RefName(ref.get())));
		} else {
			StopAll();
			Notify("Real Videos: stopped all screens");
		}
	}

	void Manager::HotkeyDump()
	{
		auto ref = PickedRef();
		if (!ref) {
			Notify("Real Videos: aim at a TV, projector or cinema screen first");
			return;
		}
		Screen::Dump(ref.get());
		Notify(std::format("Real Videos: node dump of {} written to RealVideos.log", Screen::RefName(ref.get())));
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
					Notify(std::format("Real Videos: {} {}", p->userPaused ? "paused" : "playing", p->name));
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
		const auto videos = ListVideos();
		if (videos.empty()) {
			Notify(std::format("Real Videos: no videos in {}", Narrow(Settings::Get().videoFolder)));
			return;
		}
		// position of what's playing on this screen, else the shared "next video" counter
		std::ptrdiff_t index = static_cast<std::ptrdiff_t>(_nextVideo % videos.size());
		bool           playing = false;
		{
			std::scoped_lock lock(_lock);
			for (auto& p : _players) {
				if (Is(*p, a_ref->GetFormID()) && !p->stopping && p->kind == SourceKind::kVideo) {
					const auto current = std::filesystem::path(p->file).filename().wstring();
					for (std::size_t i = 0; i < videos.size(); ++i) {
						if (_wcsicmp(std::filesystem::path(videos[i]).filename().wstring().c_str(), current.c_str()) == 0) {
							index = static_cast<std::ptrdiff_t>(i);
							playing = true;
						}
					}
				}
			}
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
		const auto videos = ListVideos();
		if (videos.empty()) {
			Notify(std::format("Real Videos: no videos in {}", Narrow(Settings::Get().videoFolder)));
			return;
		}
		Play(a_ref, videos.front(), Settings::Get().loop);
	}

	void Manager::RemotePower(RE::TESObjectREFR* a_ref)
	{
		if (!a_ref) {
			return;
		}
		if (IsPlaying(a_ref)) {
			Stop(a_ref);
			Notify(std::format("Real Videos: {} off", Screen::RefName(a_ref)));
		} else {
			RemoteStep(a_ref, 0);
		}
	}

	void Manager::RemoteSeek(std::uint32_t a_formID, float a_seconds)
	{
		std::scoped_lock lock(_lock);
		for (auto& p : _players) {
			if (Is(*p, a_formID) && !p->stopping) {
				p->seekDelta += static_cast<std::int64_t>(a_seconds * 10'000'000.0f);
			}
		}
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

	RemoteView Manager::View(std::uint32_t a_formID)
	{
		std::scoped_lock lock(_lock);
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
				v.testPattern = p->kind == SourceKind::kTestPattern;
				v.file = Narrow(std::filesystem::path(p->file).filename().wstring());
			}
		}
		return v;
	}

	std::vector<RemoteView> Manager::Views()
	{
		std::vector<std::uint32_t> ids;
		{
			std::scoped_lock lock(_lock);
			for (auto& p : _players) {
				if (!p->stopping) {
					ids.push_back(p->formID);
				}
			}
		}
		std::vector<RemoteView> out;
		for (auto id : ids) {
			out.push_back(View(id));
		}
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
		const bool a_flip = a_mode == 1 || a_mode == 3;
		auto ref = PickedRef();
		if (!ref) {
			Notify("Real Videos: aim at a screen first");
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
				message = "Real Videos: play something on it first (F10 or F9)";
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
				if (Screen::Bind(ref.get(), _slots[player->slot].niTexture.get(), b, std::max(index, 0))) {
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

		const std::array<int, 6> keys{ s.keyPlay, s.keyTestPattern, s.keyStop, s.keyDump, s.keyNextMesh, s.keyRemote };
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
				p.media->SetPaused(held);
				p.media->SetVolume(p.volume);
				if (!held && p.loop && p.media->Finished()) {
					p.media->Restart();
				}
				VideoFrame frame;
				if (p.media->AcquireFrame(p.media->Clock(), frame)) {
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
			if (glowing) {
				Screen::SetGlow(b, b.bigScreen ? 1.0f : settings.selfLitBrightness);
			}
			const float pictureBright = (b.bigScreen ? settings.bigScreenBrightness : 1.0f) * screenBright;
			const float pictureFx[4]{ pictureBright, 0.0f, static_cast<float>(std::fmod(seconds, 3600.0)), 0.0f };
			// the glowing projector's own texture (back, frame, pole) dimmed to the world's light
			const float frameScale = glowing && !b.glowOverlay ? _frameLight.load() : 1.0f;
			const bool  brightChanged = std::abs(pictureBright - p.lastPictureBrightness) > 0.0001f || std::abs(frameScale - p.lastFrameScale) > 0.01f;
			p.lastPictureBrightness = pictureBright;
			if (std::abs(frameScale - p.lastFrameScale) > 0.01f) {
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
					if (fresh || slot.compNeedsDraw || brightChanged) {
						slot.compNeedsDraw = false;
						std::array<Compositor::Region, 2> regions{};
						for (int i = 0; i < b.faceCount && i < 2; ++i) {
							regions[i] = FaceRegion(b.faceRects[i], slot.compWidth, slot.compHeight, b.faceAspect[i], p.lastVideoWidth, p.lastVideoHeight, b.faceOrient[i]);
						}
						Compositor::Get().Compose(a_context, slot.compRTV.Get(), slot.compWidth, slot.compHeight, baseSRV, slot.srv.Get(),
							std::span<const Compositor::Region>(regions.data(), static_cast<std::size_t>(std::clamp(b.faceCount, 0, 2))),
							pictureBright != 1.0f ? pictureFx : nullptr, b.glowOverlay, frameScale);
					}
					want = slot.compSRV.Get();
					if (!p.compositeLogged) {
						p.compositeLogged = true;
						log::info("{}: drawing video only inside the screen face ({}x{} texture, base {})", p.name, slot.compWidth, slot.compHeight,
							baseSRV ? "original" : "black");
					}
				}
			}

			// 3) big screens (the drive-in): brighter, with the VHS tape look, redrawn every frame (the tape noise moves)
			//    and lit projector screens given their own brightness from the remote
			const float vhs = b.bigScreen ? settings.bigScreenVHS : 0.0f;
			const bool filtered = !composite && b.bound && !b.classic && (pictureBright != 1.0f || vhs > 0.0f) && Compositor::Get().Ready(a_device);
			if (filtered && slot.srv && slot.width && slot.height && slot.EnsureComposite(a_device, slot.width, slot.height, srgb)) {
				const float fx[4]{ pictureBright, vhs, static_cast<float>(std::fmod(seconds, 3600.0)), 0.0f };
				const Compositor::Region whole{ 0.0f, 0.0f, static_cast<float>(slot.compWidth), static_cast<float>(slot.compHeight) };
				Compositor::Get().Compose(a_context, slot.compRTV.Get(), slot.compWidth, slot.compHeight, nullptr, slot.srv.Get(),
					std::span<const Compositor::Region>(&whole, 1), fx);
				want = slot.compSRV.Get();
			}

			slot.Patch(p.slot, want);
			Screen::ReapplyUV(p.binding);
		}
	}
}
