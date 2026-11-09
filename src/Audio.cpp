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

		XAUDIO2_VOICE_DETAILS details{};
		_master->GetVoiceDetails(&details);
		_outChannels = std::max<std::uint32_t>(1, details.InputChannels);
		log::info("audio engine ready (XAudio2, {} speaker channels)", _outChannels);
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
		_channels = a_format.nChannels;
		_blockAlign = a_format.nBlockAlign;
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

	namespace
	{
		std::int64_t Qpc()
		{
			LARGE_INTEGER t{};
			::QueryPerformanceCounter(&t);
			return t.QuadPart;
		}

		std::int64_t QpcFrequency()
		{
			static const std::int64_t f = [] {
				LARGE_INTEGER t{};
				::QueryPerformanceFrequency(&t);
				return t.QuadPart;
			}();
			return f;
		}
	}

	std::uint64_t AudioStream::OwnCount() const
	{
		std::uint64_t count = _endedFrames.load();
		const auto    start = _bufStart.load();
		const auto    frames = _bufFrames.load();
		if (start != 0 && frames > 0) {
			std::int64_t part = _heldPart.load();
			if (_running) {
				part = (Qpc() - start) * static_cast<std::int64_t>(_sampleRate) / QpcFrequency();
			}
			count += static_cast<std::uint64_t>(std::clamp<std::int64_t>(part, 0, frames));
		}
		return count;
	}

	std::uint64_t AudioStream::SamplesPlayed() const
	{
		if (!_voice) {
			return 0;
		}
		const std::uint64_t own = OwnCount();
		if (_stateBroken) {
			return own;
		}
		XAUDIO2_VOICE_STATE state{};
		_voice->GetState(&state, 0);
		const std::uint64_t reported = state.SamplesPlayed;
		// after a second of sound: if the voice's own figure is stuck or nonsense, use our count from now on
		const std::uint64_t second = _sampleRate;
		if (own > second && (reported + second / 2 < own || reported > own + 2 * second)) {
			if (!_stateBroken.exchange(true)) {
				log::warn("audio: the voice reports {} samples played but {} have (another mod patches XAudio2, e.g. Audio Switch Support); "
				          "counting them myself",
					reported, own);
			}
			return own;
		}
		return reported;
	}

	void AudioStream::OnBufferStart(void* a_context) noexcept
	{
		const auto* data = static_cast<std::vector<std::uint8_t>*>(a_context);
		_bufFrames = data && _blockAlign ? static_cast<std::uint32_t>(data->size() / _blockAlign) : 0;
		_heldPart = 0;
		_bufStart = Qpc();
	}

	bool AudioStream::SetSampleRate(std::uint32_t a_rate)
	{
		if (!_voice || a_rate == 0 || FAILED(_voice->SetSourceSampleRate(a_rate))) {
			return false;
		}
		_sampleRate = a_rate;
		return true;
	}

	void AudioStream::Start()
	{
		if (_voice && !_running) {
			// carry on the playing buffer's count from where the pause left it
			if (_bufStart != 0) {
				_bufStart = Qpc() - _heldPart.load() * QpcFrequency() / std::max<std::int64_t>(1, _sampleRate);
			}
			_voice->Start(0);
			_running = true;
		}
	}

	void AudioStream::Pause()
	{
		if (_voice && _running) {
			_voice->Stop(0);
			if (const auto start = _bufStart.load(); start != 0) {
				_heldPart = std::clamp<std::int64_t>((Qpc() - start) * static_cast<std::int64_t>(_sampleRate) / QpcFrequency(), 0,
					_bufFrames.load());
			}
			_running = false;
		}
	}

	void AudioStream::Flush()
	{
		if (_voice) {
			// flushed buffers also report OnBufferEnd: they were never heard, so they don't count as played
			_flushing = true;
			_voice->FlushSourceBuffers();
			for (int i = 0; i < 50 && Queued() > 0; ++i) {
				std::this_thread::sleep_for(2ms);
			}
			_flushing = false;
			_bufStart = 0;
			_bufFrames = 0;
			_heldPart = 0;
		}
	}

	void AudioStream::SetVolume(float a_volume)
	{
		if (_voice && std::abs(a_volume - _lastVolume) > 0.001f) {
			_voice->SetVolume(a_volume);
			_lastVolume = a_volume;
		}
	}

	void AudioStream::SetPan(float a_pan)
	{
		auto& engine = AudioEngine::Get();
		const std::uint32_t outCh = engine.OutputChannels();
		if (!_voice || !engine.Master() || outCh < 2 || _channels == 0 || std::abs(a_pan - _lastPan) < 0.02f) {
			return;
		}
		_lastPan = a_pan;
		a_pan = std::clamp(a_pan, -1.0f, 1.0f);
		// the far ear keeps some sound (a screen to your right is still heard on the left, just quieter)
		const float gl = a_pan > 0.0f ? 1.0f - 0.75f * a_pan : 1.0f;
		const float gr = a_pan < 0.0f ? 1.0f + 0.75f * a_pan : 1.0f;
		// source channels -> front left / right of the speakers (5.1 sources are folded down to stereo)
		std::vector<float> m(static_cast<std::size_t>(_channels) * outCh, 0.0f);
		auto set = [&](std::uint32_t a_src, float a_l, float a_r) {
			m[0 * _channels + a_src] += a_l * gl;  // matrix is [destination][source]
			m[1 * _channels + a_src] += a_r * gr;
		};
		if (_channels == 1) {
			set(0, 1.0f, 1.0f);
		} else if (_channels == 2) {
			set(0, 1.0f, 0.0f);
			set(1, 0.0f, 1.0f);
		} else {
			// FL FR FC LFE BL BR (SL SR)
			set(0, 1.0f, 0.0f);
			set(1, 0.0f, 1.0f);
			if (_channels > 2) set(2, 0.707f, 0.707f);
			if (_channels > 3) set(3, 0.5f, 0.5f);
			if (_channels > 4) set(4, 0.707f, 0.0f);
			if (_channels > 5) set(5, 0.0f, 0.707f);
			if (_channels > 6) set(6, 0.707f, 0.0f);
			if (_channels > 7) set(7, 0.0f, 0.707f);
		}
		_voice->SetOutputMatrix(engine.Master(), _channels, outCh, m.data());
	}

	void AudioStream::OnBufferEnd(void* a_context) noexcept
	{
		auto* data = static_cast<std::vector<std::uint8_t>*>(a_context);
		if (!_flushing && data && _blockAlign) {
			_endedFrames += data->size() / _blockAlign;
		}
		_bufStart = 0;
		_bufFrames = 0;
		delete data;
	}

	void AudioStream::OnVoiceError(void*, HRESULT a_error) noexcept
	{
		log::error("audio voice error 0x{:08X}", static_cast<std::uint32_t>(a_error));
	}
}
