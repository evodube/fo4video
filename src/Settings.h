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
		bool                     onlySupportedScreens{ true };  // F9/F10 only on TVs, VotW projectors/screens, the drive-in          // glowing TV screens: whole video, mesh UVs untouched (0.1 behaviour)
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
		float bigMinDistance{ 1500.0f };
		float bigMaxDistance{ 12000.0f };

		void Load();

		static Settings& Get()
		{
			static Settings s;
			return s;
		}
	};
}
