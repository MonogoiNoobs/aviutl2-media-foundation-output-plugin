/**
 * \copyright	SPDX-License-Identifier: MIT
 * \year		2025-2026
 * \author		Shion Yorigami <62567343+MonogoiNoobs@users.noreply.github.com>
 */

module;

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <windowsx.h>
#include <wil/com.h>
#include "resource.h"

#define HANDLE_DIALOG_MSG(hwnd, message, fn) \
	case (message): return SetDlgMsgResult(dialog, message, HANDLE_##message((hwnd), (wParam), (lParam), (fn)))

module mfop.configure;

import std;

using namespace std;
using namespace wil;

namespace mfop
{
	namespace configure
	{
		auto const constinit configuration_ini_path{ LR"(C:\ProgramData\aviutl2\Plugin\MFOutput.ini)" };

		enum struct audio_bit_rates : int32_t
		{
			kbps_96,
			kbps_128,
			kbps_160,
			kbps_192
		};

		template<typename Key>
		int32_t consteval get_default() noexcept
		{
			if (same_as<Key, video_quality>)
				return 70;
			if (same_as<Key, audio_bit_rate>)
				return to_underlying(audio_bit_rates::kbps_192);
			if (same_as<Key, is_accelerated>)
				return BST_UNCHECKED;

			if (same_as<Key, is_hevc_preferable>)
				return FALSE;

			unreachable();
		}

		template<typename Key>
		underlying_type<Key>::type get() noexcept
		{
			if (same_as<Key, video_quality>)
				return GetPrivateProfileIntW(L"general", L"videoQuality", get_default<Key>(), configuration_ini_path);
			if (same_as<Key, audio_bit_rate>)
				return GetPrivateProfileIntW(L"general", L"audioBitRate", get_default<Key>(), configuration_ini_path);
			if (same_as<Key, is_accelerated>)
				return cmp_equal(GetPrivateProfileIntW(L"general", L"useHardware", get_default<Key>(), configuration_ini_path), BST_CHECKED);

			if (same_as<Key, is_hevc_preferable>)
				return cmp_equal(GetPrivateProfileIntW(L"mp4", L"videoFormat", get_default<Key>(), configuration_ini_path), TRUE);

			unreachable();
		}

		template<typename Key>
		bool set(int32_t &&value) noexcept
		{
			if (same_as<Key, audio_bit_rate>)
				return WritePrivateProfileStringW(L"general", L"audioBitRate", to_wstring(value).c_str(), configuration_ini_path);
			if (same_as<Key, video_quality>)
				return WritePrivateProfileStringW(L"general", L"videoQuality", to_wstring(value).c_str(), configuration_ini_path);
			if (same_as<Key, is_accelerated>)
				return WritePrivateProfileStringW(L"general", L"useHardware", cmp_equal(value, BST_CHECKED) ? L"1" : L"0", configuration_ini_path);

			if (same_as<Key, is_hevc_preferable>)
				return WritePrivateProfileStringW(L"mp4", L"videoFormat", to_wstring(value).c_str(), configuration_ini_path);

			unreachable();
		}

		auto on_init_dialog(HWND dialog, HWND, intptr_t) noexcept
		{
			ignore = ComboBox_AddString(GetDlgItem(dialog, IDC_COMBO2), L"H.264");
			ignore = ComboBox_AddString(GetDlgItem(dialog, IDC_COMBO2), L"HEVC");

			ignore = ComboBox_AddString(GetDlgItem(dialog, IDC_COMBO1), L"96");
			ignore = ComboBox_AddString(GetDlgItem(dialog, IDC_COMBO1), L"128");
			ignore = ComboBox_AddString(GetDlgItem(dialog, IDC_COMBO1), L"160");
			ignore = ComboBox_AddString(GetDlgItem(dialog, IDC_COMBO1), L"192");

			ignore = ComboBox_SetCurSel(GetDlgItem(dialog, IDC_COMBO2), get<is_hevc_preferable>());

			ignore = SetDlgItemTextW(dialog, IDC_EDIT1, to_wstring(get<video_quality>()).c_str());
			ignore = ComboBox_SetCurSel(GetDlgItem(dialog, IDC_COMBO1), get<audio_bit_rate>());

			Button_SetCheck(GetDlgItem(dialog, IDC_CHECK1), get<is_accelerated>());

			return false;
		}

		auto on_command(HWND dialog, int32_t id, HWND, uint32_t) noexcept
		{
			switch (id)
			{
			case IDNO:
				if (cmp_equal(MessageBoxW(dialog, L"全ての設定を初期化しますか？", L"設定値のリセット", MB_YESNO bitor MB_ICONWARNING), IDYES))
				{
					ignore = ComboBox_SetCurSel(GetDlgItem(dialog, IDC_COMBO2), get_default<is_hevc_preferable>());
					ignore = SetDlgItemTextW(dialog, IDC_EDIT1, to_wstring(get_default<video_quality>()).c_str());
					ignore = ComboBox_SetCurSel(GetDlgItem(dialog, IDC_COMBO1), get_default<audio_bit_rate>());
					Button_SetCheck(GetDlgItem(dialog, IDC_CHECK1), get_default<is_accelerated>());
				}
				break;
			case IDOK:
				ignore = set<is_hevc_preferable>(ComboBox_GetCurSel(GetDlgItem(dialog, IDC_COMBO2)));
				ignore = set<video_quality>(clamp(GetDlgItemInt(dialog, IDC_EDIT1, nullptr, false), 1u, 100u));

				ignore = set<audio_bit_rate>(ComboBox_GetCurSel(GetDlgItem(dialog, IDC_COMBO1)));
				ignore = set<is_accelerated>(Button_GetCheck(GetDlgItem(dialog, IDC_CHECK1)));
				[[fallthrough]];
			case IDCANCEL:
				ignore = EndDialog(dialog, id);
				break;
			}
		}

		intptr_t CALLBACK config_dialog_proc(HWND dialog, uint32_t message, uintptr_t wParam, [[maybe_unused]] intptr_t lParam) noexcept
		{
			switch (message)
			{
				HANDLE_DIALOG_MSG(dialog, WM_INITDIALOG, on_init_dialog);
				HANDLE_DIALOG_MSG(dialog, WM_COMMAND, on_command);

			default:
				return false;
			}
		}

		void open_dialog(HWND &window, HINSTANCE &instance) noexcept
		{
			DialogBoxW(instance, MAKEINTRESOURCEW(IDD_DIALOG1), window, config_dialog_proc);
		}
	}
}
