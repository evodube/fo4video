#pragma once

namespace fv::Remote
{
	// Registers the remote window and the "Now playing" page with F4SE Menu Framework (if it is installed).
	// Call after all plugins are loaded. Safe to call repeatedly (does nothing once registered);
	// a_when only goes into the log.
	void Register(const char* a_when);

	// Main thread: open the remote for the screen under the crosshair (or the console pick), or close it.
	void Toggle();

	// Render thread, every frame: walk with W/A/S/D while the remote is open (the menu blocks the game's input).
	void Tick();

	// Main thread: open the small video menu for a video poster (Contraptions' powered posters), activated with E / A
	void OpenPoster(RE::TESObjectREFR* a_ref);
}
