/**
 * \copyright	SPDX-License-Identifier: MIT
 * \year		2025-2026
 * \author		Shion Yorigami <62567343+MonogoiNoobs@users.noreply.github.com>
 */

#define STRICT
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include "aviutl2_sdk/output2.h"
#include "aviutl2_sdk/logger2.h"

import std;
import mfop;

static LOG_HANDLE *aviutl_logger;

auto string_to_wstring(std::string_view str)
{
	auto const required_size{ static_cast<std::size_t>(MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, str.data(), static_cast<std::int32_t>(str.size()), nullptr, 0)) };
	std::wstring wstr(required_size, L'\0');
	MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, str.data(), static_cast<std::int32_t>(str.size()), &wstr.front(), static_cast<std::int32_t>(required_size));
	return wstr;
}

auto func_output(OUTPUT_INFO *oip) noexcept
{
	using namespace mfop::configure;

	auto const result{ mfop::output_file
	(
		*oip,
		mfop::output_configuration
		{
			get<video_quality>(),
			get<audio_bit_rate>(),
			get<is_hevc_preferable>(),
			get<is_accelerated>()
		},
		*aviutl_logger
	) };

	if (!result)
	{
		aviutl_logger->error(aviutl_logger, std::format(L"FAILED TO OUTPUT: {} (0x800{:x}{:04x}) in {}.", string_to_wstring(std::system_category().message(result.error().code)), HRESULT_FACILITY(result.error().code), HRESULT_CODE(result.error().code), string_to_wstring(result.error().where)).c_str());
		return false;
	}

	aviutl_logger->info(aviutl_logger, L"Output completed successfully.");

	return true;
}

auto func_config(HWND window, HINSTANCE instance)
{
	mfop::configure::open_dialog(window, instance);
	return true;
}

extern "C"
{
	__declspec(dllexport) auto InitializeLogger(LOG_HANDLE *logger) noexcept
	{
		aviutl_logger = logger;
	}

	__declspec(dllexport) auto GetOutputPluginTable() noexcept
	{
		static OUTPUT_PLUGIN_TABLE constexpr output_plugin_table{
			OUTPUT_PLUGIN_TABLE::FLAG_VIDEO | OUTPUT_PLUGIN_TABLE::FLAG_AUDIO, //	フラグ
			L"Media Foundation 出力",					// プラグインの名前
			L"MP4 (*.mp4)\0*.mp4\0Advanced Systems Format (*.wmv)\0*.wmv\0",					// 出力ファイルのフィルタ
			L"MFOutput (" __DATE__ ") by MonogoiNoobs",	// プラグインの情報
			func_output,									// 出力時に呼ばれる関数へのポインタ
			func_config,									// 出力設定のダイアログを要求された時に呼ばれる関数へのポインタ (nullptrなら呼ばれません)
			nullptr,							// 出力設定のテキスト情報を取得する時に呼ばれる関数へのポインタ (nullptrなら呼ばれません)
		};
		return &output_plugin_table;
	}
}
