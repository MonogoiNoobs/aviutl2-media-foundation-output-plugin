/**
 * \copyright	SPDX-License-Identifier: MIT
 * \year		2025-2026
 * \author		Shion Yorigami <62567343+MonogoiNoobs@users.noreply.github.com>
 */

module;

#define STRICT
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include "aviutl2_sdk/output2.h"
#include "aviutl2_sdk/logger2.h"

export module mfop.core;

import std;
import mfop.configure;

namespace mfop
{
	export 
	{
		struct output_configuration
		{
			std::underlying_type<configure::video_quality>::type video_quality;
			std::underlying_type<configure::audio_bit_rate>::type audio_bit_rate;
			std::underlying_type<configure::is_hevc_preferable>::type is_hevc_preferable;
			std::underlying_type<configure::is_accelerated>::type is_accelerated;
		};

		std::expected<HRESULT, HRESULT> output_file
		(
			OUTPUT_INFO const &oip,
			output_configuration &&configuration,
			LOG_HANDLE &logger
		); 
	}
}

