#pragma once

namespace fv
{
	// Per-model screen choice, saved in Data\F4SE\Plugins\RealVideos_Screens.ini
	struct ScreenChoice
	{
		int         index{ -1 };  // candidate index, -1 = automatic
		std::string name;         // geometry name at that index (sanity check)
		int         flip{ 0 };    // bit 0 = flip vertical, bit 1 = mirror horizontal
	};

	// Scene-graph side: finds a TV's screen geometry and points its material at one of our slot textures.
	// Everything here must run on the game's main thread unless noted.
	struct ScreenBinding
	{
		RE::NiPointer<RE::NiAVObject>       root;
		RE::NiPointer<RE::BSGeometry>       geometry;
		RE::BSShaderMaterial*               material{ nullptr };  // unique clone; we hold one intrusive ref
		RE::NiPointer<RE::NiTexture>        slotTexture;
		RE::NiPointer<RE::NiTexture>        originalBase;
		RE::NiPointer<RE::NiTexture>        originalGlow;
		RE::NiPointer<RE::NiTimeController> savedControllers;
		bool                                isEffect{ false };
		bool                                hadGlow{ false };
		RE::NiColorA                        originalBaseColor{};
		float                               originalBaseColorScale{ 1.0f };
		RE::NiPoint2                        originalUVOffset[2]{};
		RE::NiPoint2                        originalUVScale[2]{};
		RE::NiPoint2                        uvOffset{ 0.0f, 0.0f };  // what we keep writing (fights UV animation)
		RE::NiPoint2                        uvScale{ 1.0f, 1.0f };
		int                                 candidateIndex{ -1 };
		int                                 candidateCount{ 0 };
		int                                 flip{ 0 };
		std::string                         geometryName;
		bool                                bound{ false };
		bool                                originalWasSRGB{ false };
	};

	namespace Screen
	{
		// Loads (once) the placeholder texture for a slot: Data\Textures\RealVideos\slotNN.dds
		RE::NiPointer<RE::NiTexture> LoadSlotTexture(int a_slot);

		// True if the texture's renderer data is one of our 4x8 placeholders (safety check before patching it).
		bool IsPlaceholderTexture(RE::NiTexture* a_tex);

		// a_forceIndex >= 0 picks that candidate mesh; otherwise saved choice / INI / heuristics.
		bool Bind(RE::TESObjectREFR* a_ref, RE::NiTexture* a_slotTexture, ScreenBinding& a_binding, int a_forceIndex = -1);
		void Unbind(ScreenBinding& a_binding);

		// False if the bound mesh was detached, its material replaced, or the 3D rebuilt.
		bool StillValid(const ScreenBinding& a_binding, RE::NiAVObject* a_currentRoot);

		// Render thread: re-apply our UV mapping (cheap; undoes any UV animation from other mods).
		void ReapplyUV(const ScreenBinding& a_binding);

		int  CountCandidates(RE::TESObjectREFR* a_ref);
		void SaveChoice(RE::TESObjectREFR* a_ref, const ScreenChoice& a_choice);
		ScreenChoice LoadChoice(RE::TESObjectREFR* a_ref);

		// Writes the ref's geometry tree (names, shader types, textures) to the log.
		void Dump(RE::TESObjectREFR* a_ref);

		std::string RefName(RE::TESObjectREFR* a_ref);
	}
}
