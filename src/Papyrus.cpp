#include "Papyrus.h"

#include "Manager.h"
#include "Settings.h"

namespace fv::Papyrus
{
	namespace
	{
		constexpr auto kScript = "RealVideos"sv;

		std::wstring Widen(const std::string& a_str)
		{
			if (a_str.empty()) {
				return {};
			}
			const int    len = ::MultiByteToWideChar(CP_UTF8, 0, a_str.c_str(), static_cast<int>(a_str.size()), nullptr, 0);
			std::wstring out(static_cast<std::size_t>(len), L'\0');
			::MultiByteToWideChar(CP_UTF8, 0, a_str.c_str(), static_cast<int>(a_str.size()), out.data(), len);
			return out;
		}

		// Papyrus calls arrive on VM threads; all real work happens on the main thread.
		void OnMain(std::function<void()> a_fn)
		{
			if (auto* tasks = F4SE::GetTaskInterface()) {
				tasks->AddTask(std::move(a_fn));
			}
		}

		RE::ObjectRefHandle Handle(RE::TESObjectREFR* a_ref)
		{
			return a_ref ? a_ref->GetHandle() : RE::ObjectRefHandle{};
		}

		bool Play(std::monostate, RE::TESObjectREFR* a_screen, std::string a_file, bool a_loop)
		{
			if (!a_screen || a_file.empty()) {
				return false;
			}
			const auto path = Manager::Get().ResolveVideoPath(Widen(a_file));
			if (!std::filesystem::exists(path)) {
				log::warn("Papyrus Play: file not found '{}'", a_file);
				return false;
			}
			OnMain([h = Handle(a_screen), path, a_loop]() {
				if (auto ref = h.get()) {
					Manager::Get().Play(ref.get(), path, a_loop);
				}
			});
			return true;
		}

		bool PlayTestPattern(std::monostate, RE::TESObjectREFR* a_screen)
		{
			if (!a_screen) {
				return false;
			}
			OnMain([h = Handle(a_screen)]() {
				if (auto ref = h.get()) {
					Manager::Get().PlayTestPattern(ref.get());
				}
			});
			return true;
		}

		void Stop(std::monostate, RE::TESObjectREFR* a_screen)
		{
			OnMain([h = Handle(a_screen)]() {
				if (auto ref = h.get()) {
					Manager::Get().Stop(ref.get());
				}
			});
		}

		void StopAll(std::monostate)
		{
			OnMain([]() { Manager::Get().StopAll(); });
		}

		bool IsPlaying(std::monostate, RE::TESObjectREFR* a_screen)
		{
			return Manager::Get().IsPlaying(a_screen);
		}

		void SetVolume(std::monostate, RE::TESObjectREFR* a_screen, float a_volume)
		{
			Manager::Get().SetVolume(a_screen, a_volume);
		}

		void DumpNodes(std::monostate, RE::TESObjectREFR* a_screen)
		{
			OnMain([h = Handle(a_screen)]() {
				if (auto ref = h.get()) {
					Screen::Dump(ref.get());
				}
			});
		}

		std::int32_t GetVideoCount(std::monostate)
		{
			return static_cast<std::int32_t>(Manager::Get().ListVideos().size());
		}
	}

	bool Register(RE::BSScript::IVirtualMachine* a_vm)
	{
		a_vm->BindNativeMethod(kScript, "Play"sv, Play, true);
		a_vm->BindNativeMethod(kScript, "PlayTestPattern"sv, PlayTestPattern, true);
		a_vm->BindNativeMethod(kScript, "Stop"sv, Stop, true);
		a_vm->BindNativeMethod(kScript, "StopAll"sv, StopAll, true);
		a_vm->BindNativeMethod(kScript, "IsPlaying"sv, IsPlaying, true);
		a_vm->BindNativeMethod(kScript, "SetVolume"sv, SetVolume, true);
		a_vm->BindNativeMethod(kScript, "DumpNodes"sv, DumpNodes, true);
		a_vm->BindNativeMethod(kScript, "GetVideoCount"sv, GetVideoCount, true);
		log::info("Papyrus functions registered on script {}", kScript);
		return true;
	}
}
