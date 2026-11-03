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

		bool Ensure(ID3D11Device* a_device, std::uint32_t a_width, std::uint32_t a_height, bool a_srgb);
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
		std::unique_ptr<MediaSource> media;
		std::vector<std::uint8_t>    pattern;  // test pattern pixels

		ScreenBinding      binding;  // main thread only
		std::atomic<bool>  srgb{ false };
		std::atomic<float> screenAspect{ 0.0f };
		std::atomic<float> volume{ 0.0f };
		float              userVolume{ 1.0f };
		std::atomic<bool>  stopping{ false };
		std::atomic<bool>  userPaused{ false };  // paused from the remote
		std::atomic<std::int64_t> seekDelta{ 0 };  // rewind / fast-forward requested by the remote (100ns)
		std::uint64_t      framesShown{ 0 };
		int                rebindCooldown{ 0 };
		std::uint32_t      lastVideoWidth{ 0 };
		std::uint32_t      lastVideoHeight{ 0 };
		bool               compositeLogged{ false };
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
		float         volume{ 1.0f };
		bool          muted{ false };
	};

	class Manager
	{
	public:
		static Manager& Get();

		// --- remote control (main thread unless noted) ---
		RE::NiPointer<RE::TESObjectREFR> Target();          // screen under the crosshair, else the console pick
		void RemotePlayPause(RE::TESObjectREFR* a_ref);
		void RemoteStep(RE::TESObjectREFR* a_ref, int a_dir);  // +1 next video, -1 previous
		void RemotePlayFile(RE::TESObjectREFR* a_ref, const std::wstring& a_name);
		void RemoteFirst(RE::TESObjectREFR* a_ref);                  // home: first video of the folder
		void RemotePower(RE::TESObjectREFR* a_ref);                  // off if playing, else on
		void RemoteSeek(std::uint32_t a_formID, float a_seconds);    // any thread
		void RebindAll();                                            // main thread: re-fit every playing screen (after settings changes)
		void RemoteSetVolume(std::uint32_t a_formID, float a_volume);  // any thread
		void RemoteSetMuted(std::uint32_t a_formID, bool a_muted);     // any thread
		RemoteView View(std::uint32_t a_formID);                      // any thread
		std::vector<RemoteView> Views();                              // any thread: every playing screen

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
		std::vector<std::wstring> ListVideos() const;

		// Hotkey actions (main thread)
		void HotkeyPlayNext();
		bool RefuseUnsupported(RE::TESObjectREFR* a_ref);
		void HotkeyTestPattern();
		void HotkeyStop();
		void HotkeyDump();
		void HotkeyNextMesh(int a_mode);

		// --- render thread ---
		void OnPresent(ID3D11Device* a_device, ID3D11DeviceContext* a_context);

	private:
		bool Start(RE::TESObjectREFR* a_ref, SourceKind a_kind, const std::wstring& a_file, bool a_loop);
		int  FreeSlot() const;
		void StopLocked(Player& a_player);
		void PollHotkeys();
		bool IsGamePaused() const;
		std::uint32_t PrefKeyLocked(std::uint32_t a_formID) const;  // volume/mute key: the screen's object
		void ScheduleMaintain();

		std::mutex                           _lock;
		std::vector<std::unique_ptr<Player>> _players;
		std::array<Slot, 16>                 _slots;

		std::atomic<bool> _maintainPending{ false };
		std::uint64_t     _frame{ 0 };
		std::size_t       _nextVideo{ 0 };
		std::array<bool, 6> _keyWasDown{};
		struct Prefs
		{
			float volume{ -1.0f };  // -1 = fStartVolume
			bool  muted{ false };
		};
		std::unordered_map<std::uint32_t, Prefs> _prefs;  // remote volume/mute per screen, kept across videos
		bool                _wasPaused{ false };
		std::atomic<bool>   _inBuildMode{ false };
	};
}
