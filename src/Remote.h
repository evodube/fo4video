#pragma once

namespace fv::Remote
{
	// Registers the remote window and the "Now playing" page with F4SE Menu Framework (if it is installed).
	// Call after all plugins are loaded (kPostPostLoad).
	void Register();

	// Main thread: open the remote for the screen under the crosshair (or the console pick), or close it.
	void Toggle();
}
