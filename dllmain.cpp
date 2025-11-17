#define STRICT
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <windowsx.h>
#include <tchar.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wil/com.h>
#include <D3D11.h>
#include <D3D12.h>
#include <mfd3d12.h>
#include <codecapi.h>
#include <Wmcodecdsp.h>
#include "resource.h"
#include "Rgb2NV12.h"
#include "aviutl2_sdk/output2.h"
#include "aviutl2_sdk/logger2.h"

#include <filesystem>
#include <format>
#include <tuple>

#pragma comment(lib, "Mfplat")
#pragma comment(lib, "Mfreadwrite")
#pragma comment(lib, "Mfuuid")
#pragma comment(lib, "d3d11")
#pragma comment(lib, "d3d12")

auto const constinit AUDIO_BITS_PER_SAMPLE{ 16 };
auto const constinit CONFIG_INI_PATH{ _T(R"(C:\ProgramData\aviutl2\Plugin\MFOutput.ini)") };

LOG_HANDLE constinit *aviutl_logger{};
bool constinit is_dx12_available{};

auto constexpr get_pcm_block_alignment(uint32_t &&audio_ch, uint32_t &&bit) noexcept
{
	return (audio_ch * bit) / 8;
}

auto constexpr get_suitable_input_video_format_guid(bool const &is_accelerated) noexcept
{
	return is_accelerated ? MFVideoFormat_NV12 : MFVideoFormat_YUY2;
}

auto const get_suitable_output_video_format_guid(std::filesystem::path const &extension, uint32_t const &preferred_mp4_format) noexcept
{
	if (extension == _T(".mp4")) return preferred_mp4_format ? MFVideoFormat_HEVC : MFVideoFormat_H264;
	if (extension == _T(".wmv")) return MFVideoFormat_WVC1;
	return MFVideoFormat_H264;
}

auto const add_windows_media_qvba_activation_media_attributes(IMFAttributes *const &attributes, uint32_t const &quality)
{
	THROW_IF_FAILED(attributes->SetUINT32(MFPKEY_VBRENABLED.fmtid, true));
	THROW_IF_FAILED(attributes->SetUINT32(MFPKEY_CONSTRAIN_ENUMERATED_VBRQUALITY.fmtid, true));
	THROW_IF_FAILED(attributes->SetUINT32(MFPKEY_DESIRED_VBRQUALITY.fmtid, quality));
}

auto const set_color_space_media_types(OUTPUT_INFO const *const &oip, IMFMediaType *const &media_type)
{
	THROW_IF_FAILED(media_type->SetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, MFNominalRange_16_235));
	if (oip->h <= 720)
	{
		aviutl_logger->info(aviutl_logger, _T("Expected YUV color space: BT.601"));
		THROW_IF_FAILED(media_type->SetUINT32(MF_MT_VIDEO_PRIMARIES, MFVideoPrimaries_SMPTE170M));
		THROW_IF_FAILED(media_type->SetUINT32(MF_MT_YUV_MATRIX, MFVideoTransferMatrix_BT601));
		THROW_IF_FAILED(media_type->SetUINT32(MF_MT_TRANSFER_FUNCTION, MFVideoTransFunc_709));
	}
	else if (oip->h > 720 && oip->h < 2160)
	{
		aviutl_logger->info(aviutl_logger, _T("Expected YUV color space: BT.709"));
		THROW_IF_FAILED(media_type->SetUINT32(MF_MT_VIDEO_PRIMARIES, MFVideoPrimaries_BT709));
		THROW_IF_FAILED(media_type->SetUINT32(MF_MT_YUV_MATRIX, MFVideoTransferMatrix_BT709));
		THROW_IF_FAILED(media_type->SetUINT32(MF_MT_TRANSFER_FUNCTION, MFVideoTransFunc_709));
	}
	else if (oip->h >= 2160)
	{
		aviutl_logger->info(aviutl_logger, _T("Expected YUV color space: BT.2020 (12bit)"));
		THROW_IF_FAILED(media_type->SetUINT32(MF_MT_VIDEO_PRIMARIES, MFVideoPrimaries_BT2020));
		THROW_IF_FAILED(media_type->SetUINT32(MF_MT_YUV_MATRIX, MFVideoTransferMatrix_BT2020_12));
		THROW_IF_FAILED(media_type->SetUINT32(MF_MT_TRANSFER_FUNCTION, MFVideoTransFunc_2020));
	}
}

auto const make_input_video_media_type(OUTPUT_INFO const *const &oip, bool const &is_accelerated)
{
	uint32_t image_size{};
	THROW_IF_FAILED(MFCalculateImageSize(get_suitable_input_video_format_guid(is_accelerated), oip->w, oip->h, &image_size));

	//long default_stride{};
	//THROW_IF_FAILED(MFGetStrideForBitmapInfoHeader(output_video_format.Data1, oip->w, &default_stride));

	auto input_video_media_type{ wil::com_ptr<IMFMediaType>{} };
	THROW_IF_FAILED(MFCreateMediaType(&input_video_media_type));
	THROW_IF_FAILED(input_video_media_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video));
	THROW_IF_FAILED(input_video_media_type->SetGUID(MF_MT_SUBTYPE, get_suitable_input_video_format_guid(is_accelerated)));
	THROW_IF_FAILED(input_video_media_type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive));
	//THROW_IF_FAILED(input_video_media_type->SetUINT32(MF_MT_DEFAULT_STRIDE, default_stride));
	THROW_IF_FAILED(input_video_media_type->SetUINT32(MF_MT_SAMPLE_SIZE, image_size));
	THROW_IF_FAILED(input_video_media_type->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, true));
	THROW_IF_FAILED(input_video_media_type->SetUINT32(MF_MT_D3D_RESOURCE_VERSION, is_dx12_available ? MF_D3D12_RESOURCE : MF_D3D11_RESOURCE));
	set_color_space_media_types(oip, input_video_media_type.get());
	THROW_IF_FAILED(MFSetAttributeSize(input_video_media_type.get(), MF_MT_FRAME_SIZE, oip->w, oip->h));
	THROW_IF_FAILED(MFSetAttributeRatio(input_video_media_type.get(), MF_MT_FRAME_RATE, oip->rate, oip->scale));
	THROW_IF_FAILED(MFSetAttributeRatio(input_video_media_type.get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1));
	return input_video_media_type;
}

auto const make_input_audio_media_type(OUTPUT_INFO const *const &oip, GUID const &output_video_format)
{
	auto input_audio_media_type{ wil::com_ptr<IMFMediaType>{} };
	THROW_IF_FAILED(MFCreateMediaType(&input_audio_media_type));
	THROW_IF_FAILED(input_audio_media_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio));
	THROW_IF_FAILED(input_audio_media_type->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM));
	THROW_IF_FAILED(input_audio_media_type->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, AUDIO_BITS_PER_SAMPLE));
	THROW_IF_FAILED(input_audio_media_type->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, oip->audio_rate));
	THROW_IF_FAILED(input_audio_media_type->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, oip->audio_ch));

	if (output_video_format == MFVideoFormat_WVC1)
	{
		auto const block_alignment{ get_pcm_block_alignment(static_cast<uint32_t>(oip->audio_ch), AUDIO_BITS_PER_SAMPLE) };
		THROW_IF_FAILED(input_audio_media_type->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, block_alignment));
		THROW_IF_FAILED(input_audio_media_type->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, block_alignment * oip->audio_rate));
		THROW_IF_FAILED(input_audio_media_type->SetUINT32(MF_MT_AVG_BITRATE, oip->audio_rate * AUDIO_BITS_PER_SAMPLE * oip->audio_ch));
		THROW_IF_FAILED(input_audio_media_type->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, true));
		THROW_IF_FAILED(input_audio_media_type->SetUINT32(MF_MT_FIXED_SIZE_SAMPLES, true));
	}
	return input_audio_media_type;
}

std::pair<wil::com_ptr<IMFMediaType>, wil::com_ptr<IMFMediaType>> const make_input_media_types(OUTPUT_INFO const *const &oip, GUID const &output_video_format, bool const &is_accelerated)
{
	return { make_input_video_media_type(oip, is_accelerated), make_input_audio_media_type(oip, output_video_format) };
}

auto const write_sample_to_sink_writer(IMFSinkWriter *const sink_writer, DWORD const &index, IMFMediaBuffer *const buffer, int64_t const &time, int64_t const &duration)
{
	auto sample{ wil::com_ptr<IMFSample>{} };
	THROW_IF_FAILED(MFCreateSample(&sample));
	THROW_IF_FAILED(sample->AddBuffer(buffer));
	THROW_IF_FAILED(sample->SetSampleTime(time));
	THROW_IF_FAILED(sample->SetSampleDuration(duration));
	THROW_IF_FAILED(sink_writer->WriteSample(index, sample.get()));
}

[[nodiscard]] auto make_sink_writer(TCHAR const *const &output_name, bool const &is_accelerated)
{
	auto sink_writer_attributes{ wil::com_ptr<IMFAttributes>{} };
	THROW_IF_FAILED(MFCreateAttributes(&sink_writer_attributes, 3));
	THROW_IF_FAILED(sink_writer_attributes->SetUINT32(MF_SINK_WRITER_DISABLE_THROTTLING, true));

	if (is_accelerated)
	{
		aviutl_logger->info(aviutl_logger, _T("Preparing DirectX..."));
		auto directx_device{ wil::com_ptr<IUnknown>{} };
		if (is_dx12_available)
		{
			aviutl_logger->info(aviutl_logger, _T("DirectX 12 is available so going to be used."));
			THROW_IF_FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_1, __uuidof(ID3D12Device), directx_device.put_void()));
		}
		else
		{
			D3D_FEATURE_LEVEL const D3D_FEATURE_LEVELS[]{
				D3D_FEATURE_LEVEL_11_1,
				D3D_FEATURE_LEVEL_11_0,
				D3D_FEATURE_LEVEL_10_1,
				D3D_FEATURE_LEVEL_10_0,
				D3D_FEATURE_LEVEL_9_3,
				D3D_FEATURE_LEVEL_9_2,
				D3D_FEATURE_LEVEL_9_1
			};
			THROW_IF_FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_VIDEO_SUPPORT, D3D_FEATURE_LEVELS, _countof(D3D_FEATURE_LEVELS), D3D11_SDK_VERSION, reinterpret_cast<ID3D11Device **>(directx_device.put()), nullptr, nullptr));
		}

		auto dxgi_device_manager{ wil::com_ptr<IMFDXGIDeviceManager>{} };
		uint32_t reset_token{};
		THROW_IF_FAILED(MFCreateDXGIDeviceManager(&reset_token, &dxgi_device_manager));
		THROW_IF_FAILED(dxgi_device_manager->ResetDevice(directx_device.get(), reset_token));

		THROW_IF_FAILED(sink_writer_attributes->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, true));
		THROW_IF_FAILED(sink_writer_attributes->SetUnknown(MF_SINK_WRITER_D3D_MANAGER, dxgi_device_manager.get()));
	}

	auto sink_writer{ wil::com_ptr<IMFSinkWriter>{} };
	THROW_IF_FAILED(MFCreateSinkWriterFromURL(output_name, nullptr, sink_writer_attributes.get(), &sink_writer));

	return sink_writer;
}

[[nodiscard]] auto const configure_video_stream(OUTPUT_INFO const *const &oip, IMFSinkWriter *const &sink_writer, GUID const &output_video_format)
{
	auto output_video_media_type{ wil::com_ptr<IMFMediaType>{} };
	THROW_IF_FAILED(MFCreateMediaType(&output_video_media_type));

	THROW_IF_FAILED(output_video_media_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video));
	THROW_IF_FAILED(output_video_media_type->SetGUID(MF_MT_SUBTYPE, output_video_format));
	THROW_IF_FAILED(output_video_media_type->SetUINT32(MF_MT_AVG_BITRATE, 12000000));
	THROW_IF_FAILED(MFSetAttributeSize(output_video_media_type.get(), MF_MT_FRAME_SIZE, oip->w, oip->h));
	THROW_IF_FAILED(MFSetAttributeRatio(output_video_media_type.get(), MF_MT_FRAME_RATE, oip->rate, oip->scale));
	THROW_IF_FAILED(output_video_media_type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive));
	set_color_space_media_types(oip, output_video_media_type.get());

	switch (output_video_format.Data1)
	{
	case FCC('H264'):
		THROW_IF_FAILED(output_video_media_type->SetUINT32(MF_MPEG4SINK_MOOV_BEFORE_MDAT, true));
		THROW_IF_FAILED(output_video_media_type->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_High));
		break;
	case FCC('HEVC'):
		THROW_IF_FAILED(output_video_media_type->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH265VProfile_Main_420_8));
		THROW_IF_FAILED(output_video_media_type->SetUINT32(MF_MT_MPEG2_LEVEL, eAVEncH265VLevel5_1));
		break;
	default:
		break;
	}

	DWORD video_index{};
	THROW_IF_FAILED(sink_writer->AddStream(output_video_media_type.get(), &video_index));

	return video_index;
}

[[nodiscard]] auto configure_audio_stream(OUTPUT_INFO const *const &oip, IMFSinkWriter *const &sink_writer, uint32_t const &output_bit_rate, GUID const &output_video_format)
{
	__assume(output_bit_rate <= 3);

	auto output_audio_media_type{ wil::com_ptr<IMFMediaType>{} };
	THROW_IF_FAILED(MFCreateMediaType(&output_audio_media_type));

	THROW_IF_FAILED(output_audio_media_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio));
	THROW_IF_FAILED(output_audio_media_type->SetGUID(MF_MT_SUBTYPE, output_video_format == MFVideoFormat_WVC1 ? MFAudioFormat_WMAudioV9 : MFAudioFormat_AAC));
	THROW_IF_FAILED(output_audio_media_type->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, oip->audio_ch));
	THROW_IF_FAILED(output_audio_media_type->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, (3 + output_bit_rate) * 4000));
	THROW_IF_FAILED(output_audio_media_type->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, oip->audio_rate));
	THROW_IF_FAILED(output_audio_media_type->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, AUDIO_BITS_PER_SAMPLE));

	DWORD audio_index{};
	THROW_IF_FAILED(sink_writer->AddStream(output_audio_media_type.get(), &audio_index));

	return audio_index;
}

auto const configure_video_input(OUTPUT_INFO const *const &oip, IMFSinkWriter *const &sink_writer, DWORD const &video_index, uint32_t const &quality, bool const &is_accelerated, GUID const &output_video_format, IMFMediaType *const &input_video_media_type)
{
	__assume(quality <= 100);

	aviutl_logger->info(aviutl_logger, std::format(_T("Configuring video input: {}x{}, fps={}/{}, q={}, {}accelerated"), oip->w, oip->h, oip->rate, oip->scale, quality, is_accelerated ? _T("") : _T("not ")).c_str());

	auto video_encoder_attributes{ wil::com_ptr<IMFAttributes>{} };
	THROW_IF_FAILED(MFCreateAttributes(&video_encoder_attributes, 4));

	switch (output_video_format.Data1)
	{
	default:
	case FCC('H264'):
		THROW_IF_FAILED(video_encoder_attributes->SetUINT32(CODECAPI_AVEncH264CABACEnable, true));
		[[fallthrough]];
	case FCC('HEVC'):
		THROW_IF_FAILED(video_encoder_attributes->SetUINT32(CODECAPI_AVEncMPVDefaultBPictureCount, 2));
		THROW_IF_FAILED(video_encoder_attributes->SetUINT32(CODECAPI_AVEncCommonRateControlMode, eAVEncCommonRateControlMode_Quality));
		THROW_IF_FAILED(video_encoder_attributes->SetUINT32(CODECAPI_AVEncCommonQuality, quality));
		break;
	case FCC('WVC1'):
		THROW_IF_FAILED(video_encoder_attributes->SetUINT32(MFPKEY_COMPRESSIONOPTIMIZATIONTYPE.fmtid, 1));
		add_windows_media_qvba_activation_media_attributes(video_encoder_attributes.get(), quality);
		break;
	}

	THROW_IF_FAILED(sink_writer->SetInputMediaType(video_index, input_video_media_type, video_encoder_attributes.get()));
}

auto const configure_audio_input(IMFSinkWriter *const &sink_writer, DWORD const &audio_index, uint32_t const &quality, GUID const &output_video_format, IMFMediaType *const &input_audio_media_type)
{
	auto audio_encoder_attributes{ wil::com_ptr<IMFAttributes>{} };

	if (output_video_format == MFVideoFormat_WVC1)
	{
		THROW_IF_FAILED(MFCreateAttributes(&audio_encoder_attributes, 3));
		add_windows_media_qvba_activation_media_attributes(audio_encoder_attributes.get(), quality);
	}

	THROW_IF_FAILED(sink_writer->SetInputMediaType(audio_index, input_audio_media_type, audio_encoder_attributes.get()));
}

std::tuple<wil::com_ptr<IMFSinkWriter>, DWORD, DWORD> initialize_sink_writer(OUTPUT_INFO const *const &oip, bool const &is_accelerated, GUID const &output_video_format, IMFMediaType *const &input_video_media_type, IMFMediaType *const &input_audio_media_type)
{
	auto const sink_writer{ make_sink_writer(oip->savefile, is_accelerated) };

	auto const quality{ GetPrivateProfileInt(_T("mp4"), _T("videoQuality"), 70, CONFIG_INI_PATH)};
	auto video_index{ configure_video_stream(oip, sink_writer.get(), output_video_format) };
	auto audio_index{ configure_audio_stream(oip, sink_writer.get(), GetPrivateProfileInt(_T("mp4"), _T("audioBitRate"), 3, CONFIG_INI_PATH), output_video_format) };
	configure_video_input(oip, sink_writer.get(), video_index, quality, is_accelerated, output_video_format, input_video_media_type);
	configure_audio_input(sink_writer.get(), audio_index, quality, output_video_format, input_audio_media_type);

	THROW_IF_FAILED(sink_writer->BeginWriting());

	return { sink_writer, video_index, audio_index };
}

auto const write_video_sample(OUTPUT_INFO const *const &oip, IMFSinkWriter *const &sink_writer, int32_t const &f, DWORD const &index, int64_t const &time_stamp, long const &default_stride, bool const &is_accelerated, IMFMediaType *const &input_media_type)
{
	if (oip->func_is_abort()) return false;

	oip->func_rest_time_disp(f, oip->n);
	
	uint8_t *frame_image{};

	if (is_accelerated)
	{
		auto frame_dib_pixels{ static_cast<uint8_t *>(oip->func_get_video(f, BI_RGB)) };
		THROW_IF_NULL_ALLOC(frame_dib_pixels);
		auto flipped{ new uint8_t[oip->w * oip->h * 3]{} };
		Bgr2RgbAndReverseUpDown(frame_dib_pixels, oip->w, oip->h, flipped);
		auto nv12{ new uint8_t[oip->w * oip->h * 3]{} };
		Rgb2NV12_useSSE(flipped, oip->w, oip->h, nv12);
		delete[] flipped;
		frame_image = nv12;
	}
	else frame_image = static_cast<uint8_t *>(oip->func_get_video(f, FCC('YUY2')));

	auto video_buffer{ wil::com_ptr<IMFMediaBuffer>{} };
	THROW_IF_FAILED(MFCreateMediaBufferFromMediaType(input_media_type, time_stamp, 0, 0, &video_buffer));

	uint8_t *scanline{};
	long stride{};
	uint8_t *buffer_start{};
	DWORD buffer_length{};
	THROW_IF_FAILED(video_buffer.query<IMF2DBuffer2>()->Lock2DSize(MF2DBuffer_LockFlags_Write, &scanline, &stride, &buffer_start, &buffer_length));
	THROW_IF_FAILED(MFCopyImage(scanline, stride, frame_image, default_stride, default_stride, oip->h));
	THROW_IF_FAILED(video_buffer.query<IMF2DBuffer2>()->Unlock2D());
	if (is_accelerated) delete[] frame_image, frame_image = nullptr;
	// “Generally, you should avoid mixing calls to IMF2DBuffer and IMFMediaBuffer methods on the same media buffer.”
	//     —Microsoft, in https://learn.microsoft.com/en-us/windows/win32/api/mfobjects/nn-mfobjects-imf2dbuffer
	// 
	// https://stackoverflow.com/questions/47930340/
	DWORD contiguous_length{};
	THROW_IF_FAILED(video_buffer.query<IMF2DBuffer2>()->GetContiguousLength(&contiguous_length));
	THROW_IF_FAILED(video_buffer->SetCurrentLength(contiguous_length));

	write_sample_to_sink_writer(sink_writer, index, video_buffer.get(), time_stamp * f, time_stamp);

	return true;
}

auto const write_audio_sample(OUTPUT_INFO const *const &oip, IMFSinkWriter *const &sink_writer, int32_t const &n, DWORD const &index, IMFMediaType *const &input_media_type)
{
	if (oip->func_is_abort()) return false;

	oip->func_rest_time_disp(n, oip->audio_n);

	// bytes per audio-frame (block align)
	auto const block_alignment{ get_pcm_block_alignment(static_cast<uint32_t>(oip->audio_ch), AUDIO_BITS_PER_SAMPLE) }; // bytes per audio-frame
	auto const max_samples{ static_cast<int32_t>(block_alignment * oip->audio_rate) }; // bytes per second

	int32_t actual_samples{};
	auto audio_data{ oip->func_get_audio(n, max_samples, &actual_samples, WAVE_FORMAT_PCM) };
	if (!actual_samples) return true;

	// compute number of audio-frames (samples per channel) in buffer
	auto const sample_duration{ static_cast<int64_t>(actual_samples) * 10'000'000LL / max_samples }; // 100-ns units
	auto const sample_time{ static_cast<int64_t>(n) * 10'000'000LL / oip->audio_rate }; // 100-ns units for start
	if (n == 0) aviutl_logger->info(aviutl_logger, std::format(_T("audio: duration={}, time={}"), sample_duration, sample_time).c_str());

	auto audio_buffer{ wil::com_ptr<IMFMediaBuffer>{} };
	THROW_IF_FAILED(MFCreateMediaBufferFromMediaType(input_media_type, sample_duration, static_cast<DWORD>(actual_samples), 0, &audio_buffer));
	uint8_t *media_data{};
	DWORD media_data_max_length{};
	THROW_IF_FAILED(audio_buffer->Lock(&media_data, &media_data_max_length, nullptr));
	memcpy_s(media_data, media_data_max_length, audio_data, static_cast<size_t>(actual_samples));
	THROW_IF_FAILED(audio_buffer->Unlock());
	THROW_IF_FAILED(audio_buffer->SetCurrentLength(static_cast<DWORD>(actual_samples)));
	
	write_sample_to_sink_writer(sink_writer, index, audio_buffer.get(), sample_time, sample_duration);

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
	auto const com_cleanup{ wil::CoInitializeEx() };
	auto const mf_cleanup{ MyMFStartup() };

	uint64_t time_stamp{};
	THROW_IF_FAILED(MFFrameRateToAverageTimePerFrame(oip->rate, oip->scale, &time_stamp));

	auto const path{ std::filesystem::path{ oip->savefile } };
	auto const output_video_format{ get_suitable_output_video_format_guid(path.extension(), GetPrivateProfileInt(_T("mp4"), _T("videoFormat"), 0, CONFIG_INI_PATH))};

	auto const is_accelerated{ GetPrivateProfileInt(_T("general"), _T("useHardware"), BST_UNCHECKED, CONFIG_INI_PATH) == BST_CHECKED };

	long default_stride{};
	THROW_IF_FAILED(MFGetStrideForBitmapInfoHeader(get_suitable_input_video_format_guid(is_accelerated).Data1, oip->w, &default_stride));

	auto const [input_video_media_type, input_audio_media_type]{ make_input_media_types(oip, output_video_format, is_accelerated) };

	auto const [sink_writer, video_index, audio_index] { initialize_sink_writer(oip, is_accelerated, output_video_format, input_video_media_type.get(), input_audio_media_type.get()) };

	oip->func_set_buffer_size(16, 16);
	aviutl_logger->info(aviutl_logger, _T("Sending video samples to the writer..."));
	for (auto f{ 0 }; f < oip->n; ++f)
		if (!write_video_sample(oip, sink_writer.get(), f, video_index, time_stamp, default_stride, is_accelerated, input_video_media_type.get())) goto abort;
	aviutl_logger->info(aviutl_logger, _T("Sending audio samples to the writer..."));
	for (auto n{ 0 }; n < oip->audio_n; n += oip->audio_rate)
		if (!write_audio_sample(oip, sink_writer.get(), n, audio_index, input_audio_media_type.get())) goto abort;

	aviutl_logger->info(aviutl_logger, _T("Finalizing. It may take a while..."));
	sink_writer->Finalize();

abort:
	aviutl_logger->info(aviutl_logger, _T("Done."));
	return true;
}

intptr_t CALLBACK config_dialog_proc(HWND dialog, uint32_t message, WPARAM w_param, LPARAM)
{
	static TCHAR quality_wchar{};

	auto const reset{ [&](){
		ComboBox_SetCurSel(GetDlgItem(dialog, IDC_COMBO2), 0);
		THROW_IF_WIN32_BOOL_FALSE(SetDlgItemText(dialog, IDC_EDIT1, _T("70")));
		ComboBox_SetCurSel(GetDlgItem(dialog, IDC_COMBO1), 3);
		Button_SetCheck(GetDlgItem(dialog, IDC_CHECK1), BST_UNCHECKED);
	} };

	uint32_t quality{};
	TCHAR audio_bit_rate_wchar[4]{};
	TCHAR video_format_wchar[2]{};
	switch (message)
	{
	case WM_INITDIALOG:
		ComboBox_AddString(GetDlgItem(dialog, IDC_COMBO2), _T("H.264"));
		ComboBox_AddString(GetDlgItem(dialog, IDC_COMBO2), _T("HEVC"));

		ComboBox_AddString(GetDlgItem(dialog, IDC_COMBO1), _T("96"));
		ComboBox_AddString(GetDlgItem(dialog, IDC_COMBO1), _T("128"));
		ComboBox_AddString(GetDlgItem(dialog, IDC_COMBO1), _T("160"));
		ComboBox_AddString(GetDlgItem(dialog, IDC_COMBO1), _T("192"));

		ComboBox_SetCurSel(GetDlgItem(dialog, IDC_COMBO2), GetPrivateProfileInt(_T("mp4"), _T("videoFormat"), 0, CONFIG_INI_PATH));

		GetPrivateProfileString(_T("mp4"), _T("videoQuality"), _T("70"), &quality_wchar, 3, CONFIG_INI_PATH);
		THROW_IF_WIN32_BOOL_FALSE(SetDlgItemText(dialog, IDC_EDIT1, &quality_wchar));
		ComboBox_SetCurSel(GetDlgItem(dialog, IDC_COMBO1), GetPrivateProfileInt(_T("mp4"), _T("audioBitRate"), 3, CONFIG_INI_PATH));

		Button_SetCheck(GetDlgItem(dialog, IDC_CHECK1), GetPrivateProfileInt(_T("general"), _T("useHardware"), BST_UNCHECKED, CONFIG_INI_PATH));

		return false;
	case WM_COMMAND:
		switch (LOWORD(w_param))
		{
		case IDNO:
			if (MessageBox(dialog, _T("全ての設定を初期化しますか？"), _T("設定値のリセット"), MB_YESNO | MB_ICONWARNING) == IDYES)
			{
				reset();
			}
			return false;
		case IDOK:
			quality = GetDlgItemInt(dialog, IDC_EDIT1, nullptr, false);
			if (quality > 100 || quality == 0)
			{
				MessageBox(dialog, _T("映像品質は1〜100の範囲で指定してください。"), nullptr, MB_OK | MB_ICONERROR);
				return false;
			}

			_ltot_s(static_cast<long>(ComboBox_GetCurSel(GetDlgItem(dialog, IDC_COMBO2))), video_format_wchar, _countof(video_format_wchar), 10);
			THROW_IF_WIN32_BOOL_FALSE(WritePrivateProfileString(_T("mp4"), _T("videoFormat"), video_format_wchar, CONFIG_INI_PATH));

			GetDlgItemTextW(dialog, IDC_EDIT1, &quality_wchar, 3);
			THROW_IF_WIN32_BOOL_FALSE(WritePrivateProfileString(_T("mp4"), _T("videoQuality"), &quality_wchar, CONFIG_INI_PATH));

			_ltot_s(static_cast<long>(ComboBox_GetCurSel(GetDlgItem(dialog, IDC_COMBO1))), audio_bit_rate_wchar, _countof(audio_bit_rate_wchar), 10);
			THROW_IF_WIN32_BOOL_FALSE(WritePrivateProfileString(_T("mp4"), _T("audioBitRate"), audio_bit_rate_wchar, CONFIG_INI_PATH));

			THROW_IF_WIN32_BOOL_FALSE(WritePrivateProfileString(_T("general"), _T("useHardware"), Button_GetCheck(GetDlgItem(dialog, IDC_CHECK1)) ? _T("1") : _T("0"), CONFIG_INI_PATH));
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

auto func_config(HWND window, HINSTANCE instance)
{
	// ここに出力設定のダイアログを実装します
	DialogBox(instance, MAKEINTRESOURCE(IDD_DIALOG1), window, config_dialog_proc);
	return true; // 成功ならtrueを返す
}

auto CALLBACK wil_log_callback(wil::FailureInfo const &failure) noexcept
{
	using enum wil::FailureType;

	switch (failure.type)
	{
	case Exception:
		aviutl_logger->error(aviutl_logger, std::format(L"{}. At line {}. ({})", failure.pszMessage, failure.uLineNumber, failure.hr).c_str());
		break;
	case Log:
		aviutl_logger->log(aviutl_logger, failure.pszMessage);
		break;
	}
}

extern "C" __declspec(dllexport) auto const InitializeLogger(LOG_HANDLE *logger) noexcept
{
	aviutl_logger = logger;
	wil::SetResultLoggingCallback(wil_log_callback);
}

extern "C" __declspec(dllexport) auto const InitializePlugin(DWORD) noexcept
{
	is_dx12_available = SUCCEEDED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_1, __uuidof(ID3D12Device), nullptr));
}

//auto func_get_config_text()
//{
//	// ここに出力設定のテキスト情報を実装します
//	return L"a"; // 設定情報のテキストを返す
//}

auto constexpr output_plugin_table{ OUTPUT_PLUGIN_TABLE{
	OUTPUT_PLUGIN_TABLE::FLAG_VIDEO | OUTPUT_PLUGIN_TABLE::FLAG_AUDIO, //	フラグ
	_T("MFOutput"),					// プラグインの名前
	_T("MP4 (*.mp4)\0*.mp4\0Advanced Systems Format (*.wmv)\0*.wmv\0"),					// 出力ファイルのフィルタ
	_T("MFOutput (") _T(__DATE__) _T(") by MonogoiNoobs"),	// プラグインの情報
	func_output,									// 出力時に呼ばれる関数へのポインタ
	func_config,									// 出力設定のダイアログを要求された時に呼ばれる関数へのポインタ (nullptrなら呼ばれません)
	nullptr,							// 出力設定のテキスト情報を取得する時に呼ばれる関数へのポインタ (nullptrなら呼ばれません)
} };

extern "C" __declspec(dllexport) auto constexpr GetOutputPluginTable() noexcept
{
	return &output_plugin_table;
}

auto const APIENTRY DllMain(HMODULE handle, DWORD reason, void *reserved) noexcept
{
	wil::DLLMain(handle, reason, reserved);
	return true;
}