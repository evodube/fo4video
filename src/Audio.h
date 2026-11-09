#pragma once

namespace fv
{
	// One XAudio2 engine for the plugin (separate from the game's own audio).
	class AudioEngine
	{
	public:
		static AudioEngine& Get();

		bool        EnsureInit();
		IXAudio2*   XAudio() const { return _xaudio.Get(); }
		bool        Ready() const { return _master != nullptr; }
		IXAudio2MasteringVoice* Master() const { return _master; }
		std::uint32_t OutputChannels() const { return _outChannels; }

	private:
		ComPtr<IXAudio2>        _xaudio;
		IXAudio2MasteringVoice* _master{ nullptr };
		bool                    _tried{ false };
		std::uint32_t           _outChannels{ 2 };
		std::mutex              _lock;
	};

	// A streaming PCM voice. Decoder thread submits, render thread reads the clock.
	class AudioStream : public IXAudio2VoiceCallback
	{
	public:
		AudioStream() = default;
		~AudioStream();
		AudioStream(const AudioStream&) = delete;
		AudioStream& operator=(const AudioStream&) = delete;

		bool Create(const WAVEFORMATEX& a_format);
		void Destroy();

		// takes ownership of the data
		bool Submit(std::vector<std::uint8_t>&& a_pcm);

		std::uint32_t Queued() const;
		std::uint64_t SamplesPlayed() const;
		std::uint32_t SampleRate() const { return _sampleRate; }
		bool          SetSampleRate(std::uint32_t a_rate);  // the decoder switched rate mid-track (no buffers queued)

		void Start();
		void Pause();
		void Flush();
		void SetVolume(float a_volume);
		// -1 = all left, 0 = centre, +1 = all right (where the screen is, seen from the camera)
		void SetPan(float a_pan);
		bool Valid() const { return _voice != nullptr; }

		// IXAudio2VoiceCallback
		void STDMETHODCALLTYPE OnVoiceProcessingPassStart(UINT32) noexcept override {}
		void STDMETHODCALLTYPE OnVoiceProcessingPassEnd() noexcept override {}
		void STDMETHODCALLTYPE OnStreamEnd() noexcept override {}
		void STDMETHODCALLTYPE OnBufferStart(void* a_context) noexcept override;
		void STDMETHODCALLTYPE OnBufferEnd(void* a_context) noexcept override;
		void STDMETHODCALLTYPE OnLoopEnd(void*) noexcept override {}
		void STDMETHODCALLTYPE OnVoiceError(void*, HRESULT a_error) noexcept override;

	private:
		IXAudio2SourceVoice* _voice{ nullptr };
		std::uint32_t        _sampleRate{ 0 };
		bool                 _running{ false };
		float                _lastVolume{ -1.0f };
		std::uint32_t        _channels{ 0 };
		float                _lastPan{ 9.0f };
		std::uint32_t        _blockAlign{ 0 };

		// Our own count of what has played, from the buffer callbacks: some audio mods (e.g. Audio Switch Support /
		// AudioDeviceFollowFix) patch XAudio2's GetState for every voice in the game, and then the voice reports no
		// play position - the sound plays but the picture, which follows it, stays on one frame.
		std::uint64_t                      OwnCount() const;
		std::atomic<std::uint64_t>         _endedFrames{ 0 };  // frames of buffers that finished playing
		std::atomic<std::int64_t>          _bufStart{ 0 };     // QPC when the playing buffer started (0 = none)
		std::atomic<std::uint32_t>         _bufFrames{ 0 };    // its length
		std::atomic<std::int64_t>          _heldPart{ 0 };     // frames of it played before a pause
		std::atomic<bool>                  _flushing{ false };
		mutable std::atomic<bool>          _stateBroken{ false };
	};
}
