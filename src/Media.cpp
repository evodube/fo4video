#include "Media.h"

namespace fv
{
	namespace
	{
		constexpr std::size_t   kMaxFrames = 6;         // decoded video frames buffered ahead
		constexpr std::uint32_t kMaxAudioBuffers = 24;  // queued XAudio2 buffers (~0.5-1.5s)

		std::string HrStr(HRESULT a_hr)
		{
			return std::format("0x{:08X}", static_cast<std::uint32_t>(a_hr));
		}

		struct ComScope
		{
			ComScope() { _hr = ::CoInitializeEx(nullptr, COINIT_MULTITHREADED); }
			~ComScope()
			{
				if (SUCCEEDED(_hr)) {
					::CoUninitialize();
				}
			}
			HRESULT _hr;
		};
	}

	bool MediaSource::StartupMF()
	{
		static std::once_flag once;
		static bool           ok = false;
		std::call_once(once, [] {
			::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
			const HRESULT hr = ::MFStartup(MF_VERSION, MFSTARTUP_FULL);
			ok = SUCCEEDED(hr);
			if (ok) {
				log::info("Media Foundation started");
			} else {
				log::error("MFStartup failed ({})", HrStr(hr));
			}
		});
		return ok;
	}

	MediaSource::~MediaSource()
	{
		Stop();
		if (_audio) {
			_audio->Destroy();
			_audio.reset();
		}
	}

	bool MediaSource::Open(const std::wstring& a_path, bool a_wantAudio)
	{
		if (!StartupMF()) {
			return false;
		}
		ComScope com;

		if (!OpenVideo(a_path)) {
			return false;
		}
		if (a_wantAudio) {
			if (!OpenAudio(a_path)) {
				log::info("no usable audio track; playing video only");
				_audioReader.Reset();
				_audio.reset();
			}
		}
		return true;
	}

	bool MediaSource::OpenVideo(const std::wstring& a_path)
	{
		ComPtr<IMFAttributes> attrs;
		::MFCreateAttributes(&attrs, 2);
		attrs->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);  // lets MF convert YUV -> RGB32 for us
		attrs->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);

		HRESULT hr = ::MFCreateSourceReaderFromURL(a_path.c_str(), attrs.Get(), &_videoReader);
		if (FAILED(hr)) {
			log::error("could not open video file ({})", HrStr(hr));
			return false;
		}

		_videoReader->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS), FALSE);
		hr = _videoReader->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), TRUE);
		if (FAILED(hr)) {
			log::error("file has no video stream ({})", HrStr(hr));
			return false;
		}

		ComPtr<IMFMediaType> type;
		::MFCreateMediaType(&type);
		type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
		type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
		hr = _videoReader->SetCurrentMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), nullptr, type.Get());
		if (FAILED(hr)) {
			log::error("could not get RGB32 output from the video decoder ({}). Is the codec supported by Windows?", HrStr(hr));
			return false;
		}

		if (!UpdateVideoFormat()) {
			return false;
		}

		PROPVARIANT var;
		::PropVariantInit(&var);
		if (SUCCEEDED(_videoReader->GetPresentationAttribute(static_cast<DWORD>(MF_SOURCE_READER_MEDIASOURCE), MF_PD_DURATION, &var))) {
			if (var.vt == VT_UI8) {
				_duration = static_cast<std::int64_t>(var.uhVal.QuadPart);
			}
		}
		::PropVariantClear(&var);

		log::info("video: {}x{} stride {} duration {:.1f}s", _width, _height, _stride, _duration / 1.0e7);
		return true;
	}

	bool MediaSource::UpdateVideoFormat()
	{
		ComPtr<IMFMediaType> cur;
		if (FAILED(_videoReader->GetCurrentMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), &cur))) {
			log::error("GetCurrentMediaType failed");
			return false;
		}

		UINT32 w = 0, h = 0;
		if (FAILED(::MFGetAttributeSize(cur.Get(), MF_MT_FRAME_SIZE, &w, &h)) || w == 0 || h == 0) {
			log::error("video has no frame size");
			return false;
		}

		UINT32 strideU = 0;
		if (SUCCEEDED(cur->GetUINT32(MF_MT_DEFAULT_STRIDE, &strideU))) {
			_stride = static_cast<LONG>(strideU);
		} else {
			LONG stride = 0;
			if (FAILED(::MFGetStrideForBitmapInfoHeader(MFVideoFormat_RGB32.Data1, w, &stride))) {
				stride = static_cast<LONG>(w * 4);
			}
			_stride = stride;
		}

		std::scoped_lock lock(_frameLock);
		_width = w;
		_height = h;
		return true;
	}

	bool MediaSource::OpenAudio(const std::wstring& a_path)
	{
		HRESULT hr = ::MFCreateSourceReaderFromURL(a_path.c_str(), nullptr, &_audioReader);
		if (FAILED(hr)) {
			return false;
		}

		_audioReader->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS), FALSE);
		if (FAILED(_audioReader->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM), TRUE))) {
			return false;
		}

		ComPtr<IMFMediaType> type;
		::MFCreateMediaType(&type);
		type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
		type->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
		type->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
		hr = _audioReader->SetCurrentMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM), nullptr, type.Get());
		if (FAILED(hr)) {
			log::warn("audio decoder can't output PCM ({})", HrStr(hr));
			return false;
		}

		ComPtr<IMFMediaType> cur;
		if (FAILED(_audioReader->GetCurrentMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM), &cur))) {
			return false;
		}

		WAVEFORMATEX* wfx = nullptr;
		UINT32        size = 0;
		if (FAILED(::MFCreateWaveFormatExFromMFMediaType(cur.Get(), &wfx, &size)) || !wfx) {
			return false;
		}

		auto stream = std::make_unique<AudioStream>();
		const bool ok = stream->Create(*wfx);
		_blockAlign = wfx->nBlockAlign;
		log::info("audio: {} Hz, {} ch, {} bit -> {}", wfx->nSamplesPerSec, wfx->nChannels, wfx->wBitsPerSample, ok ? "ok" : "voice failed");
		::CoTaskMemFree(wfx);

		if (!ok) {
			return false;
		}
		_audio = std::move(stream);
		return true;
	}

	void MediaSource::Start()
	{
		if (_running) {
			return;
		}
		_stop = false;
		_wallStart = steady_t::now();
		_pausedTotal = {};
		if (_paused) {
			_pauseStart = _wallStart;
		}

		_videoThread = std::thread([this] { VideoThread(); });
		if (_audio && _audioReader) {
			_audioThread = std::thread([this] { AudioThread(); });
			if (!_paused) {
				_audio->Start();
			}
		}
		_running = true;
	}

	void MediaSource::Stop()
	{
		if (!_running) {
			return;
		}
		_stop = true;
		_frameCv.notify_all();
		if (_videoThread.joinable()) {
			_videoThread.join();
		}
		if (_audioThread.joinable()) {
			_audioThread.join();
		}
		if (_audio) {
			_audio->Pause();
		}
		_running = false;
	}

	void MediaSource::Restart()
	{
		Stop();

		{
			std::scoped_lock lock(_frameLock);
			while (!_frames.empty()) {
				_pool.push_back(std::move(_frames.front()));
				_frames.pop_front();
			}
		}

		PROPVARIANT var;
		::PropVariantInit(&var);
		var.vt = VT_I8;
		var.hVal.QuadPart = 0;
		if (_videoReader) {
			_videoReader->SetCurrentPosition(GUID_NULL, var);
		}
		if (_audioReader) {
			_audioReader->SetCurrentPosition(GUID_NULL, var);
		}
		if (_audio) {
			_audio->Flush();
			for (int i = 0; i < 50 && _audio->Queued() > 0; ++i) {
				std::this_thread::sleep_for(1ms);
			}
			_samplesBase = _audio->SamplesPlayed();
		}

		_videoEOS = false;
		_audioEOS = false;
		Start();
	}

	std::int64_t MediaSource::Clock() const
	{
		const auto wall = [this]() -> std::int64_t {
			const auto now = _paused ? _pauseStart : steady_t::now();
			const auto elapsed = now - _wallStart - _pausedTotal;
			return std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count() / 100;
		};

		if (HasAudio() && _audio->SampleRate() > 0) {
			const auto played = _audio->SamplesPlayed();
			const auto samples = played >= _samplesBase ? played - _samplesBase : 0;
			const auto audioClock = static_cast<std::int64_t>(samples * 10'000'000ull / _audio->SampleRate());
			// if the audio track ended early, keep the video going on the wall clock
			if (_audioEOS && _audio->Queued() == 0) {
				return std::max(audioClock, wall());
			}
			return audioClock;
		}
		return wall();
	}

	void MediaSource::SetPaused(bool a_paused)
	{
		if (a_paused == _paused) {
			return;
		}
		_paused = a_paused;
		if (a_paused) {
			_pauseStart = steady_t::now();
			if (_audio) {
				_audio->Pause();
			}
		} else {
			_pausedTotal += steady_t::now() - _pauseStart;
			if (_audio && _running) {
				_audio->Start();
			}
		}
	}

	void MediaSource::SetVolume(float a_volume)
	{
		if (_audio) {
			_audio->SetVolume(a_volume);
		}
	}

	bool MediaSource::AcquireFrame(std::int64_t a_clock, VideoFrame& a_out)
	{
		bool got = false;
		{
			std::scoped_lock lock(_frameLock);
			while (!_frames.empty() && _frames.front().pts <= a_clock) {
				if (got) {
					_pool.push_back(std::move(a_out));  // late frame we are skipping
				}
				a_out = std::move(_frames.front());
				_frames.pop_front();
				got = true;
			}
		}
		if (got) {
			_frameCv.notify_all();
		}
		return got;
	}

	void MediaSource::RecycleFrame(VideoFrame&& a_frame)
	{
		std::scoped_lock lock(_frameLock);
		if (_pool.size() < kMaxFrames + 2) {
			_pool.push_back(std::move(a_frame));
		}
	}

	bool MediaSource::Finished() const
	{
		bool framesLeft;
		{
			std::scoped_lock lock(_frameLock);
			framesLeft = !_frames.empty();
		}
		if (!_videoEOS || framesLeft) {
			return false;
		}
		if (HasAudio() && !(_audioEOS && _audio->Queued() == 0)) {
			return false;
		}
		return true;
	}

	void MediaSource::VideoThread()
	{
		ComScope com;

		while (!_stop) {
			{
				std::unique_lock lock(_frameLock);
				_frameCv.wait_for(lock, 20ms, [this] { return _stop.load() || _frames.size() < kMaxFrames; });
				if (_stop) {
					break;
				}
				if (_frames.size() >= kMaxFrames) {
					continue;
				}
			}

			DWORD              streamIndex = 0, flags = 0;
			LONGLONG           timestamp = 0;
			ComPtr<IMFSample>  sample;
			const HRESULT      hr = _videoReader->ReadSample(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), 0, &streamIndex, &flags, &timestamp, &sample);
			if (FAILED(hr)) {
				log::error("video ReadSample failed ({})", HrStr(hr));
				_videoEOS = true;
				break;
			}
			if (flags & MF_SOURCE_READERF_ENDOFSTREAM) {
				_videoEOS = true;
				break;
			}
			if (flags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED) {
				UpdateVideoFormat();
			}
			if (!sample) {
				continue;
			}

			ComPtr<IMFMediaBuffer> buffer;
			if (FAILED(sample->ConvertToContiguousBuffer(&buffer))) {
				continue;
			}

			std::uint32_t w, h;
			LONG          defaultStride;
			{
				std::scoped_lock lock(_frameLock);
				w = _width;
				h = _height;
				defaultStride = _stride;
			}

			BYTE* scan0 = nullptr;
			LONG  pitch = 0;
			bool  locked2D = false;
			BYTE* base = nullptr;
			DWORD length = 0;

			ComPtr<IMF2DBuffer> buffer2D;
			if (SUCCEEDED(buffer.As(&buffer2D)) && SUCCEEDED(buffer2D->Lock2D(&scan0, &pitch))) {
				locked2D = true;
			} else if (SUCCEEDED(buffer->Lock(&base, nullptr, &length))) {
				pitch = defaultStride;
				const auto absPitch = static_cast<std::size_t>(pitch < 0 ? -pitch : pitch);
				if (length < absPitch * h) {
					buffer->Unlock();
					continue;
				}
				scan0 = pitch < 0 ? base + absPitch * (h - 1) : base;
			} else {
				continue;
			}

			VideoFrame frame;
			{
				std::scoped_lock lock(_frameLock);
				if (!_pool.empty()) {
					frame = std::move(_pool.back());
					_pool.pop_back();
				}
			}
			frame.width = w;
			frame.height = h;
			frame.pts = timestamp;
			frame.pixels.resize(static_cast<std::size_t>(w) * h * 4);

			const std::size_t rowBytes = static_cast<std::size_t>(w) * 4;
			for (std::uint32_t y = 0; y < h; ++y) {
				std::memcpy(frame.pixels.data() + y * rowBytes, scan0 + static_cast<std::ptrdiff_t>(y) * pitch, rowBytes);
			}

			if (locked2D) {
				buffer2D->Unlock2D();
			} else {
				buffer->Unlock();
			}

			std::scoped_lock lock(_frameLock);
			_frames.push_back(std::move(frame));
		}
	}

	void MediaSource::AudioThread()
	{
		ComScope com;

		while (!_stop) {
			if (_audio->Queued() >= kMaxAudioBuffers) {
				std::this_thread::sleep_for(5ms);
				continue;
			}

			DWORD             streamIndex = 0, flags = 0;
			LONGLONG          timestamp = 0;
			ComPtr<IMFSample> sample;
			const HRESULT     hr = _audioReader->ReadSample(static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM), 0, &streamIndex, &flags, &timestamp, &sample);
			if (FAILED(hr)) {
				log::error("audio ReadSample failed ({})", HrStr(hr));
				_audioEOS = true;
				break;
			}
			if (flags & MF_SOURCE_READERF_ENDOFSTREAM) {
				_audioEOS = true;
				break;
			}
			if (!sample) {
				continue;
			}

			ComPtr<IMFMediaBuffer> buffer;
			if (FAILED(sample->ConvertToContiguousBuffer(&buffer))) {
				continue;
			}
			BYTE* data = nullptr;
			DWORD length = 0;
			if (FAILED(buffer->Lock(&data, nullptr, &length))) {
				continue;
			}
			std::vector<std::uint8_t> pcm(data, data + length);
			buffer->Unlock();

			_audio->Submit(std::move(pcm));
		}
	}
}
