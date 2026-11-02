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
		std::wstring videoFolder{ L"Data\\Videos" };
		bool         loop{ true };
		bool         pauseInMenus{ false };
		int          maxScreens{ 8 };

		// [Screen]
		std::vector<std::string> screenNames{ "screen", "display", "monitor", "crt", "tube" };
		std::string              forceNode;
		int                      srgb{ -1 };  // -1 auto (match original texture), 0 linear, 1 sRGB
		bool                     resetUV{ true };
		float                    brightness{ 1.0f };
		std::string              placeholderPrefix;  // empty = try "Textures\\" then ""

		// [Audio]
		bool  enableAudio{ true };
		float volume{ 1.0f };
		float minDistance{ 150.0f };
		float maxDistance{ 1500.0f };

		void Load();

		static Settings& Get()
		{
			static Settings s;
			return s;
		}
	};
}
