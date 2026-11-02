#include "Audio.h"

namespace fv
{
	AudioEngine& AudioEngine::Get()
	{
		static AudioEngine engine;
		return engine;
	}

	bool AudioEngine::EnsureInit()
	{
		std::scoped_lock lock(_lock);
		if (_master) {
			return true;
		}
		if (_tried) {
			return false;
		}
		_tried = true;

		// XAudio2 needs COM on the calling thread.
		::CoInitializeEx(nullptr, COINIT_MULTITHREADED);

		HRESULT hr = ::XAudio2Create(_xaudio.ReleaseAndGetAddressOf(), 0, XAUDIO2_DEFAULT_PROCESSOR);
		if (FAILED(hr)) {
			log::error("XAudio2Create failed (0x{:08X}); video will play without sound", static_cast<std::uint32_t>(hr));
			_xaudio.Reset();
			return false;
		}

		hr = _xaudio->CreateMasteringVoice(&_master);
		if (FAILED(hr)) {
			log::error("CreateMasteringVoice failed (0x{:08X}); video will play without sound", static_cast<std::uint32_t>(hr));
			_master = nullptr;
			_xaudio.Reset();
			return false;
		}

		log::info("audio engine ready (XAudio2)");
		return true;
	}

	AudioStream::~AudioStream()
	{
		Destroy();
	}

	bool AudioStream::Create(const WAVEFORMATEX& a_format)
	{
		auto& engine = AudioEngine::Get();
		if (!engine.EnsureInit()) {
			return false;
		}

		const HRESULT hr = engine.XAudio()->CreateSourceVoice(&_voice, &a_format, 0, XAUDIO2_DEFAULT_FREQ_RATIO, this);
		if (FAILED(hr)) {
			log::error("CreateSourceVoice failed (0x{:08X})", static_cast<std::uint32_t>(hr));
			_voice = nullptr;
			return false;
		}
		_sampleRate = a_format.nSamplesPerSec;
		return true;
	}

	void AudioStream::Destroy()
	{
		if (!_voice) {
			return;
		}
		_voice->Stop(0);
		_voice->FlushSourceBuffers();
		// let flushed buffers report OnBufferEnd so their memory is released
		for (int i = 0; i < 50 && Queued() > 0; ++i) {
			std::this_thread::sleep_for(2ms);
		}
		_voice->DestroyVoice();
		_voice = nullptr;
		_running = false;
	}

	bool AudioStream::Submit(std::vector<std::uint8_t>&& a_pcm)
	{
		if (!_voice || a_pcm.empty()) {
			return false;
		}
		auto* owned = new std::vector<std::uint8_t>(std::move(a_pcm));

		XAUDIO2_BUFFER buf{};
		buf.AudioBytes = static_cast<UINT32>(owned->size());
		buf.pAudioData = owned->data();
		buf.pContext = owned;

		const HRESULT hr = _voice->SubmitSourceBuffer(&buf);
		if (FAILED(hr)) {
			delete owned;
			return false;
		}
		return true;
	}

	std::uint32_t AudioStream::Queued() const
	{
		if (!_voice) {
			return 0;
		}
		XAUDIO2_VOICE_STATE state{};
		_voice->GetState(&state, XAUDIO2_VOICE_NOSAMPLESPLAYED);
		return state.BuffersQueued;
	}

	std::uint64_t AudioStream::SamplesPlayed() const
	{
		if (!_voice) {
			return 0;
		}
		XAUDIO2_VOICE_STATE state{};
		_voice->GetState(&state, 0);
		return state.SamplesPlayed;
	}

	void AudioStream::Start()
	{
		if (_voice && !_running) {
			_voice->Start(0);
			_running = true;
		}
	}

	void AudioStream::Pause()
	{
		if (_voice && _running) {
			_voice->Stop(0);
			_running = false;
		}
	}

	void AudioStream::Flush()
	{
		if (_voice) {
			_voice->FlushSourceBuffers();
		}
	}

	void AudioStream::SetVolume(float a_volume)
	{
		if (_voice && std::abs(a_volume - _lastVolume) > 0.001f) {
			_voice->SetVolume(a_volume);
			_lastVolume = a_volume;
		}
	}

	void AudioStream::OnBufferEnd(void* a_context) noexcept
	{
		delete static_cast<std::vector<std::uint8_t>*>(a_context);
	}

	void AudioStream::OnVoiceError(void*, HRESULT a_error) noexcept
	{
		log::error("audio voice error 0x{:08X}", static_cast<std::uint32_t>(a_error));
	}
}
