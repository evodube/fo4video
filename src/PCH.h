#pragma once

// CommonLib must come before any Windows API header (it checks for this).
#pragma warning(push)
#include "F4SE/F4SE.h"
#include "RE/Fallout.h"
#include "REX/REX.h"
#pragma warning(pop)

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cfloat>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <cwctype>
#include <deque>
#include <list>
#include <filesystem>
#include <format>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

// Windows / D3D11 / Media Foundation / XAudio2 (after CommonLib)
#include <Windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <xaudio2.h>
#include <wrl/client.h>

// wingdi.h defines ERROR, which collides with REX::ERROR. Use REX::WARN / fv::log instead.
#ifdef ERROR
#	undef ERROR
#endif

using namespace std::literals;
using Microsoft::WRL::ComPtr;

namespace fv::log
{
	template <class... Args>
	void info(std::format_string<Args...> a_fmt, Args&&... a_args)
	{
		REX::Impl::Log(std::source_location::current(), REX::ELogLevel::Info, a_fmt, std::forward<Args>(a_args)...);
	}

	template <class... Args>
	void warn(std::format_string<Args...> a_fmt, Args&&... a_args)
	{
		REX::Impl::Log(std::source_location::current(), REX::ELogLevel::Warning, a_fmt, std::forward<Args>(a_args)...);
	}

	template <class... Args>
	void error(std::format_string<Args...> a_fmt, Args&&... a_args)
	{
		REX::Impl::Log(std::source_location::current(), REX::ELogLevel::Error, a_fmt, std::forward<Args>(a_args)...);
	}

	template <class... Args>
	void debug(std::format_string<Args...> a_fmt, Args&&... a_args)
	{
		REX::Impl::Log(std::source_location::current(), REX::ELogLevel::Debug, a_fmt, std::forward<Args>(a_args)...);
	}
}
