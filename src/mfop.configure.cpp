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

#include "aviutl2_sdk/plugin2.h"

#define HANDLE_DIALOG_MSG(hwnd, message, fn) \
	case (message): return SetDlgMsgResult(dialog, message, HANDLE_##message((hwnd), (wParam), (lParam), (fn)))

module mfop;

import :configure;

import std;

using namespace std;
using namespace wil;

namespace mfop
{
	namespace configure
	{
		namespace keys
		{
			auto constexpr video_quality{ "MFOutput: Video quality" };
			auto constexpr audio_bit_rate{ "MFOutput: Audio bit rate" };
			auto constexpr is_accelerated{ "MFOutput: Is hardware acceleration used?" };
			auto constexpr is_hevc_preferable{ "MFOutput: Is HEVC preferable?" };
		}

		template<typename Key>
		int32_t consteval get_default() noexcept
		{
			if (same_as<Key, video_quality>)		return 70;
			if (same_as<Key, audio_bit_rate>)		return 3;
			if (same_as<Key, is_accelerated>)		return BST_UNCHECKED;

			if (same_as<Key, is_hevc_preferable>)	return FALSE;

			unreachable();
		}

		output_configuration temp
		{
			.video_quality = 70,
			.audio_bit_rate = 3,
			.is_hevc_preferable = false,
			.is_accelerated = false
		};

		enum struct audio_bit_rates : int32_t
		{
			kbps_96,
			kbps_128,
			kbps_160,
			kbps_192
		};

		output_configuration constexpr *load(PROJECT_FILE const &project_file) noexcept
		{
			if (auto t { project_file.get_param_string(keys::video_quality) }) temp.video_quality = stoi(t);
			if (auto t{ project_file.get_param_string(keys::audio_bit_rate) }) temp.audio_bit_rate = stoi(t);
			if (auto t{ project_file.get_param_string(keys::is_accelerated) }) temp.is_accelerated = t;
			if (auto t{ project_file.get_param_string(keys::is_hevc_preferable) }) temp.is_hevc_preferable = t;
			return &temp;
		}

		bool constexpr store(PROJECT_FILE const &project_file) noexcept
		{
			project_file.set_param_string(keys::video_quality, to_string(temp.video_quality).data());
			project_file.set_param_string(keys::audio_bit_rate, to_string(temp.audio_bit_rate).data());
			project_file.set_param_string(keys::is_accelerated, temp.is_accelerated ? "1" : "0");
			project_file.set_param_string(keys::is_hevc_preferable, to_string(temp.is_hevc_preferable).data());
			return true;
		}

		auto constexpr on_init_dialog(HWND dialog, HWND, intptr_t) noexcept
		{
			ignore = ComboBox_AddString(GetDlgItem(dialog, IDC_COMBO2), L"H.264");
			ignore = ComboBox_AddString(GetDlgItem(dialog, IDC_COMBO2), L"HEVC");

			ignore = ComboBox_AddString(GetDlgItem(dialog, IDC_COMBO1), L"96");
			ignore = ComboBox_AddString(GetDlgItem(dialog, IDC_COMBO1), L"128");
			ignore = ComboBox_AddString(GetDlgItem(dialog, IDC_COMBO1), L"160");
			ignore = ComboBox_AddString(GetDlgItem(dialog, IDC_COMBO1), L"192");

			ignore = ComboBox_SetCurSel(GetDlgItem(dialog, IDC_COMBO2), temp.is_hevc_preferable);

			ignore = SetDlgItemInt(dialog, IDC_EDIT1, temp.video_quality, false);
			ignore = ComboBox_SetCurSel(GetDlgItem(dialog, IDC_COMBO1), temp.audio_bit_rate);

			Button_SetCheck(GetDlgItem(dialog, IDC_CHECK1), temp.is_accelerated);

			return false;
		}

		auto constexpr on_command(HWND dialog, int32_t id, HWND, uint32_t) noexcept
		{
			switch (id)
			{
			case IDNO:
				if (cmp_equal(MessageBoxW(dialog, L"全ての設定を初期化しますか？", L"設定値のリセット", MB_YESNO bitor MB_ICONWARNING), IDYES))
				{
					ignore = ComboBox_SetCurSel(GetDlgItem(dialog, IDC_COMBO2), get_default<is_hevc_preferable>());
					ignore = SetDlgItemInt(dialog, IDC_EDIT1, get_default<video_quality>(), false);
					ignore = ComboBox_SetCurSel(GetDlgItem(dialog, IDC_COMBO1), get_default<audio_bit_rate>());
					Button_SetCheck(GetDlgItem(dialog, IDC_CHECK1), get_default<is_accelerated>());
				}
				break;
			case IDOK:
				temp.is_hevc_preferable = ComboBox_GetCurSel(GetDlgItem(dialog, IDC_COMBO2));
				temp.video_quality = clamp(GetDlgItemInt(dialog, IDC_EDIT1, nullptr, false), 1u, 100u);

				temp.audio_bit_rate = ComboBox_GetCurSel(GetDlgItem(dialog, IDC_COMBO1));
				temp.is_accelerated = Button_GetCheck(GetDlgItem(dialog, IDC_CHECK1));
				[[fallthrough]];
			case IDCANCEL:
				ignore = EndDialog(dialog, id);
				break;
			}
		}

		intptr_t constexpr CALLBACK config_dialog_proc(HWND dialog, uint32_t message, uintptr_t wParam, [[maybe_unused]] intptr_t lParam) noexcept
		{
			switch (message)
			{
				HANDLE_DIALOG_MSG(dialog, WM_INITDIALOG, on_init_dialog);
				HANDLE_DIALOG_MSG(dialog, WM_COMMAND, on_command);

			default:
				return false;
			}
		}

		void constexpr open_dialog(HWND &window, HINSTANCE &instance) noexcept
		{
			DialogBoxW(instance, MAKEINTRESOURCEW(IDD_DIALOG1), window, config_dialog_proc);
		}
	}
}
