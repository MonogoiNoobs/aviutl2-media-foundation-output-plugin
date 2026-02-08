module;

#define STRICT
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <windowsx.h>
#include <wil/com.h>
#include "resource.h"

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

		enum struct defaults : int32_t
		{
			video_quality = 70,
			audio_bit_rate = audio_bit_rates::kbps_192,
			is_hevc_preferred = 0,
			is_accelerated = BST_UNCHECKED
		};

		template<typename Key>
		underlying_type<Key>::type get()
		{
			if (is_same<Key, video_quality>::value)
				return GetPrivateProfileIntW(L"general", L"videoQuality", to_underlying(defaults::video_quality), configuration_ini_path);
			if (is_same<Key, audio_bit_rate>::value)
				return GetPrivateProfileIntW(L"general", L"audioBitRate", to_underlying(defaults::audio_bit_rate), configuration_ini_path);
			if (is_same<Key, is_hevc_preferred>::value)
				return GetPrivateProfileIntW(L"mp4", L"videoFormat", to_underlying(defaults::is_hevc_preferred), configuration_ini_path) == 1;
			if (is_same<Key, is_accelerated>::value)
				return GetPrivateProfileIntW(L"general", L"useHardware", to_underlying(defaults::is_accelerated), configuration_ini_path) == BST_CHECKED;
			throw invalid_argument{ "Unknown key" };
		}

		template<typename Key>
		void set(std::int32_t &&value)
		{
			if (is_same<Key, audio_bit_rate>::value)
			{
				THROW_IF_WIN32_BOOL_FALSE(WritePrivateProfileStringW(L"general", L"audioBitRate", to_wstring(value).c_str(), configuration_ini_path));
				return;
			}
			if (is_same<Key, is_hevc_preferred>::value)
			{
				THROW_IF_WIN32_BOOL_FALSE(WritePrivateProfileStringW(L"mp4", L"videoFormat", to_wstring(value).c_str(), configuration_ini_path));
				return;
			}
			if (is_same<Key, is_accelerated>::value)
			{
				THROW_IF_WIN32_BOOL_FALSE(WritePrivateProfileStringW(L"general", L"useHardware", value == BST_CHECKED ? L"1" : L"0", configuration_ini_path));
				return;
			}
			throw invalid_argument{ "Unknown key" };
		}

		template<typename Key>
		void set(wchar_t const *const &&value)
		{
			if (is_same<Key, video_quality>::value)
			{
				THROW_IF_WIN32_BOOL_FALSE(WritePrivateProfileStringW(L"general", L"videoQuality", value, configuration_ini_path));
				return;
			}
			throw invalid_argument{ "Unknown key" };
		}

		intptr_t CALLBACK config_dialog_proc(HWND dialog, uint32_t message, uintptr_t w_param, intptr_t)
		{
			uint32_t quality{};
			static wchar_t constinit quality_wchar{};

			auto const get_handle{ [&dialog](int32_t &&id) noexcept
			{
				return GetDlgItem(dialog, id);
			} };

			switch (message)
			{
			case WM_INITDIALOG:
				ComboBox_AddString(get_handle(IDC_COMBO2), L"H.264");
				ComboBox_AddString(get_handle(IDC_COMBO2), L"HEVC");

				ComboBox_AddString(get_handle(IDC_COMBO1), L"96");
				ComboBox_AddString(get_handle(IDC_COMBO1), L"128");
				ComboBox_AddString(get_handle(IDC_COMBO1), L"160");
				ComboBox_AddString(get_handle(IDC_COMBO1), L"192");

				ComboBox_SetCurSel(get_handle(IDC_COMBO2), get<is_hevc_preferred>());

				THROW_IF_WIN32_BOOL_FALSE(SetDlgItemTextW(dialog, IDC_EDIT1, to_wstring(get<video_quality>()).c_str()));
				ComboBox_SetCurSel(get_handle(IDC_COMBO1), get<audio_bit_rate>());

				Button_SetCheck(get_handle(IDC_CHECK1), get<is_accelerated>());

				return false;
			case WM_COMMAND:
				switch (LOWORD(w_param))
				{
				case IDNO:
					if (MessageBoxW(dialog, L"全ての設定を初期化しますか？", L"設定値のリセット", MB_YESNO | MB_ICONWARNING) == IDYES)
					{
						ComboBox_SetCurSel(get_handle(IDC_COMBO2), defaults::is_hevc_preferred);
						THROW_IF_WIN32_BOOL_FALSE(SetDlgItemTextW(dialog, IDC_EDIT1, to_wstring(to_underlying(defaults::video_quality)).c_str()));
						ComboBox_SetCurSel(get_handle(IDC_COMBO1), defaults::audio_bit_rate);
						Button_SetCheck(get_handle(IDC_CHECK1), defaults::is_accelerated);
					}
					return false;
				case IDOK:
					quality = GetDlgItemInt(dialog, IDC_EDIT1, nullptr, false);
					if (quality > 100 || quality == 0)
					{
						MessageBoxW(dialog, L"映像品質は1〜100の範囲で指定してください。", nullptr, MB_OK | MB_ICONERROR);
						return false;
					}

					set<is_hevc_preferred>(ComboBox_GetCurSel(get_handle(IDC_COMBO2)));

					GetDlgItemTextW(dialog, IDC_EDIT1, &quality_wchar, 3);
					set<video_quality>(&quality_wchar);

					set<audio_bit_rate>(ComboBox_GetCurSel(get_handle(IDC_COMBO1)));
					set<is_accelerated>(Button_GetCheck(get_handle(IDC_CHECK1)));

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
		auto open_dialog(HWND window, HINSTANCE instance) -> void
		{
			DialogBoxW(instance, MAKEINTRESOURCEW(IDD_DIALOG1), window, config_dialog_proc);
		}
	}
}