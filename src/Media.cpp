#include "Media.h"
#include <d3d10.h>

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

	namespace
	{
		// GPU decoding: one decode device for every video, on the game's graphics card
		struct GpuDecode
		{
			std::mutex                   lock;
			ComPtr<IDXGIAdapter>         adapter;
			ComPtr<ID3D11Device>         device;
			ComPtr<ID3D11DeviceContext>  context;
			ComPtr<IMFDXGIDeviceManager> manager;
			UINT                         resetToken{ 0 };
			bool                         tried{ false };
			bool                         ok{ false };
		};
		GpuDecode         g_gpu;
		std::atomic<bool> g_gpuDisabled{ false };

		bool EnsureGpuDecode()
		{
			std::scoped_lock lock(g_gpu.lock);
			if (g_gpu.tried) {
				return g_gpu.ok;
			}
			g_gpu.tried = true;
			if (!g_gpu.adapter) {
				log::warn("GPU decoding: graphics card not known yet; decoding on the CPU");
				return false;
			}
			const D3D_FEATURE_LEVEL levels[]{ D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
			HRESULT hr = ::D3D11CreateDevice(g_gpu.adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
				D3D11_CREATE_DEVICE_VIDEO_SUPPORT | D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, 2, D3D11_SDK_VERSION, &g_gpu.device, nullptr,
				&g_gpu.context);
			if (FAILED(hr)) {
				log::warn("GPU decoding: couldn't create a video device ({}); decoding on the CPU", HrStr(hr));
				return false;
			}
			ComPtr<ID3D10Multithread> mt;
			if (SUCCEEDED(g_gpu.device.As(&mt))) {
				mt->SetMultithreadProtected(TRUE);  // decoders on several threads share it
			}
			hr = ::MFCreateDXGIDeviceManager(&g_gpu.resetToken, &g_gpu.manager);
			if (SUCCEEDED(hr)) {
				hr = g_gpu.manager->ResetDevice(g_gpu.device.Get(), g_gpu.resetToken);
			}
			if (FAILED(hr)) {
				log::warn("GPU decoding: no device manager ({}); decoding on the CPU", HrStr(hr));
				g_gpu.manager.Reset();
				g_gpu.device.Reset();
				g_gpu.context.Reset();
				return false;
			}
			g_gpu.ok = true;
			log::info("GPU decoding ready");
			return true;
		}
	}

	void MediaSource::DisableGpu()
	{
		g_gpuDisabled = true;
	}

	void MediaSource::SetGameDevice(ID3D11Device* a_device)
	{
		ComPtr<IDXGIDevice> dxgi;
		if (a_device && SUCCEEDED(a_device->QueryInterface(IID_PPV_ARGS(&dxgi)))) {
			std::scoped_lock lock(g_gpu.lock);
			dxgi->GetAdapter(&g_gpu.adapter);
		}
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

	bool MediaSource::Open(const std::wstring& a_path, bool a_wantAudio, std::uint32_t a_maxHeight, bool a_gpu)
	{
		if (!StartupMF()) {
			return false;
		}
		ComScope com;
		_gpu = a_gpu && !g_gpuDisabled && EnsureGpuDecode();

		if (!OpenVideo(a_path, a_maxHeight)) {
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

	// a_outW/H > 0: ask the decoder for frames scaled to that size (advanced video processing)
	bool MediaSource::CreateVideoReader(const std::wstring& a_path, std::uint32_t a_outW, std::uint32_t a_outH)
	{
		_videoReader.Reset();
		const bool scale = a_outW > 0 && a_outH > 0;
		ComPtr<IMFAttributes> attrs;
		::MFCreateAttributes(&attrs, 3);
		if (_gpu) {
			// decode on the graphics card; frames arrive as textures on our decode device
			attrs->SetUnknown(MF_SOURCE_READER_D3D_MANAGER, g_gpu.manager.Get());
			attrs->SetUINT32(MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING, TRUE);
		} else if (scale) {
			attrs->SetUINT32(MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING, TRUE);  // converts and resizes
		} else {
			attrs->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);  // lets MF convert YUV -> RGB32 for us
		}
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
		type->SetGUID(MF_MT_SUBTYPE, _gpu ? MFVideoFormat_ARGB32 : MFVideoFormat_RGB32);  // GPU: BGRA textures
		if (scale) {
			::MFSetAttributeSize(type.Get(), MF_MT_FRAME_SIZE, a_outW, a_outH);
			::MFSetAttributeRatio(type.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
		}
		hr = _videoReader->SetCurrentMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), nullptr, type.Get());
		if (FAILED(hr)) {
			if (scale) {
				log::warn("the decoder can't scale this video to {}x{} ({}); using its full size", a_outW, a_outH, HrStr(hr));
			} else {
				log::error("could not get RGB32 output from the video decoder ({}). Is the codec supported by Windows?", HrStr(hr));
			}
			_videoReader.Reset();
			return false;
		}
		return UpdateVideoFormat();
	}

	bool MediaSource::OpenVideo(const std::wstring& a_path, std::uint32_t a_maxHeight)
	{
		if (!CreateVideoReader(a_path, 0, 0)) {
			if (!_gpu) {
				return false;
			}
			log::warn("GPU decoding didn't work for this video; decoding on the CPU");
			_gpu = false;
			if (!CreateVideoReader(a_path, 0, 0)) {
				return false;
			}
		}
		// taller than this screen needs: open it again with the decoder scaling it down (keeps the shape)
		if (a_maxHeight > 0 && _height > a_maxHeight) {
			const std::uint32_t fullW = _width, fullH = _height;
			const std::uint32_t outH = a_maxHeight & ~1u;
			const std::uint32_t outW = static_cast<std::uint32_t>(std::lround(static_cast<double>(fullW) * outH / fullH)) & ~1u;
			if (outW > 0 && CreateVideoReader(a_path, outW, outH)) {
				log::info("video: {}x{} scaled down to {}x{} for this screen", fullW, fullH, _width, _height);
			} else if (!CreateVideoReader(a_path, 0, 0)) {
				return false;
			}
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

		// decoders pad the height (1080 -> 1088); only show the real picture area
		std::uint32_t cropX = 0, cropY = 0, showW = w, showH = h;
		MFVideoArea   area{};
		if (SUCCEEDED(cur->GetBlob(MF_MT_MINIMUM_DISPLAY_APERTURE, reinterpret_cast<UINT8*>(&area), sizeof(area), nullptr))) {
			const auto ax = static_cast<std::uint32_t>(std::max<LONG>(area.OffsetX.value, 0));
			const auto ay = static_cast<std::uint32_t>(std::max<LONG>(area.OffsetY.value, 0));
			const auto aw = static_cast<std::uint32_t>(std::max<LONG>(area.Area.cx, 0));
			const auto ah = static_cast<std::uint32_t>(std::max<LONG>(area.Area.cy, 0));
			if (aw > 0 && ah > 0 && ax + aw <= w && ay + ah <= h) {
				cropX = ax;
				cropY = ay;
				showW = aw;
				showH = ah;
			}
		}

		std::scoped_lock lock(_frameLock);
		_frameWidth = w;
		_frameHeight = h;
		_cropX = cropX;
		_cropY = cropY;
		_width = showW;
		_height = showH;
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

		// Ask for one fixed format: 48 kHz, stereo, 16 bit. The source reader resamples / remixes to it.
		// Just asking for "PCM" lets the decoder pick, and some AAC tracks (HE-AAC, common in 30 fps phone and web
		// videos) report one sample rate or channel count up front and then decode at another. The voice was made for
		// the first, so the sound played too fast (chipmunk voices) and the video, which follows the sound, sped up too.
		ComPtr<IMFMediaType> type;
		::MFCreateMediaType(&type);
		type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
		type->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
		type->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
		type->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 2);
		type->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, 48000);
		type->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, 4);
		type->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, 48000 * 4);
		type->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE);
		hr = _audioReader->SetCurrentMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM), nullptr, type.Get());
		if (FAILED(hr)) {
			// older / unusual decoders: let it pick (the format is then checked again while playing)
			log::info("audio: can't convert to 48 kHz stereo ({}); using the track's own format", HrStr(hr));
			::MFCreateMediaType(type.ReleaseAndGetAddressOf());
			type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
			type->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
			type->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
			hr = _audioReader->SetCurrentMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM), nullptr, type.Get());
			if (FAILED(hr)) {
				log::warn("audio decoder can't output PCM ({})", HrStr(hr));
				return false;
			}
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
		_audioRate = wfx->nSamplesPerSec;
		_audioChannels = wfx->nChannels;
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

	void MediaSource::Seek(std::int64_t a_position)
	{
		const std::int64_t last = _duration > 20'000'000 ? _duration - 10'000'000 : 0;  // not past the last second
		RestartAt(std::clamp<std::int64_t>(a_position, 0, last));
	}

	void MediaSource::RestartAt(std::int64_t a_position)
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
		var.hVal.QuadPart = a_position;
		_clockOffset = a_position;
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

	void MediaSource::SeekVideo(std::int64_t a_position)
	{
		if (_duration > 0) {
			a_position = std::clamp<std::int64_t>(a_position, 0, std::max<std::int64_t>(0, _duration - 10'000'000));
		}
		_videoSeek = std::max<std::int64_t>(a_position, 0);
		_frameCv.notify_all();
	}

	std::int64_t MediaSource::Clock() const
	{
		const auto wall = [this]() -> std::int64_t {
			const auto now = _paused ? _pauseStart : steady_t::now();
			const auto elapsed = now - _wallStart - _pausedTotal;
			return _clockOffset + std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count() / 100;
		};

		if (HasAudio() && _audio->SampleRate() > 0) {
			const auto played = _audio->SamplesPlayed();
			const auto samples = played >= _samplesBase ? played - _samplesBase : 0;
			const auto audioClock = _clockOffset + static_cast<std::int64_t>(samples * 10'000'000ull / _audio->SampleRate());
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

	void MediaSource::SetPan(float a_pan)
	{
		if (_audio) {
			_audio->SetPan(a_pan);
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
				_frameCv.wait_for(lock, 20ms, [this] { return _stop.load() || _frames.size() < kMaxFrames || _videoSeek.load() >= 0; });
				if (_stop) {
					break;
				}
				if (const auto jump = _videoSeek.exchange(-1); jump >= 0) {
					// picture-only jump: drop what's queued and read on from there
					for (auto& f : _frames) {
						_pool.push_back(std::move(f));
					}
					_frames.clear();
					PROPVARIANT var;
					::PropVariantInit(&var);
					var.vt = VT_I8;
					var.hVal.QuadPart = jump;
					_videoReader->SetCurrentPosition(GUID_NULL, var);
					_videoEOS = false;
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

			std::uint32_t w, h, frameH, cropX, cropY;
			LONG          defaultStride;
			{
				std::scoped_lock lock(_frameLock);
				w = _width;
				h = _height;
				frameH = _frameHeight;
				cropX = _cropX;
				cropY = _cropY;
				defaultStride = _stride;
			}

			if (_gpu) {
				// the frame is a texture on our decode device: copy it into one of our shared textures
				ComPtr<IMFMediaBuffer> gpuBuffer;
				if (FAILED(sample->GetBufferByIndex(0, &gpuBuffer))) {
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
				frame.pts = timestamp;
				if (GpuFrame(gpuBuffer.Get(), frame, w, h, cropX, cropY)) {
					std::scoped_lock lock(_frameLock);
					_frames.push_back(std::move(frame));
				} else {
					std::scoped_lock lock(_frameLock);
					_pool.push_back(std::move(frame));
				}
				continue;
			}

			ComPtr<IMFMediaBuffer> buffer;
			if (FAILED(sample->ConvertToContiguousBuffer(&buffer))) {
				continue;
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
				if (length < absPitch * frameH) {
					buffer->Unlock();
					continue;
				}
				scan0 = pitch < 0 ? base + absPitch * (frameH - 1) : base;
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
				std::memcpy(frame.pixels.data() + y * rowBytes, scan0 + static_cast<std::ptrdiff_t>(y + cropY) * pitch + cropX * 4, rowBytes);
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

	bool MediaSource::GpuFrame(IMFMediaBuffer* a_buffer, VideoFrame& a_frame, std::uint32_t a_w, std::uint32_t a_h, std::uint32_t a_cropX,
		std::uint32_t a_cropY)
	{
		ComPtr<IMFDXGIBuffer> dxgi;
		if (FAILED(a_buffer->QueryInterface(IID_PPV_ARGS(&dxgi)))) {
			static bool logged = false;
			if (!logged) {
				logged = true;
				log::warn("GPU decoding: the decoder handed back a CPU frame; this video stays blank");
			}
			return false;
		}
		ComPtr<ID3D11Texture2D> src;
		UINT                    sub = 0;
		if (FAILED(dxgi->GetResource(IID_PPV_ARGS(&src))) || FAILED(dxgi->GetSubresourceIndex(&sub))) {
			return false;
		}

		// our shared texture for this frame (kept with the frame in the pool, remade if the size changes)
		if (!a_frame.gpuTexture || a_frame.gpuWidth != a_w || a_frame.gpuHeight != a_h) {
			a_frame.gpuTexture.Reset();
			a_frame.gpuShared = nullptr;
			a_frame.gpuKey = 0;
			D3D11_TEXTURE2D_DESC desc{};
			desc.Width = a_w;
			desc.Height = a_h;
			desc.MipLevels = 1;
			desc.ArraySize = 1;
			desc.Format = DXGI_FORMAT_B8G8R8A8_TYPELESS;
			desc.SampleDesc.Count = 1;
			desc.Usage = D3D11_USAGE_DEFAULT;
			desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
			desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;
			if (FAILED(g_gpu.device->CreateTexture2D(&desc, nullptr, &a_frame.gpuTexture))) {
				return false;
			}
			ComPtr<IDXGIResource> res;
			if (FAILED(a_frame.gpuTexture.As(&res)) || FAILED(res->GetSharedHandle(&a_frame.gpuShared))) {
				a_frame.gpuTexture.Reset();
				return false;
			}
			a_frame.gpuWidth = a_w;
			a_frame.gpuHeight = a_h;
		}

		ComPtr<IDXGIKeyedMutex> mutex;
		if (FAILED(a_frame.gpuTexture.As(&mutex)) || mutex->AcquireSync(a_frame.gpuKey, 100) != S_OK) {
			return false;  // the game still holds it; skip this frame
		}
		D3D11_TEXTURE2D_DESC srcDesc{};
		src->GetDesc(&srcDesc);
		D3D11_BOX box{ a_cropX, a_cropY, 0, std::min(a_cropX + a_w, srcDesc.Width), std::min(a_cropY + a_h, srcDesc.Height), 1 };
		g_gpu.context->CopySubresourceRegion(a_frame.gpuTexture.Get(), 0, 0, 0, 0, src.Get(), sub, &box);
		g_gpu.context->Flush();
		mutex->ReleaseSync(1);
		a_frame.gpuKey = 1;
		a_frame.width = a_w;
		a_frame.height = a_h;
		a_frame.pixels.clear();
		return true;
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
			if (flags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED) {
				// the decoder changed its output format while playing: if it no longer matches the voice, the sound
				// would play at the wrong speed (and the video with it) - make a new voice for the new format
				ComPtr<IMFMediaType> cur;
				WAVEFORMATEX*        wfx = nullptr;
				UINT32               size = 0;
				if (SUCCEEDED(_audioReader->GetCurrentMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM), &cur)) &&
					SUCCEEDED(::MFCreateWaveFormatExFromMFMediaType(cur.Get(), &wfx, &size)) && wfx) {
					if (wfx->nChannels != _audioChannels) {
						log::warn("audio: the track changed from {} to {} channels while playing; the sound may be wrong", _audioChannels,
							wfx->nChannels);
					} else if (wfx->nSamplesPerSec != _audioRate) {
						log::info("audio: the track changed from {} Hz to {} Hz while playing; following it", _audioRate, wfx->nSamplesPerSec);
						for (int i = 0; i < 200 && _audio->Queued() > 0 && !_stop; ++i) {
							std::this_thread::sleep_for(5ms);  // let the sound in the old rate finish first
						}
						// carry the clock over: from here on it counts samples at the new rate
						const auto now = Clock();
						if (_audio->SetSampleRate(wfx->nSamplesPerSec)) {
							_samplesBase = _audio->SamplesPlayed();
							_clockOffset = now;
							_audioRate = wfx->nSamplesPerSec;
						} else {
							log::warn("audio: couldn't switch the voice to {} Hz", wfx->nSamplesPerSec);
						}
					}
					::CoTaskMemFree(wfx);
				}
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
