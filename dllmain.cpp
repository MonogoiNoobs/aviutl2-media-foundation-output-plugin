import std;
import mfop;

#define STRICT
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include "aviutl2_sdk/output2.h"
#include "aviutl2_sdk/logger2.h"

LOG_HANDLE constinit *aviutl_logger{};

auto func_output(OUTPUT_INFO *oip)
{
	auto const video_quality{ std::any_cast<uint32_t>(mfop::configure::get(mfop::configure::keys::video_quality)) };
	auto const audio_bit_rate{ std::any_cast<uint32_t>(mfop::configure::get(mfop::configure::keys::audio_bit_rate)) };
	auto const is_hevc_preferred{ std::any_cast<bool>(mfop::configure::get(mfop::configure::keys::is_mp4_preferred_hevc)) };
	auto const is_accelerated{ std::any_cast<bool>(mfop::configure::get(mfop::configure::keys::is_accelerated)) };

	mfop::output_file(oip, video_quality, audio_bit_rate, is_hevc_preferred, is_accelerated, aviutl_logger);

	return true;
}

auto func_config(HWND window, HINSTANCE instance)
{
	mfop::configure::open_configuration_dialog(window, instance);
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
		static auto constexpr output_plugin_table{ OUTPUT_PLUGIN_TABLE{
			OUTPUT_PLUGIN_TABLE::FLAG_VIDEO | OUTPUT_PLUGIN_TABLE::FLAG_AUDIO, //	フラグ
			L"Media Foundation 出力",					// プラグインの名前
			L"MP4 (*.mp4)\0*.mp4\0Advanced Systems Format (*.wmv)\0*.wmv\0",					// 出力ファイルのフィルタ
			L"MFOutput (" __DATE__ ") by MonogoiNoobs",	// プラグインの情報
			func_output,									// 出力時に呼ばれる関数へのポインタ
			func_config,									// 出力設定のダイアログを要求された時に呼ばれる関数へのポインタ (nullptrなら呼ばれません)
			nullptr,							// 出力設定のテキスト情報を取得する時に呼ばれる関数へのポインタ (nullptrなら呼ばれません)
		} };
		return &output_plugin_table;
	}
}

