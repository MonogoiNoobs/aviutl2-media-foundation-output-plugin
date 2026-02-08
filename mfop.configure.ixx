module;

#define STRICT
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

export module mfop.configure;

import std;

namespace mfop
{
	namespace configure
	{
		export
		{
			auto open_dialog(HWND window, HINSTANCE instance) -> void;

			enum struct video_quality : std::uint32_t {};
			enum struct audio_bit_rate : std::uint32_t {};
			enum struct is_hevc_preferred : bool {};
			enum struct is_accelerated : bool {};

			template<typename Key> std::underlying_type<Key>::type get();
			template<typename Key> void set(std::int32_t &&value);
			template<typename Key> void set(wchar_t const *const &&value);
		}
	}
}
