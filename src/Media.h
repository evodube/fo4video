#pragma once

#include "Audio.h"

namespace fv
{
	struct VideoFrame
	{
		std::int64_t              pts{ 0 };  // 100ns units
		std::uint32_t             width{ 0 };
		std::uint32_t             height{ 0 };
		std::vector<std::uint8_t> pixels;  // BGRX, top-down, stride = width * 4
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

		bool Open(const std::wstring& a_path, bool a_wantAudio);
		void Start();
		void Stop();

		// Restart from the beginning (used for looping).
		void Restart();

		// Playback clock in 100ns units (audio-driven when there is audio).
		std::int64_t Clock() const;
		void         SetPaused(bool a_paused);
		void         SetVolume(float a_volume);

		// Moves the newest frame with pts <= clock into a_out. Returns false if nothing new.
		bool AcquireFrame(std::int64_t a_clock, VideoFrame& a_out);
		void RecycleFrame(VideoFrame&& a_frame);

		// True once everything has been decoded and presented.
		bool Finished() const;

		std::uint32_t Width() const { return _width; }
		std::uint32_t Height() const { return _height; }
		std::int64_t  Duration() const { return _duration; }
		bool          HasAudio() const { return _audio != nullptr && _audio->Valid(); }

	private:
		bool OpenVideo(const std::wstring& a_path);
		bool OpenAudio(const std::wstring& a_path);
		bool UpdateVideoFormat();

		void VideoThread();
		void AudioThread();

		// video
		ComPtr<IMFSourceReader> _videoReader;
		std::uint32_t           _width{ 0 };
		std::uint32_t           _height{ 0 };
		LONG                    _stride{ 0 };
		std::int64_t            _duration{ 0 };

		mutable std::mutex      _frameLock;
		std::condition_variable _frameCv;
		std::deque<VideoFrame>  _frames;
		std::vector<VideoFrame> _pool;
		std::atomic<bool>       _videoEOS{ false };

		// audio
		ComPtr<IMFSourceReader>      _audioReader;
		std::unique_ptr<AudioStream> _audio;
		std::uint32_t                _blockAlign{ 0 };
		std::atomic<bool>            _audioEOS{ false };
		std::uint64_t                _samplesBase{ 0 };

		// clock (used when there is no audio)
		using clock_t = std::chrono::steady_clock;
		clock_t::time_point _wallStart{};
		clock_t::time_point _pauseStart{};
		clock_t::duration   _pausedTotal{};
		bool                _paused{ false };

		// threads
		std::atomic<bool> _stop{ false };
		std::thread       _videoThread;
		std::thread       _audioThread;
		bool              _running{ false };
	};
}
