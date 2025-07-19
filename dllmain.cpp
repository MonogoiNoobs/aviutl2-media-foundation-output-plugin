#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wil/com.h>
#include "output2.h"
#include <D3D11.h>
import std;
#include <io.h>
#include <Fcntl.h>
#include <codecapi.h>

#pragma comment(lib, "Mfplat")
#pragma comment(lib, "Mfreadwrite")
#pragma comment(lib, "Mfuuid")
#pragma comment(lib, "d3d11")
#pragma comment(lib, "User32")

#ifdef NDEBUG
#define MN_MFOUTPUT_PRINT_DEBUG_CONSOLE(format, ...) do { } while (0)
#else
#define MN_MFOUTPUT_PRINT_DEBUG_CONSOLE(format, ...) std::println(format __VA_OPT__(,) __VA_ARGS__)
#endif // NDEBUG

auto constexpr get_bitmap_stride(size_t&& image_width, size_t&& bit) noexcept {
	return -static_cast<int32_t>(((((image_width * bit) + 31) & ~31) >> 3));
}

uint32_t constexpr get_pcm_block_alignment(uint32_t&& audio_ch, uint32_t&& bit) noexcept {
	return (audio_ch * bit) / 8;
}

[[nodiscard]] auto make_sink_writer(std::wstring_view output_name)
{
	auto d3d11_device{ wil::com_ptr<ID3D11Device>{} };
	auto constexpr feature_levels{ std::array<D3D_FEATURE_LEVEL, 7>{
		D3D_FEATURE_LEVEL_11_1,
		D3D_FEATURE_LEVEL_11_0,
		D3D_FEATURE_LEVEL_10_1,
		D3D_FEATURE_LEVEL_10_0,
		D3D_FEATURE_LEVEL_9_3,
		D3D_FEATURE_LEVEL_9_2,
		D3D_FEATURE_LEVEL_9_1
	} };
	THROW_IF_FAILED(D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, D3D11_CREATE_DEVICE_VIDEO_SUPPORT, feature_levels.data(), static_cast<unsigned int>(feature_levels.size()), D3D11_SDK_VERSION, d3d11_device.put(), NULL, NULL));
	auto dxgi_device_manager{ wil::com_ptr<IMFDXGIDeviceManager>{} };
	unsigned int reset_token{};
	THROW_IF_FAILED(MFCreateDXGIDeviceManager(&reset_token, dxgi_device_manager.put()));
	THROW_IF_FAILED(dxgi_device_manager->ResetDevice(d3d11_device.get(), reset_token));

	auto sink_writer_attributes{ wil::com_ptr<IMFAttributes>{} };
	THROW_IF_FAILED(MFCreateAttributes(sink_writer_attributes.put(), 4));
	THROW_IF_FAILED(sink_writer_attributes->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, true));
	THROW_IF_FAILED(sink_writer_attributes->SetUINT32(MF_SINK_WRITER_DISABLE_THROTTLING, true));
	THROW_IF_FAILED(sink_writer_attributes->SetGUID(MF_TRANSCODE_CONTAINERTYPE, MFTranscodeContainerType_MPEG4));
	THROW_IF_FAILED(sink_writer_attributes->SetUnknown(MF_SINK_WRITER_D3D_MANAGER, dxgi_device_manager.get()));

	auto sink_writer{ wil::com_ptr<IMFSinkWriter>{} };
	THROW_IF_FAILED(MFCreateSinkWriterFromURL(output_name.data(), nullptr, sink_writer_attributes.get(), sink_writer.put()));
	MN_MFOUTPUT_PRINT_DEBUG_CONSOLE("[INFO] Set Sink Writer.");
	return std::move(sink_writer);
}

[[nodiscard]] auto configure_video_stream(OUTPUT_INFO const* const& oip, IMFSinkWriter* sink_writer)
{
	MN_MFOUTPUT_PRINT_DEBUG_CONSOLE("[INFO] Configuring video stream for Sink Writer...");

	auto output_video_media_type{ wil::com_ptr<IMFMediaType>{} };
	THROW_IF_FAILED(MFCreateMediaType(output_video_media_type.put()));
	unsigned long video_index{};

	THROW_IF_FAILED(output_video_media_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video));
	THROW_IF_FAILED(output_video_media_type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264));
	THROW_IF_FAILED(output_video_media_type->SetUINT32(MF_MT_AVG_BITRATE, 12000000));
	THROW_IF_FAILED(MFSetAttributeSize(output_video_media_type.get(), MF_MT_FRAME_SIZE, oip->w, oip->h));
	THROW_IF_FAILED(MFSetAttributeRatio(output_video_media_type.get(), MF_MT_FRAME_RATE, oip->rate, oip->scale));
	THROW_IF_FAILED(output_video_media_type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive));
	THROW_IF_FAILED(output_video_media_type->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_High));

	THROW_IF_FAILED(sink_writer->AddStream(output_video_media_type.get(), &video_index));
	MN_MFOUTPUT_PRINT_DEBUG_CONSOLE("[INFO] Set video stream at index {}.", video_index);
	return video_index;
}

[[nodiscard]] auto configure_audio_stream(OUTPUT_INFO const* const& oip, IMFSinkWriter* sink_writer)
{
	MN_MFOUTPUT_PRINT_DEBUG_CONSOLE("[INFO] Configuring audio stream for Sink Writer...");

	auto output_audio_media_type{ wil::com_ptr<IMFMediaType>{} };
	THROW_IF_FAILED(MFCreateMediaType(output_audio_media_type.put()));
	unsigned long audio_index{};
	THROW_IF_FAILED(output_audio_media_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio));
	THROW_IF_FAILED(output_audio_media_type->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_AAC));
	THROW_IF_FAILED(output_audio_media_type->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, oip->audio_ch));
	THROW_IF_FAILED(output_audio_media_type->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, 24000 /* 192kbps */));
	THROW_IF_FAILED(output_audio_media_type->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, oip->audio_rate));
	THROW_IF_FAILED(output_audio_media_type->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16));
	THROW_IF_FAILED(sink_writer->AddStream(output_audio_media_type.get(), &audio_index));
	MN_MFOUTPUT_PRINT_DEBUG_CONSOLE("[INFO] Set audio stream at index {}.", audio_index);
	return audio_index;
}

auto configure_video_input(OUTPUT_INFO const* const& oip, IMFSinkWriter* sink_writer, unsigned long const& video_index)
{
	MN_MFOUTPUT_PRINT_DEBUG_CONSOLE("[INFO] Configuring video input for Sink Writer...");

	uint32_t image_size{};
	THROW_IF_FAILED(MFCalculateImageSize(MFVideoFormat_YUY2, oip->w, oip->h, &image_size));

	//long default_stride{};
	//THROW_IF_FAILED(MFGetStrideForBitmapInfoHeader(MFVideoFormat_YUY2.Data1, oip->w, &default_stride));

	//MN_MFOUTPUT_PRINT_DEBUG_CONSOLE("[INFO] Default stride is {}.", default_stride);
	auto input_video_media_type{ wil::com_ptr<IMFMediaType>{} };
	THROW_IF_FAILED(MFCreateMediaType(input_video_media_type.put()));
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
	MN_MFOUTPUT_PRINT_DEBUG_CONSOLE("[INFO] Set input video media type with {} image size.", image_size);
}

auto configure_audio_input(OUTPUT_INFO const* const& oip, IMFSinkWriter* sink_writer, unsigned long const& audio_index)
{
	MN_MFOUTPUT_PRINT_DEBUG_CONSOLE("[INFO] Configuring audio input for Sink Writer...");

	auto input_audio_media_type{ wil::com_ptr<IMFMediaType>{} };

	MN_MFOUTPUT_PRINT_DEBUG_CONSOLE("[INFO] Creating input audio media type...");
	THROW_IF_FAILED(MFCreateMediaType(input_audio_media_type.put()));

	THROW_IF_FAILED(input_audio_media_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio));
	THROW_IF_FAILED(input_audio_media_type->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM));
	THROW_IF_FAILED(input_audio_media_type->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16));
	THROW_IF_FAILED(input_audio_media_type->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, oip->audio_rate));
	THROW_IF_FAILED(input_audio_media_type->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, oip->audio_ch));

	MN_MFOUTPUT_PRINT_DEBUG_CONSOLE("[INFO] Configuring audio input media type to index {}...", audio_index);
	THROW_IF_FAILED(sink_writer->SetInputMediaType(audio_index, input_audio_media_type.get(), nullptr));

	MN_MFOUTPUT_PRINT_DEBUG_CONSOLE("[INFO] Set input audio media type: alignment is {} and average bps is {}.", static_cast<uint32_t>((oip->audio_ch * 32) / 8), oip->audio_rate * static_cast<uint32_t>((oip->audio_ch * 32) / 8));
}

auto initialize_sink_writer(OUTPUT_INFO const* const& oip)
{
	MN_MFOUTPUT_PRINT_DEBUG_CONSOLE("[INFO] Initializing Sink Writer...");

	auto const sink_writer{ make_sink_writer(oip->savefile) };

	auto video_index{ configure_video_stream(oip, sink_writer.get()) };
	auto audio_index{ configure_audio_stream(oip, sink_writer.get()) };
	configure_video_input(oip, sink_writer.get(), video_index);
	configure_audio_input(oip, sink_writer.get(), audio_index);

	THROW_IF_FAILED(sink_writer->BeginWriting());
	MN_MFOUTPUT_PRINT_DEBUG_CONSOLE("[INFO] Sink Writer initialized and writing started.");

	return std::make_pair(std::move(sink_writer), std::make_pair(video_index, audio_index));
}

auto write_video_frame_to_sink_writer(OUTPUT_INFO const* const& oip, IMFSinkWriter* sink_writer, int const& f, unsigned long& index, long long const& time_stamp)
{
	auto const frame_dib_pixel_ptr{ oip->func_get_video(f, FCC('YUY2')) };

	long default_stride{};
	THROW_IF_FAILED(MFGetStrideForBitmapInfoHeader(MFVideoFormat_YUY2.Data1, oip->w, &default_stride));

	uint32_t image_size{};
	THROW_IF_FAILED(MFCalculateImageSize(MFVideoFormat_YUY2, oip->w, oip->h, &image_size));

	MN_MFOUTPUT_PRINT_DEBUG_CONSOLE("[INFO] Default stride is {}, and the image size is {}.", default_stride, image_size);

	BYTE* scanline{};
	long stride{};
	BYTE* video_2d_buffer_front{};
	DWORD video_2d_buffer_max_size{};

	auto video_buffer{ wil::com_ptr<IMFMediaBuffer>{} };
	THROW_IF_FAILED(MFCreate2DMediaBuffer(oip->w, oip->h, MFVideoFormat_YUY2.Data1, false, video_buffer.put()));
	THROW_IF_FAILED(video_buffer.query<IMF2DBuffer2>()->Lock2DSize(MF2DBuffer_LockFlags_Write, &scanline, &stride, &video_2d_buffer_front, &video_2d_buffer_max_size));
	if (f == 0) MN_MFOUTPUT_PRINT_DEBUG_CONSOLE("[INFO] IMF2DBuffer opened: {} given stride. Max size: {}", stride, video_2d_buffer_max_size);
	THROW_IF_FAILED(MFCopyImage(scanline, stride, static_cast<BYTE*>(frame_dib_pixel_ptr), default_stride, default_stride, oip->h));
	THROW_IF_FAILED(video_buffer.query<IMF2DBuffer2>()->Unlock2D());

	auto video_sample{ wil::com_ptr<IMFSample>{} };
	THROW_IF_FAILED(MFCreateSample(video_sample.put()));
	if (f == 0) MN_MFOUTPUT_PRINT_DEBUG_CONSOLE("[INFO] Created a sample.");
	THROW_IF_FAILED(video_sample->AddBuffer(video_buffer.get()));
	if (f == 0) MN_MFOUTPUT_PRINT_DEBUG_CONSOLE("[INFO] Added a buffer to the sample.");
	THROW_IF_FAILED(video_sample->SetSampleTime(time_stamp * f));
	if (f == 0) MN_MFOUTPUT_PRINT_DEBUG_CONSOLE("[INFO] Sample time: {}", time_stamp * f);
	THROW_IF_FAILED(video_sample->SetSampleDuration(time_stamp));
	if (f == 0) MN_MFOUTPUT_PRINT_DEBUG_CONSOLE("[INFO] Sample duration: {}", time_stamp);

	if (f == 0) MN_MFOUTPUT_PRINT_DEBUG_CONSOLE("[INFO] Trying to write the sample at index {}...", index);
	THROW_IF_FAILED(sink_writer->WriteSample(index, video_sample.get())); //! ここでMF_E_INVALIDREQUESTを吐いて死にます。
	if (f == 0) MN_MFOUTPUT_PRINT_DEBUG_CONSOLE("[INFO] Wrote a sample ({}) at {}.", time_stamp, time_stamp * f);
}

using unique_mfshutdown_call = wil::unique_call<decltype(&::MFShutdown), ::MFShutdown>;
[[nodiscard]] inline unique_mfshutdown_call MyMFStartup()
{
	THROW_IF_FAILED(::MFStartup(MF_VERSION));
	return unique_mfshutdown_call();
}

auto func_output(OUTPUT_INFO* oip)
{
#ifndef NDEBUG
	AllocConsole();
	auto console{ _open_osfhandle((intptr_t)GetStdHandle(STD_OUTPUT_HANDLE), _O_TEXT) };
	auto console_file_handle{ _fdopen(console, "w") };
	freopen_s(&console_file_handle, "CONOUT$", "w", stdout);
#endif // !NDEBUG

	MN_MFOUTPUT_PRINT_DEBUG_CONSOLE("[INFO] Initializing COM and MediaFoundation...");
	auto com_cleanup{ wil::CoInitializeEx() }; // CoInitializeExはDllMainで呼んではいけない
	auto mf_cleanup{ MyMFStartup() };

	MN_MFOUTPUT_PRINT_DEBUG_CONSOLE("[INFO] {}x{}; FPS: {}/{}. {} frames.", oip->w, oip->h, oip->rate, oip->scale, oip->n);
	MN_MFOUTPUT_PRINT_DEBUG_CONSOLE("[INFO] {}hz; {} ch. {} samples.", oip->audio_rate, oip->audio_ch, oip->audio_n);

	//auto const video_frame_duration{ static_cast<uint64_t>(oip->n / oip->rate) };

	uint64_t time_stamp{};
	THROW_IF_FAILED(MFFrameRateToAverageTimePerFrame(oip->rate, oip->scale, &time_stamp));

	auto [sink_writer, stream_indices] { initialize_sink_writer(oip) };
	auto [video_index, audio_index] { stream_indices };

	MN_MFOUTPUT_PRINT_DEBUG_CONSOLE("[INFO] Starting writing...");
	oip->func_set_buffer_size(32, 32);
	for (auto f{ 0 }; f < oip->n; ++f)
	{
		oip->func_rest_time_disp(static_cast<int>(f), oip->n);
		if (oip->func_is_abort()) break;

		write_video_frame_to_sink_writer(oip, sink_writer.get(), f, video_index, time_stamp);

		auto audio_start_position{ static_cast<int>(static_cast<double>(f) / oip->rate * oip->scale * oip->audio_rate) };
		auto audio_length{ static_cast<int>((static_cast<double>(f + 1) / oip->rate * oip->scale * oip->audio_rate) - audio_start_position) };
		int readed_audio_samples{};
		auto audio_data{ oip->func_get_audio(audio_start_position, audio_length, &readed_audio_samples, WAVE_FORMAT_PCM) };
		if (!readed_audio_samples) continue;

		for (auto ch{ 0 }; ch < oip->audio_ch; ++ch)
		{
			auto audio_buffer{ wil::com_ptr<IMFMediaBuffer>{} };
			MFCreateMemoryBuffer(get_pcm_block_alignment(1, 16), audio_buffer.put());
			byte* media_data{};
			DWORD media_data_max_length{};

			audio_buffer->Lock(&media_data, &media_data_max_length, nullptr);
			std::memcpy(media_data + (get_pcm_block_alignment(1, 16) * ch), audio_data, get_pcm_block_alignment(1, 16));
			audio_buffer->Unlock();

			audio_buffer->SetCurrentLength(get_pcm_block_alignment(1, 16));
			auto audio_sample{ wil::com_ptr<IMFSample>{} };
			audio_sample->AddBuffer(audio_buffer.get());
			MFCreateSample(audio_sample.put());
			audio_sample->SetSampleTime(time_stamp * f);
			audio_sample->SetSampleDuration(time_stamp);
			sink_writer->WriteSample(audio_index, audio_sample.get());
		}


	}
	sink_writer->Finalize();

#ifndef NDEBUG
	FreeConsole();
	_close(console);
#endif // !NDEBUG
	return true;
}

bool func_config(HWND, HINSTANCE)
{
	// ここに出力設定のダイアログを実装します
	return true; // 成功ならtrueを返す
}

wchar_t const* func_get_config_text()
{
	// ここに出力設定のテキスト情報を実装します
	return L"MonogoiNoobs's MediaFoundation File Saver preferences"; // 設定情報のテキストを返す
}

auto output_plugin_table{ OUTPUT_PLUGIN_TABLE{
	OUTPUT_PLUGIN_TABLE::FLAG_VIDEO | OUTPUT_PLUGIN_TABLE::FLAG_AUDIO, //	フラグ
	L"MediaFoundation File Saver",					// プラグインの名前
	L"MPEG-4 (*.mp4)\0*.mp4\0",					// 出力ファイルのフィルタ
	L"MonogoiNoobs's MediaFoundation File Saver v0.1.0",	// プラグインの情報
	func_output,									// 出力時に呼ばれる関数へのポインタ
	func_config,									// 出力設定のダイアログを要求された時に呼ばれる関数へのポインタ (nullptrなら呼ばれません)
	func_get_config_text,							// 出力設定のテキスト情報を取得する時に呼ばれる関数へのポインタ (nullptrなら呼ばれません)
} };

extern "C" __declspec(dllexport) auto GetOutputPluginTable()
{
	return &output_plugin_table;
}

auto APIENTRY DllMain(HMODULE, DWORD, void*)
{
	return true;
}