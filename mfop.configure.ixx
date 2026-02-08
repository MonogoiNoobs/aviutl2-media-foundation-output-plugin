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
			enum struct keys : std::int32_t
			{
				video_quality,
				audio_bit_rate,
				is_hevc_preferred,
				is_accelerated
			};
			auto set(keys &&key, std::int32_t &&value) -> void;
			auto set(keys &&key, wchar_t const *value) -> void;
			auto open_configuration_dialog(HWND window, HINSTANCE instance) -> void;

			enum struct video_quality : std::uint32_t {};
			enum struct audio_bit_rate : std::uint32_t {};
			enum struct is_hevc_preferred : bool {};
			enum struct is_accelerated : bool {};

			template<typename Key> std::underlying_type<Key>::type get();
		}
	}
}
