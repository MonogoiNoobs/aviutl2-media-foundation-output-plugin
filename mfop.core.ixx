module;

#define STRICT
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include "aviutl2_sdk/output2.h"
#include "aviutl2_sdk/logger2.h"

export module mfop.core;

import std;

namespace mfop
{
	export 
	{
		void output_file
		(
			OUTPUT_INFO const *const &oip,
			std::uint32_t const &video_quality,
			std::uint32_t const audio_bit_rate,
			bool const &is_hevc_preferred,
			bool const &is_accelerated,
			LOG_HANDLE *logger = nullptr
		); 
	}
}

