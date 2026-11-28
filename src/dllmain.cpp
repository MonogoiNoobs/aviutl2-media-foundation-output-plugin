/**
 * \copyright	SPDX-License-Identifier: MIT
 * \year		2025-2026
 * \author		Shion Yorigami <62567343+MonogoiNoobs@users.noreply.github.com>
 */

#pragma warning(push)
#pragma warning(disable: 26812)
#include <CppCoreCheck\Warnings.h>
#pragma warning(pop)

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include "aviutl2_sdk/output2.h"
#include "aviutl2_sdk/logger2.h"
#include <gsl/util>
#include <gsl/pointers>

#pragma warning(push)
#pragma warning(disable: ALL_CPPCORECHECK_WARNINGS)
#pragma warning(disable: 26433 26434 26456 28182)
#include <wil/stl.h>
#pragma warning(pop)

import std;
import mfop;

static LOG_HANDLE *aviutl_logger;
static mfop::configure::output_configuration *config;

auto constexpr utf8_to_utf16(wil::zstring_view source, wchar_t *dest = nullptr, std::int32_t const dest_size = 0)
{
	return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, source.data(), gsl::narrow_cast<std::int32_t>(source.size()), dest, dest_size);
}

auto constexpr widen_string(wil::zstring_view str)
{
	auto const required_size{ utf8_to_utf16(str) };
	std::wstring wstr(required_size, L'\0');
	std::ignore = utf8_to_utf16(str, wstr.data(), required_size);
	return wstr;
}

[[gsl::suppress("f.23", justification: "Required by OUTPUT_INFO")]]
[[gsl::suppress("con.3", justification: "Required by OUTPUT_INFO")]]
auto constexpr output(OUTPUT_INFO *oip) noexcept
{
	using namespace mfop::configure;

	if (auto const succeeded{ mfop::output_file(*oip, *config, *aviutl_logger)}; not succeeded)
	{
		aviutl_logger->error
		(
			aviutl_logger,
			std::format
			(
				L"FAILED TO OUTPUT: {} ({}, in {})",
				widen_string(std::system_category().message(succeeded.error().code)),
				succeeded.error().code,
				widen_string(succeeded.error().where)
			).data()
		);
		return false;
	}

	aviutl_logger->info(aviutl_logger, L"Output completed successfully.");

	return true;
}

auto constexpr open_configuration_dialog(HWND window, HINSTANCE instance)
{
	mfop::configure::open_dialog(window, instance);
	return true;
}

[[gsl::suppress("f.23", justification: "Required by OUTPUT_INFO")]]
[[gsl::suppress("con.3", justification: "Required by OUTPUT_INFO")]]
auto constexpr load_project_configuration(PROJECT_FILE *project_file)
{
	config = &mfop::configure::load(*project_file);
	return true;
}

[[gsl::suppress("f.23", justification: "Required by OUTPUT_INFO")]]
[[gsl::suppress("con.3", justification: "Required by OUTPUT_INFO")]]
auto constexpr store_project_configuration(PROJECT_FILE *project_file)
{
	return mfop::configure::store(*gsl::make_strict_not_null(project_file));
}

auto WINAPI DllMain([[maybe_unused]] HINSTANCE instance, DWORD reason, void *reserved)
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
			output,									// 出力時に呼ばれる関数へのポインタ
			open_configuration_dialog,									// 出力設定のダイアログを要求された時に呼ばれる関数へのポインタ (nullptrなら呼ばれません)
			nullptr,							// 出力設定のテキスト情報を取得する時に呼ばれる関数へのポインタ (nullptrなら呼ばれません)
			load_project_configuration,
			store_project_configuration,
		};
		return &output_plugin_table;
	}
}
