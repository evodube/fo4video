#include "Manager.h"

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

		RE::NiPointer<RE::TESObjectREFR> PickedRef()
		{
			return RE::Console::GetPickRef().get();
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

		float Attenuate(float a_distance)
		{
			const auto& s = Settings::Get();
			if (a_distance <= s.minDistance) {
				return 1.0f;
			}
			if (a_distance >= s.maxDistance) {
				return 0.0f;
			}
			const float t = 1.0f - (a_distance - s.minDistance) / (s.maxDistance - s.minDistance);
			return t * t;
		}

		HWND GameWindow()
		{
			auto* data = RE::BSGraphics::GetRendererData();
			return data ? reinterpret_cast<HWND>(data->renderWindow[0].hwnd) : nullptr;
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

	void Slot::Upload(ID3D11DeviceContext* a_context, const std::uint8_t* a_pixels, std::uint32_t a_width, std::uint32_t a_height)
	{
		if (!texture || a_width != width || a_height != height) {
			return;
		}
		D3D11_MAPPED_SUBRESOURCE mapped{};
		if (FAILED(a_context->Map(texture.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
			return;
		}
		const std::size_t rowBytes = static_cast<std::size_t>(a_width) * 4;
		auto*             dst = static_cast<std::uint8_t*>(mapped.pData);
		if (mapped.RowPitch == rowBytes) {
			std::memcpy(dst, a_pixels, rowBytes * a_height);
		} else {
			for (std::uint32_t y = 0; y < a_height; ++y) {
				std::memcpy(dst + static_cast<std::size_t>(y) * mapped.RowPitch, a_pixels + y * rowBytes, rowBytes);
			}
		}
		a_context->Unmap(texture.Get(), 0);
	}

	void Slot::Patch(int a_index)
	{
		auto* tex = niTexture.get();
		if (!tex || !srv) {
			return;
		}
		auto* rt = tex->rendererTexture;
		if (!rt) {
			return;
		}
		auto* cur = reinterpret_cast<ID3D11ShaderResourceView*>(rt->srView);
		auto* want = srv.Get();

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
		width = height = 0;
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
				if (p->formID == a_ref->GetFormID()) {
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
		player->srgb = player->binding.originalWasSRGB;

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
			if (p->formID == a_ref->GetFormID()) {
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
			if (p->formID == a_ref->GetFormID() && !p->stopping) {
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
			if (p->formID == a_ref->GetFormID()) {
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

		for (auto& p : _players) {
			if (p->stopping) {
				continue;
			}
			auto ref = p->ref.get();
			if (!ref) {
				log::info("{} no longer exists; stopping", p->name);
				StopLocked(*p);
				continue;
			}

			auto* root = ref->Get3D();
			const bool valid = Screen::StillValid(p->binding, root);
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
							p->srgb = p->binding.originalWasSRGB;
						}
					}
					p->rebindCooldown = 6;
				}
			}

			float volume = 0.0f;
			if (p->binding.bound && player) {
				const auto pos = ref->GetPosition();
				const float dx = pos.x - playerPos.x;
				const float dy = pos.y - playerPos.y;
				const float dz = pos.z - playerPos.z;
				volume = settings.volume * p->userVolume * Attenuate(std::sqrt(dx * dx + dy * dy + dz * dz));
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

	void Manager::HotkeyPlayNext()
	{
		auto ref = PickedRef();
		if (!ref) {
			Notify("Real Videos: open the console and click a TV first");
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
			Notify("Real Videos: open the console and click a TV first");
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
			Notify("Real Videos: open the console and click a TV first");
			return;
		}
		Screen::Dump(ref.get());
		Notify(std::format("Real Videos: node dump of {} written to RealVideos.log", Screen::RefName(ref.get())));
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
		static const std::array<RE::BSFixedString, 4> menus{
			RE::BSFixedString("PauseMenu"), RE::BSFixedString("PipboyMenu"), RE::BSFixedString("LoadingMenu"), RE::BSFixedString("MainMenu")
		};
		for (const auto& name : menus) {
			if (ui->GetMenuOpen(name)) {
				return true;
			}
		}
		return false;
	}

	void Manager::HotkeyNextMesh(bool a_flip)
	{
		auto ref = PickedRef();
		if (!ref) {
			Notify("Real Videos: open the console and click a screen first");
			return;
		}

		std::string message;
		{
			std::scoped_lock lock(_lock);
			Player*          player = nullptr;
			for (auto& p : _players) {
				if (p->formID == ref->GetFormID() && !p->stopping) {
					player = p.get();
				}
			}
			if (!player || !_slots[player->slot].niTexture) {
				message = "Real Videos: play something on it first (F10 or F9)";
			} else {
				auto&     b = player->binding;
				const int count = std::max(1, b.bound ? b.candidateCount : Screen::CountCandidates(ref.get()));
				const int index = ((b.bound ? b.candidateIndex : -1) + (a_flip ? 0 : 1) + count) % count;
				const int flip = a_flip ? (b.flip + 1) % 4 : b.flip;

				Screen::SaveChoice(ref.get(), { index, {}, flip });
				Screen::Unbind(b);
				if (Screen::Bind(ref.get(), _slots[player->slot].niTexture.get(), b, std::max(index, 0))) {
					player->srgb = b.originalWasSRGB;
					Screen::SaveChoice(ref.get(), { b.candidateIndex, b.geometryName, b.flip });
					static constexpr std::array<const char*, 4> flips{ "normal", "flipped vertically", "mirrored", "rotated 180" };
					message = std::format("Real Videos: mesh {}/{} '{}', {}", b.candidateIndex + 1, b.candidateCount, b.geometryName, flips[b.flip & 3]);
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

		const std::array<int, 5> keys{ s.keyPlay, s.keyTestPattern, s.keyStop, s.keyDump, s.keyNextMesh };
		for (std::size_t i = 0; i < keys.size(); ++i) {
			if (keys[i] <= 0) {
				continue;
			}
			const bool down = (::GetAsyncKeyState(keys[i]) & 0x8000) != 0;
			if (down && !_keyWasDown[i]) {
				auto*      tasks = F4SE::GetTaskInterface();
				const bool shift = (::GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
				if (tasks) {
					tasks->AddTask([this, i, shift]() {
						switch (i) {
						case 0: HotkeyPlayNext(); break;
						case 1: HotkeyTestPattern(); break;
						case 2: HotkeyStop(); break;
						case 3: HotkeyDump(); break;
						default: HotkeyNextMesh(shift); break;
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
			const bool srgb = settings.srgb < 0 ? p.srgb.load() : settings.srgb > 0;

			if (p.kind == SourceKind::kTestPattern) {
				if (!paused || p.pattern.empty()) {
					MakePattern(p.pattern, kPatternSize, kPatternSize, seconds);
					if (slot.Ensure(a_device, kPatternSize, kPatternSize, srgb)) {
						slot.Upload(a_context, p.pattern.data(), kPatternSize, kPatternSize);
						++p.framesShown;
					}
				}
			} else if (p.media) {
				p.media->SetPaused(paused);
				p.media->SetVolume(p.volume);
				if (!paused && p.loop && p.media->Finished()) {
					p.media->Restart();
				}
				VideoFrame frame;
				if (p.media->AcquireFrame(p.media->Clock(), frame)) {
					if (slot.Ensure(a_device, frame.width, frame.height, srgb)) {
						slot.Upload(a_context, frame.pixels.data(), frame.width, frame.height);
						if (p.framesShown++ == 0) {
							log::info("{}: first video frame on screen ({}x{})", p.name, frame.width, frame.height);
						}
					}
					p.media->RecycleFrame(std::move(frame));
				}
			}

			slot.Patch(p.slot);
			Screen::ReapplyUV(p.binding);
		}
	}
}
