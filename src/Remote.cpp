#include "Remote.h"

#include "Manager.h"
#include "Screen.h"
#include "Settings.h"

#include "F4SEMenuFramework.h"

// The remote: a small window drawn by F4SE Menu Framework (it owns ImGui, the mouse cursor and input blocking).
// Drawing happens on the render thread; every button only queues the action for the game's main thread.
namespace fv::Remote
{
	namespace
	{
		using namespace ImGuiMCP;

		MENU_WINDOW                g_window{ nullptr };
		std::atomic<std::uint32_t> g_target{ 0 };

		std::mutex               g_lock;  // guards the strings below (written on the main thread, read when drawing)
		std::string              g_targetName;
		std::vector<std::wstring> g_videoFiles;
		std::vector<std::string>  g_videoNames;

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

		// Runs a_fn(ref) on the main thread for the screen a_formID.
		void Post(std::uint32_t a_formID, std::function<void(RE::TESObjectREFR*)> a_fn)
		{
			auto* tasks = F4SE::GetTaskInterface();
			if (!tasks || a_formID == 0) {
				return;
			}
			tasks->AddTask([a_formID, fn = std::move(a_fn)]() {
				if (auto* ref = RE::TESForm::GetFormByID<RE::TESObjectREFR>(a_formID); ref && ref->Get3D()) {
					fn(ref);
				}
			});
		}

		void RefreshVideoList()
		{
			auto files = Manager::Get().ListVideos();
			std::vector<std::string> names;
			names.reserve(files.size());
			for (const auto& f : files) {
				names.push_back(Narrow(std::filesystem::path(f).stem().wstring()));
			}
			std::scoped_lock lock(g_lock);
			g_videoFiles = std::move(files);
			g_videoNames = std::move(names);
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
				Manager::Get().RemoteSetVolume(a_formID, volume / 100.0f);
			}
			SameLine();
			bool muted = v.muted;
			if (Checkbox("Mute", &muted)) {
				Manager::Get().RemoteSetMuted(a_formID, muted);
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
			kOpacity
		};

		bool g_listOpen = false;
		bool g_scrollToCurrent = false;

		// position / lock (saved in RealVideos_Screens.ini [Remote])
		constexpr auto kStateIni = "Data\\F4SE\\Plugins\\RealVideos_Screens.ini";
		bool           g_stateLoaded = false;
		bool           g_locked = false;
		bool           g_havePos = false;
		ImVec2         g_pos{};
		bool           g_dragging = false;
		float          g_opacity = 1.0f;
		constexpr std::array<float, 5> kOpacitySteps{ 1.0f, 0.85f, 0.7f, 0.55f, 0.4f };
		ImVec2         g_grab{};

		void LoadState()
		{
			if (g_stateLoaded) {
				return;
			}
			g_stateLoaded = true;
			g_locked = ::GetPrivateProfileIntA("Remote", "Locked", 0, kStateIni) != 0;
			g_opacity = std::clamp(static_cast<float>(::GetPrivateProfileIntA("Remote", "Opacity", 100, kStateIni)) / 100.0f, 0.2f, 1.0f);
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
		constexpr float kFadeX = 93, kFadeY = 148, kFadeR = 62;  // opacity, left of the top row

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
			if (a_x >= kBarX0 && a_x <= kBarX1 && a_y >= kBarY0 && a_y <= kBarY1) {
				return Key::kName;
			}
			if (a_x >= kDesignW - 70 && a_y <= 70) {
				return Key::kClose;
			}
			if (a_x <= 90 && a_y <= 90) {
				return Key::kLock;
			}
			if ((a_x - kFadeX) * (a_x - kFadeX) + (a_y - kFadeY) * (a_y - kFadeY) <= kFadeR * kFadeR) {
				return Key::kOpacity;
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
				const float size = 64.0f * c.s;
				const float scale = size / std::max(1.0f, GetFontSize());
				const float maxW = (kBarX1 - kBarX0 - 60) * c.s;
				ImVec2      ts{};
				CalcTextSize(&ts, text.c_str(), nullptr, false, -1.0f);
				while (text.size() > 4 && ts.x * scale > maxW) {
					text.resize(text.size() - 4);
					text += "...";
					CalcTextSize(&ts, text.c_str(), nullptr, false, -1.0f);
				}
				const auto mid = c.P((kBarX0 + kBarX1) * 0.5f, (kBarY0 + kBarY1) * 0.5f);
				ImDrawListManager::AddText(dl, GetFont(), size, ImVec2{ mid.x - ts.x * scale * 0.5f, mid.y - ts.y * scale * 0.5f }, kText, text.c_str());
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

			// opacity: a half-filled circle, how see-through the remote is
			{
				ImDrawListManager::AddCircleFilled(dl, c.P(kFadeX, kFadeY), kFadeR * c.s, KeyColor(Key::kOpacity, a_hover, a_down), 40);
				ImDrawListManager::AddCircle(dl, c.P(kFadeX, kFadeY - 6), 24 * c.s, kIcon, 32, 5 * c.s);
				ImDrawListManager::PathArcTo(dl, c.P(kFadeX, kFadeY - 6), 24 * c.s, 1.5707963f, 4.712389f, 20);
				ImDrawListManager::PathFillConvex(dl, kIcon);
				const auto pct = std::format("{:.0f}%", g_opacity * 100.0f);
				const float size = 26.0f * c.s;
				const float scale = size / std::max(1.0f, GetFontSize());
				ImVec2      ts{};
				CalcTextSize(&ts, pct.c_str(), nullptr, false, -1.0f);
				const auto at = c.P(kFadeX, kFadeY + 38);
				ImDrawListManager::AddText(dl, GetFont(), size, ImVec2{ at.x - ts.x * scale * 0.5f, at.y - ts.y * scale * 0.5f }, kIcon, pct.c_str());
			}

			// small close mark
			Line(c, kDesignW - 52, 22, kDesignW - 22, 52, 6, a_hover == Key::kClose ? kText : IM_COL32(110, 110, 110, 255));
			Line(c, kDesignW - 22, 22, kDesignW - 52, 52, 6, a_hover == Key::kClose ? kText : IM_COL32(110, 110, 110, 255));
		}

		void Press(Key a_key, std::uint32_t a_target, const RemoteView& a_view)
		{
			auto& mgr = Manager::Get();
			switch (a_key) {
			case Key::kRewind:
				mgr.RemoteSeek(a_target, -10.0f);
				break;
			case Key::kForward:
				mgr.RemoteSeek(a_target, +10.0f);
				break;
			case Key::kPlayPause:
				Post(a_target, [](RE::TESObjectREFR* a_ref) { Manager::Get().RemotePlayPause(a_ref); });
				break;
			case Key::kUp:
				mgr.RemoteSetVolume(a_target, std::min(2.0f, std::round((a_view.volume + 0.1f) * 10.0f) / 10.0f));
				if (a_view.muted) {
					mgr.RemoteSetMuted(a_target, false);
				}
				break;
			case Key::kDown:
				mgr.RemoteSetVolume(a_target, std::max(0.0f, std::round((a_view.volume - 0.1f) * 10.0f) / 10.0f));
				break;
			case Key::kLeft:
				Post(a_target, [](RE::TESObjectREFR* a_ref) { Manager::Get().RemoteStep(a_ref, -1); });
				break;
			case Key::kRight:
				Post(a_target, [](RE::TESObjectREFR* a_ref) { Manager::Get().RemoteStep(a_ref, +1); });
				break;
			case Key::kCenter:
				mgr.RemoteSetMuted(a_target, !a_view.muted);
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
			case Key::kOpacity:
				{
					std::size_t i = 0;
					while (i < kOpacitySteps.size() && kOpacitySteps[i] > g_opacity + 0.01f) {
						++i;
					}
					g_opacity = kOpacitySteps[(i + 1) % kOpacitySteps.size()];
					SaveState();
				}
				break;
			default:
				break;
			}
		}

		void __stdcall RenderWindow()
		{
			const auto target = g_target.load();
			const auto view = Manager::Get().View(target);
			auto*      viewport = GetMainViewport();
			const float scale = viewport->Size.y * 0.5f / kDesignH;
			const float w = kDesignW * scale, h = kDesignH * scale;

			LoadState();
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
					if (hover == Key::kNone && !g_locked) {
						g_dragging = true;  // grabbed an empty spot: move the remote
						g_grab = ImVec2{ mouse.x - g_pos.x, mouse.y - g_pos.y };
					} else {
						Press(hover, target, view);
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

				// the video list under the name bar (search key / click the name): scroll and pick
				if (g_listOpen) {
					std::scoped_lock lock(g_lock);
					SetCursorScreenPos(ImVec2{ origin.x + kBarX0 * scale, origin.y + h + 6.0f });
					const float rows = static_cast<float>(std::min<std::size_t>(g_videoNames.size(), 10));
					const float listH = std::max(1.0f, rows) * (GetFontSize() + 8.0f) + 12.0f;
					if (BeginChild("##RealVideosList", ImVec2{ (kBarX1 - kBarX0) * scale, listH }, 0, 0)) {
						if (g_videoNames.empty()) {
							TextDisabled("No videos in the video folder");
						}
						for (std::size_t i = 0; i < g_videoNames.size(); ++i) {
							const bool current = view.playing && !view.testPattern &&
							                     _stricmp(Narrow(std::filesystem::path(g_videoFiles[i]).filename().wstring()).c_str(), view.file.c_str()) == 0;
							PushID(static_cast<int>(i));
							if (Selectable(g_videoNames[i].c_str(), current)) {
								Post(target, [file = g_videoFiles[i]](RE::TESObjectREFR* a_ref) { Manager::Get().RemotePlayFile(a_ref, file); });
							}
							PopID();
							if (current && g_scrollToCurrent) {
								SetScrollHereY(0.5f);
								g_scrollToCurrent = false;
							}
						}
					}
					EndChild();
					Dummy(ImVec2{ w, 8.0f });
				}

				if (IsKeyPressed(ImGuiKey_Escape, false)) {
					open = false;
				}
			}
			End();
			PopStyleVar(3);
			PopStyleColor(3);
			if (!open && g_window) {
				g_window->IsOpen = false;
			}
		}

		// ---- settings page in the framework's menu ----

		void Tip(const char* a_text)
		{
			if (IsItemHovered()) {
				SetTooltip("%s", a_text);
			}
		}

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

		void __stdcall RenderSettings()
		{
			LoadState();
			auto&       s = Settings::Get();
			static bool dirty = false;

			SeparatorText("Playback");
			dirty |= Checkbox("Loop videos", &s.loop);
			dirty |= Checkbox("Pause in menus (Esc, Pip-Boy, loading...)", &s.pauseInMenus);
			dirty |= Checkbox("Pause when the game is in the background (alt-tab)", &s.pauseUnfocused);
			dirty |= SliderInt("Screens playing at once", &s.maxScreens, 1, 16, "%d");

			SeparatorText("Picking a screen");
			dirty |= Checkbox("Use the screen under the crosshair", &s.aimToSelect);
			Tip("Off: hotkeys and the remote use the object clicked in the console.");
			dirty |= Meters("How far you can aim", &s.aimDistance, 400.0f, "Screens further away than this aren't picked by aiming.");
			dirty |= Checkbox("Only TVs, projectors and the cinema", &s.onlySupportedScreens);
			Tip("Off: F9/F10 work on any object (posters, signs...).");

			SeparatorText("Picture");
			bool bars = s.aspectMode == 1;
			if (Checkbox("Keep the video's shape (black bars)", &bars)) {
				s.aspectMode = bars ? 1 : 0;
				dirty = true;
			}
			Tip("Off: stretch every video to fill its screen. Applies when a video starts (or press Apply below).");
			dirty |= Checkbox("Cinema: stretch to fill the screen", &s.bigScreenStretch);
			dirty |= Percent("Cinema brightness", &s.bigScreenBrightness, 2.0f, "100% = as is. Applies right away.");
			dirty |= Percent("Cinema VHS look", &s.bigScreenVHS, 1.0f, "Soft picture, colour bleed, faded colours, faint scanlines. 0% = off.");
			dirty |= Percent("TV brightness (glowing screens)", &s.brightness, 3.0f, "Brightness of TVs and other glowing screens.");
			dirty |= Checkbox("Projectors: draw the video only inside the screen", &s.composite);
			Tip("Off: the video covers the whole mesh, frame included.");
			dirty |= Checkbox("Two-sided screens: show on both sides", &s.bothSides);
			dirty |= Checkbox("Stop the screen's own animation while playing", &s.stopScreenAnimation);
			dirty |= Checkbox("Hide glowing layers over the screen", &s.hideOverlays);
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
			dirty |= Percent("Master volume", &s.volume, 2.0f, "Every screen.");
			dirty |= Percent("Volume a screen starts at", &s.startVolume, 2.0f, "What the remote's middle button starts at.");
			dirty |= Meters("TVs/projectors: full volume within", &s.minDistance, 50.0f, "");
			dirty |= Meters("TVs/projectors: silent beyond", &s.maxDistance, 150.0f, "");
			if (s.maxDistance < s.minDistance + 1.0f) {
				s.maxDistance = s.minDistance + 1.0f;
			}
			dirty |= Meters("Cinema: full volume within", &s.bigMinDistance, 150.0f, "");
			dirty |= Meters("Cinema: silent beyond", &s.bigMaxDistance, 600.0f, "");
			if (s.bigMaxDistance < s.bigMinDistance + 1.0f) {
				s.bigMaxDistance = s.bigMinDistance + 1.0f;
			}
			dirty |= SliderFloat("Fade with distance", &s.falloffCurve, 0.5f, 4.0f, "%.1f");
			Tip("1 = fades evenly, 2 = smooth (default), higher = quieter sooner.");

			SeparatorText("Remote");
			{
				float opacity = g_opacity * 100.0f;
				if (SliderFloat("Remote opacity", &opacity, 20.0f, 100.0f, "%.0f%%")) {
					g_opacity = opacity / 100.0f;
					SaveState();
				}
				if (Checkbox("Lock the remote in place", &g_locked)) {
					SaveState();
				}
				if (Button("Move the remote back to the right side")) {
					g_havePos = false;
					::WritePrivateProfileStringA("Remote", "X", nullptr, kStateIni);
					::WritePrivateProfileStringA("Remote", "Y", nullptr, kStateIni);
				}
			}
			TextDisabled("Keys: remote F6, play/next F9, test pattern F10, stop F11, mesh F7, log F8 (change them in RealVideos.ini)");

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
		// While the remote is open: give the keyboard back to the game for walking (the menu framework tells the
		// game to ignore keyboard and mouse), but switch off looking, attacking, activating, menus and the rest
		// with an input-enable layer, so the mouse only drives the remote.
		void GameInputForRemote(bool a_open)
		{
			using UE = RE::UserEvents::USER_EVENT_FLAG;
			static RE::BSTSmartPointer<RE::BSInputEnableLayer> layer;
			static bool                                        blocked = false;
			auto*                                              inputs = RE::BSInputEnableManager::GetSingleton();
			if (!inputs) {
				return;
			}
			if (!layer) {
				inputs->AllocateNewLayer(layer, "RealVideos remote");
			}
			const auto flags = static_cast<UE>(static_cast<std::int32_t>(UE::kLooking) | static_cast<std::int32_t>(UE::kActivate) |
											   static_cast<std::int32_t>(UE::kMenu) | static_cast<std::int32_t>(UE::kConsole) |
											   static_cast<std::int32_t>(UE::kPOVSwitch) | static_cast<std::int32_t>(UE::kFighting) |
											   static_cast<std::int32_t>(UE::kSneaking) | static_cast<std::int32_t>(UE::kMainFour) |
											   static_cast<std::int32_t>(UE::kWheelZoom) | static_cast<std::int32_t>(UE::kJumping) |
											   static_cast<std::int32_t>(UE::kVATS));
			if (a_open) {
				if (layer && !blocked) {
					inputs->EnableUserEvent(layer->layerID, flags, false, RE::UserEvents::SENDER_ID::kGameplay);
					blocked = true;
				}
				if (auto* controls = RE::ControlMap::GetSingleton()) {
					controls->ignoreKeyboardMouse = false;  // the framework set it when the remote opened
				}
			} else if (layer && blocked) {
				inputs->EnableUserEvent(layer->layerID, flags, true, RE::UserEvents::SENDER_ID::kGameplay);
				blocked = false;
			}
		}
	}

	void Tick()
	{
		static bool active = false;
		const bool  open = g_window && g_window->IsOpen.load();
		auto*       tasks = F4SE::GetTaskInterface();
		if (!tasks || (!open && !active)) {
			return;
		}
		active = open;
		tasks->AddTask([open]() { GameInputForRemote(open); });
	}

	void Register()
	{
		if (!F4SEMenuFramework::IsInstalled()) {
			log::info("F4SE Menu Framework not installed: the remote (F6) is unavailable, hotkeys still work");
			return;
		}
		F4SEMenuFramework::SetSection("Real Videos");
		F4SEMenuFramework::AddSectionItem("Now playing", RenderPage);
		F4SEMenuFramework::AddSectionItem("Settings", RenderSettings);
		g_window = F4SEMenuFramework::AddWindow(RenderWindow, true);
		log::info("remote registered with F4SE Menu Framework{}", g_window ? "" : " (window could not be created)");
	}

	void Toggle()
	{
		if (!g_window) {
			Notify("Real Videos: the remote needs F4SE Menu Framework");
			return;
		}
		if (g_window->IsOpen) {
			g_window->IsOpen = false;
			return;
		}
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
		RefreshVideoList();
		{
			std::scoped_lock lock(g_lock);
			g_targetName = Screen::RefName(ref.get());
		}
		g_target = ref->GetFormID();
		g_window->IsOpen = true;
	}
}
