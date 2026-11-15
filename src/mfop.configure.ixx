/**
 * \copyright	SPDX-License-Identifier: MIT
 * \year		2025-2026
 * \author		Shion Yorigami <62567343+MonogoiNoobs@users.noreply.github.com>
 */

module;

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include "aviutl2_sdk/plugin2.h"

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

			struct output_configuration
			{
				std::underlying_type<video_quality>::type video_quality;
				std::underlying_type<audio_bit_rate>::type audio_bit_rate;
				std::underlying_type<is_hevc_preferable>::type is_hevc_preferable;
				std::underlying_type<is_accelerated>::type is_accelerated;
			};

			output_configuration constexpr &load(PROJECT_FILE const &project_file) noexcept;
			bool constexpr store(PROJECT_FILE const &project_file) noexcept;
		}
	}
}
