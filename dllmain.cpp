#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wil/com.h>
#include "output2.h"
//#include <D3D11.h>
import std;
#include <io.h>
#include <Fcntl.h>
#include <codecapi.h>

#pragma comment(lib, "Mfplat")
#pragma comment(lib, "Mfreadwrite")
#pragma comment(lib, "Mfuuid")
//#pragma comment(lib, "d3d11")
#pragma comment(lib, "User32")

#ifdef NDEBUG
#define MN_MFOUTPUT_PRINT_DEBUG_CONSOLE(format, ...) do { } while (0)
#else
#define MN_MFOUTPUT_PRINT_DEBUG_CONSOLE(format, ...) std::println(format __VA_OPT__(,) __VA_ARGS__)
#endif // NDEBUG

uint32_t constexpr get_pcm_block_alignment(uint32_t &&audio_ch, uint32_t &&bit) noexcept
{
	return (audio_ch * bit) / 8;
}

[[nodiscard]] auto make_sink_writer(wchar_t const *const &output_name)
{
	//D3D_FEATURE_LEVEL feature_levels[]{
	//	D3D_FEATURE_LEVEL_11_1,
	//	D3D_FEATURE_LEVEL_11_0,
	//	D3D_FEATURE_LEVEL_10_1,
	//	D3D_FEATURE_LEVEL_10_0,
	//	D3D_FEATURE_LEVEL_9_3,
	//	D3D_FEATURE_LEVEL_9_2,
	//	D3D_FEATURE_LEVEL_9_1
	//};

	//auto d3d11_device{ wil::com_ptr<ID3D11Device>{} };
	//THROW_IF_FAILED(D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, D3D11_CREATE_DEVICE_VIDEO_SUPPORT, feature_levels, sizeof(feature_levels) / sizeof(D3D_FEATURE_LEVEL), D3D11_SDK_VERSION, &d3d11_device, NULL, NULL));

	//auto dxgi_device_manager{ wil::com_ptr<IMFDXGIDeviceManager>{} };
	//unsigned int reset_token{};
	//THROW_IF_FAILED(MFCreateDXGIDeviceManager(&reset_token, &dxgi_device_manager));
	//THROW_IF_FAILED(dxgi_device_manager->ResetDevice(d3d11_device.get(), reset_token));

	auto sink_writer_attributes{ wil::com_ptr<IMFAttributes>{} };
	THROW_IF_FAILED(MFCreateAttributes(&sink_writer_attributes, 4));
	//THROW_IF_FAILED(sink_writer_attributes->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, true));
	THROW_IF_FAILED(sink_writer_attributes->SetUINT32(MF_SINK_WRITER_DISABLE_THROTTLING, true));
	THROW_IF_FAILED(sink_writer_attributes->SetGUID(MF_TRANSCODE_CONTAINERTYPE, MFTranscodeContainerType_MPEG4));
	//THROW_IF_FAILED(sink_writer_attributes->SetUnknown(MF_SINK_WRITER_D3D_MANAGER, dxgi_device_manager.get()));

	auto sink_writer{ wil::com_ptr<IMFSinkWriter>{} };
	THROW_IF_FAILED(MFCreateSinkWriterFromURL(output_name, nullptr, sink_writer_attributes.get(), &sink_writer));

	return sink_writer;
}

[[nodiscard]] auto configure_video_stream(OUTPUT_INFO const *const &oip, IMFSinkWriter *const &sink_writer)
{
	auto output_video_media_type{ wil::com_ptr<IMFMediaType>{} };
	THROW_IF_FAILED(MFCreateMediaType(&output_video_media_type));

	THROW_IF_FAILED(output_video_media_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video));
	THROW_IF_FAILED(output_video_media_type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264));
	THROW_IF_FAILED(output_video_media_type->SetUINT32(MF_MT_AVG_BITRATE, 12000000));
	THROW_IF_FAILED(MFSetAttributeSize(output_video_media_type.get(), MF_MT_FRAME_SIZE, oip->w, oip->h));
	THROW_IF_FAILED(MFSetAttributeRatio(output_video_media_type.get(), MF_MT_FRAME_RATE, oip->rate, oip->scale));
	THROW_IF_FAILED(output_video_media_type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive));
	THROW_IF_FAILED(output_video_media_type->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_High));

	DWORD video_index{};
	THROW_IF_FAILED(sink_writer->AddStream(output_video_media_type.get(), &video_index));

	return video_index;
}

[[nodiscard]] auto configure_audio_stream(OUTPUT_INFO const *const &oip, IMFSinkWriter *const &sink_writer)
{
	auto output_audio_media_type{ wil::com_ptr<IMFMediaType>{} };
	THROW_IF_FAILED(MFCreateMediaType(&output_audio_media_type));

	THROW_IF_FAILED(output_audio_media_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio));
	THROW_IF_FAILED(output_audio_media_type->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_AAC));
	THROW_IF_FAILED(output_audio_media_type->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, oip->audio_ch));
	THROW_IF_FAILED(output_audio_media_type->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, 24000 /* 192kbps */));
	THROW_IF_FAILED(output_audio_media_type->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, oip->audio_rate));
	THROW_IF_FAILED(output_audio_media_type->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16));

	DWORD audio_index{};
	THROW_IF_FAILED(sink_writer->AddStream(output_audio_media_type.get(), &audio_index));

	return audio_index;
}

auto configure_video_input(OUTPUT_INFO const *const &oip, IMFSinkWriter *const &sink_writer, DWORD const &video_index)
{
	uint32_t image_size{};
	THROW_IF_FAILED(MFCalculateImageSize(MFVideoFormat_YUY2, oip->w, oip->h, &image_size));

	//long default_stride{};
	//THROW_IF_FAILED(MFGetStrideForBitmapInfoHeader(MFVideoFormat_YUY2.Data1, oip->w, &default_stride));

	auto input_video_media_type{ wil::com_ptr<IMFMediaType>{} };
	THROW_IF_FAILED(MFCreateMediaType(&input_video_media_type));
	THROW_IF_FAILED(input_video_media_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video));
	THROW_IF_FAILED(input_video_media_type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_YUY2));
	THROW_IF_FAILED(input_video_media_type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive));
	//THROW_IF_FAILED(input_video_media_type->SetUINT32(MF_MT_DEFAULT_STRIDE, default_stride));
	THROW_IF_FAILED(input_video_media_type->SetUINT32(MF_MT_SAMPLE_SIZE, image_size));
	THROW_IF_FAILED(input_video_media_type->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, true));
	THROW_IF_FAILED(MFSetAttributeSize(input_video_media_type.get(), MF_MT_FRAME_SIZE, oip->w, oip->h));
	THROW_IF_FAILED(MFSetAttributeRatio(input_video_media_type.get(), MF_MT_FRAME_RATE, oip->rate, oip->scale));
	THROW_IF_FAILED(MFSetAttributeRatio(input_video_media_type.get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1));
	THROW_IF_FAILED(sink_writer->SetInputMediaType(video_index, input_video_media_type.get(), nullptr));
}

auto configure_audio_input(OUTPUT_INFO const *const &oip, IMFSinkWriter *const &sink_writer, DWORD const &audio_index)
{
	auto input_audio_media_type{ wil::com_ptr<IMFMediaType>{} };
	THROW_IF_FAILED(MFCreateMediaType(&input_audio_media_type));
	THROW_IF_FAILED(input_audio_media_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio));
	THROW_IF_FAILED(input_audio_media_type->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM));
	THROW_IF_FAILED(input_audio_media_type->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16));
	THROW_IF_FAILED(input_audio_media_type->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, oip->audio_rate));
	THROW_IF_FAILED(input_audio_media_type->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, oip->audio_ch));

	THROW_IF_FAILED(sink_writer->SetInputMediaType(audio_index, input_audio_media_type.get(), nullptr));
}

auto initialize_sink_writer(OUTPUT_INFO const *const &oip)
{
	auto const sink_writer{ make_sink_writer(oip->savefile) };

	auto video_index{ configure_video_stream(oip, sink_writer.get()) };
	auto audio_index{ configure_audio_stream(oip, sink_writer.get()) };
	configure_video_input(oip, sink_writer.get(), video_index);
	configure_audio_input(oip, sink_writer.get(), audio_index);

	THROW_IF_FAILED(sink_writer->BeginWriting());

	return std::make_pair(std::move(sink_writer), std::make_pair(video_index, audio_index));
}

auto write_video_sample(OUTPUT_INFO const *const &oip, IMFSinkWriter *const &sink_writer, int const &f, DWORD const &index, long long const &time_stamp, long const &default_stride)
{
	if (oip->func_is_abort()) return false;

	oip->func_rest_time_disp(static_cast<int>(f), oip->n);

	auto frame_dib_pixel_ptr{ static_cast<BYTE *>(oip->func_get_video(f, FCC('YUY2'))) };
	THROW_IF_NULL_ALLOC(frame_dib_pixel_ptr);

	auto video_buffer{ wil::com_ptr<IMFMediaBuffer>{} };
	THROW_IF_FAILED(MFCreate2DMediaBuffer(oip->w, oip->h, MFVideoFormat_YUY2.Data1, false, &video_buffer));

	BYTE *scanline{};
	long stride{};
	BYTE *buffer_start{};
	DWORD buffer_length{};
	THROW_IF_FAILED(video_buffer.query<IMF2DBuffer2>()->Lock2DSize(MF2DBuffer_LockFlags_Write, &scanline, &stride, &buffer_start, &buffer_length));
	THROW_IF_FAILED(MFCopyImage(scanline, stride, frame_dib_pixel_ptr, default_stride, default_stride, oip->h));
	THROW_IF_FAILED(video_buffer.query<IMF2DBuffer2>()->Unlock2D());

	// “Generally, you should avoid mixing calls to IMF2DBuffer and IMFMediaBuffer methods on the same media buffer.”
	//     —Microsoft, in https://learn.microsoft.com/en-us/windows/win32/api/mfobjects/nn-mfobjects-imf2dbuffer
	// 
	// https://stackoverflow.com/questions/47930340/
	DWORD contiguous_length{};
	THROW_IF_FAILED(video_buffer.query<IMF2DBuffer2>()->GetContiguousLength(&contiguous_length));
	THROW_IF_FAILED(video_buffer->SetCurrentLength(contiguous_length));

	auto video_sample{ wil::com_ptr<IMFSample>{} };
	THROW_IF_FAILED(MFCreateSample(&video_sample));
	THROW_IF_FAILED(video_sample->AddBuffer(video_buffer.get()));
	THROW_IF_FAILED(video_sample->SetSampleTime(time_stamp * f));
	THROW_IF_FAILED(video_sample->SetSampleDuration(time_stamp));

	THROW_IF_FAILED(sink_writer->WriteSample(index, video_sample.get()));

	return true;
}

auto write_audio_sample(OUTPUT_INFO const *const &oip, IMFSinkWriter *const &sink_writer, int const &n, DWORD const &index)
{
	if (oip->func_is_abort()) return false;

	oip->func_rest_time_disp(n, oip->audio_n);

	// bytes per audio-frame (block align)
	auto block_align = get_pcm_block_alignment(static_cast<uint32_t>(oip->audio_ch), 16); // bytes per audio-frame
	auto max_sample_size = static_cast<int>(block_align * oip->audio_rate); // bytes per second

	int read_audio_sample_size{};
	auto audio_data{ oip->func_get_audio(n, max_sample_size, &read_audio_sample_size, WAVE_FORMAT_PCM) };
	THROW_IF_NULL_ALLOC(audio_data);
	if (!read_audio_sample_size) return true;

	// compute number of audio-frames (samples per channel) in buffer
	auto const frames = static_cast<int64_t>(read_audio_sample_size) / static_cast<int64_t>(block_align);
	auto const sample_duration = static_cast<int64_t>(frames) * 10'000'000LL / oip->audio_rate; // 100-ns units
	auto const sample_time = static_cast<int64_t>(n) * 10'000'000LL / oip->audio_rate; // 100-ns units for start

	auto audio_buffer{ wil::com_ptr<IMFMediaBuffer>{} };
	THROW_IF_FAILED(MFCreateMemoryBuffer(static_cast<DWORD>(read_audio_sample_size), &audio_buffer));
	BYTE *media_data{};
	DWORD media_data_max_length{};
	THROW_IF_FAILED(audio_buffer->Lock(&media_data, &media_data_max_length, nullptr));
	memcpy_s(media_data, media_data_max_length, audio_data, static_cast<size_t>(read_audio_sample_size));
	THROW_IF_FAILED(audio_buffer->Unlock());
	THROW_IF_FAILED(audio_buffer->SetCurrentLength(static_cast<DWORD>(read_audio_sample_size)));

	auto audio_sample{ wil::com_ptr<IMFSample>{} };
	THROW_IF_FAILED(MFCreateSample(&audio_sample));
	THROW_IF_FAILED(audio_sample->AddBuffer(audio_buffer.get()));
	THROW_IF_FAILED(audio_sample->SetSampleTime(sample_time));
	THROW_IF_FAILED(audio_sample->SetSampleDuration(sample_duration));
	THROW_IF_FAILED(sink_writer->WriteSample(index, audio_sample.get()));

	return true;
}

using unique_mfshutdown_call = wil::unique_call<decltype(&::MFShutdown), ::MFShutdown>;
[[nodiscard]] inline unique_mfshutdown_call MyMFStartup()
{
	THROW_IF_FAILED(::MFStartup(MF_VERSION));
	return unique_mfshutdown_call();
}

auto func_output(OUTPUT_INFO *oip)
{
#ifndef NDEBUG
	AllocConsole();
	auto console{ _open_osfhandle((intptr_t)GetStdHandle(STD_OUTPUT_HANDLE), _O_TEXT) };
	auto console_file_handle{ _fdopen(console, "w") };
	freopen_s(&console_file_handle, "CONOUT$", "w", stdout);
#endif // !NDEBUG

	auto com_cleanup{ wil::CoInitializeEx() };
	auto mf_cleanup{ MyMFStartup() };

	uint64_t time_stamp{};
	THROW_IF_FAILED(MFFrameRateToAverageTimePerFrame(oip->rate, oip->scale, &time_stamp));

	long default_stride{};
	THROW_IF_FAILED(MFGetStrideForBitmapInfoHeader(MFVideoFormat_YUY2.Data1, oip->w, &default_stride));

	auto const [sink_writer, stream_indices] { initialize_sink_writer(oip) };
	auto const &[video_index, audio_index] { stream_indices };

	MN_MFOUTPUT_PRINT_DEBUG_CONSOLE("[INFO] Starting writing...");
	oip->func_set_buffer_size(32, 32);
	for (auto f{ 0 }; f < oip->n; ++f)
		if (!write_video_sample(oip, sink_writer.get(), f, video_index, time_stamp, default_stride)) goto abort;
	for (auto n{ 0 }; n < oip->audio_n; n += oip->audio_rate)
		if (!write_audio_sample(oip, sink_writer.get(), n, audio_index)) goto abort;

abort:
	sink_writer->Finalize();

#ifndef NDEBUG
	FreeConsole();
	_close(console);
#endif // !NDEBUG
	return true;
}

auto func_config(HWND, HINSTANCE)
{
	// ここに出力設定のダイアログを実装します
	return true; // 成功ならtrueを返す
}

auto func_get_config_text()
{
	// ここに出力設定のテキスト情報を実装します
	return L"MonogoiNoobs's MediaFoundation File Saver preferences"; // 設定情報のテキストを返す
}

auto constexpr output_plugin_table{ OUTPUT_PLUGIN_TABLE{
	OUTPUT_PLUGIN_TABLE::FLAG_VIDEO | OUTPUT_PLUGIN_TABLE::FLAG_AUDIO, //	フラグ
	L"MediaFoundation File Saver",					// プラグインの名前
	L"MPEG-4 AVC/H.264 + AAC-LC (*.mp4)\0*.mp4\0",					// 出力ファイルのフィルタ
	L"MonogoiNoobs's MediaFoundation File Saver v0.1.0",	// プラグインの情報
	func_output,									// 出力時に呼ばれる関数へのポインタ
	func_config,									// 出力設定のダイアログを要求された時に呼ばれる関数へのポインタ (nullptrなら呼ばれません)
	func_get_config_text,							// 出力設定のテキスト情報を取得する時に呼ばれる関数へのポインタ (nullptrなら呼ばれません)
} };

extern "C" __declspec(dllexport) auto GetOutputPluginTable()
{
	return &output_plugin_table;
}

auto APIENTRY DllMain(HMODULE, DWORD, void *)
{
	return true;
}