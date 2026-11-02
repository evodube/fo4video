#pragma once

namespace fv::Renderer
{
	// Hooks IDXGISwapChain::Present on the game's swap chain. Safe to call more than once.
	bool Install();
}
