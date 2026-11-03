#pragma once

#include "Media.h"
#include "Screen.h"

namespace fv
{
	// GPU side of one screen slot: our dynamic texture, and the patch that makes the
	// slot's placeholder NiTexture sample it.
	struct Slot
	{
		RE::NiPointer<RE::NiTexture>     niTexture;
		ComPtr<ID3D11Texture2D>          texture;
		ComPtr<ID3D11ShaderResourceView> srv;
		std::uint32_t                    width{ 0 };
		std::uint32_t                    height{ 0 };
		bool                             srgb{ false };
		bool                             gpuTarget{ false };  // GPU decoding: drawn into (render target), not written by the CPU
		ComPtr<ID3D11RenderTargetView>   rtv;
		bool                             inUse{ false };

		// patch state (render thread)
		RE::BSGraphics::Texture*  patchedRT{ nullptr };
		ID3D11ShaderResourceView* originalSRV{ nullptr };
		ID3D11ShaderResourceView* installedSRV{ nullptr };
		bool                      refusedLogged{ false };

		// composite target (screen texture + video), used when the frame shares the screen's texture
		ComPtr<ID3D11Texture2D>          compTexture;
		ComPtr<ID3D11RenderTargetView>   compRTV;
		ComPtr<ID3D11ShaderResourceView> compSRV;
		std::uint32_t                    compWidth{ 0 };
		std::uint32_t                    compHeight{ 0 };
		bool                             compSRGB{ false };
		bool                             compNeedsDraw{ false };

		bool Ensure(ID3D11Device* a_device, std::uint32_t a_width, std::uint32_t a_height, bool a_srgb, bool a_gpuTarget = false);
		bool EnsureComposite(ID3D11Device* a_device, std::uint32_t a_width, std::uint32_t a_height, bool a_srgb);
		void Upload(ID3D11DeviceContext* a_context, const std::uint8_t* a_pixels, std::uint32_t a_width, std::uint32_t a_height);
		void Patch(int a_index, ID3D11ShaderResourceView* a_want);
		void Unpatch();
		void Release();
	};

	enum class SourceKind
	{
		kTestPattern,
		kVideo
	};

	struct Player
	{
		RE::ObjectRefHandle          ref;
		std::uint32_t                formID{ 0 };     // the object it was started on (screen, projector, switch...)
		std::atomic<std::uint32_t>   screenID{ 0 };   // the object whose mesh shows the video (can be a nearby one)
		std::string                  name;
		int                          slot{ -1 };
		SourceKind                   kind{ SourceKind::kTestPattern };
		std::wstring                 file;
		bool                         loop{ true };
		std::unique_ptr<MediaSource> media;          // null while the video is still being opened in the background
		std::uint64_t                serial{ 0 };      // tells a reused Player address apart from this one
		std::vector<std::uint8_t>    pattern;  // test pattern pixels

		ScreenBinding      binding;  // main thread only
		std::atomic<bool>  srgb{ false };
		std::atomic<float> screenAspect{ 0.0f };
		std::atomic<float> volume{ 0.0f };
		std::atomic<float> pan{ 0.0f };  // -1 left .. +1 right
		float              userVolume{ 1.0f };
		std::atomic<bool>  stopping{ false };
		std::atomic<bool>  userPaused{ false };  // paused from the remote
		std::atomic<std::int64_t> seekDelta{ 0 };  // rewind / fast-forward requested by the remote (100ns)
		std::atomic<float> brightness{ 1.0f };     // this screen's own brightness from the remote (1 = as is)
		std::atomic<bool>  brightenable{ false };  // projector / cinema screen (TVs stay as they are)
		std::uint64_t      framesShown{ 0 };
		std::uint32_t      openHeight{ 0 };      // the size cap the video was opened with (for reopening it)
		std::chrono::steady_clock::time_point mediaSince{};  // when the video was ready
		int                gpuMisses{ 0 };        // GPU frames in a row that couldn't be drawn
		bool               gpuMissLogged{ false };
		bool               noPictureLogged{ false };
		int                rebindCooldown{ 0 };
		RE::ObjectRefHandle workshop;      // the settlement (workbench) the screen is in, if any
		bool               workshopChecked{ false };
		bool               cinema{ false };  // the drive-in (remembered while its 3D is unloaded far away)
		bool               elsewhere{ false };  // you're in a different settlement
		std::chrono::steady_clock::time_point elsewhereSince{};
		bool               away{ false };  // further than the auto-off distance
		std::chrono::steady_clock::time_point awaySince{};
		bool               unseen{ false };  // far or out of view (freeze timer running)
		std::chrono::steady_clock::time_point unseenSince{};
		std::atomic<bool>  frozen{ false };
		bool               advancePending{ false };  // shuffle: the next random video was asked for at the end  // picture not updated (sound carries on)
		bool               wasFrozen{ false };  // render thread
		// GPU decoding: the decoder's shared textures opened on the game's device (render thread)
		struct GpuIn
		{
			ComPtr<ID3D11Texture2D>          texture;
			ComPtr<IDXGIKeyedMutex>          mutex;
			ComPtr<ID3D11ShaderResourceView> srv[2];  // [sRGB]
		};
		std::unordered_map<HANDLE, GpuIn> gpuIn;
		std::uint32_t      lastVideoWidth{ 0 };
		std::uint32_t      lastVideoHeight{ 0 };
		bool               compositeLogged{ false };
		float              lastPictureBrightness{ 1.0f };  // render thread: redraw the composite when it changes
		float              lastFrameScale{ 1.0f };
	};

	// What the remote shows about one screen (copied under the lock; safe on any thread).
	struct RemoteView
	{
		std::uint32_t formID{ 0 };
		std::string   name;
		bool          playing{ false };
		bool          paused{ false };
		bool          testPattern{ false };
		std::string   file;
		std::string   folder;  // the video's folder inside the video folder ("" = the video folder itself)
		float         volume{ 1.0f };
		bool          muted{ false };
		float         brightness{ 1.0f };
		bool          brightenable{ false };
		float         position{ 0.0f };  // seconds
		float         duration{ 0.0f };  // seconds, 0 = unknown (test pattern)
	};

	class Manager
	{
	public:
		// the save game remembers what every screen was playing (F4SE co-save); main thread
		void SaveScreens(const F4SE::SerializationInterface* a_intfc);
		void LoadScreens(const F4SE::SerializationInterface* a_intfc);
		void ForgetScreens();

		static Manager& Get();

		// --- remote control (main thread unless noted) ---
		RE::NiPointer<RE::TESObjectREFR> Target();          // screen under the crosshair, else the console pick
		void RemotePlayPause(RE::TESObjectREFR* a_ref);
		void RemoteStep(RE::TESObjectREFR* a_ref, int a_dir);  // +1 next video, -1 previous
		void RemotePlayFile(RE::TESObjectREFR* a_ref, const std::wstring& a_name);
		void RemoteFirst(RE::TESObjectREFR* a_ref);                  // home: first video of the folder
		void RemotePower(RE::TESObjectREFR* a_ref);                  // off if playing, else on
		void RemoteSeek(std::uint32_t a_formID, float a_seconds);    // any thread
		void RemoteSeekTo(std::uint32_t a_formID, float a_seconds);  // any thread: jump to this time
		// every TV / projector / cinema screen around you (this settlement): on = the video playing on a_from
		// (or the first video), all in step; off = all stopped. Toggle: off if any is playing, else on. Main thread.
		void AllScreens(RE::TESObjectREFR* a_from, bool a_on);
		void AllScreensToggle(RE::TESObjectREFR* a_from);
		void RebindAll();                                            // main thread: re-fit every playing screen (after settings changes)
		void RemoteSetVolume(std::uint32_t a_formID, float a_volume);  // any thread
		void RemoteSetMuted(std::uint32_t a_formID, bool a_muted);     // any thread
		void RemoteSetBrightness(std::uint32_t a_formID, float a_brightness);  // any thread; saved per screen
		RemoteView View(std::uint32_t a_formID);                      // any thread; never waits (last known view if busy)
		std::vector<RemoteView> Views();                              // any thread: every playing screen; never waits

		// --- main thread ---
		bool Play(RE::TESObjectREFR* a_ref, const std::wstring& a_file, bool a_loop);
		bool PlayTestPattern(RE::TESObjectREFR* a_ref);
		void Stop(RE::TESObjectREFR* a_ref);
		void StopAll();
		bool IsPlaying(RE::TESObjectREFR* a_ref);
		void SetVolume(RE::TESObjectREFR* a_ref, float a_volume);
		void Maintain();

		// Resolves a Papyrus/INI file name to a full path under the video folder.
		std::wstring ResolveVideoPath(const std::wstring& a_name) const;
		std::vector<std::wstring> ListVideos() const;  // relative to the video folder, subfolders included
		std::wstring CurrentVideo(RE::TESObjectREFR* a_ref);  // what that screen plays, relative to the video folder

		// Hotkey actions (main thread)
		void HotkeyPlayNext();
		bool RefuseUnsupported(RE::TESObjectREFR* a_ref);
		void HotkeyTestPattern();
		void HotkeyStop();
		void HotkeyDump();
		void HotkeyNextMesh(int a_mode);
		// the "Fix this screen" buttons: 0 next mesh, 1 turn/flip, 2 nearby screens, 3 other side
		void FixScreen(RE::TESObjectREFR* a_ref, int a_mode);
		void DumpScreen(RE::TESObjectREFR* a_ref);

		// --- render thread ---
		void OnPresent(ID3D11Device* a_device, ID3D11DeviceContext* a_context);

	private:
		bool Start(RE::TESObjectREFR* a_ref, SourceKind a_kind, const std::wstring& a_file, bool a_loop, std::int64_t a_startAt = 0,
			bool a_quiet = false);

		// a screen turned off because you walked away: what it played and where, to carry on when you come back
		struct Sleeper
		{
			RE::ObjectRefHandle ref;
			std::uint32_t       formID{ 0 };
			SourceKind          kind{ SourceKind::kVideo };
			std::wstring        file;
			bool                loop{ true };
			std::int64_t        position{ 0 };  // 100 ns
			RE::NiPoint3        where{};
			float               wakeDistance{ 0.0f };
			std::string         name;
			RE::ObjectRefHandle workshop;  // its settlement: back on when you're in it again
			bool                paused{ false };  // paused with the remote when it went off / was saved
			bool                cinema{ false };  // uses the cinema's turn-off settings
		};
		RE::ObjectRefHandle _playerWorkshop;  // the settlement you're standing in (none = outside any)
		bool                _playerWorkshopKnown{ false };
		std::vector<Sleeper> _sleepers;
		RemoteView ViewLocked(std::uint32_t a_formID);
		void       OpenMedia(Player* a_target, std::uint64_t a_serial, bool a_audio, bool a_gpu);
		void       FallBackToCpuLocked(Player& a_player, const std::string& a_why);
		std::mutex                                   _viewLock;   // the remote's last known views (its UI thread never waits on _lock)
		std::unordered_map<std::uint32_t, RemoteView> _viewCache;
		std::vector<RemoteView>                      _viewsCache;
		std::chrono::steady_clock::time_point _lastWake{};  // saved screens come back one at a time, not all in one frame
		std::uint64_t        _nextSerial{ 1 };
		int  FreeSlot() const;
		void StopLocked(Player& a_player);
		void PollHotkeys();
		bool IsGamePaused() const;
		std::uint32_t PrefKeyLocked(std::uint32_t a_formID) const;  // volume/mute key: the screen's object
		void ScheduleMaintain();

		std::mutex                           _lock;
		std::vector<std::unique_ptr<Player>> _players;
		static constexpr int                 kMaxSlots = 20;
		std::array<Slot, kMaxSlots>          _slots;
		std::array<Slot, kMaxSlots>          _glowSlots;  // glow-map textures (Textures\RealVideos\slot20-39.dds)

		std::atomic<bool> _maintainPending{ false };
		std::uint64_t     _frame{ 0 };
		std::size_t       _nextVideo{ 0 };
		std::unordered_map<std::uint32_t, std::vector<std::wstring>> _shuffleHistory;  // per screen: videos played, newest last
		std::mt19937                                                 _rng{ std::random_device{}() };
		std::array<bool, 6> _keyWasDown{};
		struct Prefs
		{
			float volume{ -1.0f };  // -1 = fStartVolume
			bool  muted{ false };
			float brightness{ 1.0f };
		};
		void LoadBrightnessLocked();
		bool _brightnessLoaded{ false };
		std::unordered_map<std::uint32_t, Prefs> _prefs;  // remote volume/mute per screen, kept across videos
		bool                _wasPaused{ false };
		std::atomic<bool>   _inBuildMode{ false };
		std::atomic<float>  _frameLight{ 0.5f };  // brightness for the non-video parts of glowing projector screens
	};
}
