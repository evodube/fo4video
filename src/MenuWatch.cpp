#include "MenuWatch.h"

namespace fv::menus
{
	namespace
	{
		constexpr std::array<std::string_view, kCount> kNames{ "PauseMenu", "PipboyMenu", "LoadingMenu", "MainMenu", "WorkshopMenu" };

		std::array<std::atomic<bool>, kCount> g_open{};

		// (CommonLibF4 only declares it) the game's own layout: the menu's name, and whether it's opening
		struct EventData
		{
			RE::BSFixedString menuName;
			bool              opening;
		};

		struct Sink final : RE::BSTEventSink<RE::MenuOpenCloseEvent>
		{
			RE::BSEventNotifyControl ProcessEvent(const RE::MenuOpenCloseEvent& a_event, RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override
			{
				const auto& e = reinterpret_cast<const EventData&>(a_event);
				const char* name = e.menuName.c_str();
				if (!name) {
					return RE::BSEventNotifyControl::kContinue;
				}
				for (std::size_t i = 0; i < kCount; ++i) {
					if (kNames[i] == name) {
						g_open[i] = e.opening;
						if (i == kWorkshop) {
							log::info("menus: WorkshopMenu {} (thread {})", e.opening ? "opened" : "closed", ::GetCurrentThreadId());
						}
						break;
					}
				}
				return RE::BSEventNotifyControl::kContinue;
			}
		};
	}

	void Watch()
	{
		static Sink sink;
		static bool registered = false;
		auto*       ui = RE::UI::GetSingleton();
		if (registered || !ui) {
			return;
		}
		registered = true;
		ui->RegisterSink<RE::MenuOpenCloseEvent>(&sink);
		// what's already open (the main menu, usually): asked once, here, then only the events
		for (std::size_t i = 0; i < kCount; ++i) {
			g_open[i] = ui->GetMenuOpen(RE::BSFixedString(kNames[i]));
		}
		log::info("menus: watching open/close events (the menus themselves are never asked again)");
	}

	bool Open(Id a_id) noexcept
	{
		return a_id < kCount && g_open[a_id].load(std::memory_order_relaxed);
	}
}
