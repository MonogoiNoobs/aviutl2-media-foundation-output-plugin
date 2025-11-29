module;

#define STRICT
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <windowsx.h>
#include "resource.h"
#include <wil/com.h>

export module mfop.configure;

import std;

namespace mfop
{
namespace configure
{
export {
	enum struct keys
	{
		video_quality,
		audio_bit_rate,
		is_mp4_preferred_hevc,
		is_accelerated
	};
	auto get(keys &&key);
	auto set(keys &&key, int32_t &&value);
	auto open_configuration_dialog(HWND window, HINSTANCE instance);
}
}
}
module :private;

namespace mfop
{
namespace configure
{
auto const constinit configuration_ini_path{ LR"(C:\ProgramData\aviutl2\Plugin\MFOutput.ini)" };
enum audio_bit_rate : int32_t
{
	kbps_96,
	kbps_128,
	kbps_160,
	kbps_192
};
enum defaults : int32_t
{
	video_quality = 70,
	audio_bit_rate = audio_bit_rate::kbps_192,
	is_mp4_preferred_hevc = 0,
	is_accelerated = BST_UNCHECKED
};
auto get(keys &&key)
{
	std::any result{};

	switch (key)
	{
		using enum keys;

	case video_quality:
		result = GetPrivateProfileIntW(L"general", L"videoQuality", defaults::video_quality, configuration_ini_path);
		break;
	case audio_bit_rate:
		result = GetPrivateProfileIntW(L"general", L"audioBitRate", defaults::audio_bit_rate, configuration_ini_path);
		break;
	case is_mp4_preferred_hevc:
		result = GetPrivateProfileIntW(L"mp4", L"videoFormat", defaults::is_mp4_preferred_hevc, configuration_ini_path) == 1;
		break;
	case is_accelerated:
		result = GetPrivateProfileIntW(L"general", L"useHardware", defaults::is_accelerated, configuration_ini_path) == BST_CHECKED;
		break;
	default:
		throw std::exception("Unknown key");
		break;
	}

	return result;
}
auto set(keys &&key, int32_t &&value)
{
	switch (key)
	{
		using enum keys;

	case audio_bit_rate:
		THROW_IF_WIN32_BOOL_FALSE(WritePrivateProfileStringW(L"general", L"audioBitRate", std::to_wstring(value).c_str(), configuration_ini_path));
		break;
	case is_mp4_preferred_hevc:
		THROW_IF_WIN32_BOOL_FALSE(WritePrivateProfileStringW(L"mp4", L"videoFormat", std::to_wstring(value).c_str(), configuration_ini_path));
		break;
	case is_accelerated:
		THROW_IF_WIN32_BOOL_FALSE(WritePrivateProfileStringW(L"general", L"useHardware", value == BST_CHECKED ? L"1" : L"0", configuration_ini_path));
		break;
	default:
		throw std::exception("Unknown key");
		break;
	}
}
auto set(keys &&key, wchar_t const *value)
{
	switch (key)
	{
		using enum keys;

	case video_quality:
		THROW_IF_WIN32_BOOL_FALSE(WritePrivateProfileStringW(L"general", L"videoQuality", value, configuration_ini_path));
		break;
	default:
		throw std::exception("Unknown key");
		break;
	}
}
intptr_t CALLBACK config_dialog_proc(HWND dialog, uint32_t message, WPARAM w_param, LPARAM)
{
	static wchar_t quality_wchar{};

	uint32_t quality{};
	switch (message)
	{
	case WM_INITDIALOG:
		ComboBox_AddString(GetDlgItem(dialog, IDC_COMBO2), L"H.264");
		ComboBox_AddString(GetDlgItem(dialog, IDC_COMBO2), L"HEVC");

		ComboBox_AddString(GetDlgItem(dialog, IDC_COMBO1), L"96");
		ComboBox_AddString(GetDlgItem(dialog, IDC_COMBO1), L"128");
		ComboBox_AddString(GetDlgItem(dialog, IDC_COMBO1), L"160");
		ComboBox_AddString(GetDlgItem(dialog, IDC_COMBO1), L"192");

		ComboBox_SetCurSel(GetDlgItem(dialog, IDC_COMBO2), std::any_cast<bool>(configure::get(configure::keys::is_mp4_preferred_hevc)));

		THROW_IF_WIN32_BOOL_FALSE(SetDlgItemTextW(dialog, IDC_EDIT1, std::to_wstring(any_cast<uint32_t>(configure::get(configure::keys::video_quality))).c_str()));
		ComboBox_SetCurSel(GetDlgItem(dialog, IDC_COMBO1), std::any_cast<uint32_t>(configure::get(configure::keys::audio_bit_rate)));

		Button_SetCheck(GetDlgItem(dialog, IDC_CHECK1), std::any_cast<bool>(configure::get(configure::keys::is_accelerated)));

		return false;
	case WM_COMMAND:
		switch (LOWORD(w_param))
		{
		case IDNO:
			if (MessageBoxW(dialog, L"全ての設定を初期化しますか？", L"設定値のリセット", MB_YESNO | MB_ICONWARNING) == IDYES)
			{
				ComboBox_SetCurSel(GetDlgItem(dialog, IDC_COMBO2), configure::defaults::is_mp4_preferred_hevc);
				THROW_IF_WIN32_BOOL_FALSE(SetDlgItemTextW(dialog, IDC_EDIT1, std::to_wstring(configure::defaults::video_quality).c_str()));
				ComboBox_SetCurSel(GetDlgItem(dialog, IDC_COMBO1), configure::defaults::audio_bit_rate);
				Button_SetCheck(GetDlgItem(dialog, IDC_CHECK1), configure::defaults::is_accelerated);
			}
			return false;
		case IDOK:
			quality = GetDlgItemInt(dialog, IDC_EDIT1, nullptr, false);
			if (quality > 100 || quality == 0)
			{
				MessageBoxW(dialog, L"映像品質は1〜100の範囲で指定してください。", nullptr, MB_OK | MB_ICONERROR);
				return false;
			}

			configure::set(configure::keys::is_mp4_preferred_hevc, ComboBox_GetCurSel(GetDlgItem(dialog, IDC_COMBO2)));

			GetDlgItemTextW(dialog, IDC_EDIT1, &quality_wchar, 3);
			configure::set(configure::keys::video_quality, &quality_wchar);

			configure::set(configure::keys::audio_bit_rate, ComboBox_GetCurSel(GetDlgItem(dialog, IDC_COMBO1)));
			configure::set(configure::keys::is_accelerated, Button_GetCheck(GetDlgItem(dialog, IDC_CHECK1)));

			THROW_IF_WIN32_BOOL_FALSE(EndDialog(dialog, IDOK));
			return true;
		case IDCANCEL:
			THROW_IF_WIN32_BOOL_FALSE(EndDialog(dialog, IDCANCEL));
			return true;
		default:
			return false;
		}
	default:
		return false;
	}
}
auto open_configuration_dialog(HWND window, HINSTANCE instance)
{
	DialogBoxW(instance, MAKEINTRESOURCEW(IDD_DIALOG1), window, config_dialog_proc);
}
}
}