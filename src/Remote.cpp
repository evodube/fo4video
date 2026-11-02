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

		void __stdcall RenderWindow()
		{
			const auto target = g_target.load();
			auto*      viewport = GetMainViewport();
			SetNextWindowPos(ImVec2{ viewport->Pos.x + viewport->Size.x * 0.5f, viewport->Pos.y + viewport->Size.y * 0.72f }, ImGuiCond_Appearing,
				ImVec2{ 0.5f, 0.5f });

			bool open = true;
			if (Begin("Real Videos Remote##RealVideos", &open, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize)) {
				{
					std::scoped_lock lock(g_lock);
					Text("%s", g_targetName.c_str());
				}
				Separator();
				Controls(target, true);
				Separator();
				if (Button("Close") || IsKeyPressed(ImGuiKey_Escape, false)) {
					open = false;
				}
			}
			End();
			if (!open && g_window) {
				g_window->IsOpen = false;
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

	void Register()
	{
		if (!F4SEMenuFramework::IsInstalled()) {
			log::info("F4SE Menu Framework not installed: the remote (F6) is unavailable, hotkeys still work");
			return;
		}
		F4SEMenuFramework::SetSection("Real Videos");
		F4SEMenuFramework::AddSectionItem("Now playing", RenderPage);
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
