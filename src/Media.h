#pragma once

#include "Audio.h"

namespace fv
{
	struct VideoFrame
	{
		std::int64_t              pts{ 0 };  // 100ns units
		std::uint32_t             width{ 0 };
		std::uint32_t             height{ 0 };
		std::vector<std::uint8_t> pixels;  // BGRX, top-down, stride = width * 4 (CPU decoding)
		// GPU decoding: the picture is in a texture shared with the game's device, guarded by a keyed mutex
		// (the decoder releases it with key 1, the game takes it with key 1 and gives it back with key 0)
		ComPtr<ID3D11Texture2D>   gpuTexture;
		HANDLE                    gpuShared{ nullptr };
		std::uint64_t             gpuKey{ 0 };  // the key it was last released with
		std::uint32_t             gpuWidth{ 0 };
		std::uint32_t             gpuHeight{ 0 };
	};

	// Decodes one media file with Media Foundation.
	// Video and audio each get their own source reader + thread, so neither blocks the other.
	class MediaSource
	{
	public:
		MediaSource() = default;
		~MediaSource();
		MediaSource(const MediaSource&) = delete;
		MediaSource& operator=(const MediaSource&) = delete;

		static bool StartupMF();
		// The game's device: GPU decoding uses its own device on the same graphics card.
		static void SetGameDevice(ID3D11Device* a_device);

		// a_maxHeight > 0: bigger videos are scaled down to this height while decoding (less work everywhere after).
		bool Open(const std::wstring& a_path, bool a_wantAudio, std::uint32_t a_maxHeight = 0, bool a_gpu = false);
		bool OnGpu() const { return _gpu; }
		void Start();
		void Stop();

		// Restart from the beginning (used for looping).
		void Restart() { RestartAt(0); }
		// Jump to a position (100ns units, clamped to the file). Render thread, like Restart.
		void Seek(std::int64_t a_position);
		// Jump the picture only (the sound keeps playing): used when a frozen screen comes back to life.
		void SeekVideo(std::int64_t a_position);

		// Playback clock in 100ns units (audio-driven when there is audio).
		std::int64_t Clock() const;
		void         SetPaused(bool a_paused);
		void         SetVolume(float a_volume);
		void         SetPan(float a_pan);

		// Moves the newest frame with pts <= clock into a_out. Returns false if nothing new.
		bool AcquireFrame(std::int64_t a_clock, VideoFrame& a_out);
		void RecycleFrame(VideoFrame&& a_frame);

		// True once everything has been decoded and presented.
		bool Finished() const;

		std::uint32_t Width() const { return _width; }
		std::uint32_t Height() const { return _height; }
		std::int64_t  Duration() const { return _duration; }
		bool          HasAudio() const { return _audio != nullptr && _audio->Valid(); }
		bool          VideoEnded() const { return _videoEOS.load(); }

	private:
		bool OpenVideo(const std::wstring& a_path, std::uint32_t a_maxHeight);
		bool CreateVideoReader(const std::wstring& a_path, std::uint32_t a_outW, std::uint32_t a_outH);
		bool GpuFrame(IMFMediaBuffer* a_buffer, VideoFrame& a_frame, std::uint32_t a_w, std::uint32_t a_h, std::uint32_t a_cropX,
			std::uint32_t a_cropY);
		bool OpenAudio(const std::wstring& a_path);
		bool UpdateVideoFormat();

		void RestartAt(std::int64_t a_position);
		void VideoThread();
		void AudioThread();

		// video
		bool                    _gpu{ false };  // decoding into GPU textures
		ComPtr<IMFSourceReader> _videoReader;
		std::uint32_t           _width{ 0 };
		std::uint32_t           _height{ 0 };
		LONG                    _stride{ 0 };
		std::uint32_t           _frameWidth{ 0 };   // decoded buffer size
		std::uint32_t           _frameHeight{ 0 };
		std::uint32_t           _cropX{ 0 };        // visible picture inside it
		std::uint32_t           _cropY{ 0 };
		std::int64_t            _duration{ 0 };

		mutable std::mutex      _frameLock;
		std::condition_variable _frameCv;
		std::deque<VideoFrame>  _frames;
		std::vector<VideoFrame> _pool;
		std::atomic<bool>       _videoEOS{ false };
		std::atomic<std::int64_t> _videoSeek{ -1 };  // picture-only jump waiting for the video thread

		// audio
		ComPtr<IMFSourceReader>      _audioReader;
		std::unique_ptr<AudioStream> _audio;
		std::uint32_t                _blockAlign{ 0 };
		std::atomic<bool>            _audioEOS{ false };
		std::uint64_t                _samplesBase{ 0 };

		// clock (used when there is no audio)
		using steady_t = std::chrono::steady_clock;
		steady_t::time_point _wallStart{};
		steady_t::time_point _pauseStart{};
		steady_t::duration   _pausedTotal{};
		bool                _paused{ false };
		std::int64_t        _clockOffset{ 0 };  // where the clock starts after a seek

		// threads
		std::atomic<bool> _stop{ false };
		std::thread       _videoThread;
		std::thread       _audioThread;
		bool              _running{ false };
	};
}
