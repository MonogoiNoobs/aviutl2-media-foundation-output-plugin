/**
 * \copyright	SPDX-License-Identifier: MIT
 * \year		2025-2026
 * \author		Shion Yorigami <62567343+MonogoiNoobs@users.noreply.github.com>
 */

module;

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

export module mfop:configure;

import std;

namespace mfop
{
	namespace configure
	{
		export
		{
			void constexpr open_dialog(HWND &window, HINSTANCE &instance) noexcept;

			enum struct video_quality : std::uint32_t {};
			enum struct audio_bit_rate : std::uint32_t {};
			enum struct is_hevc_preferable : bool {};
			enum struct is_accelerated : bool {};

			template<typename Key> std::underlying_type<Key>::type constexpr get() noexcept;
			template<typename Key> bool constexpr set(std::int32_t &&value) noexcept;
		}
	}
}
