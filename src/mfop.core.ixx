/**
 * \copyright	SPDX-License-Identifier: MIT
 * \year		2025-2026
 * \author		Shion Yorigami <62567343+MonogoiNoobs@users.noreply.github.com>
 */

module;

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include "aviutl2_sdk/output2.h"
#include "aviutl2_sdk/logger2.h"

export module mfop:core;

import std;
import :configure;

namespace mfop
{
	export 
	{
		struct error
		{
			HRESULT code;
			std::string where;
		};

		std::expected<HRESULT, error> constexpr output_file
		(
			OUTPUT_INFO const &oip,
			configure::output_configuration const &configuration,
			LOG_HANDLE &logger
		); 
	}
}
