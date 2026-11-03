#pragma once

namespace fv
{
	struct Settings
	{
		// [General]
		bool         enableHotkeys{ true };
		int          keyPlay{ VK_F9 };
		int          keyTestPattern{ VK_F10 };
		int          keyStop{ VK_F11 };
		int          keyDump{ VK_F8 };
		int          keyNextMesh{ VK_F7 };  // Shift+key flips the picture
		std::wstring videoFolder{ L"Data\\Videos" };
		bool         loop{ true };
		bool         pauseInMenus{ true };
		bool         pauseUnfocused{ true };
		int          maxScreens{ 8 };

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
		bool                     onlySupportedScreens{ true };
		bool                     aimToSelect{ true };     // hotkeys and the remote use the screen under the crosshair
		float                    aimDistance{ 15000.0f };
		int                      keyRemote{ 0x75 };       // F6: open/close the remote  // F9/F10 only on TVs, VotW projectors/screens, the drive-in          // glowing TV screens: whole video, mesh UVs untouched (0.1 behaviour)
		float                    nearbyRadius{ 15000.0f };    // projectors / Ctrl+F7 look for screens this close
		float                    brightness{ 1.0f };
		std::string              placeholderPrefix;  // empty = try "Textures\\" then ""

		// [Audio]
		bool  enableAudio{ true };
		float volume{ 1.0f };
		float minDistance{ 150.0f };
		float maxDistance{ 1500.0f };
		float falloffCurve{ 2.0f };
		float bigScreenRadius{ 500.0f };   // screens this big (mesh radius) use the big-screen distances
		bool  bigScreenStretch{ true };    // big screens: stretch the video to fill them (no black bars)
		float bigScreenBrightness{ 1.15f };  // big screens (the drive-in): picture brightness
		bool  selfLit{ true };               // projector & cinema screens glow like TVs (same brightness day/night, any direction)
		float selfLitBrightness{ 1.0f };
		bool  selfLitLatePass{ true };
		bool  selfLitFaceOnly{ true };       // only the picture glows (a glowing copy over it); frame keeps normal lighting
		float frameDay{ 2.0f };              // glowing projectors: back/frame/pole = the world's light times this by day
		float frameNight{ 1.0f };            // ... and this at night (blended through dawn and dusk)
		float bigScreenVHS{ 0.5f };          // big screens: VHS tape look, 0 = off, 1 = strong
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
