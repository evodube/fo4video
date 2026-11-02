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

		bool Ensure(ID3D11Device* a_device, std::uint32_t a_width, std::uint32_t a_height, bool a_srgb);
		void Upload(ID3D11DeviceContext* a_context, const std::uint8_t* a_pixels, std::uint32_t a_width, std::uint32_t a_height);
		void Patch(int a_index);
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
		std::uint32_t                formID{ 0 };
		std::string                  name;
		int                          slot{ -1 };
		SourceKind                   kind{ SourceKind::kTestPattern };
		std::wstring                 file;
		bool                         loop{ true };
		std::unique_ptr<MediaSource> media;
		std::vector<std::uint8_t>    pattern;  // test pattern pixels

		ScreenBinding      binding;  // main thread only
		std::atomic<bool>  srgb{ false };
		std::atomic<float> volume{ 0.0f };
		float              userVolume{ 1.0f };
		std::atomic<bool>  stopping{ false };
		std::uint64_t      framesShown{ 0 };
	};

	class Manager
	{
	public:
		static Manager& Get();

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
		void HotkeyTestPattern();
		void HotkeyStop();
		void HotkeyDump();

		// --- render thread ---
		void OnPresent(ID3D11Device* a_device, ID3D11DeviceContext* a_context);

	private:
		bool Start(RE::TESObjectREFR* a_ref, SourceKind a_kind, const std::wstring& a_file, bool a_loop);
		int  FreeSlot() const;
		void StopLocked(Player& a_player);
		void PollHotkeys();
		void ScheduleMaintain();

		std::mutex                           _lock;
		std::vector<std::unique_ptr<Player>> _players;
		std::array<Slot, 16>                 _slots;

		std::atomic<bool> _maintainPending{ false };
		std::uint64_t     _frame{ 0 };
		std::size_t       _nextVideo{ 0 };
		std::array<bool, 4> _keyWasDown{};
	};
}
