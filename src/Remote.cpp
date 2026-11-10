#include "Remote.h"

#include "Manager.h"
#include "Screen.h"
#include "Settings.h"

#include "F4SEMenuFramework.h"

#include <Xinput.h>

// The remote: a small window drawn by F4SE Menu Framework (it owns ImGui, the mouse cursor and input blocking).
// Drawing happens on the render thread; every button only queues the action for the game's main thread.
namespace fv::Remote
{
	namespace
	{
		using namespace ImGuiMCP;

		MENU_WINDOW                g_window{ nullptr };
		// The menu framework freezes the game (+1 on the UI's pause counter) while a window that takes the mouse is
		// open. For the remote we take that +1 back, so the world keeps running and you can walk (main thread).
		std::uint32_t g_menuModeBase = 0;
		int           g_unfreezeFrames = 0;  // frames left to spot the framework's +1 after opening
		std::atomic<bool> g_drawnSinceOpen{ true };  // logs the first frame the remote is drawn after F6
		std::atomic<std::uint32_t> g_target{ 0 };

		std::mutex               g_lock;  // guards the strings below (written on the main thread, read when drawing)
		std::string              g_targetName;
		std::vector<std::wstring> g_videoFiles;
		std::vector<std::string>  g_videoNames;
		std::vector<std::string>  g_videoFolders;  // each video's folder ("" = the video folder itself)
		char                      g_search[96]{};  // the Videos panel's search box
		std::vector<std::string>  g_openPlaylists;  // playlists unfolded in the Playlists panel
		std::atomic<bool>         g_typing{ false };  // search box has the keyboard: W/A/S/D are letters, not walking

		// shown from a game task, never from the remote's UI thread (see Manager's Notify)
		void Notify(const std::string& a_message)
		{
			log::info("{}", a_message);
			if (auto* tasks = F4SE::GetTaskInterface()) {
				tasks->AddTask([m = a_message]() { RE::SendHUDMessage::ShowHUDMessage(m.c_str(), nullptr, false, false); });
			}
		}

		// runs a_fn on the main thread: the remote's UI thread never takes the manager's lock itself
		void OnMain(std::function<void()> a_fn)
		{
			if (auto* tasks = F4SE::GetTaskInterface()) {
				tasks->AddTask(std::move(a_fn));
			}
		}

		// the same, for a button press: logged when it's sent and when the game runs it (sliders aren't, they'd flood the log)
		void OnMainLogged(std::function<void()> a_fn)
		{
			if (auto* tasks = F4SE::GetTaskInterface()) {
				log::info("remote: sent to the game");
				tasks->AddTask([fn = std::move(a_fn)]() {
					log::info("game: running it");
					fn();
					log::info("game: done");
				});
			}
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

		// Runs a_fn(ref) on the main thread for the screen a_formID.
		void Post(std::uint32_t a_formID, std::function<void(RE::TESObjectREFR*)> a_fn)
		{
			auto* tasks = F4SE::GetTaskInterface();
			if (!tasks || a_formID == 0) {
				return;
			}
			log::info("remote: sent to the game");
			tasks->AddTask([a_formID, fn = std::move(a_fn)]() {
				if (auto* ref = RE::TESForm::GetFormByID<RE::TESObjectREFR>(a_formID); ref && ref->Get3D()) {
					log::info("game: running it on {}", Screen::RefName(ref));
					fn(ref);
					log::info("game: done");
				} else {
					log::info("game: that screen isn't loaded any more; nothing to do");
				}
			});
		}

		void RefreshVideoList()
		{
			auto files = Manager::Get().ListVideos();
			std::vector<std::string> names, folders;
			names.reserve(files.size());
			for (const auto& f : files) {
				names.push_back(Narrow(std::filesystem::path(f).stem().wstring()));
				folders.push_back(Narrow(std::filesystem::path(f).parent_path().wstring()));
			}
			std::scoped_lock lock(g_lock);
			g_videoFiles = std::move(files);
			g_videoNames = std::move(names);
			g_videoFolders = std::move(folders);
		}

		void Tip(const char* a_text)
		{
			if (IsItemHovered()) {
				SetTooltip("%s", a_text);
			}
		}

		std::string Clock(float a_seconds)
		{
			const int t = static_cast<int>(std::max(0.0f, a_seconds));
			return t >= 3600 ? std::format("{}:{:02}:{:02}", t / 3600, (t / 60) % 60, t % 60) : std::format("{}:{:02}", t / 60, t % 60);
		}

		// One screen's controls (shared by the remote window and the "Now playing" page).
		void Controls(std::uint32_t a_formID, bool a_withPicker)
		{
			const auto v = Manager::Get().View(a_formID);
			PushID(static_cast<int>(a_formID));

			if (!v.playing) {
				TextDisabled("Off");
			} else if (v.testPattern) {
				Text("Test pattern%s", v.paused ? "  (paused)" : "");
			} else {
				Text("%s%s", v.file.c_str(), v.paused ? "  (paused)" : "");
			}
			Spacing();

			if (Button("<< Back")) {
				Post(a_formID, [](RE::TESObjectREFR* a_ref) { Manager::Get().RemoteStep(a_ref, -1); });
			}
			SameLine();
			if (Button(v.playing && !v.paused ? "  Pause  " : "  Play  ")) {
				Post(a_formID, [](RE::TESObjectREFR* a_ref) { Manager::Get().RemotePlayPause(a_ref); });
			}
			SameLine();
			if (Button("Next >>")) {
				Post(a_formID, [](RE::TESObjectREFR* a_ref) { Manager::Get().RemoteStep(a_ref, +1); });
			}
			SameLine();
			if (Button("Power off")) {
				Post(a_formID, [](RE::TESObjectREFR* a_ref) { Manager::Get().Stop(a_ref); });
			}

			float volume = v.volume * 100.0f;
			if (SliderFloat("Volume", &volume, 0.0f, 200.0f, "%.0f%%")) {
				OnMain([id = a_formID, x = volume / 100.0f]() { Manager::Get().RemoteSetVolume(id, x); });
			}
			SameLine();
			bool muted = v.muted;
			if (Checkbox("Mute", &muted)) {
				OnMain([id = a_formID, x = muted]() { Manager::Get().RemoteSetMuted(id, x); });
			}
			if (v.playing && v.duration > 0.0f) {
				// while dragging only the slider moves; the video jumps when you let go
				static float dragging = -1.0f;
				float        at = dragging >= 0.0f ? dragging : v.position;
				const auto   label = std::format("{} / {}", Clock(at), Clock(v.duration));
				if (SliderFloat("Position", &at, 0.0f, v.duration, label.c_str())) {
					dragging = at;
				}
				if (IsItemDeactivatedAfterEdit() && dragging >= 0.0f) {
					OnMain([id = a_formID, x = dragging]() { Manager::Get().RemoteSeekTo(id, x); });
				}
				if (!IsItemActive()) {
					dragging = -1.0f;
				}
			}
						if (v.brightenable) {
				float bright = v.brightness * 100.0f;
				if (SliderFloat("Brightness", &bright, 25.0f, 100.0f, "%.0f%%")) {
					OnMain([id = a_formID, x = bright / 100.0f]() { Manager::Get().RemoteSetBrightness(id, x); });
				}
			}

			if (a_withPicker) {
				std::scoped_lock lock(g_lock);
				const char* preview = v.playing && !v.testPattern ? v.file.c_str() : "Pick a video...";
				if (BeginCombo("Video", preview)) {
					for (std::size_t i = 0; i < g_videoNames.size(); ++i) {
						if (Selectable(g_videoNames[i].c_str(), false)) {
							Post(a_formID, [file = g_videoFiles[i]](RE::TESObjectREFR* a_ref) { Manager::Get().RemotePlayFile(a_ref, file); });
						}
					}
					EndCombo();
				}

			}
			// what the F7 / F8 keys used to do
			if (v.playing && TreeNode("Fix this screen")) {
				if (Button("Next mesh")) {
					Post(a_formID, [](RE::TESObjectREFR* a_ref) { Manager::Get().FixScreen(a_ref, 0); });
				}
				Tip("The video is on the wrong part of the object: try the next one.");
				SameLine();
				if (Button("Nearby screen")) {
					Post(a_formID, [](RE::TESObjectREFR* a_ref) { Manager::Get().FixScreen(a_ref, 2); });
				}
				Tip("Show it on a screen next to this object instead (switches, projectors that light up a separate screen).");
				if (Button("Turn / flip")) {
					Post(a_formID, [](RE::TESObjectREFR* a_ref) { Manager::Get().FixScreen(a_ref, 1); });
				}
				Tip("Upside down, mirrored or sideways: each press tries the next way round. Remembered for this screen.");
				SameLine();
				if (Button("Other side")) {
					Post(a_formID, [](RE::TESObjectREFR* a_ref) { Manager::Get().FixScreen(a_ref, 3); });
				}
				Tip("Two-sided screens: put the picture on the other side.");
				SameLine();
				if (Button("Write details to the log")) {
					Post(a_formID, [](RE::TESObjectREFR* a_ref) { Manager::Get().DumpScreen(a_ref); });
				}
				Tip("For troubleshooting: what this screen is made of, into RealVideos.log.");
				TreePop();
			}
			PopID();
		}

		// ---- the remote, drawn after the template: 1125 x 1500 design units, scaled to the screen ----

		constexpr float kDesignW = 1125.0f;
		constexpr float kDesignH = 1500.0f;

		const ImU32 kBack = IM_COL32(21, 21, 23, 255);
		const ImU32 kKey = IM_COL32(49, 49, 52, 255);
		const ImU32 kKeyHover = IM_COL32(62, 62, 66, 255);
		const ImU32 kKeyDown = IM_COL32(78, 78, 84, 255);
		const ImU32 kIcon = IM_COL32(190, 190, 190, 255);
		const ImU32 kText = IM_COL32(245, 245, 245, 255);
		const ImU32 kAccent = IM_COL32(120, 200, 255, 255);

		enum class Key
		{
			kNone,
			kRewind,
			kPlayPause,
			kForward,
			kUp,
			kDown,
			kLeft,
			kRight,
			kCenter,
			kPower,
			kHome,
			kSearch,
			kName,
			kClose,
			kLock,
			kResize,
			kOpacity,
			kBright,
			kSeek,
			kAll,
			kPlaylists
		};

		// ---- game controller (XInput) ----
		// Buttons are read on the present thread (Tick) and queued; the remote's own window (UI thread) acts on them.
		enum class Pad
		{
			kA,
			kB,
			kX,
			kY,
			kLB,
			kRB,
			kUp,
			kDown,
			kLeft,
			kRight,
			kBack,
			kStart
		};
		std::mutex       g_padLock;
		std::vector<Pad> g_padQueue;
		int              g_padRow = -1;           // the highlighted row in the Videos list (controller), -1 = none
		bool             g_padScroll = false;     // scroll the highlighted row into view
		bool             g_padActivate = false;   // play the highlighted row

		using XInputGetState_t = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);
		XInputGetState_t PadApi()
		{
			// loaded by hand: works with whichever XInput the PC (or Proton) has, and no extra link dependency
			static XInputGetState_t fn = [] {
				for (const auto* dll : { "xinput1_4.dll", "xinput1_3.dll", "xinput9_1_0.dll" }) {
					if (auto* mod = ::LoadLibraryA(dll)) {
						if (auto* f = ::GetProcAddress(mod, "XInputGetState")) {
							log::info("controller: using {}", dll);
							return reinterpret_cast<XInputGetState_t>(f);
						}
					}
				}
				log::info("controller: XInput not available; controller support off");
				return static_cast<XInputGetState_t>(nullptr);
			}();
			return fn;
		}

		bool   g_listOpen = false;       // the Videos (search) panel, left of the remote
		bool   g_playlistsOpen = false;  // the Playlists panel, right of the remote
		bool   g_scrollToCurrent = false;
		ImVec4 g_remoteRect{};           // where the remote was drawn this frame (x, y, w, h)

		// position / lock (saved in RealVideos_Screens.ini [Remote])
		constexpr auto kStateIni = "Data\\F4SE\\Plugins\\RealVideos_Screens.ini";
		bool           g_stateLoaded = false;
		bool           g_locked = false;
		bool           g_havePos = false;
		ImVec2         g_pos{};
		bool           g_dragging = false;
		float          g_opacity = 1.0f;
		float          g_size = 1.0f;  // remote size (1 = half the screen height)
		bool           g_resizing = false;
		ImVec2         g_grab{};

		void LoadState()
		{
			if (g_stateLoaded) {
				return;
			}
			g_stateLoaded = true;
			g_locked = ::GetPrivateProfileIntA("Remote", "Locked", 0, kStateIni) != 0;
			g_opacity = std::clamp(static_cast<float>(::GetPrivateProfileIntA("Remote", "Opacity", 100, kStateIni)) / 100.0f, 0.2f, 1.0f);
			g_size = std::clamp(static_cast<float>(::GetPrivateProfileIntA("Remote", "Size", 100, kStateIni)) / 100.0f, 0.4f, 1.6f);
			g_listOpen = ::GetPrivateProfileIntA("Remote", "VideosOpen", 0, kStateIni) != 0;
			g_playlistsOpen = ::GetPrivateProfileIntA("Remote", "PlaylistsOpen", 0, kStateIni) != 0;
			char x[32]{}, y[32]{};
			::GetPrivateProfileStringA("Remote", "X", "", x, sizeof(x), kStateIni);
			::GetPrivateProfileStringA("Remote", "Y", "", y, sizeof(y), kStateIni);
			if (x[0] && y[0]) {
				g_pos = ImVec2{ static_cast<float>(std::atof(x)), static_cast<float>(std::atof(y)) };
				g_havePos = true;
			}
		}

		void SaveState()
		{
			::WritePrivateProfileStringA("Remote", "Locked", g_locked ? "1" : "0", kStateIni);
			::WritePrivateProfileStringA("Remote", "Opacity", std::format("{:.0f}", g_opacity * 100.0f).c_str(), kStateIni);
			::WritePrivateProfileStringA("Remote", "Size", std::format("{:.0f}", g_size * 100.0f).c_str(), kStateIni);
			::WritePrivateProfileStringA("Remote", "VideosOpen", g_listOpen ? "1" : "0", kStateIni);
			::WritePrivateProfileStringA("Remote", "PlaylistsOpen", g_playlistsOpen ? "1" : "0", kStateIni);
			::WritePrivateProfileStringA("Remote", "X", std::format("{:.0f}", g_pos.x).c_str(), kStateIni);
			::WritePrivateProfileStringA("Remote", "Y", std::format("{:.0f}", g_pos.y).c_str(), kStateIni);
		}

		struct Canvas
		{
			ImDrawList* dl;
			ImVec2      origin;
			float       s;  // design unit -> pixels

			ImVec2 P(float a_x, float a_y) const { return ImVec2{ origin.x + a_x * s, origin.y + a_y * s }; }
		};

		struct Round
		{
			Key   key;
			float x, y, r;
		};
		constexpr std::array<Round, 6> kRoundKeys{ {
			{ Key::kRewind, 295, 148, 94 },
			{ Key::kPlayPause, 562, 148, 94 },
			{ Key::kForward, 830, 148, 94 },
			{ Key::kPower, 293, 1158, 94 },
			{ Key::kHome, 562, 1158, 94 },
			{ Key::kSearch, 831, 1158, 94 },
		} };
		constexpr float kPadX = 562, kPadY = 655, kPadR = 352, kOkR = 133;
		constexpr float kBarX0 = 83, kBarY0 = 1298, kBarX1 = 1023, kBarY1 = 1453;
		// progress bar along the bottom of the name bar: click it to jump there
		constexpr float kSeekX0 = kBarX0 + 30, kSeekX1 = kBarX1 - 30, kSeekY0 = 1416, kSeekY1 = 1432;


		constexpr float kSunX = 1032, kSunY = 148, kSunR = 62;   // this screen's brightness, right of the top row
		constexpr float kAllX = 93, kAllY = 148, kAllR = 62;     // every screen here on/off, left of the top row
		constexpr float kTabX0 = 1060, kTabX1 = 1112, kTabY0 = 565, kTabY1 = 745;  // playlists tab, right edge
		constexpr std::array<float, 4> kBrightSteps{ 0.25f, 0.5f, 0.75f, 1.0f };

		// next (+1) or previous (-1) brightness step; going up past 100% wraps round to 25%
		float BrightStep(float a_now, int a_dir)
		{
			std::size_t i = 0;
			while (i + 1 < kBrightSteps.size() && kBrightSteps[i] < a_now - 0.01f) {
				++i;
			}
			const bool onStep = std::abs(kBrightSteps[i] - a_now) <= 0.01f;
			if (a_dir > 0) {
				if (onStep || kBrightSteps[i] < a_now) {
					return i + 1 < kBrightSteps.size() ? kBrightSteps[i + 1] : kBrightSteps.front();
				}
				return kBrightSteps[i];
			}
			if (i == 0) {
				return kBrightSteps.front();
			}
			return kBrightSteps[i - 1];
		}

		Key HitTest(float a_x, float a_y)  // design units
		{
			for (const auto& k : kRoundKeys) {
				if ((a_x - k.x) * (a_x - k.x) + (a_y - k.y) * (a_y - k.y) <= k.r * k.r) {
					return k.key;
				}
			}
			const float dx = a_x - kPadX, dy = a_y - kPadY, d = std::sqrt(dx * dx + dy * dy);
			if (d <= kOkR) {
				return Key::kCenter;
			}
			if (d <= kPadR) {
				if (std::abs(dx) > std::abs(dy)) {
					return dx < 0 ? Key::kLeft : Key::kRight;
				}
				return dy < 0 ? Key::kUp : Key::kDown;
			}
			if (a_x >= kSeekX0 - 10 && a_x <= kSeekX1 + 10 && a_y >= kSeekY0 - 18 && a_y <= kBarY1) {
				return Key::kSeek;
			}
			if (a_x >= kBarX0 && a_x <= kBarX1 && a_y >= kBarY0 && a_y <= kBarY1) {
				return Key::kName;
			}
			if (a_x >= kDesignW - 70 && a_y <= 70) {
				return Key::kClose;
			}
			if (a_x <= 90 && a_y <= 90) {
				return Key::kLock;
			}
			if (!g_locked && a_x >= kDesignW - 95 && a_y >= kDesignH - 95 && a_x > kBarX1) {
				return Key::kResize;
			}
			if ((a_x - kSunX) * (a_x - kSunX) + (a_y - kSunY) * (a_y - kSunY) <= kSunR * kSunR) {
				return Key::kBright;
			}
			if ((a_x - kAllX) * (a_x - kAllX) + (a_y - kAllY) * (a_y - kAllY) <= kAllR * kAllR) {
				return Key::kAll;
			}
			if (a_x >= kTabX0 && a_x <= kTabX1 && a_y >= kTabY0 && a_y <= kTabY1) {
				return Key::kPlaylists;
			}
			return Key::kNone;
		}

		ImU32 KeyColor(Key a_key, Key a_hover, bool a_down) { return a_key != a_hover ? kKey : a_down ? kKeyDown : kKeyHover; }

		void Line(const Canvas& c, float x0, float y0, float x1, float y1, float w, ImU32 col)
		{
			ImDrawListManager::AddLine(c.dl, c.P(x0, y0), c.P(x1, y1), col, w * c.s);
		}

		void Tri(const Canvas& c, float x0, float y0, float x1, float y1, float x2, float y2, ImU32 col)
		{
			ImDrawListManager::AddTriangleFilled(c.dl, c.P(x0, y0), c.P(x1, y1), c.P(x2, y2), col);
		}

		void Chevron(const Canvas& c, float cx, float cy, float dx, float dy)  // tip points along (dx, dy)
		{
			const float len = 54, back = 56, w = 14;
			const float tx = cx + dx * back * 0.5f, ty = cy + dy * back * 0.5f;      // tip
			const float bx = cx - dx * back * 0.5f, by = cy - dy * back * 0.5f;      // back of the arms
			const float px = -dy, py = dx;                                         // across
			Line(c, bx + px * len, by + py * len, tx, ty, w, kIcon);
			Line(c, bx - px * len, by - py * len, tx, ty, w, kIcon);
		}

		void DrawRemote(const Canvas& c, Key a_hover, bool a_down, const RemoteView& a_view, const std::string& a_label)
		{
			auto* dl = c.dl;
			ImDrawListManager::AddRectFilled(dl, c.P(0, 0), c.P(kDesignW, kDesignH), kBack, 28.0f * c.s, 0);

			for (const auto& k : kRoundKeys) {
				ImDrawListManager::AddCircleFilled(dl, c.P(k.x, k.y), k.r * c.s, KeyColor(k.key, a_hover, a_down), 48);
			}

			// rewind / play-pause / fast forward
			Tri(c, 252, 148, 290, 120, 290, 176, kIcon);
			Tri(c, 286, 148, 324, 120, 324, 176, kIcon);
			Tri(c, 527, 120, 527, 176, 558, 148, a_view.playing && !a_view.paused ? kAccent : kIcon);
			ImDrawListManager::AddRectFilled(dl, c.P(568, 122), c.P(580, 174), kIcon, 3.0f * c.s, 0);
			ImDrawListManager::AddRectFilled(dl, c.P(588, 122), c.P(600, 174), kIcon, 3.0f * c.s, 0);
			Tri(c, 800, 120, 800, 176, 838, 148, kIcon);
			Tri(c, 834, 120, 834, 176, 872, 148, kIcon);

			// direction pad: four wedges split by dark gaps, round OK key in the middle
			const auto padHover = a_hover == Key::kUp || a_hover == Key::kDown || a_hover == Key::kLeft || a_hover == Key::kRight;
			ImDrawListManager::AddCircleFilled(dl, c.P(kPadX, kPadY), kPadR * c.s, kKey, 96);
			if (padHover) {
				// light up only the hovered quarter
				const float mid = a_hover == Key::kRight ? 0.0f : a_hover == Key::kDown ? 1.5707963f : a_hover == Key::kLeft ? 3.1415926f : -1.5707963f;
				ImDrawListManager::PathLineTo(dl, c.P(kPadX, kPadY));
				ImDrawListManager::PathArcTo(dl, c.P(kPadX, kPadY), kPadR * c.s, mid - 0.785398f, mid + 0.785398f, 32);
				ImDrawListManager::PathFillConvex(dl, a_down ? kKeyDown : kKeyHover);
			}
			const float g = 12;
			const float d = kPadR * 0.7071f;
			Line(c, kPadX - d, kPadY - d, kPadX + d, kPadY + d, g, kBack);
			Line(c, kPadX + d, kPadY - d, kPadX - d, kPadY + d, g, kBack);
			ImDrawListManager::AddCircleFilled(dl, c.P(kPadX, kPadY), (kOkR + 10) * c.s, kBack, 64);
			ImDrawListManager::AddCircleFilled(dl, c.P(kPadX, kPadY), kOkR * c.s, KeyColor(Key::kCenter, a_hover, a_down), 64);
			Chevron(c, kPadX, kPadY - 250, 0, -1);
			Chevron(c, kPadX, kPadY + 250, 0, 1);
			Chevron(c, kPadX - 250, kPadY, -1, 0);
			Chevron(c, kPadX + 250, kPadY, 1, 0);

			// OK key: volume (click = mute)
			{
				const auto text = a_view.muted ? std::string("MUTE") : std::format("{:.0f}%", a_view.volume * 100.0f);
				const float size = 54.0f * c.s;
				ImVec2      ts{};
				CalcTextSize(&ts, text.c_str(), nullptr, false, -1.0f);
				const float scale = size / std::max(1.0f, GetFontSize());
				const auto  at = c.P(kPadX, kPadY);
				ImDrawListManager::AddText(dl, GetFont(), size, ImVec2{ at.x - ts.x * scale * 0.5f, at.y - ts.y * scale * 0.5f },
					a_view.muted ? IM_COL32(255, 120, 110, 255) : kIcon, text.c_str());
			}

			// power: open ring + bar
			ImDrawListManager::PathArcTo(dl, c.P(293, 1162), 36 * c.s, -1.5707963f + 0.75f, -1.5707963f + 6.2831853f - 0.75f, 40);
			ImDrawListManager::PathStroke(dl, a_view.playing ? kIcon : IM_COL32(255, 120, 110, 255), 0, 9 * c.s);
			Line(c, 293, 1112, 293, 1158, 9, a_view.playing ? kIcon : IM_COL32(255, 120, 110, 255));

			// home
			Tri(c, 562, 1118, 522, 1158, 602, 1158, kIcon);
			ImDrawListManager::AddRectFilled(dl, c.P(530, 1154), c.P(594, 1196), kIcon, 3.0f * c.s, 0);
			ImDrawListManager::AddRectFilled(dl, c.P(552, 1170), c.P(572, 1196), KeyColor(Key::kHome, a_hover, a_down), 0.0f, 0);

			// search
			ImDrawListManager::AddCircle(dl, c.P(822, 1150), 30 * c.s, g_listOpen ? kAccent : kIcon, 40, 8 * c.s);
			Line(c, 844, 1172, 870, 1198, 9, g_listOpen ? kAccent : kIcon);

			// name bar
			ImDrawListManager::AddRectFilled(dl, c.P(kBarX0, kBarY0), c.P(kBarX1, kBarY1), a_hover == Key::kName ? kKeyHover : kKey, 0.0f, 0);
			{
				std::string text = a_label;
				const bool  timed = a_view.playing && a_view.duration > 0.0f;
				const float size = (timed ? 52.0f : 64.0f) * c.s;
				const float scale = size / std::max(1.0f, GetFontSize());
				const float maxW = (kBarX1 - kBarX0 - 60) * c.s;
				ImVec2      ts{};
				CalcTextSize(&ts, text.c_str(), nullptr, false, -1.0f);
				while (text.size() > 4 && ts.x * scale > maxW) {
					text.resize(text.size() - 4);
					text += "...";
					CalcTextSize(&ts, text.c_str(), nullptr, false, -1.0f);
				}
				const auto mid = c.P((kBarX0 + kBarX1) * 0.5f, timed ? kBarY0 + 42.0f : (kBarY0 + kBarY1) * 0.5f);
				ImDrawListManager::AddText(dl, GetFont(), size, ImVec2{ mid.x - ts.x * scale * 0.5f, mid.y - ts.y * scale * 0.5f }, kText, text.c_str());

				if (timed) {
					// 12:34 / 1:32:10 and the progress bar
					const auto  t = std::format("{} / {}", Clock(a_view.position), Clock(a_view.duration));
					const float tsize = 34.0f * c.s;
					const float tscale = tsize / std::max(1.0f, GetFontSize());
					ImVec2      tts{};
					CalcTextSize(&tts, t.c_str(), nullptr, false, -1.0f);
					const auto tmid = c.P((kBarX0 + kBarX1) * 0.5f, kBarY0 + 88.0f);
					ImDrawListManager::AddText(dl, GetFont(), tsize, ImVec2{ tmid.x - tts.x * tscale * 0.5f, tmid.y - tts.y * tscale * 0.5f }, kIcon,
						t.c_str());
					const float f = std::clamp(a_view.position / a_view.duration, 0.0f, 1.0f);
					ImDrawListManager::AddRectFilled(dl, c.P(kSeekX0, kSeekY0), c.P(kSeekX1, kSeekY1), IM_COL32(30, 30, 32, 255), 6.0f * c.s, 0);
					ImDrawListManager::AddRectFilled(dl, c.P(kSeekX0, kSeekY0), c.P(kSeekX0 + (kSeekX1 - kSeekX0) * f, kSeekY1),
						a_hover == Key::kSeek ? kText : kAccent, 6.0f * c.s, 0);
				}
			}

			// lock (top left): closed = the remote stays put, open = drag it by any empty spot
			{
				const ImU32 col = a_hover == Key::kLock ? kText : g_locked ? IM_COL32(150, 150, 150, 255) : kAccent;
				ImDrawListManager::AddRectFilled(dl, c.P(26, 44), c.P(66, 76), col, 4.0f * c.s, 0);
				const float sx = g_locked ? 46.0f : 58.0f;  // open: shackle swung up and to the right
				const float sy = g_locked ? 44.0f : 36.0f;
				ImDrawListManager::PathArcTo(dl, c.P(sx, sy), 13 * c.s, 3.1415926f, 6.2831853f, 20);
				ImDrawListManager::PathStroke(dl, col, 0, 6 * c.s);
				Line(c, sx - 13, sy, sx - 13, g_locked ? 46.0f : 30.0f, 6, col);
				Line(c, sx + 13, sy, sx + 13, 46.0f, 6, col);
			}

			// resize grip (bottom right, only while unlocked)
			if (!g_locked) {
				const ImU32 col = a_hover == Key::kResize ? kText : IM_COL32(120, 120, 120, 255);
				for (int i = 0; i < 3; ++i) {
					const float o = 22.0f + i * 22.0f;
					Line(c, kDesignW - 18, kDesignH - 18 - o, kDesignW - 18 - o, kDesignH - 18, 6, col);
				}
			}

			// playlists tab on the right edge: three lines and an arrow (points out when closed, in when open)
			{
				const ImU32 col = g_playlistsOpen ? kAccent : (a_hover == Key::kPlaylists ? kText : kIcon);
				ImDrawListManager::AddRectFilled(dl, c.P(kTabX0, kTabY0), c.P(kTabX1, kTabY1), a_hover == Key::kPlaylists ? kKeyHover : kKey,
					10.0f * c.s, 0);
				for (int i = 0; i < 3; ++i) {
					ImDrawListManager::AddRectFilled(dl, c.P(kTabX0 + 13, kTabY0 + 38 + i * 22), c.P(kTabX1 - 13, kTabY0 + 46 + i * 22), col,
						3.0f * c.s, 0);
				}
				const float ay = kTabY0 + 140, mx = (kTabX0 + kTabX1) * 0.5f;
				if (g_playlistsOpen) {
					Line(c, mx + 7, ay - 14, mx - 7, ay, 6, col);
					Line(c, mx - 7, ay, mx + 7, ay + 14, 6, col);
				} else {
					Line(c, mx - 7, ay - 14, mx + 7, ay, 6, col);
					Line(c, mx + 7, ay, mx - 7, ay + 14, 6, col);
				}
			}

			// every screen here: a power sign with "ALL" under it
			{
				ImDrawListManager::AddCircleFilled(dl, c.P(kAllX, kAllY), kAllR * c.s, KeyColor(Key::kAll, a_hover, a_down), 40);
				const float cy = kAllY - 10;
				ImDrawListManager::PathArcTo(dl, c.P(kAllX, cy), 22 * c.s, -1.0f, 4.1416f, 24);
				ImDrawListManager::PathStroke(dl, kIcon, 0, 5 * c.s);
				Line(c, kAllX, cy - 30, kAllX, cy - 6, 5, kIcon);
				const char* all = "ALL";
				const float size = 26.0f * c.s;
				const float scale = size / std::max(1.0f, GetFontSize());
				ImVec2      ts{};
				CalcTextSize(&ts, all, nullptr, false, -1.0f);
				const auto at = c.P(kAllX, kAllY + 38);
				ImDrawListManager::AddText(dl, GetFont(), size, ImVec2{ at.x - ts.x * scale * 0.5f, at.y - ts.y * scale * 0.5f }, kIcon, all);
			}

			// brightness of this screen: a little sun and the percentage (dim on TVs: they stay as they are)
			{
				const bool  usable = a_view.brightenable;
				const ImU32 icon = usable ? kIcon : IM_COL32(90, 90, 90, 255);
				ImDrawListManager::AddCircleFilled(dl, c.P(kSunX, kSunY), kSunR * c.s, usable ? KeyColor(Key::kBright, a_hover, a_down) : kKey, 40);
				const float cy = kSunY - 8;
				ImDrawListManager::AddCircleFilled(dl, c.P(kSunX, cy), 12 * c.s, icon, 24);
				for (int i = 0; i < 8; ++i) {
					const float a = 0.7853982f * static_cast<float>(i);
					Line(c, kSunX + std::cos(a) * 19, cy + std::sin(a) * 19, kSunX + std::cos(a) * 28, cy + std::sin(a) * 28, 5, icon);
				}
				const auto  pct = usable ? std::format("{:.0f}%", a_view.brightness * 100.0f) : std::string("--");
				const float size = 26.0f * c.s;
				const float scale = size / std::max(1.0f, GetFontSize());
				ImVec2      ts{};
				CalcTextSize(&ts, pct.c_str(), nullptr, false, -1.0f);
				const auto at = c.P(kSunX, kSunY + 38);
				ImDrawListManager::AddText(dl, GetFont(), size, ImVec2{ at.x - ts.x * scale * 0.5f, at.y - ts.y * scale * 0.5f }, icon, pct.c_str());
			}

			// small close mark
			Line(c, kDesignW - 52, 22, kDesignW - 22, 52, 6, a_hover == Key::kClose ? kText : IM_COL32(110, 110, 110, 255));
			Line(c, kDesignW - 22, 22, kDesignW - 52, 52, 6, a_hover == Key::kClose ? kText : IM_COL32(110, 110, 110, 255));
		}

		const char* KeyName(Key a_key)
		{
			switch (a_key) {
			case Key::kRewind: return "REWIND";
			case Key::kPlayPause: return "PLAY/PAUSE";
			case Key::kForward: return "FAST FORWARD";
			case Key::kUp: return "VOLUME UP";
			case Key::kDown: return "VOLUME DOWN";
			case Key::kLeft: return "PREVIOUS";
			case Key::kRight: return "NEXT";
			case Key::kCenter: return "MUTE";
			case Key::kPower: return "POWER";
			case Key::kHome: return "HOME";
			case Key::kSearch: return "SEARCH";
			case Key::kName: return "NAME BAR";
			case Key::kClose: return "CLOSE";
			case Key::kLock: return "LOCK";
			case Key::kResize: return "RESIZE";
			case Key::kOpacity: return "OPACITY";
			case Key::kBright: return "BRIGHTNESS";
			case Key::kSeek: return "SEEK BAR";
			case Key::kAll: return "ALL";
			case Key::kPlaylists: return "PLAYLISTS";
			default: return "nothing";
			}
		}

		void Press(Key a_key, std::uint32_t a_target, const RemoteView& a_view)
		{
			log::info("remote: clicked {}", KeyName(a_key));
			switch (a_key) {
			case Key::kRewind:
				OnMainLogged([a_target, x = -10.0f]() { Manager::Get().RemoteSeek(a_target, x); });
				break;
			case Key::kForward:
				OnMainLogged([a_target, x = +10.0f]() { Manager::Get().RemoteSeek(a_target, x); });
				break;
			case Key::kPlayPause:
				Post(a_target, [](RE::TESObjectREFR* a_ref) { Manager::Get().RemotePlayPause(a_ref); });
				break;
			case Key::kUp:
				OnMainLogged([a_target, x = std::min(2.0f, std::round((a_view.volume + 0.1f) * 10.0f) / 10.0f)]() { Manager::Get().RemoteSetVolume(a_target, x); });
				if (a_view.muted) {
					OnMainLogged([a_target, x = false]() { Manager::Get().RemoteSetMuted(a_target, x); });
				}
				break;
			case Key::kDown:
				OnMainLogged([a_target, x = std::max(0.0f, std::round((a_view.volume - 0.1f) * 10.0f) / 10.0f)]() { Manager::Get().RemoteSetVolume(a_target, x); });
				break;
			case Key::kLeft:
				Post(a_target, [](RE::TESObjectREFR* a_ref) { Manager::Get().RemoteStep(a_ref, -1); });
				break;
			case Key::kRight:
				Post(a_target, [](RE::TESObjectREFR* a_ref) { Manager::Get().RemoteStep(a_ref, +1); });
				break;
			case Key::kCenter:
				OnMainLogged([a_target, x = !a_view.muted]() { Manager::Get().RemoteSetMuted(a_target, x); });
				break;
			case Key::kPower:
				Post(a_target, [](RE::TESObjectREFR* a_ref) { Manager::Get().RemotePower(a_ref); });
				break;
			case Key::kHome:
				Post(a_target, [](RE::TESObjectREFR* a_ref) { Manager::Get().RemoteFirst(a_ref); });
				break;
			case Key::kSearch:
			case Key::kName:
				g_listOpen = !g_listOpen;
				g_scrollToCurrent = g_listOpen;
				SaveState();
				break;
			case Key::kPlaylists:
				g_playlistsOpen = !g_playlistsOpen;
				SaveState();
				break;
			case Key::kAll:
				Post(a_target, [](RE::TESObjectREFR* a_ref) { Manager::Get().AllScreensToggle(a_ref); });
				break;
			case Key::kBright:
				if (a_view.brightenable) {
					OnMainLogged([a_target, x = BrightStep(a_view.brightness, +1)]() { Manager::Get().RemoteSetBrightness(a_target, x); });
				}
				break;
			case Key::kClose:
				if (g_window) {
					g_window->IsOpen = false;
				}
				break;
			case Key::kLock:
				g_locked = !g_locked;
				SaveState();
				break;
			default:
				break;
			}
		}


		// ---- the side panels: Videos (search, left of the remote) and Playlists (right of it) ----

		std::string LowerAscii(std::string a_s)
		{
			for (auto& ch : a_s) {
				ch = static_cast<char>(::tolower(static_cast<unsigned char>(ch)));
			}
			return a_s;
		}

		// "Playlists\\Horror\\..." -> "Horror"; anything else -> ""
		std::string PlaylistOf(const std::string& a_folder)
		{
			constexpr std::string_view kRoot = "playlists\\";
			const auto                lower = LowerAscii(a_folder);
			if (lower.size() <= kRoot.size() || lower.compare(0, kRoot.size(), kRoot) != 0) {
				return {};
			}
			const auto rest = a_folder.substr(kRoot.size());
			return rest.substr(0, rest.find('\\'));
		}

		// The side panels wear the remote's look: same dark body and corner rounding, keys coloured like the
		// remote's buttons (lighter on hover / press), the accent blue for what's playing, and the remote's opacity.
		constexpr ImU32 kDim = IM_COL32(140, 140, 146, 255);
		constexpr int   kPanelColors = 21;
		constexpr int   kPanelVars = 10;
		float           g_panelFont = 1.0f;  // font scale of the panel text (child lists don't inherit it)

		bool PanelBegin(const char* a_id, const char* a_title, ImVec2 a_pos, ImVec2 a_size, bool& a_open, float a_scale)
		{
			const auto px = [&](float a_v, float a_min) { return std::max(a_min, a_v * a_scale); };
			SetNextWindowPos(a_pos, ImGuiCond_Always, ImVec2{ 0.0f, 0.0f });
			SetNextWindowSize(a_size, ImGuiCond_Always);
			PushStyleColor(ImGuiCol_WindowBg, kBack);
			PushStyleColor(ImGuiCol_ChildBg, IM_COL32(0, 0, 0, 0));
			PushStyleColor(ImGuiCol_Border, kBack);
			PushStyleColor(ImGuiCol_Text, kText);
			PushStyleColor(ImGuiCol_TextDisabled, kDim);
			PushStyleColor(ImGuiCol_FrameBg, kKey);
			PushStyleColor(ImGuiCol_FrameBgHovered, kKeyHover);
			PushStyleColor(ImGuiCol_FrameBgActive, kKeyDown);
			PushStyleColor(ImGuiCol_Button, kKey);
			PushStyleColor(ImGuiCol_ButtonHovered, kKeyHover);
			PushStyleColor(ImGuiCol_ButtonActive, kKeyDown);
			PushStyleColor(ImGuiCol_Header, kKey);
			PushStyleColor(ImGuiCol_HeaderHovered, kKeyHover);
			PushStyleColor(ImGuiCol_HeaderActive, kKeyDown);
			PushStyleColor(ImGuiCol_Separator, kKey);
			PushStyleColor(ImGuiCol_ScrollbarBg, IM_COL32(0, 0, 0, 0));
			PushStyleColor(ImGuiCol_ScrollbarGrab, kKey);
			PushStyleColor(ImGuiCol_ScrollbarGrabHovered, kKeyHover);
			PushStyleColor(ImGuiCol_ScrollbarGrabActive, kKeyDown);
			PushStyleColor(ImGuiCol_TextSelectedBg, IM_COL32(120, 200, 255, 90));
			PushStyleColor(ImGuiCol_NavHighlight, IM_COL32(0, 0, 0, 0));
			PushStyleVar(ImGuiStyleVar_Alpha, std::max(0.4f, g_opacity));
			PushStyleVar(ImGuiStyleVar_WindowRounding, 28.0f * a_scale);
			PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
			PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2{ px(48.0f, 10.0f), px(44.0f, 10.0f) });
			PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2{ px(24.0f, 6.0f), px(16.0f, 4.0f) });
			PushStyleVar(ImGuiStyleVar_FrameRounding, px(14.0f, 4.0f));
			PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2{ px(20.0f, 6.0f), px(14.0f, 4.0f) });
			PushStyleVar(ImGuiStyleVar_ScrollbarSize, px(26.0f, 8.0f));
			PushStyleVar(ImGuiStyleVar_ScrollbarRounding, px(13.0f, 4.0f));
			PushStyleVar(ImGuiStyleVar_GrabRounding, px(13.0f, 4.0f));
			const bool shown = Begin(a_id, nullptr,
				ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
					ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar);
			if (shown) {
				// text sized like the remote's (its name bar), never smaller than the game's menu text
				const float body = std::clamp(44.0f * a_scale / std::max(1.0f, GetFontSize()), 1.0f, 3.0f);
				SetWindowFontScale(body * 1.3f);
				Text("%s", a_title);
				SetWindowFontScale(body);
				g_panelFont = body;
				SameLine();
				ImVec2 ts{};
				CalcTextSize(&ts, "Hide", nullptr, false, -1.0f);
				const float hideW = ts.x + px(48.0f, 12.0f);
				ImVec2      avail{};
				GetContentRegionAvail(&avail);
				SetCursorPosX(GetCursorPosX() + std::max(0.0f, avail.x - hideW));
				if (Button("Hide", ImVec2{ hideW, 0.0f })) {
					a_open = false;
					SaveState();
				}
				Dummy(ImVec2{ 0.0f, px(6.0f, 2.0f) });
			}
			return shown;
		}

		void PanelEnd()
		{
			End();
			PopStyleVar(kPanelVars);
			PopStyleColor(kPanelColors);
		}

		// a list row; the one playing is drawn in the remote's accent blue, like its play key
		bool PanelRow(const char* a_label, bool a_current)
		{
			if (a_current) {
				PushStyleColor(ImGuiCol_Text, kAccent);
			}
			const bool clicked = Selectable(a_label, a_current);
			if (a_current) {
				PopStyleColor(1);
			}
			return clicked;
		}

		void SidePanels(std::uint32_t a_target, const RemoteView& a_view, float a_scale)
		{
			const float gap = 10.0f;
			const float panelW = std::max(240.0f, 620.0f * a_scale);
			const ImVec2 size{ panelW, g_remoteRect.w };
			std::scoped_lock lock(g_lock);
			const auto isCurrent = [&](std::size_t a_i) {
				return a_view.playing && !a_view.testPattern && g_videoFolders[a_i] == a_view.folder &&
				       _stricmp(Narrow(std::filesystem::path(g_videoFiles[a_i]).filename().wstring()).c_str(), a_view.file.c_str()) == 0;
			};
			const auto play = [&](std::size_t a_i) {
				Post(a_target, [file = g_videoFiles[a_i]](RE::TESObjectREFR* a_ref) { Manager::Get().RemotePlayFile(a_ref, file); });
			};

			// Videos: search bar on top, matching videos below (videos only, never folders)
			g_typing = false;
			if (g_listOpen) {
				if (PanelBegin("##RealVideosVideos", "Videos", ImVec2{ g_remoteRect.x - panelW - gap, g_remoteRect.y }, size, g_listOpen, a_scale)) {
					SetNextItemWidth(-1.0f);
					InputTextWithHint("##RealVideosSearch", "Search videos...", g_search, sizeof(g_search));
					g_typing = IsItemActive();
					const auto query = LowerAscii(g_search);
					std::size_t found = 0;
					for (std::size_t i = 0; i < g_videoNames.size(); ++i) {
						found += query.empty() || LowerAscii(g_videoNames[i]).find(query) != std::string::npos;
					}
					TextDisabled("%zu video%s", found, found == 1 ? "" : "s");
					if (g_padRow >= static_cast<int>(found)) {
						g_padRow = found ? static_cast<int>(found) - 1 : -1;
					}
					if (g_padRow >= 0) {
						SameLine();
						TextDisabled("  (D-pad, A to play)");
					}
					std::size_t row = 0;
					if (BeginChild("##RealVideosResults", ImVec2{ 0.0f, 0.0f }, 0, 0)) {
						SetWindowFontScale(g_panelFont);
						for (std::size_t i = 0; i < g_videoNames.size(); ++i) {
							if (!query.empty() && LowerAscii(g_videoNames[i]).find(query) == std::string::npos) {
								continue;
							}
							PushID(static_cast<int>(i));
							const bool current = isCurrent(i);
							const bool padSel = static_cast<int>(row) == g_padRow;
							if (PanelRow(g_videoNames[i].c_str(), current || padSel) || (padSel && g_padActivate)) {
								play(i);
								g_padActivate = false;
							}
							if (padSel && g_padScroll) {
								SetScrollHereY(0.5f);
								g_padScroll = false;
							}
							++row;
							if (const auto pl = PlaylistOf(g_videoFolders[i]); !pl.empty()) {
								SameLine();
								TextDisabled("%s", pl.c_str());
							}
							if (current && g_scrollToCurrent) {
								SetScrollHereY(0.5f);
								g_scrollToCurrent = false;
							}
							PopID();
						}
					}
					EndChild();
				}
				PanelEnd();
			}

			// Playlists: every folder in Videos\\Playlists; play from the start, or unfold and pick a video
			if (g_playlistsOpen) {
				if (PanelBegin("##RealVideosPlaylists", "Playlists", ImVec2{ g_remoteRect.x + g_remoteRect.z + gap, g_remoteRect.y }, size,
						g_playlistsOpen, a_scale)) {
					std::vector<std::string> names;
					for (const auto& f : g_videoFolders) {
						if (auto pl = PlaylistOf(f); !pl.empty() && std::ranges::find(names, pl) == names.end()) {
							names.push_back(pl);
						}
					}
					if (BeginChild("##RealVideosPlaylistList", ImVec2{ 0.0f, 0.0f }, 0, 0)) {
						SetWindowFontScale(g_panelFont);
						if (names.empty()) {
							TextWrapped("No playlists yet. Make folders in your video folder's Playlists folder, e.g. "
										"Data\\Videos\\Playlists\\Horror, and put videos in them.");
						}
						const auto playing = PlaylistOf(a_view.folder);
						for (std::size_t n = 0; n < names.size(); ++n) {
							const auto& name = names[n];
							PushID(static_cast<int>(n));
							// play from its first video
							if (Button("Play")) {
								for (std::size_t i = 0; i < g_videoFolders.size(); ++i) {
									if (PlaylistOf(g_videoFolders[i]) == name) {
										play(i);
										break;
									}
								}
							}
							SameLine();
							auto       it = std::ranges::find(g_openPlaylists, name);
							const bool unfolded = it != g_openPlaylists.end();
							const auto label = std::format("{} {}", unfolded ? "v" : ">", name);
							if (PanelRow(label.c_str(), playing == name)) {
								if (unfolded) {
									g_openPlaylists.erase(it);
								} else {
									g_openPlaylists.push_back(name);
								}
							}
							if (unfolded) {
								Indent(std::max(16.0f, 60.0f * a_scale));
								for (std::size_t i = 0; i < g_videoNames.size(); ++i) {
									if (PlaylistOf(g_videoFolders[i]) != name) {
										continue;
									}
									PushID(static_cast<int>(i) + 100000);
									if (PanelRow(g_videoNames[i].c_str(), isCurrent(i))) {
										play(i);
									}
									PopID();
								}
								Unindent(std::max(16.0f, 60.0f * a_scale));
							}
							PopID();
						}
					}
					EndChild();
				}
				PanelEnd();
			}
		}

		// controller buttons queued by PollPad: with the Videos list open the D-pad and A / B work the list,
		// otherwise every button is a remote key
		void HandlePad(std::uint32_t a_target, const RemoteView& a_view)
		{
			std::vector<Pad> pressed;
			{
				std::scoped_lock lock(g_padLock);
				pressed.swap(g_padQueue);
			}
			for (const auto b : pressed) {
				if (g_listOpen && (b == Pad::kUp || b == Pad::kDown || b == Pad::kA || b == Pad::kB)) {
					switch (b) {
					case Pad::kUp:
						g_padRow = std::max(0, g_padRow - 1);
						g_padScroll = true;
						break;
					case Pad::kDown:
						g_padRow = g_padRow + 1;  // clamped to the list when it's drawn
						g_padScroll = true;
						break;
					case Pad::kA:
						if (g_padRow < 0) {
							g_padRow = 0;  // first press picks the first row
							g_padScroll = true;
						} else {
							g_padActivate = true;
						}
						break;
					default:  // B: close the list
						g_listOpen = false;
						g_padRow = -1;
						SaveState();
						break;
					}
					continue;
				}
				Key key = Key::kNone;
				switch (b) {
				case Pad::kA: key = Key::kPlayPause; break;
				case Pad::kB: key = Key::kClose; break;
				case Pad::kX: key = Key::kCenter; break;
				case Pad::kY: key = Key::kPower; break;
				case Pad::kLB: key = Key::kRewind; break;
				case Pad::kRB: key = Key::kForward; break;
				case Pad::kUp: key = Key::kUp; break;
				case Pad::kDown: key = Key::kDown; break;
				case Pad::kLeft: key = Key::kLeft; break;
				case Pad::kRight: key = Key::kRight; break;
				case Pad::kBack: key = Key::kSearch; break;
				case Pad::kStart: key = Key::kPlaylists; break;
				}
				if (key == Key::kSearch) {
					g_padRow = g_listOpen ? -1 : 0;  // opening the list from the controller highlights its first row
					g_padScroll = true;
				}
				if (key != Key::kNone) {
					Press(key, a_target, a_view);  // logs "remote: clicked ..."
				}
			}
		}

		void __stdcall RenderWindow()
		{
			if (!g_drawnSinceOpen.exchange(true)) {
				log::info("remote: window drawn for the first time");
			}
			const auto target = g_target.load();
			const auto view = Manager::Get().View(target);
			HandlePad(target, view);
			auto*      viewport = GetMainViewport();
			LoadState();
			const float baseScale = viewport->Size.y * 0.5f / kDesignH;
			const float scale = baseScale * g_size;
			const float w = kDesignW * scale, h = kDesignH * scale;

			if (!g_havePos) {
				g_pos = ImVec2{ viewport->Pos.x + viewport->Size.x * 0.80f - w * 0.5f, viewport->Pos.y + viewport->Size.y * 0.5f - h * 0.5f };
				g_havePos = true;
			}
			// keep it on screen (resolution changes, old saved spot)
			g_pos.x = std::clamp(g_pos.x, viewport->Pos.x - w * 0.5f, viewport->Pos.x + viewport->Size.x - w * 0.5f);
			g_pos.y = std::clamp(g_pos.y, viewport->Pos.y, viewport->Pos.y + viewport->Size.y - h * 0.25f);
			SetNextWindowPos(g_pos, ImGuiCond_Always, ImVec2{ 0.0f, 0.0f });
			const auto faded = [](ImU32 a_col) {
				const auto a = static_cast<ImU32>(((a_col >> IM_COL32_A_SHIFT) & 0xFF) * g_opacity);
				return (a_col & ~IM_COL32_A_MASK) | (a << IM_COL32_A_SHIFT);
			};
			PushStyleColor(ImGuiCol_WindowBg, faded(kBack));
			PushStyleColor(ImGuiCol_ChildBg, kKey);
			PushStyleColor(ImGuiCol_Border, kBack);
			PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2{ 0.0f, 0.0f });
			PushStyleVar(ImGuiStyleVar_Alpha, std::max(0.4f, g_opacity));
			PushStyleVar(ImGuiStyleVar_WindowRounding, 14.0f * scale * 2.0f);

			bool open = true;
			if (Begin("##RealVideosRemote", nullptr,
					ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoScrollbar |
						ImGuiWindowFlags_NoCollapse)) {
				ImVec2 origin{};
				GetCursorScreenPos(&origin);
				InvisibleButton("##remote", ImVec2{ w, h });
				const bool hovered = IsItemHovered();
				const bool down = IsItemActive();
				ImVec2     mouse{};
				GetMousePos(&mouse);
				const Key hover = hovered ? HitTest((mouse.x - origin.x) / scale, (mouse.y - origin.y) / scale) : Key::kNone;

				std::string label;
				{
					std::scoped_lock lock(g_lock);
					label = !view.playing ? "POWER OFF" : view.testPattern ? "TEST PATTERN" : view.file;
				}
				auto*     drawList = GetWindowDrawList();
				const int firstVertex = drawList->VtxBuffer.Size;
				DrawRemote(Canvas{ drawList, origin, scale }, hover, down, view, label);
				if (g_opacity < 0.999f) {
					for (int vi = firstVertex; vi < drawList->VtxBuffer.Size; ++vi) {
						drawList->VtxBuffer.Data[vi].col = faded(drawList->VtxBuffer.Data[vi].col);
					}
				}
				if (hovered && IsItemClicked(0)) {
					if (hover == Key::kResize) {
						g_resizing = true;  // drag the corner: bigger / smaller
					} else if (hover == Key::kNone && !g_locked) {
						g_dragging = true;  // grabbed an empty spot: move the remote
						g_grab = ImVec2{ mouse.x - g_pos.x, mouse.y - g_pos.y };
					} else if (hover == Key::kSeek) {
						if (view.playing && view.duration > 0.0f) {
							const float x = (mouse.x - origin.x) / scale;
							const float f = std::clamp((x - kSeekX0) / (kSeekX1 - kSeekX0), 0.0f, 1.0f);
							OnMain([target, at = f * view.duration]() { Manager::Get().RemoteSeekTo(target, at); });
						}
					} else {
						Press(hover, target, view);
					}
				}
				if (hovered && hover == Key::kBright && view.brightenable && IsItemClicked(1)) {
					OnMain([target, b = BrightStep(view.brightness, -1)]() { Manager::Get().RemoteSetBrightness(target, b); });  // right click: darker
				}
				if (g_resizing) {
					if (IsMouseDown(0)) {
						const float wantW = std::max(mouse.x - g_pos.x, 1.0f);
						const float wantH = std::max(mouse.y - g_pos.y, 1.0f);
						g_size = std::clamp(std::max(wantW / (kDesignW * baseScale), wantH / (kDesignH * baseScale)), 0.4f, 1.6f);
					} else {
						g_resizing = false;
						SaveState();
					}
				}
				if (g_dragging) {
					if (IsMouseDown(0)) {
						g_pos = ImVec2{ mouse.x - g_grab.x, mouse.y - g_grab.y };
					} else {
						g_dragging = false;
						SaveState();
					}
				}

				g_remoteRect = ImVec4{ origin.x, origin.y, w, h };

				if (IsKeyPressed(ImGuiKey_Escape, false)) {
					open = false;
					log::info("remote: closed (Esc)");
				}
			}
			End();
			PopStyleVar(3);
			PopStyleColor(3);
			if (open) {
				SidePanels(target, view, scale);
			}
			if (!open && g_window) {
				g_window->IsOpen = false;
			}
		}

		// ---- settings page in the framework's menu ----


		bool Percent(const char* a_label, float* a_value, float a_max, const char* a_tip)
		{
			float pct = *a_value * 100.0f;
			const bool changed = SliderFloat(a_label, &pct, 0.0f, a_max * 100.0f, "%.0f%%");
			Tip(a_tip);
			if (changed) {
				*a_value = pct / 100.0f;
			}
			return changed;
		}

		bool Meters(const char* a_label, float* a_units, float a_maxMeters, const char* a_tip)
		{
			float m = *a_units / 70.0f;  // ~70 game units per metre
			const bool changed = SliderFloat(a_label, &m, 0.0f, a_maxMeters, "%.0f m");
			Tip(a_tip);
			if (changed) {
				*a_units = m * 70.0f;
			}
			return changed;
		}

		void ApplyNow()
		{
			if (auto* tasks = F4SE::GetTaskInterface()) {
				tasks->AddTask([]() { Manager::Get().RebindAll(); });
			}
		}

		void __stdcall RenderSettings()
		{
			LoadState();
			auto&       s = Settings::Get();
			static bool dirty = false;

			SeparatorText("Playback");
			if (Checkbox("Loop videos", &s.loop)) {
				dirty = true;
				OnMain([loop = s.loop]() { Manager::Get().SetLoopAll(loop); });  // the screens playing now too
			}
			Tip("On: a video starts again when it ends. Off: the next video in its folder plays (after the last one, the "
				"first again).");
			dirty |= Checkbox("Shuffle: play videos in random order", &s.shuffle);
			Tip("Random videos, but only from the folder (playlist) the screen is playing from - folders never mix. Next and "
				"turning a screen on pick a random one, a video that ends moves on to another, and previous goes back to the "
				"one played before.");
			dirty |= Checkbox("Pause in menus (Esc, Pip-Boy, loading...)", &s.pauseInMenus);
			dirty |= Checkbox("Pause when the game is in the background (alt-tab)", &s.pauseUnfocused);
			dirty |= SliderInt("Screens playing at once", &s.maxScreens, 1, 20, "%d");
			dirty |= Checkbox("Show messages (playing, paused, off)", &s.showMessages);
			Tip("Off: only problems are shown at the top left.");
			dirty |= Meters("\"All screens here\" reaches", &s.allRadius, 400.0f, "How far around you the ALL button and the All screens buttons reach.");

			SeparatorText("Performance");
			{
				// video size per kind of screen: bigger videos are scaled down while decoding (applies to videos started next)
				const auto sizeCombo = [&](const char* a_label, int* a_value, std::initializer_list<std::pair<const char*, int>> a_choices) {
					const char* current = "Custom";
					for (const auto& [name, value] : a_choices) {
						if (*a_value == value) {
							current = name;
						}
					}
					if (BeginCombo(a_label, current)) {
						for (const auto& [name, value] : a_choices) {
							if (Selectable(name, *a_value == value)) {
								*a_value = value;
								dirty = true;
							}
						}
						EndCombo();
					}
				};
				sizeCombo("TV video size", &s.maxHeightTV, { { "480p", 480 }, { "720p", 720 }, { "1080p", 1080 }, { "Full size", 0 } });
				Tip("Videos taller than this are scaled down while decoding. TVs are small: 720p looks the same as 1080p there. "
					"Applies to videos started from now on.");
				sizeCombo("Projector video size", &s.maxHeightProjector, { { "720p", 720 }, { "1080p", 1080 }, { "1440p", 1440 }, { "Full size", 0 } });
				Tip("Applies to videos started from now on.");
				sizeCombo("Cinema video size", &s.maxHeightCinema, { { "1080p", 1080 }, { "4K (full size)", 0 } });
				Tip("The drive-in screen fills a lot of your view; 4K is sharper but much more work. Applies to videos started from now on.");
			}
			dirty |= Checkbox("Decode videos on the graphics card", &s.gpuDecode);
			Tip("Much less work for the CPU and memory. Applies to videos started from now on. If a screen stays blank or "
				"looks wrong, untick it and start the video again.");
			// the drive-in first: seen and heard from much further away, so its own distances
			dirty |= Checkbox("Cinema: pause when far away or out of view", &s.cinemaFreeze);
			Tip("Picture and sound pause, and carry on from the same spot when you look at the screen or come closer.");
			if (s.cinemaFreeze) {
				dirty |= Meters("...further away than##cfreeze", &s.cinemaFreezeDistance, 600.0f, "Or whenever the drive-in screen isn't in front of you.");
				dirty |= SliderFloat("...for longer than##cfreeze", &s.cinemaFreezeSeconds, 0.0f, 120.0f, "%.0f s");
			}
			dirty |= Checkbox("Cinema: turn off when you walk away", &s.cinemaAutoOff);
			Tip("Remembers the film and where it was, and carries on from there when you come back.");
			if (s.cinemaAutoOff) {
				dirty |= Meters("...further away than##coff", &s.cinemaAutoOffDistance, 600.0f, "Measured from the drive-in screen.");
				dirty |= SliderFloat("...for longer than##coff", &s.cinemaAutoOffSeconds, 0.0f, 120.0f, "%.0f s");
			}
			dirty |= Checkbox("TVs, Projectors: pause when far away or out of view", &s.freeze);
			Tip("Picture and sound pause, and carry on from the same spot when you look at the screen or come closer.");
			if (s.freeze) {
				dirty |= Meters("...further away than##freeze", &s.freezeDistance, 300.0f, "Or whenever the screen isn't in front of you.");
				dirty |= SliderFloat("...for longer than##freeze", &s.freezeSeconds, 0.0f, 120.0f, "%.0f s");
			}
			dirty |= Checkbox("TVs, Projectors: turn off when you walk away", &s.autoOff);
			Tip("Remembers the video and where it was, and carries on from there when you come back.");
			if (s.autoOff) {
				dirty |= Meters("...further away than##off", &s.autoOffDistance, 300.0f, "Measured from the screen.");
				dirty |= SliderFloat("...for longer than##off", &s.autoOffSeconds, 0.0f, 120.0f, "%.0f s");
			}

			SeparatorText("Picking a screen");
			dirty |= Checkbox("Use the screen under the crosshair", &s.aimToSelect);
			Tip("Off: hotkeys and the remote use the object clicked in the console.");
			dirty |= Meters("How far you can aim", &s.aimDistance, 400.0f, "Screens further away than this aren't picked by aiming.");
			dirty |= Checkbox("Only TVs, projectors and the cinema", &s.onlySupportedScreens);
			Tip("Off: the remote works on any object (posters, signs...).");

			SeparatorText("Picture");
			bool bars = s.aspectMode == 1;
			if (Checkbox("Keep the video's shape (black bars)", &bars)) {
				s.aspectMode = bars ? 1 : 0;
				dirty = true;
			}
			Tip("Off: stretch every video to fill its screen. Applies when a video starts (or press Apply below).");
			if (Checkbox("Projector and cinema screens glow like TVs", &s.selfLit)) {
				dirty = true;
				ApplyNow();  // switch the screens playing now too
			}
			Tip("Same brightness day and night, whichever way the screen faces. Off: lit by the world (dark at night). "
				"Switches the screens playing now right away.");
			if (Checkbox("Glowing screens: draw after the world", &s.selfLitLatePass)) {
				dirty = true;
				ApplyNow();  // switch the screens playing now too
			}
			Tip("Stops what stands behind a screen (the drive-in's catwalk) from showing through the picture.");
			if (Checkbox("Projectors: frame lit by the world", &s.glowMap)) {
				dirty = true;
				ApplyNow();  // switch the screens playing now too
			}
			Tip("The screen keeps the game's lighting, so its frame, back and pole react to light like anything else; only the "
				"picture glows. Off: the whole screen glows.");
			dirty |= Checkbox("Cinema: stretch to fill the screen", &s.bigScreenStretch);
			dirty |= Percent("Cinema brightness", &s.bigScreenBrightness, 4.0f, "100% = as is, up to 400%. Only the drive-in screen. Applies right away.");
			dirty |= Checkbox("Projectors: stretch to fill the screen", &s.projectorStretch);
			Tip("Off: the video keeps its shape with black bars. Applies when a video starts (or press Apply below).");
			dirty |= Checkbox("Projectors: draw the video only inside the screen", &s.composite);
			Tip("Off: the video covers the whole mesh, frame included.");
			dirty |= Checkbox("Two-sided screens: show on both sides", &s.bothSides);
			dirty |= Checkbox("Stop the screen's own animation while playing", &s.stopScreenAnimation);
			dirty |= Checkbox("Hide glowing layers over the screen", &s.hideOverlays);
			dirty |= Checkbox("TVs: hide the TV's static, glass and colour tint", &s.tvClean);
			Tip("The video shows clean on TVs, without the vanilla TV filter over it. Use the TVs' old film look below for "
				"an old look instead. Applies when a video starts (or press Apply below).");
			// old cinema film look, every part on its own slider; the cinema and projectors each have their own set
			const auto filmSliders = [&](const char* a_id, Settings::Film& a_f) {
				PushID(a_id);
				dirty |= Percent("Grain", &a_f.grain, 1.0f, "Fine film grain that crawls over the picture.");
				dirty |= Percent("Scratches and dust", &a_f.scratches, 1.0f, "Thin vertical scratches and specks of dust that come and go.");
				dirty |= Percent("Flicker", &a_f.flicker, 1.0f, "The brightness flickering slightly, like an old projector lamp.");
				dirty |= Percent("Dark corners", &a_f.vignette, 1.0f, "The corners and edges of the picture darker than the middle.");
				dirty |= Percent("Faded colours (sepia)", &a_f.sepia, 1.0f, "Colours fading towards warm brown, like old film stock.");
				dirty |= Percent("Shake", &a_f.weave, 1.0f, "The picture moving very slightly, like film running through an old projector.");
				dirty |= Percent("Softness", &a_f.softness, 1.0f, "A slightly soft, out-of-focus picture.");
				PopID();
			};
			SeparatorText("Cinema: old film look");
			filmSliders("cinemafilm", s.cinemaFilm);
			SeparatorText("Projectors: old film look");
			filmSliders("projectorfilm", s.projectorFilm);
			SeparatorText("TVs: old film look");
			filmSliders("tvfilm", s.tvFilm);
			TextDisabled("All at 0% = no film look. Changes show right away.");
			Separator();

			Text("Colours");
			SameLine();
			dirty |= RadioButton("Auto", &s.srgb, -1);
			SameLine();
			dirty |= RadioButton("Linear", &s.srgb, 0);
			SameLine();
			dirty |= RadioButton("sRGB", &s.srgb, 1);
			Tip("Try Linear or sRGB if colours look washed out or too dark.");
			if (Button("Apply to the screens playing now")) {
				if (auto* tasks = F4SE::GetTaskInterface()) {
					tasks->AddTask([]() { Manager::Get().RebindAll(); });
				}
			}

			SeparatorText("Sound");
			dirty |= Checkbox("Sound on", &s.enableAudio);
			Tip("Applies when a video starts.");
			dirty |= Checkbox("Sound comes from the screen's direction", &s.directionalSound);
			Tip("A screen on your left is louder in your left ear. Off: the same in both ears.");
			dirty |= Percent("Master volume", &s.volume, 2.0f, "Every screen.");
			dirty |= Percent("Volume a screen starts at", &s.startVolume, 2.0f, "What the remote's middle button starts at.");
			TextWrapped("How far away you can hear a screen. Closer than the first distance it plays at full volume; walking "
						"away from there it gets quieter, and past the second distance you can't hear it at all.");
			dirty |= Meters("TVs & projectors: full volume up to", &s.minDistance, 50.0f,
				"Stand this close or closer and you hear the TV or projector at full volume.");
			dirty |= Meters("TVs & projectors: can't be heard past", &s.maxDistance, 150.0f,
				"Further away than this the TV or projector is silent.");
			if (s.maxDistance < s.minDistance + 1.0f) {
				s.maxDistance = s.minDistance + 1.0f;
			}
			dirty |= Meters("Cinema: full volume up to", &s.bigMinDistance, 150.0f,
				"Within this distance of the drive-in screen you hear the film at full volume.");
			dirty |= Meters("Cinema: can't be heard past", &s.bigMaxDistance, 600.0f,
				"Further from the drive-in screen than this the film is silent.");
			if (s.bigMaxDistance < s.bigMinDistance + 1.0f) {
				s.bigMaxDistance = s.bigMinDistance + 1.0f;
			}
			dirty |= SliderFloat("How it fades between the two", &s.falloffCurve, 0.5f, 4.0f, "%.1f");
			Tip("1 = gets quieter at an even rate, 2 = natural (default), higher = drops off quickly once you step back.");
			TextDisabled("Fade: 1 = even, 2 = natural, higher = quieter sooner.");

			SeparatorText("Remote");
			{
				float opacity = g_opacity * 100.0f;
				if (SliderFloat("Remote opacity", &opacity, 20.0f, 100.0f, "%.0f%%")) {
					g_opacity = opacity / 100.0f;
					SaveState();
				}
				float size = g_size * 100.0f;
				if (SliderFloat("Remote size", &size, 40.0f, 160.0f, "%.0f%%")) {
					g_size = size / 100.0f;
					SaveState();
				}
				Tip("Or unlock the remote and drag its bottom-right corner.");
				if (Checkbox("Lock the remote in place", &g_locked)) {
					SaveState();
				}
				if (Button("Move the remote back to the right side")) {
					g_havePos = false;
					::WritePrivateProfileStringA("Remote", "X", nullptr, kStateIni);
					::WritePrivateProfileStringA("Remote", "Y", nullptr, kStateIni);
				}
			}
			{
				static bool waiting = false;
				const auto  keyName = [](int a_vk) {
					char      name[64]{};
					const UINT scan = ::MapVirtualKeyA(static_cast<UINT>(a_vk), MAPVK_VK_TO_VSC);
					LONG       lparam = static_cast<LONG>(scan << 16);
					if (a_vk == VK_INSERT || a_vk == VK_DELETE || a_vk == VK_HOME || a_vk == VK_END || a_vk == VK_PRIOR || a_vk == VK_NEXT ||
						(a_vk >= VK_LEFT && a_vk <= VK_DOWN)) {
						lparam |= 1 << 24;  // extended key
					}
					if (a_vk > 0 && ::GetKeyNameTextA(lparam, name, sizeof(name)) > 0) {
						return std::string(name);
					}
					return a_vk > 0 ? std::format("key {:#x}", a_vk) : std::string("none");
				};
				if (waiting) {
					Text("Remote key: press a key... (Esc = cancel)");
					for (int vk = 0x08; vk <= 0xFE; ++vk) {
						if (vk == VK_SHIFT || vk == VK_CONTROL || vk == VK_MENU || (vk >= VK_LSHIFT && vk <= VK_RMENU) || vk == VK_LWIN ||
							vk == VK_RWIN) {
							continue;
						}
						if ((::GetAsyncKeyState(vk) & 1) != 0) {
							if (vk != VK_ESCAPE) {
								s.keyRemote = vk;
								dirty = true;
								s.Save();  // keep it for next time straight away
							}
							waiting = false;
							break;
						}
					}
				} else {
					Text("Remote key: %s", keyName(s.keyRemote).c_str());
					SameLine();
					if (Button("Change")) {
						for (int vk = 0x08; vk <= 0xFE; ++vk) {
							(void)::GetAsyncKeyState(vk);  // forget earlier presses
						}
						waiting = true;
					}
					Tip("Click, then press the key that should open and close the remote. Saved right away.");
				}
				dirty |= Checkbox("Controller: click both sticks (L3 + R3) to open the remote", &s.controller);
				Tip("A play/pause, B close, X mute, Y power, D-pad left/right previous/next, D-pad up/down volume, LB/RB "
					"rewind/fast forward, View/Back the Videos list (D-pad + A to play, B to close it), Start the Playlists. "
					"The left stick walks with the remote on screen; L3 + R3 brings it back.");
			}

			Separator();
			if (Button("Save to RealVideos.ini")) {
				s.Save();
				dirty = false;
			}
			SameLine();
			if (Button("Reload from RealVideos.ini")) {
				s.Load();
			}
			if (dirty) {
				TextDisabled("Changes are live; Save keeps them for next time.");
			}
		}

		void __stdcall RenderPage()
		{
			TextWrapped("Aim at a TV, projector or the drive-in screen and press the remote key (F6 by default) to control it. "
						"Screens playing right now:");
			if (Button("All screens here on")) {
				if (auto* tasks = F4SE::GetTaskInterface()) {
					tasks->AddTask([]() { Manager::Get().AllScreens(nullptr, true); });
				}
			}
			Tip("Every TV, projector and cinema screen around you (this settlement) plays the first video. "
				"The remote's ALL button plays the video on the screen you're aiming at.");
			SameLine();
			if (Button("All screens here off")) {
				if (auto* tasks = F4SE::GetTaskInterface()) {
					tasks->AddTask([]() { Manager::Get().AllScreens(nullptr, false); });
				}
			}
			Separator();
			const auto views = Manager::Get().Views();
			if (views.empty()) {
				TextDisabled("Nothing is playing.");
			}
			for (const auto& v : views) {
				Text("%s", v.name.c_str());
				Controls(v.formID, false);
				Separator();
			}
		}
	}

	namespace
	{
		// While the remote is open the menu framework makes the game ignore keyboard and mouse (good: the mouse
		// only drives the remote). Walking is fed in directly from the held W/A/S/D keys.
		// Two modes while the remote is open:
		//  * cursor mode: the menu framework shows the mouse cursor for clicking and keeps keyboard and mouse
		//    away from the game (that is why you can't walk then);
		//  * walk mode: the remote stays on screen but the game gets keyboard and mouse back, so you walk and
		//    look around normally.
		// Pressing W/A/S/D switches to walk mode; Left Alt or the remote key (F6) brings the cursor back.
		void WatchFreeze()  // main thread, every frame while the remote takes the mouse
		{
			if (g_unfreezeFrames > 0) {
				--g_unfreezeFrames;
				if (auto* ui = RE::UI::GetSingleton(); ui && ui->menuMode == g_menuModeBase + 1) {
					log::info("remote: menu framework paused the game -> taking that back");
					ui->menuMode -= 1;  // the framework's own guard keeps its -1 on close from going below zero
					g_unfreezeFrames = 0;
					log::info("remote: game keeps running while the remote is open");
				} else if (g_unfreezeFrames == 0) {
					log::info("remote: the menu framework didn't pause the game (menu mode {} -> {}); nothing to take back",
						g_menuModeBase, ui ? ui->menuMode : 0u);
				}
			}
		}

		void CursorMode(bool a_cursor)
		{
			if (!g_window || g_window->BlockUserInput.load() == a_cursor) {
				return;
			}
			if (a_cursor) {
				if (auto* ui = RE::UI::GetSingleton()) {
					g_menuModeBase = ui->menuMode;  // the framework pauses again when the cursor comes back
					g_unfreezeFrames = 60;
				}
			}
			g_window->BlockUserInput = a_cursor;
			log::info("remote: {}", a_cursor ? "cursor mode (click the remote)" : "walk mode (W/A/S/D, mouse looks around)");
		}
	}

	// the game controller, every frame (also while the remote is closed: L3 + R3 opens it)
	void PollPad()
	{
		static WORD  prev = 0;
		static DWORD pad = 0;  // the controller in use (0-3)
		if (!g_window || !Settings::Get().controller) {
			return;
		}
		auto* api = PadApi();
		if (!api) {
			return;
		}
		DWORD pid = 0;
		::GetWindowThreadProcessId(::GetForegroundWindow(), &pid);
		if (pid != ::GetCurrentProcessId()) {
			prev = 0;
			return;
		}
		XINPUT_STATE st{};
		if (api(pad, &st) != ERROR_SUCCESS) {
			static int probe = 0;
			if (++probe % 60 == 0) {  // look for a controller on the other ports about once a second
				pad = (pad + 1) % 4;
			}
			prev = 0;
			return;
		}
		const WORD cur = st.Gamepad.wButtons;
		const WORD hit = cur & ~prev;
		prev = cur;

		constexpr WORD sticks = XINPUT_GAMEPAD_LEFT_THUMB | XINPUT_GAMEPAD_RIGHT_THUMB;
		if ((cur & sticks) == sticks && (hit & sticks)) {
			Toggle();  // same as the remote key: open, bring the cursor back, or close
			return;
		}
		if (!g_window->IsOpen.load()) {
			return;
		}
		if (g_window->BlockUserInput.load()) {
			// moving the left stick walks with the remote on screen, like W/A/S/D
			const int lx = st.Gamepad.sThumbLX, ly = st.Gamepad.sThumbLY;
			if (lx * lx + ly * ly > 16000 * 16000) {
				CursorMode(false);
				return;
			}
			static const std::pair<WORD, Pad> kMap[]{
				{ XINPUT_GAMEPAD_A, Pad::kA }, { XINPUT_GAMEPAD_B, Pad::kB }, { XINPUT_GAMEPAD_X, Pad::kX }, { XINPUT_GAMEPAD_Y, Pad::kY },
				{ XINPUT_GAMEPAD_LEFT_SHOULDER, Pad::kLB }, { XINPUT_GAMEPAD_RIGHT_SHOULDER, Pad::kRB },
				{ XINPUT_GAMEPAD_DPAD_UP, Pad::kUp }, { XINPUT_GAMEPAD_DPAD_DOWN, Pad::kDown },
				{ XINPUT_GAMEPAD_DPAD_LEFT, Pad::kLeft }, { XINPUT_GAMEPAD_DPAD_RIGHT, Pad::kRight },
				{ XINPUT_GAMEPAD_BACK, Pad::kBack }, { XINPUT_GAMEPAD_START, Pad::kStart },
			};
			std::scoped_lock lock(g_padLock);
			for (const auto& [bit, b] : kMap) {
				if (hit & bit) {
					g_padQueue.push_back(b);
				}
			}
		}
	}

	void Tick()
	{
		PollPad();
		const bool open = g_window && g_window->IsOpen.load();
		if (!open) {
			return;
		}
		DWORD pid = 0;
		::GetWindowThreadProcessId(::GetForegroundWindow(), &pid);
		if (pid != ::GetCurrentProcessId()) {
			return;
		}
		auto down = [](int a_vk) { return (::GetAsyncKeyState(a_vk) & 0x8000) != 0; };
		static bool altWas = false;
		const bool  alt = down(VK_LMENU);
		if (g_window->BlockUserInput.load()) {
			if (!g_typing.load() && (down('W') || down('A') || down('S') || down('D'))) {  // not while typing a search
				CursorMode(false);  // start walking right away
			}
		} else if (alt && !altWas) {
			CursorMode(true);
		}
		altWas = alt;
		if (g_window->BlockUserInput.load()) {
			if (auto* tasks = F4SE::GetTaskInterface()) {
				tasks->AddTask([]() { WatchFreeze(); });
			}
		}
	}

	// Safe to call more than once: it runs at several startup points and again on F6, so a missed
	// F4SE message (or the framework not being ready yet) no longer leaves the remote off all session.
	void Register(const char* a_when)
	{
		static bool sectionsAdded = false;
		if (g_window && sectionsAdded) {
			return;
		}
		const HMODULE mf = F4SEMenuFramework_GetHostModule();
		if (!mf) {
			log::info("remote ({}): F4SE Menu Framework not loaded (F4SEMenuFramework.dll not in the process): the remote (F6) is unavailable, hotkeys still work", a_when);
			return;
		}
		if (!::GetProcAddress(mf, "AddWindow")) {
			log::warn("remote ({}): F4SE Menu Framework is loaded but has no AddWindow export; the remote can't be shown", a_when);
			return;
		}
		if (!sectionsAdded) {
			F4SEMenuFramework::SetSection("Real Videos");
			F4SEMenuFramework::AddSectionItem("Now playing", RenderPage);
			F4SEMenuFramework::AddSectionItem("Settings", RenderSettings);
			sectionsAdded = true;
		}
		if (!g_window) {
			g_window = F4SEMenuFramework::AddWindow(RenderWindow, true);
		}
		log::info("remote ({}): registered with F4SE Menu Framework{}", a_when, g_window ? "" : " (window could not be created)");
	}

	void Toggle()
	{
		if (!g_window) {
			Register("remote key");
		}
		log::info("remote: remote key pressed (remote {})", !g_window ? "unavailable" : g_window->IsOpen ? "open" : "closed");
		if (!g_window) {
			Notify(F4SEMenuFramework::IsInstalled() ? "Real Videos: F4SE Menu Framework didn't accept the remote (see RealVideos.log)" :
			                                          "Real Videos: the remote needs F4SE Menu Framework");
			return;
		}
		if (g_window->IsOpen) {
			if (!g_window->BlockUserInput.load()) {
				CursorMode(true);  // walking: F6 brings the cursor back first
			} else {
				g_window->IsOpen = false;
				log::info("remote: closed");
			}
			return;
		}
		log::info("remote: looking for the screen under the crosshair");
		auto ref = Manager::Get().Target();
		if (!ref) {
			Notify("Real Videos: aim at a TV, projector or cinema screen first");
			return;
		}
		std::string what;
		if (Settings::Get().onlySupportedScreens && !Screen::IsSupported(ref.get(), &what)) {
			Notify("Real Videos: that isn't a TV, projector or cinema screen");
			return;
		}
		log::info("remote: found {}", Screen::RefName(ref.get()));
		RefreshVideoList();
		{
			std::scoped_lock lock(g_lock);
			g_targetName = Screen::RefName(ref.get());
			log::info("remote: video list read ({} video{})", g_videoFiles.size(), g_videoFiles.size() == 1 ? "" : "s");
		}
		g_target = ref->GetFormID();
		if (auto* ui = RE::UI::GetSingleton()) {
			g_menuModeBase = ui->menuMode;
			g_unfreezeFrames = 60;
		}
		g_window->BlockUserInput = true;  // opens in cursor mode
		g_drawnSinceOpen = false;
		log::info("remote: opening the window");
		g_window->IsOpen = true;
	}
}
