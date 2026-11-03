#pragma once

namespace fv
{
	// Per-model screen choice, saved in Data\F4SE\Plugins\RealVideos_Screens.ini
	struct ScreenChoice
	{
		int         index{ -1 };  // candidate index, -1 = automatic
		std::string name;         // geometry name at that index (sanity check)
		int         flip{ 0 };    // bit 0 = flip vertical, bit 1 = mirror, bit 2 = rotate 90, bit 3 = other side
		std::string owner;        // model key of the object that owns the mesh (may be a nearby object)
	};

	// Scene-graph side: finds a TV's screen geometry and points its material at one of our slot textures.
	// Everything here must run on the game's main thread unless noted.
	struct ScreenBinding
	{
		RE::ObjectRefHandle                 owner;  // object whose mesh we drew on (the selected one, or one next to it)
		std::string                         ownerKey;
		bool                                ownerIsNearby{ false };
		RE::NiPointer<RE::NiAVObject>       root;   // owner's 3D
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
		float                               screenAspect{ 0.0f };  // width / height of the screen face, 0 = unknown
		bool                                overrideUV{ false };   // we remap the mesh UVs (whole-mesh mode)
		bool                                composite{ false };    // video drawn only into faceRects of the original texture
		std::array<float, 4>                faceRects[2]{};        // u0, v0, u1, v1
		float                               faceAspect[2]{};       // width/height of each face as seen from the front
		int                                 faceOrient[2]{};       // bit 0 flip vertical, bit 1 mirror, bit 2 rotated 90
		int                                 faceCount{ 0 };
		std::uint64_t                       originalFlags{ 0 };
		bool                                flagsChanged{ false };
		std::vector<RE::NiPointer<RE::NiAVObject>> hiddenLayers;
		std::vector<bool>                   hiddenLayerWasCulled;  // parallel to hiddenLayers: restore exactly
		bool                                classic{ false };       // TV screen: whole texture, untouched UVs (like 0.1)
		bool                                twoSidedCleared{ false };  // we stopped drawing the back of the sheet
		bool                                bigScreen{ false };        // the drive-in: own brightness / VHS look
		// self-lit: a lit (projector) screen temporarily drawn with a glowing effect shader, like a TV
		bool                                selfLit{ false };
		RE::NiPointer<RE::NiProperty>       originalProp;   // the screen's own lighting shader property
		RE::NiPointer<RE::NiProperty>       effectProp;     // ours
		RE::BSShaderMaterial*               effectMaterial{ nullptr };
		// while glowing the screen is drawn in the late (blended) pass, after the world, so the scaffolding
		// behind it can't paint over the picture
		bool                                alphaSwapped{ false };
		RE::NiPointer<RE::NiProperty>       originalAlpha;  // whatever alpha property the mesh had (often none)
		RE::NiPointer<RE::NiProperty>       ourAlpha;
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

		// Glow strength of a self-lit screen (1 = the video as is). Render thread; does nothing on lit screens.
		void SetGlow(const ScreenBinding& a_binding, float a_scale);

		// False if the bound mesh was detached, its material replaced, or the 3D rebuilt.
		bool StillValid(const ScreenBinding& a_binding);

		// Main thread: re-hide overlay layers that the object's own animation turned back on.
		void KeepOverlaysHidden(ScreenBinding& a_binding);

		// Render thread: re-apply our UV mapping (cheap; undoes any UV animation from other mods).
		void ReapplyUV(const ScreenBinding& a_binding);

		int  CountCandidates(RE::TESObjectREFR* a_ref);     // own meshes + nearby screens
		int  CountOwnCandidates(RE::TESObjectREFR* a_ref);  // own meshes only (they come first)
		void SaveChoice(RE::TESObjectREFR* a_ref, const ScreenChoice& a_choice);
		ScreenChoice LoadChoice(RE::TESObjectREFR* a_ref);

		// Writes the ref's geometry tree (names, shader types, textures) to the log.
		void Dump(RE::TESObjectREFR* a_ref);

		std::string RefName(RE::TESObjectREFR* a_ref);

		// True for objects Real Videos plays on: TVs (a ScreenAnim mesh), Videos of the Wasteland projectors and
		// screens, and the drive-in cinema (screen and projector). a_what gets a short description for the log.
		bool IsSupported(RE::TESObjectREFR* a_ref, std::string* a_what = nullptr);

		// The nearest supported screen (see IsSupported) under the crosshair, within a_maxDistance. Main thread.
		RE::TESObjectREFR* PickAimed(float a_maxDistance);
	}
}
