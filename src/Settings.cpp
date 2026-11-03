#include "Settings.h"

namespace fv
{
	namespace
	{
		constexpr auto kIniPath = L"Data\\F4SE\\Plugins\\RealVideos.ini";

		std::wstring ReadString(const wchar_t* a_section, const wchar_t* a_key, const std::wstring& a_default)
		{
			wchar_t buf[1024]{};
			::GetPrivateProfileStringW(a_section, a_key, a_default.c_str(), buf, static_cast<DWORD>(std::size(buf)), kIniPath);
			std::wstring value = buf;
			// strip inline "; comment" and surrounding whitespace
			for (std::size_t i = 0; i < value.size(); ++i) {
				if (value[i] == L';' && (i == 0 || value[i - 1] == L' ' || value[i - 1] == L'\t')) {
					value.resize(i);
					break;
				}
			}
			while (!value.empty() && (value.back() == L' ' || value.back() == L'\t')) {
				value.pop_back();
			}
			return value;
		}

		std::string Narrow(const std::wstring& a_str)
		{
			if (a_str.empty()) {
				return {};
			}
			const int len = ::WideCharToMultiByte(CP_UTF8, 0, a_str.c_str(), static_cast<int>(a_str.size()), nullptr, 0, nullptr, nullptr);
			std::string out(static_cast<std::size_t>(len), '\0');
			::WideCharToMultiByte(CP_UTF8, 0, a_str.c_str(), static_cast<int>(a_str.size()), out.data(), len, nullptr, nullptr);
			return out;
		}

		int ReadInt(const wchar_t* a_section, const wchar_t* a_key, int a_default)
		{
			const auto str = ReadString(a_section, a_key, std::to_wstring(a_default));
			try {
				return static_cast<int>(std::stol(str, nullptr, 0));  // accepts 0x78 style
			} catch (...) {
				return a_default;
			}
		}

		float ReadFloat(const wchar_t* a_section, const wchar_t* a_key, float a_default)
		{
			const auto str = ReadString(a_section, a_key, std::to_wstring(a_default));
			try {
				return std::stof(str);
			} catch (...) {
				return a_default;
			}
		}

		bool ReadBool(const wchar_t* a_section, const wchar_t* a_key, bool a_default)
		{
			return ReadInt(a_section, a_key, a_default ? 1 : 0) != 0;
		}

		std::vector<std::string> SplitLower(const std::string& a_str)
		{
			std::vector<std::string> out;
			std::string              cur;
			auto                     flush = [&]() {
                while (!cur.empty() && cur.back() == ' ') cur.pop_back();
                if (!cur.empty()) out.push_back(cur);
                cur.clear();
			};
			for (const char c : a_str) {
				if (c == ',' || c == ';') {
					flush();
				} else if (!(cur.empty() && c == ' ')) {
					cur.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
				}
			}
			flush();
			return out;
		}
	}

	void Settings::Save() const
	{
		auto put = [](const wchar_t* a_section, const wchar_t* a_key, const std::wstring& a_value) {
			::WritePrivateProfileStringW(a_section, a_key, a_value.c_str(), kIniPath);
		};
		auto b = [](bool a_v) { return std::wstring(a_v ? L"1" : L"0"); };
		auto f = [](float a_v) { return std::format(L"{:.3g}", a_v); };
		auto i = [](int a_v) { return std::to_wstring(a_v); };

		put(L"General", L"bLoop", b(loop));
		put(L"General", L"bPauseInMenus", b(pauseInMenus));
		put(L"General", L"bPauseWhenAltTabbed", b(pauseUnfocused));
		put(L"General", L"iMaxScreens", i(maxScreens));
		put(L"General", L"bOnlySupportedScreens", b(onlySupportedScreens));
		put(L"General", L"bAimToSelect", b(aimToSelect));
		put(L"General", L"fAimDistance", f(aimDistance));

		put(L"Screen", L"iSRGB", i(srgb));
		put(L"Screen", L"iAspectMode", i(aspectMode));
		put(L"Screen", L"bStretchOnBigScreens", b(bigScreenStretch));
		put(L"Screen", L"fBigScreenBrightness", f(bigScreenBrightness));
		put(L"Screen", L"fBigScreenVHS", f(bigScreenVHS));
		put(L"Screen", L"bOnlyInsideScreenArea", b(composite));
		put(L"Screen", L"bShowOnBothSides", b(bothSides));
		put(L"Screen", L"bHideOverlays", b(hideOverlays));
		put(L"Screen", L"bStopScreenAnimation", b(stopScreenAnimation));
		put(L"Screen", L"fNearbyObjectRadius", f(nearbyRadius));
		put(L"Screen", L"fBrightness", f(brightness));
		put(L"Screen", L"bSelfLitScreens", b(selfLit));
		put(L"Screen", L"bSelfLitLatePass", b(selfLitLatePass));
		put(L"Screen", L"bGlowPictureOnly", b(selfLitFaceOnly));
		put(L"Screen", L"fFrameLight", f(frameLight));
		put(L"Screen", L"bGlowMap", b(glowMap));

		put(L"Audio", L"bEnableAudio", b(enableAudio));
		put(L"Audio", L"fVolume", f(volume));
		put(L"Audio", L"fStartVolume", f(startVolume));
		put(L"Audio", L"fMinDistance", f(minDistance));
		put(L"Audio", L"fMaxDistance", f(maxDistance));
		put(L"Audio", L"fFalloffCurve", f(falloffCurve));
		put(L"Audio", L"fBigScreenSize", f(bigScreenRadius));
		put(L"Audio", L"fBigScreenMinDistance", f(bigMinDistance));
		put(L"Audio", L"fBigScreenMaxDistance", f(bigMaxDistance));
		log::info("settings saved to RealVideos.ini");
	}

	void Settings::Load()
	{
		enableHotkeys = ReadBool(L"General", L"bEnableHotkeys", enableHotkeys);
		keyPlay = ReadInt(L"General", L"iKeyPlayNext", keyPlay);
		keyTestPattern = ReadInt(L"General", L"iKeyTestPattern", keyTestPattern);
		keyStop = ReadInt(L"General", L"iKeyStop", keyStop);
		keyDump = ReadInt(L"General", L"iKeyDumpNodes", keyDump);
		keyNextMesh = ReadInt(L"General", L"iKeyNextMesh", keyNextMesh);
		videoFolder = ReadString(L"General", L"sVideoFolder", videoFolder);
		loop = ReadBool(L"General", L"bLoop", loop);
		pauseInMenus = ReadBool(L"General", L"bPauseInMenus", pauseInMenus);
		pauseUnfocused = ReadBool(L"General", L"bPauseWhenAltTabbed", pauseUnfocused);
		maxScreens = std::clamp(ReadInt(L"General", L"iMaxScreens", maxScreens), 1, 16);
		onlySupportedScreens = ReadBool(L"General", L"bOnlySupportedScreens", onlySupportedScreens);
		aimToSelect = ReadBool(L"General", L"bAimToSelect", aimToSelect);
		aimDistance = std::max(100.0f, ReadFloat(L"General", L"fAimDistance", aimDistance));
		keyRemote = ReadInt(L"General", L"iKeyRemote", keyRemote);

		const auto names = Narrow(ReadString(L"Screen", L"sScreenNames", L""));
		if (!names.empty()) {
			screenNames = SplitLower(names);
		}
		forceNode = Narrow(ReadString(L"Screen", L"sForceNode", L""));
		srgb = ReadInt(L"Screen", L"iSRGB", srgb);
		fitUV = ReadBool(L"Screen", L"bFitVideoToScreen", fitUV);
		stopScreenAnimation = ReadBool(L"Screen", L"bStopScreenAnimation", stopScreenAnimation);
		aspectMode = std::clamp(ReadInt(L"Screen", L"iAspectMode", aspectMode), 0, 1);
		composite = ReadBool(L"Screen", L"bOnlyInsideScreenArea", composite);
		bothSides = ReadBool(L"Screen", L"bShowOnBothSides", bothSides);
		hideOverlays = ReadBool(L"Screen", L"bHideOverlays", hideOverlays);
		classicTV = ReadBool(L"Screen", L"bClassicTVScreens", classicTV);
		nearbyRadius = std::max(0.0f, ReadFloat(L"Screen", L"fNearbyObjectRadius", nearbyRadius));
		brightness = std::clamp(ReadFloat(L"Screen", L"fBrightness", brightness), 0.0f, 4.0f);
		placeholderPrefix = Narrow(ReadString(L"Screen", L"sPlaceholderPrefix", L""));

		enableAudio = ReadBool(L"Audio", L"bEnableAudio", enableAudio);
		volume = std::clamp(ReadFloat(L"Audio", L"fVolume", volume), 0.0f, 4.0f);
		minDistance = ReadFloat(L"Audio", L"fMinDistance", minDistance);
		maxDistance = std::max(ReadFloat(L"Audio", L"fMaxDistance", maxDistance), minDistance + 1.0f);
		falloffCurve = std::clamp(ReadFloat(L"Audio", L"fFalloffCurve", falloffCurve), 0.25f, 8.0f);
		bigScreenRadius = ReadFloat(L"Audio", L"fBigScreenSize", bigScreenRadius);
		bigScreenStretch = ReadBool(L"Screen", L"bStretchOnBigScreens", bigScreenStretch);
		selfLit = ReadBool(L"Screen", L"bSelfLitScreens", selfLit);
		selfLitLatePass = ReadBool(L"Screen", L"bSelfLitLatePass", selfLitLatePass);
		selfLitFaceOnly = ReadBool(L"Screen", L"bGlowPictureOnly", selfLitFaceOnly);
		frameLight = std::clamp(ReadFloat(L"Screen", L"fFrameLight", frameLight), 0.0f, 4.0f);
		glowMap = ReadBool(L"Screen", L"bGlowMap", glowMap);
		bigScreenBrightness = std::clamp(ReadFloat(L"Screen", L"fBigScreenBrightness", bigScreenBrightness), 0.1f, 4.0f);
		bigScreenVHS = std::clamp(ReadFloat(L"Screen", L"fBigScreenVHS", bigScreenVHS), 0.0f, 1.0f);
		startVolume = std::clamp(ReadFloat(L"Audio", L"fStartVolume", startVolume), 0.0f, 2.0f);
		bigMinDistance = ReadFloat(L"Audio", L"fBigScreenMinDistance", bigMinDistance);
		bigMaxDistance = std::max(ReadFloat(L"Audio", L"fBigScreenMaxDistance", bigMaxDistance), bigMinDistance + 1.0f);

		log::info("settings: hotkeys={} play=0x{:X} test=0x{:X} stop=0x{:X} dump=0x{:X} mesh=0x{:X} loop={} pause={} maxScreens={}",
			enableHotkeys, keyPlay, keyTestPattern, keyStop, keyDump, keyNextMesh, loop, pauseInMenus, maxScreens);
		log::info("settings: screenNames=[{}] forceNode='{}' srgb={} fitUV={} stopAnim={} aspect={} inside={} classicTV={} nearby={:.0f} audio={} volume={:.2f} dist={:.0f}-{:.0f}",
			[&] { std::string s; for (auto& n : screenNames) { if (!s.empty()) s += ","; s += n; } return s; }(),
			forceNode, srgb, fitUV, stopScreenAnimation, aspectMode, composite, classicTV, nearbyRadius, enableAudio, volume, minDistance, maxDistance);
		log::info("settings: falloff curve {:.2f}, big screens (size >= {:.0f}) heard {:.0f}-{:.0f}", falloffCurve, bigScreenRadius, bigMinDistance, bigMaxDistance);
	}
}
