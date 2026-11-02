#pragma once

namespace fv
{
	// Scene-graph side: finds a TV's screen geometry and points its material at one of our slot textures.
	// Everything here must run on the game's main thread.
	struct ScreenBinding
	{
		RE::NiPointer<RE::NiAVObject> root;
		RE::NiPointer<RE::BSGeometry> geometry;
		RE::BSShaderMaterial*         material{ nullptr };  // unique clone owned by the shader property
		RE::NiPointer<RE::NiTexture>  originalBase;
		RE::NiPointer<RE::NiTexture>  originalGlow;
		bool                          isEffect{ false };
		bool                          hadGlow{ false };
		RE::NiColorA                  originalBaseColor{};
		float                         originalBaseColorScale{ 1.0f };
		RE::NiPoint2                  originalUVOffset[2]{};
		RE::NiPoint2                  originalUVScale[2]{};
		bool                          bound{ false };
		bool                          originalWasSRGB{ false };
	};

	namespace Screen
	{
		// Loads (once) the placeholder texture for a slot: Data\Textures\fo4video\slotNN.dds
		RE::NiPointer<RE::NiTexture> LoadSlotTexture(int a_slot);

		// True if the texture's renderer data is one of our 4x8 placeholders (safety check before patching it).
		bool IsPlaceholderTexture(RE::NiTexture* a_tex);

		RE::BSGeometry* FindScreenGeometry(RE::NiAVObject* a_root);

		bool Bind(RE::TESObjectREFR* a_ref, RE::NiTexture* a_slotTexture, ScreenBinding& a_binding);
		void Unbind(ScreenBinding& a_binding);

		// Writes the ref's geometry tree (names, shader types, textures) to the log.
		void Dump(RE::TESObjectREFR* a_ref);

		std::string RefName(RE::TESObjectREFR* a_ref);
	}
}
