#pragma once

namespace fv
{
	struct Settings
	{
		// old cinema film look, each part 0 (off) .. 1
		struct Film
		{
			float grain{ 0.0f };
			float scratches{ 0.0f };  // scratches and dust
			float flicker{ 0.0f };
			float vignette{ 0.0f };   // dark corners
			float sepia{ 0.0f };      // faded, warm colours
			float weave{ 0.0f };      // the frame shaking in the projector gate
			float softness{ 0.0f };

			[[nodiscard]] bool Any() const { return grain + scratches + flicker + vignette + sepia + weave + softness > 0.001f; }
			// the shader's layout: grain, scratches, flicker, vignette, sepia, weave, softness, on
			void Pack(float (&a_out)[8]) const
			{
				a_out[0] = grain;
				a_out[1] = scratches;
				a_out[2] = flicker;
				a_out[3] = vignette;
				a_out[4] = sepia;
				a_out[5] = weave;
				a_out[6] = softness;
				a_out[7] = Any() ? 1.0f : 0.0f;
			}
		};

		// [General]
		bool         enableHotkeys{ true };
		int          keyPlay{ VK_F9 };
		int          keyTestPattern{ VK_F10 };
		int          keyStop{ VK_F11 };
		int          keyDump{ VK_F8 };
		int          keyNextMesh{ VK_F7 };  // Shift+key flips the picture
		std::wstring videoFolder{ L"Data\\Videos" };
		bool         loop{ true };
		bool         shuffle{ false };  // next / previous / power-on / end of a video: a random video from the same folder
		bool         pauseInMenus{ true };
		bool         pauseUnfocused{ true };
		int          maxScreens{ 10 };

		// [Screen]
		std::vector<std::string> screenNames{ "screen", "display", "monitor", "crt" };
		std::string              forceNode;
		int                      srgb{ -1 };  // -1 auto (match original texture), 0 linear, 1 sRGB
		bool                     fitUV{ true };
		bool                     stopScreenAnimation{ true };
		int                      aspectMode{ 1 };            // 0 = stretch, 1 = keep aspect (black bars)
		bool                     composite{ true };          // draw the video only into the screen part of a shared texture
		bool                     bothSides{ false };
		bool                     hideOverlays{ true };       // hide glowing layers / film stills drawn over the screen while playing
		bool                     classicTV{ true };
		bool                     tvClean{ true };  // TVs: hide the TV's static / glass layers and drop its palette tint while playing
		bool                     onlySupportedScreens{ true };
		bool                     aimToSelect{ true };     // hotkeys and the remote use the screen under the crosshair
		float                    aimDistance{ 14000.0f };  // ~200 m
		bool                     controller{ true };  // a game controller: L3 + R3 opens the remote, buttons work it
		int                      keyRemote{ 0x75 };       // F6: open/close the remote  // F9/F10 only on TVs, VotW projectors/screens, the drive-in          // glowing TV screens: whole video, mesh UVs untouched (0.1 behaviour)
		float                    nearbyRadius{ 15000.0f };    // projectors / Ctrl+F7 look for screens this close
		float                    brightness{ 1.0f };
		std::string              placeholderPrefix;  // empty = try "Textures\\" then ""

		// [Audio]
		bool  enableAudio{ true };
		float volume{ 1.0f };
		bool  offInOtherSettlement{ false };  // (removed from the settings; kept off)
		float otherSettlementSeconds{ 5.0f };
		bool  autoOff{ true };               // screens you've walked away from turn off, and resume when you come back
		float autoOffDistance{ 7000.0f };    // further than this (game units, ~100 m; never closer than you can hear it)
		float autoOffSeconds{ 30.0f };       // ... for this long
		bool  directionalSound{ true };      // sound comes from the screen's side
		bool  syncSameVideo{ false };        // (removed from the settings; kept off)
		bool  showMessages{ true };          // "playing ... / ... off" messages at the top left
		float allRadius{ 10500.0f };         // "all screens here": within this (~150 m)
		bool  freeze{ true };                // picture stops updating when far or out of view (sound carries on)
		bool  freezeOutOfView{ true };       // ...also when out of view (behind you), not only when far away; all screens
		float freezeDistance{ 3500.0f };     // ~50 m
		float freezeSeconds{ 10.0f };
		// the cinema has its own pause / turn-off distances (it's seen and heard from much further away)
		bool  cinemaFreeze{ true };
		float cinemaFreezeDistance{ 10500.0f };     // ~150 m
		float cinemaFreezeSeconds{ 10.0f };
		bool  cinemaAutoOff{ true };
		float cinemaAutoOffDistance{ 21000.0f };    // ~300 m
		float cinemaAutoOffSeconds{ 30.0f };
		int   maxHeightTV{ 480 };            // decode videos no taller than this (0 = full size)
		int   maxHeightProjector{ 1080 };
		int   maxHeightCinema{ 1080 };
		bool  gpuDecode{ true };             // decode on the graphics card
		float minDistance{ 150.0f };
		float maxDistance{ 1500.0f };
		float falloffCurve{ 2.0f };
		float bigScreenRadius{ 500.0f };   // screens this big (mesh radius) use the big-screen distances
		bool  bigScreenStretch{ true };    // big screens: stretch the video to fill them (no black bars)
		bool  projectorStretch{ false };   // projectors: stretch the video to fill the screen (no black bars)
		bool  posterStretch{ true };       // video posters: stretch the video to fill the poster (no black bars)
		Film  cinemaFilm{ 0.25f, 0.25f, 0.2f, 0.35f, 0.3f, 0.15f, 0.2f };
		Film  projectorFilm{};
		Film  tvFilm{};  // TVs' own old film look (all 0 = off)
		float bigScreenBrightness{ 1.15f };  // big screens (the drive-in): picture brightness
		bool  selfLit{ true };               // projector & cinema screens glow like TVs (same brightness day/night, any direction)
		bool  selfLitLatePass{ true };
		bool  selfLitFaceOnly{ true };       // only the picture glows (a glowing copy over it); frame keeps normal lighting
		bool  glowMap{ true };               // projectors: lit screen + glowing picture (glow map) instead of the glowing shader
		float frameLight{ 1.0f };            // fallback glow (no glow map possible): back/frame/pole follow the world's light
		float bigScreenVHS{ 0.0f };  // (replaced by the old film look)          // big screens: VHS tape look, 0 = off, 1 = strong
		float startVolume{ 0.5f };           // volume each screen starts at (the remote's middle button)
		float bigMinDistance{ 1500.0f };
		float bigMaxDistance{ 12000.0f };

		void Load();
		void Save() const;  // writes everything the settings page can change back to RealVideos.ini

		static Settings& Get()
		{
			static Settings s;
			return s;
		}
	};
}
