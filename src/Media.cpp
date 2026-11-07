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
		// a readable name for a Media Foundation codec / format. Matched on the GUID's first field (the wave format tag or
		// FourCC), so it doesn't need the newest Windows SDK for Opus / FLAC / AV1 and friends.
		std::string SubtypeName(const GUID& a_sub)
		{
			constexpr auto fcc = [](char a, char b, char c, char d) {
				return static_cast<std::uint32_t>(static_cast<std::uint8_t>(a)) | (static_cast<std::uint32_t>(static_cast<std::uint8_t>(b)) << 8) |
				       (static_cast<std::uint32_t>(static_cast<std::uint8_t>(c)) << 16) | (static_cast<std::uint32_t>(static_cast<std::uint8_t>(d)) << 24);
			};
			struct Name
			{
				std::uint32_t id;
				const char*   name;
			};
			static const Name kNames[]{
				{ 0x1610, "AAC" }, { 0x00FF, "AAC (raw)" }, { 0x0055, "MP3" }, { 0x0050, "MPEG audio" }, { 0x2000, "AC-3" },
				{ 0xA7FB87AF, "E-AC-3" }, { 0xE06D802C, "AC-3" }, { 0x0001, "PCM" }, { 0x0003, "float PCM" }, { 0x0161, "WMA" }, { 0x0162, "WMA Pro" },
				{ 0xF1AC, "FLAC" }, { 0x704F, "Opus" }, { 0x6C61, "ALAC" }, { fcc('H', '2', '6', '4'), "H.264" },
				{ fcc('h', '2', '6', '4'), "H.264" }, { fcc('H', 'E', 'V', 'C'), "HEVC" }, { fcc('V', 'P', '9', '0'), "VP9" },
				{ fcc('V', 'P', '8', '0'), "VP8" }, { fcc('A', 'V', '0', '1'), "AV1" }, { fcc('M', 'P', '4', 'V'), "MPEG-4 part 2" },
				{ fcc('X', 'V', 'I', 'D'), "Xvid" }, { fcc('D', 'I', 'V', 'X'), "DivX" }, { fcc('W', 'M', 'V', '3'), "WMV" },
				{ fcc('W', 'V', 'C', '1'), "VC-1" }, { fcc('M', 'J', 'P', 'G'), "MJPEG" },
			};
			for (const auto& n : kNames) {
				if (a_sub.Data1 == n.id) {
					return n.name;
				}
			}
			return std::format("{:08X}", a_sub.Data1);  // anything else: its id
		}

		// 16-bit PCM from a_srcCh channels to a_dstCh (1 or 2). Surround is in the usual order: front left, front right,
		// centre, LFE, then the back / side pairs; centre and surrounds are mixed in at -3 dB, LFE left out.
		std::vector<std::uint8_t> Downmix16(const BYTE* a_data, DWORD a_bytes, std::uint32_t a_srcCh, std::uint32_t a_dstCh)
		{
			const auto* in = reinterpret_cast<const std::int16_t*>(a_data);
			const auto  frames = a_bytes / (2 * a_srcCh);
			std::vector<std::uint8_t> out(frames * 2 * a_dstCh);
			auto*                     o = reinterpret_cast<std::int16_t*>(out.data());
			const auto clamp = [](float a_v) { return static_cast<std::int16_t>(std::clamp(a_v, -32768.0f, 32767.0f)); };
			for (DWORD f = 0; f < frames; ++f) {
				const std::int16_t* s = in + f * a_srcCh;
				float               l = s[0], r = a_srcCh > 1 ? s[1] : s[0];
				if (a_srcCh >= 3) {
					l += 0.707f * s[2];
					r += 0.707f * s[2];
				}
				for (std::uint32_t c = 4; c + 1 < a_srcCh; c += 2) {  // surround pairs
					l += 0.707f * s[c];
					r += 0.707f * s[c + 1];
				}
				const float scale = a_srcCh >= 3 ? 0.5f : 1.0f;  // keep the sum from clipping
				if (a_dstCh == 1) {
					o[f] = clamp((l + r) * 0.5f * scale);
				} else {
					o[f * a_dstCh] = clamp(l * scale);
					o[f * a_dstCh + 1] = clamp(r * scale);
					for (std::uint32_t c = 2; c < a_dstCh; ++c) {
						o[f * a_dstCh + c] = 0;
					}
				}
			}
			return out;
		}

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

		{
			// the track as stored in the file: codec and frame rate (for problem reports)
			double       fps = 0.0;
			std::string  codec = "?";
			ComPtr<IMFMediaType> native;
			if (SUCCEEDED(_videoReader->GetNativeMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), 0, &native))) {
				UINT32 num = 0, den = 0;
				if (SUCCEEDED(::MFGetAttributeRatio(native.Get(), MF_MT_FRAME_RATE, &num, &den)) && den) {
					fps = static_cast<double>(num) / den;
				}
				GUID sub{};
				if (SUCCEEDED(native->GetGUID(MF_MT_SUBTYPE, &sub))) {
					codec = SubtypeName(sub);
				}
			}
			_frameDuration = fps >= 1.0 && fps <= 240.0 ? static_cast<std::int64_t>(1.0e7 / fps) : 333'667;  // unknown: 29.97
			log::info("video: {}x{} stride {} duration {:.1f}s, {} at {:.2f} fps", _width, _height, _stride, _duration / 1.0e7, codec, fps);
		}
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

		// the track as stored in the file (for problem reports: HE-AAC / mono / odd rates are what used to play too fast)
		{
			ComPtr<IMFMediaType> native;
			if (SUCCEEDED(_audioReader->GetNativeMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM), 0, &native))) {
				GUID   sub{};
				UINT32 rate = 0, channels = 0, profile = 0;
				native->GetGUID(MF_MT_SUBTYPE, &sub);
				native->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &rate);
				native->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &channels);
				const bool hasProfile = SUCCEEDED(native->GetUINT32(MF_MT_AAC_AUDIO_PROFILE_LEVEL_INDICATION, &profile));
				log::info("audio track: {}{}, {} Hz, {} ch", SubtypeName(sub), hasProfile ? std::format(" (profile 0x{:02X})", profile) : std::string{},
					rate, channels);
			}
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
		_srcChannels = wfx->nChannels;
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
		_anchorPending = HasAudio();  // the first sound decoded sets the clock (see AudioThread)
		_waitSince = {};
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
		_askedPos = a_position;
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
			const std::uint64_t base = _samplesBase.load();
			const auto          samples = played >= base ? played - base : 0;
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
					++_late;
				}
				a_out = std::move(_frames.front());
				_frames.pop_front();
				got = true;
			}
			// The next picture is more than a second ahead of the sound and the queue is full (so nothing else can come):
			// the picture would sit on one frame while the sound plays. After 2 s of that, move the clock to the picture.
			const auto now = steady_t::now();
			if (!got && !_paused && _frames.size() >= kMaxFrames && _frames.front().pts > a_clock + 10'000'000) {
				if (_waitSince == steady_t::time_point{}) {
					_waitSince = now;
				} else if (now - _waitSince > 2s) {
					const auto ahead = _frames.front().pts - a_clock;
					if (!_catchUpLogged) {
						_catchUpLogged = true;
						log::warn("video: the next picture is {:.1f} s ahead of the sound (picture at {:.1f} s, sound at {:.1f} s); catching the clock up",
							ahead / 1.0e7, _frames.front().pts / 1.0e7, a_clock / 1.0e7);
					}
					_clockOffset += ahead;
					_waitSince = {};
				}
			} else {
				_waitSince = {};
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

	std::string MediaSource::Stats() const
	{
		const double secs = std::chrono::duration<double>(steady_t::now() - _openedAt).count();
		const auto   decoded = _decoded.load();
		return std::format("decoded {} frames in {:.1f} s ({:.1f} a second, the video has {:.1f}), {} too late to show", decoded, secs,
			secs > 0 ? decoded / secs : 0.0, _frameDuration > 0 ? 1.0e7 / static_cast<double>(_frameDuration) : 0.0, _late.load());
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
		// the game keeps all cores busy; without this, decoding can fall far behind on some PCs (a few frames a second)
		::SetThreadPriority(::GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);

		// Frame times as the decoder gives them can be broken (some MKV files through Windows' MKV reader: the frames
		// after a keyframe come out stamped far ahead of where they belong). The picture is shown when the sound's clock
		// reaches a frame's time, so such a frame - and everything queued behind it - would never come, and the picture
		// sticks on the first frame after every start or jump while the sound plays on. A time that's far from the
		// previous frame's plus one frame is taken as the previous frame's plus one frame instead. Small differences
		// (frame reordering, a slightly uneven frame rate) are left alone.
		std::int64_t                lastPts = -1;
		std::vector<std::int64_t>   firstTimes;
		auto fixTime = [&](std::int64_t a_ts, IMFSample* a_sample) {
			LONGLONG dur = 0;
			const std::int64_t step = a_sample && SUCCEEDED(a_sample->GetSampleDuration(&dur)) && dur > 0 && dur < 10'000'000 ? dur : _frameDuration;
			if (!_timesLogged && firstTimes.size() < 8) {
				firstTimes.push_back(a_ts);
				if (firstTimes.size() == 8) {
					_timesLogged = true;
					std::string list;
					for (const auto t : firstTimes) {
						list += std::format("{}{:.3f}", list.empty() ? "" : " ", t / 1.0e7);
					}
					log::info("video: first frame times {} s (one frame {:.3f} s)", list, step / 1.0e7);
				}
			}
			if (lastPts < 0) {
				lastPts = a_ts;  // the first frame after a start or jump: trust it
				return a_ts;
			}
			const std::int64_t expected = lastPts + step;
			constexpr std::int64_t kTolerance = 10'000'000;  // 1 s
			std::int64_t           ts = a_ts;
			if (a_ts > expected + kTolerance || a_ts < lastPts - kTolerance) {
				if (!_fixLogged) {
					_fixLogged = true;
					log::warn("video: the file's frame times are broken (a frame stamped {:.3f} s, after one at {:.3f} s); timing the "
							  "picture by the frame rate instead",
						a_ts / 1.0e7, lastPts / 1.0e7);
				}
				ts = expected;
			}
			lastPts = std::max(lastPts, ts);
			return ts;
		};

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
					lastPts = -1;  // trust the first frame after the jump again
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
			timestamp = fixTime(timestamp, sample.Get());

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
					++_decoded;
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

			++_decoded;
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
		static std::atomic<bool> s_failLogged{ false };
		const auto fail = [&](const char* a_why) {
			if (!s_failLogged.exchange(true)) {
				log::warn("GPU decoding: a frame couldn't be copied ({}); after a few seconds this video switches to CPU decoding", a_why);
			}
			return false;
		};
		if (FAILED(dxgi->GetResource(IID_PPV_ARGS(&src))) || FAILED(dxgi->GetSubresourceIndex(&sub))) {
			return fail("the decoder's frame has no texture");
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
			// Some drivers only share a texture that can also be drawn into (render target), or only in a plain format:
			// try the variants in turn and remember the one that works.
			struct Variant
			{
				DXGI_FORMAT format;
				UINT        bind;
				const char* name;
			};
			static const Variant kVariants[]{
				{ DXGI_FORMAT_B8G8R8A8_TYPELESS, D3D11_BIND_SHADER_RESOURCE, "typeless" },
				{ DXGI_FORMAT_B8G8R8A8_TYPELESS, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET, "typeless + render target" },
				{ DXGI_FORMAT_B8G8R8A8_UNORM, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET, "BGRA + render target" },
				{ DXGI_FORMAT_B8G8R8A8_UNORM, D3D11_BIND_SHADER_RESOURCE, "BGRA" },
			};
			static std::atomic<int> s_variant{ -1 };  // the one that worked
			std::string tried;
			bool        made = false;
			for (int v = 0; v < static_cast<int>(std::size(kVariants)) && !made; ++v) {
				if (s_variant >= 0 && v != s_variant) {
					continue;
				}
				desc.Format = kVariants[v].format;
				desc.BindFlags = kVariants[v].bind;
				const HRESULT hr = g_gpu.device->CreateTexture2D(&desc, nullptr, a_frame.gpuTexture.ReleaseAndGetAddressOf());
				if (SUCCEEDED(hr)) {
					made = true;
					if (s_variant.exchange(v) != v && v > 0) {
						log::info("GPU decoding: shared textures work here as {}", kVariants[v].name);
					}
				} else {
					tried += std::format("{}{}: {}", tried.empty() ? "" : ", ", kVariants[v].name, HrStr(hr));
				}
			}
			if (!made) {
				return fail(std::format("couldn't make a {}x{} shared texture ({})", a_w, a_h, tried).c_str());
			}
			ComPtr<IDXGIResource> res;
			if (FAILED(a_frame.gpuTexture.As(&res)) || FAILED(res->GetSharedHandle(&a_frame.gpuShared))) {
				a_frame.gpuTexture.Reset();
				return fail("couldn't share the texture with the game");
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
		static std::atomic<bool> s_formatLogged{ false };
		if (srcDesc.Format != DXGI_FORMAT_B8G8R8A8_UNORM && srcDesc.Format != DXGI_FORMAT_B8G8R8A8_TYPELESS &&
			srcDesc.Format != DXGI_FORMAT_B8G8R8A8_UNORM_SRGB && srcDesc.Format != DXGI_FORMAT_B8G8R8X8_UNORM &&
			!s_formatLogged.exchange(true)) {
			log::warn("GPU decoding: the decoder's frames are DXGI format {}, not BGRA ({}x{}); the picture may not show",
				static_cast<int>(srcDesc.Format), srcDesc.Width, srcDesc.Height);
		}
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
					if (wfx->nChannels != _srcChannels && wfx->nSamplesPerSec == _audioRate && wfx->wBitsPerSample == 16 && wfx->nChannels > 0) {
						// e.g. AC-3 that says 2 channels and then decodes 5.1: mix it down to the voice's channels ourselves
						log::info("audio: the track changed from {} to {} channels while playing; mixing it to {}", _srcChannels, wfx->nChannels,
							_audioChannels);
						_srcChannels = wfx->nChannels;
					} else if (wfx->nChannels != _srcChannels) {
						log::warn("audio: the track changed from {} to {} channels ({} Hz) while playing; the sound may be wrong", _srcChannels,
							wfx->nChannels, wfx->nSamplesPerSec);
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
			// the first sound after a start or jump: the clock starts where the sound really is. A jump can land a few
			// seconds from where it was asked (MKV / AVI files seek to the nearest index point); counting from the asked
			// spot left the sound and the picture apart, and the picture waited on one frame.
			if (_anchorPending) {
				// a jump landed before the asked spot: skip the sound up to it, so the sound starts where it was asked and
				// the picture (whose earlier frames are skipped too) can play right away instead of waiting
				LONGLONG dur = 0;
				sample->GetSampleDuration(&dur);
				if (timestamp + dur < _askedPos && _askedPos - timestamp < 600'000'000) {  // within 60 s
					continue;
				}
				_anchorPending = false;
				if (std::abs(timestamp - _askedPos) > 2'500'000) {
					log::info("audio: the sound starts at {:.2f} s (asked for {:.2f} s); the clock follows the sound", timestamp / 1.0e7,
						_askedPos / 1.0e7);
				}
				_samplesBase = _audio->SamplesPlayed();
				_clockOffset = timestamp;
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
			std::vector<std::uint8_t> pcm;
			if (_srcChannels != _audioChannels && _srcChannels > 0 && _audioChannels > 0) {
				pcm = Downmix16(data, length, _srcChannels, _audioChannels);
			} else {
				pcm.assign(data, data + length);
			}
			buffer->Unlock();
			length = static_cast<DWORD>(pcm.size());

			// once, early on: does the sound arriving match the speed the voice plays at? (bytes per second of track time)
			if (!_audioChecked && _blockAlign > 0) {
				LONGLONG duration = 0;
				if (SUCCEEDED(sample->GetSampleDuration(&duration)) && duration > 0) {
					_checkFrames += length / _blockAlign;
					_checkTime += duration;
					if (_checkTime >= 20'000'000) {  // 2 s of track time
						_audioChecked = true;
						const double measured = static_cast<double>(_checkFrames) * 1e7 / static_cast<double>(_checkTime);
						const double voice = static_cast<double>(_audio->SampleRate());
						if (voice > 0 && std::abs(measured / voice - 1.0) > 0.05) {
							log::warn("audio check: the sound arrives at {:.0f} Hz but plays at {:.0f} Hz - it will play {:.2f}x too {}", measured,
								voice, voice / measured, voice > measured ? "fast" : "slow");
						} else {
							log::info("audio check: speed ok ({:.0f} Hz)", measured);
						}
					}
				}
			}

			_audio->Submit(std::move(pcm));
		}
	}
}
