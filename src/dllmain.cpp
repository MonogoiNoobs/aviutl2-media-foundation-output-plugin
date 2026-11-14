/**
 * \copyright	SPDX-License-Identifier: MIT
 * \year		2025-2026
 * \author		Shion Yorigami <62567343+MonogoiNoobs@users.noreply.github.com>
 */

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <wil/result.h>
#include "aviutl2_sdk/output2.h"
#include "aviutl2_sdk/logger2.h"

import std;
import mfop;

static LOG_HANDLE *aviutl_logger;
static mfop::configure::output_configuration *config;

auto constexpr utf8_to_utf16(std::string_view source, wchar_t *dest = nullptr, std::int32_t const dest_size = 0)
{
	return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, source.data(), static_cast<std::int32_t>(source.size()), dest, dest_size);
}

auto constexpr string_to_wstring(std::string_view str)
{
	auto const required_size{ utf8_to_utf16(str) };
	std::wstring wstr(required_size, L'\0');
	std::ignore = utf8_to_utf16(str, wstr.data(), required_size);
	return wstr;
}

auto constexpr func_output(OUTPUT_INFO *oip) noexcept
{
	using namespace mfop::configure;

	if (auto const succeeded{ mfop::output_file(*oip, *config, *aviutl_logger) }; not succeeded)
	{
		aviutl_logger->error
		(
			aviutl_logger,
			std::format
			(
				L"FAILED TO OUTPUT: {} ({}, in {})",
				string_to_wstring(std::system_category().message(succeeded.error().code)),
				succeeded.error().code,
				string_to_wstring(succeeded.error().where)
			).c_str()
		);
		return false;
	}

	aviutl_logger->info(aviutl_logger, L"Output completed successfully.");

	return true;
}

auto constexpr func_config(HWND window, HINSTANCE instance)
{
	mfop::configure::open_dialog(window, instance);
	return true;
}

auto constexpr func_load_project_config(PROJECT_FILE *project_file)
{
	config = mfop::configure::load(*project_file);
	return true;
}

auto constexpr func_save_project_config(PROJECT_FILE *project_file)
{
	return mfop::configure::store(*project_file);
}

auto WINAPI DllMain(HINSTANCE instance, DWORD reason, void *reserved)
{
	wil::DLLMain(instance, reason, reserved);
	return true;
}

extern "C"
{
	__declspec(dllexport) auto constexpr InitializeLogger(LOG_HANDLE *logger) noexcept
	{
		aviutl_logger = logger;
	}

	__declspec(dllexport) auto constexpr GetOutputPluginTable() noexcept
	{
		static OUTPUT_PLUGIN_TABLE constexpr output_plugin_table{
			OUTPUT_PLUGIN_TABLE::FLAG_VIDEO bitor OUTPUT_PLUGIN_TABLE::FLAG_AUDIO bitor OUTPUT_PLUGIN_TABLE::FLAG_PROJECT_CONFIG, //	フラグ
			L"Media Foundation 出力",					// プラグインの名前
			L"MP4 (*.mp4; *.m4a)\0"							"*.mp4;*.m4a\0"
			"Advanced Systems Format (*.wmv; *.wma)\0"		"*.wmv;*.wma\0"
			,
			L"MFOutput.auo2 (github.com/MonogoiNoobs)",	// プラグインの情報
			func_output,									// 出力時に呼ばれる関数へのポインタ
			func_config,									// 出力設定のダイアログを要求された時に呼ばれる関数へのポインタ (nullptrなら呼ばれません)
			nullptr,							// 出力設定のテキスト情報を取得する時に呼ばれる関数へのポインタ (nullptrなら呼ばれません)
			func_load_project_config,
			func_save_project_config,
		};
		return &output_plugin_table;
	}
}
