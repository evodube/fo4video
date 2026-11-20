#pragma once

// Which of the game's menus are open, kept from the menu open/close events.
//
// Asking the UI with GetMenuOpen() copies a reference to the menu itself. Done every few frames (Maintain) and every
// frame from Present (the pause check), sooner or later that copy is the last one when the game closes the menu, and
// the menu is then torn down on our thread instead of the UI's: the workshop menu's Scaleform pieces get freed under
// the UI, which leaves its item icons stuck on screen after build mode (or crashes on the way out). These flags never
// touch the menus.
namespace fv::menus
{
	enum Id : std::size_t
	{
		kPause,
		kPipboy,
		kLoading,
		kMain,
		kWorkshop,
		kCount
	};

	// starts listening (once; safe to call again) and reads where things stand right now
	void Watch();

	[[nodiscard]] bool Open(Id a_id) noexcept;
}
