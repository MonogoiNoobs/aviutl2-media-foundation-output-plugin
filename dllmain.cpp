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
#include "aviutl2_sdk/output2.h"
#include "aviutl2_sdk/logger2.h"

#include <filesystem>
#include <format>
#include <tuple>
#include <array>

#pragma comment(lib, "Mfplat")
#pragma comment(lib, "Mfreadwrite")
#pragma comment(lib, "Mfuuid")
#pragma comment(lib, "d3d11")
#pragma comment(lib, "d3d12")

#pragma warning(push)
#pragma warning(default: 4557 5266)

namespace mfop::GLOBAL
{
LOG_HANDLE constinit *aviutl_logger{};
}
namespace mfop
{
auto const constinit audio_bits_per_sample{ 16 };
auto const constinit configuration_ini_path{ _T(R"(C:\ProgramData\aviutl2\Plugin\MFOutput.ini)") };

using nv12_ptr = std::unique_ptr<uint8_t[]>;
using resolution_t = std::pair<int32_t, int32_t>;
using fps_t = std::pair<int32_t, int32_t>;
using stream_indices_t = std::pair<DWORD, DWORD>;
using IMFMediaTypes = std::pair<wil::com_ptr<IMFMediaType>, wil::com_ptr<IMFMediaType>>;

auto yuy2_to_nv12(uint8_t const yuy2[], resolution_t &&resolution)
{
	__assume(resolution.first % 2 == 0 && resolution.second % 2 == 0);

	auto const [width, height] { resolution };

	nv12_ptr output{ std::make_unique_for_overwrite<uint8_t[]>(width * (static_cast<size_t>(height / 2) + height)) };

	auto const stride{ width * 2 };
	auto const image_size{ static_cast<size_t>(width * height) };

#pragma omp parallel
	{
#pragma omp for nowait
		for (auto i{ 0 }; i < stride * height; i += 2)
			_mm256_store_si256
			(
				reinterpret_cast<__m256i *>(&output[i / 2]),
				_mm256_load_si256(reinterpret_cast<__m256i const *>(&yuy2[i]))
			);

		for (size_t current_height{ 0 }; current_height < height; current_height += 2)
#pragma omp for nowait
			for (auto i{ 0 }; i < stride; i += 4)
				_mm256_store_si256(reinterpret_cast<__m256i *>(&output[image_size + (width * current_height / 2) + 0]), _mm256_load_si256(reinterpret_cast<__m256i const *>(&yuy2[stride * current_height + i + 1]))),
				_mm256_store_si256(reinterpret_cast<__m256i *>(&output[image_size + (width * current_height / 2) + 1]), _mm256_load_si256(reinterpret_cast<__m256i const *>(&yuy2[stride * current_height + i + 3])));
	}

	return output;
}
auto is_dx12_available()
{
	return SUCCEEDED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_1, __uuidof(ID3D12Device), nullptr));
}
auto constexpr get_pcm_block_alignment(uint32_t &&audio_ch, uint32_t &&bit) noexcept
{
	return (audio_ch * bit) / 8;
}
auto constexpr get_suitable_input_video_format_guid(bool const &is_accelerated) noexcept
{
	return is_accelerated ? MFVideoFormat_NV12 : MFVideoFormat_YUY2;
}
auto get_suitable_output_video_format_guid(std::filesystem::path &&extension, uint32_t &&preferred_mp4_format) noexcept
{
	if (extension == _T(".mp4")) return preferred_mp4_format ? MFVideoFormat_HEVC : MFVideoFormat_H264;
	if (extension == _T(".wmv")) return MFVideoFormat_WVC1;
	return MFVideoFormat_H264;
}
auto add_windows_media_qvba_activation_media_attributes(IMFAttributes *const &attributes, uint32_t const &quality)
{
	THROW_IF_FAILED(attributes->SetUINT32(MFPKEY_VBRENABLED.fmtid, true));
	THROW_IF_FAILED(attributes->SetUINT32(MFPKEY_CONSTRAIN_ENUMERATED_VBRQUALITY.fmtid, true));
	THROW_IF_FAILED(attributes->SetUINT32(MFPKEY_DESIRED_VBRQUALITY.fmtid, quality));
}
auto set_color_space_media_types(IMFMediaType *const &media_type, int32_t const &height)
{
	THROW_IF_FAILED(media_type->SetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, MFNominalRange_16_235));
	if (height <= 720)
	{
		GLOBAL::aviutl_logger->info(GLOBAL::aviutl_logger, _T("Expected YUV colour space: BT.601"));
		THROW_IF_FAILED(media_type->SetUINT32(MF_MT_VIDEO_PRIMARIES, MFVideoPrimaries_SMPTE170M));
		THROW_IF_FAILED(media_type->SetUINT32(MF_MT_YUV_MATRIX, MFVideoTransferMatrix_BT601));
		THROW_IF_FAILED(media_type->SetUINT32(MF_MT_TRANSFER_FUNCTION, MFVideoTransFunc_709));
		THROW_IF_FAILED(media_type->SetUINT32(MF_MT_VIDEO_CHROMA_SITING, MFVideoChromaSubsampling_Horizontally_Cosited));
	}
	else if (height > 720 && height < 2160)
	{
		GLOBAL::aviutl_logger->info(GLOBAL::aviutl_logger, _T("Expected YUV colour space: BT.709"));
		THROW_IF_FAILED(media_type->SetUINT32(MF_MT_VIDEO_PRIMARIES, MFVideoPrimaries_BT709));
		THROW_IF_FAILED(media_type->SetUINT32(MF_MT_YUV_MATRIX, MFVideoTransferMatrix_BT709));
		THROW_IF_FAILED(media_type->SetUINT32(MF_MT_TRANSFER_FUNCTION, MFVideoTransFunc_709));
		THROW_IF_FAILED(media_type->SetUINT32(MF_MT_VIDEO_CHROMA_SITING, MFVideoChromaSubsampling_Horizontally_Cosited));
	}
	else if (height >= 2160)
	{
		GLOBAL::aviutl_logger->info(GLOBAL::aviutl_logger, _T("Expected YUV colour space: BT.2020 (12bit)"));
		THROW_IF_FAILED(media_type->SetUINT32(MF_MT_VIDEO_PRIMARIES, MFVideoPrimaries_BT2020));
		THROW_IF_FAILED(media_type->SetUINT32(MF_MT_YUV_MATRIX, MFVideoTransferMatrix_BT2020_12));
		THROW_IF_FAILED(media_type->SetUINT32(MF_MT_TRANSFER_FUNCTION, MFVideoTransFunc_2020));
		THROW_IF_FAILED(media_type->SetUINT32(MF_MT_VIDEO_CHROMA_SITING, MFVideoChromaSubsampling_Cosited));
	}
}
auto make_input_video_media_type(resolution_t &&resolution, fps_t &&fps, bool const &is_accelerated)
{
	auto const [width, height] { resolution };
	auto const [rate, scale] { fps };

	uint32_t image_size{};
	THROW_IF_FAILED(MFCalculateImageSize(get_suitable_input_video_format_guid(is_accelerated), width, height, &image_size));

	//long default_stride{};
	//THROW_IF_FAILED(MFGetStrideForBitmapInfoHeader(output_video_format.Data1, width, &default_stride));

	auto input_video_media_type{ wil::com_ptr<IMFMediaType>{} };
	THROW_IF_FAILED(MFCreateMediaType(&input_video_media_type));
	THROW_IF_FAILED(input_video_media_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video));
	THROW_IF_FAILED(input_video_media_type->SetGUID(MF_MT_SUBTYPE, get_suitable_input_video_format_guid(is_accelerated)));
	THROW_IF_FAILED(input_video_media_type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive));
	//THROW_IF_FAILED(input_video_media_type->SetUINT32(MF_MT_DEFAULT_STRIDE, default_stride));
	THROW_IF_FAILED(input_video_media_type->SetUINT32(MF_MT_SAMPLE_SIZE, image_size));
	THROW_IF_FAILED(input_video_media_type->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, true));
	THROW_IF_FAILED(input_video_media_type->SetUINT32(MF_MT_D3D_RESOURCE_VERSION, is_dx12_available() ? MF_D3D12_RESOURCE : MF_D3D11_RESOURCE));
	THROW_IF_FAILED(MFSetAttributeSize(input_video_media_type.get(), MF_MT_FRAME_SIZE, width, height));
	THROW_IF_FAILED(MFSetAttributeRatio(input_video_media_type.get(), MF_MT_FRAME_RATE, rate, scale));
	THROW_IF_FAILED(MFSetAttributeRatio(input_video_media_type.get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1));
	return input_video_media_type;
}
auto make_input_audio_media_type(int32_t const &channel_count, int32_t const &sampling_rate, GUID const &output_video_format)
{
	auto input_audio_media_type{ wil::com_ptr<IMFMediaType>{} };
	THROW_IF_FAILED(MFCreateMediaType(&input_audio_media_type));
	THROW_IF_FAILED(input_audio_media_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio));
	THROW_IF_FAILED(input_audio_media_type->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM));
	THROW_IF_FAILED(input_audio_media_type->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, mfop::audio_bits_per_sample));
	THROW_IF_FAILED(input_audio_media_type->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, sampling_rate));
	THROW_IF_FAILED(input_audio_media_type->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, channel_count));

	if (output_video_format == MFVideoFormat_WVC1)
	{
		auto const block_alignment{ get_pcm_block_alignment(static_cast<uint32_t>(channel_count), mfop::audio_bits_per_sample) };
		THROW_IF_FAILED(input_audio_media_type->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, block_alignment));
		THROW_IF_FAILED(input_audio_media_type->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, block_alignment * sampling_rate));
		THROW_IF_FAILED(input_audio_media_type->SetUINT32(MF_MT_AVG_BITRATE, sampling_rate * mfop::audio_bits_per_sample * channel_count));
		THROW_IF_FAILED(input_audio_media_type->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, true));
		THROW_IF_FAILED(input_audio_media_type->SetUINT32(MF_MT_FIXED_SIZE_SAMPLES, true));
	}
	return input_audio_media_type;
}
IMFMediaTypes const make_input_media_types(OUTPUT_INFO const *const &oip, GUID const &output_video_format, bool const &is_accelerated)
{
	return { make_input_video_media_type({ oip->w, oip->h }, { oip->rate, oip->scale }, is_accelerated), make_input_audio_media_type(oip->audio_ch, oip->audio_rate, output_video_format) };
}
auto write_sample_to_sink_writer(IMFSinkWriter *const sink_writer, DWORD const &index, IMFMediaBuffer *const buffer, int64_t const &time, int64_t const &duration)
{
	auto sample{ wil::com_ptr<IMFSample>{} };
	THROW_IF_FAILED(MFCreateSample(&sample));
	THROW_IF_FAILED(sample->AddBuffer(buffer));
	THROW_IF_FAILED(sample->SetSampleTime(time));
	THROW_IF_FAILED(sample->SetSampleDuration(duration));
	THROW_IF_FAILED(sink_writer->WriteSample(index, sample.get()));
}
[[nodiscard]] auto make_sink_writer(TCHAR const *const &output_name, bool const &is_accelerated, GUID const &output_video_format)
{
	auto sink_writer_attributes{ wil::com_ptr<IMFAttributes>{} };
	THROW_IF_FAILED(MFCreateAttributes(&sink_writer_attributes, 3));
	THROW_IF_FAILED(sink_writer_attributes->SetUINT32(MF_SINK_WRITER_DISABLE_THROTTLING, true));
	if (output_video_format == MFVideoFormat_H264)
	{
		// Fast-started MP4 requires FMPEG4, not MP4.
		// https://stackoverflow.com/a/52444686
		THROW_IF_FAILED(sink_writer_attributes->SetGUID(MF_TRANSCODE_CONTAINERTYPE, MFTranscodeContainerType_FMPEG4));
		THROW_IF_FAILED(sink_writer_attributes->SetUINT32(MF_MPEG4SINK_MOOV_BEFORE_MDAT, true));
	}

	if (is_accelerated)
	{
		GLOBAL::aviutl_logger->info(GLOBAL::aviutl_logger, _T("Preparing DirectX..."));
		auto directx_device{ wil::com_ptr<IUnknown>{} };
		if (is_dx12_available())
		{
			GLOBAL::aviutl_logger->info(GLOBAL::aviutl_logger, _T("DirectX 12 is available so going to be used."));
			THROW_IF_FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_1, __uuidof(ID3D12Device), directx_device.put_void()));
		}
		else
		{
			static auto const constinit d3d_feature_levels{ std::to_array({
				D3D_FEATURE_LEVEL_11_1,
				D3D_FEATURE_LEVEL_11_0,
				D3D_FEATURE_LEVEL_10_1,
				D3D_FEATURE_LEVEL_10_0,
				D3D_FEATURE_LEVEL_9_3,
				D3D_FEATURE_LEVEL_9_2,
				D3D_FEATURE_LEVEL_9_1
			}) };
			THROW_IF_FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_VIDEO_SUPPORT, d3d_feature_levels.data(), static_cast<uint32_t>(d3d_feature_levels.size()), D3D11_SDK_VERSION, reinterpret_cast<ID3D11Device **>(directx_device.put()), nullptr, nullptr));
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
[[nodiscard]] auto configure_video_output(IMFSinkWriter *const &sink_writer, resolution_t const &resolution, fps_t const &fps, GUID const &output_video_format)
{
	auto const [width, height] { resolution };
	auto const [rate, scale] { fps };

	auto output_video_media_type{ wil::com_ptr<IMFMediaType>{} };
	THROW_IF_FAILED(MFCreateMediaType(&output_video_media_type));

	THROW_IF_FAILED(output_video_media_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video));
	THROW_IF_FAILED(output_video_media_type->SetGUID(MF_MT_SUBTYPE, output_video_format));
	THROW_IF_FAILED(MFSetAttributeSize(output_video_media_type.get(), MF_MT_FRAME_SIZE, width, height));
	THROW_IF_FAILED(MFSetAttributeRatio(output_video_media_type.get(), MF_MT_FRAME_RATE, rate, scale));
	THROW_IF_FAILED(output_video_media_type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive));
	set_color_space_media_types(output_video_media_type.get(), height);

	switch (output_video_format.Data1)
	{
	case FCC('H264'):
		THROW_IF_FAILED(output_video_media_type->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_High));
		break;
	case FCC('HEVC'):
		THROW_IF_FAILED(output_video_media_type->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH265VProfile_Main_420_8));
		THROW_IF_FAILED(output_video_media_type->SetUINT32(MF_MT_MPEG2_LEVEL, eAVEncH265VLevel5_1));
		[[fallthrough]];
	default:
		THROW_IF_FAILED(output_video_media_type->SetUINT32(MF_MT_AVG_BITRATE, 12000000));
		break;
	}

	DWORD video_index{};
	THROW_IF_FAILED(sink_writer->AddStream(output_video_media_type.get(), &video_index));

	return video_index;
}
[[nodiscard]] auto configure_audio_output(IMFSinkWriter *const &sink_writer, int32_t const &channel_count, int32_t const &sampling_rate, uint32_t const &output_bit_rate, GUID const &output_video_format)
{
	__assume(output_bit_rate <= 3);

	auto output_audio_media_type{ wil::com_ptr<IMFMediaType>{} };
	THROW_IF_FAILED(MFCreateMediaType(&output_audio_media_type));

	THROW_IF_FAILED(output_audio_media_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio));
	THROW_IF_FAILED(output_audio_media_type->SetGUID(MF_MT_SUBTYPE, output_video_format == MFVideoFormat_WVC1 ? MFAudioFormat_WMAudioV9 : MFAudioFormat_AAC));
	THROW_IF_FAILED(output_audio_media_type->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, channel_count));
	THROW_IF_FAILED(output_audio_media_type->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, (3 + output_bit_rate) * 4000));
	THROW_IF_FAILED(output_audio_media_type->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, sampling_rate));
	THROW_IF_FAILED(output_audio_media_type->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, audio_bits_per_sample));

	DWORD audio_index{};
	THROW_IF_FAILED(sink_writer->AddStream(output_audio_media_type.get(), &audio_index));

	return audio_index;
}
auto configure_video_input(IMFSinkWriter *const &sink_writer, DWORD const &video_index, uint32_t const &quality, GUID const &output_video_format, IMFMediaType *const &input_video_media_type)
{
	__assume(quality <= 100);

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
		THROW_IF_FAILED(video_encoder_attributes->SetUINT32(CODECAPI_AVEncNumWorkerThreads, 0));
		break;
	case FCC('WVC1'):
		THROW_IF_FAILED(video_encoder_attributes->SetUINT32(MFPKEY_COMPRESSIONOPTIMIZATIONTYPE.fmtid, 1));
		add_windows_media_qvba_activation_media_attributes(video_encoder_attributes.get(), quality);
		break;
	}

	THROW_IF_FAILED(sink_writer->SetInputMediaType(video_index, input_video_media_type, video_encoder_attributes.get()));
}
auto configure_audio_input(IMFSinkWriter *const &sink_writer, DWORD const &audio_index, uint32_t const &quality, GUID const &output_video_format, IMFMediaType *const &input_audio_media_type)
{
	auto audio_encoder_attributes{ wil::com_ptr<IMFAttributes>{} };

	if (output_video_format == MFVideoFormat_WVC1)
	{
		THROW_IF_FAILED(MFCreateAttributes(&audio_encoder_attributes, 3));
		add_windows_media_qvba_activation_media_attributes(audio_encoder_attributes.get(), quality);
	}

	THROW_IF_FAILED(sink_writer->SetInputMediaType(audio_index, input_audio_media_type, audio_encoder_attributes.get()));
}
auto configure_video_stream(IMFSinkWriter *const &sink_writer, resolution_t &&resolution, fps_t &&fps, uint32_t const &quality, IMFMediaType *const input_media_type, GUID const &output_video_format)
{
	auto index{ configure_video_output(sink_writer, resolution, fps, output_video_format) };
	configure_video_input(sink_writer, index, quality, output_video_format, input_media_type);
	return index;
}
auto configure_audio_stream(IMFSinkWriter *const &sink_writer, int32_t const &channel_count, int32_t const &sampling_rate, int32_t const &output_bit_rate, uint32_t const &quality, IMFMediaType *const input_media_type, GUID const &output_video_format)
{
	auto index{ configure_audio_output(sink_writer, channel_count, sampling_rate, output_bit_rate, output_video_format) };
	configure_audio_input(sink_writer, index, quality, output_video_format, input_media_type);
	return index;
}
std::tuple<wil::com_ptr<IMFSinkWriter>, DWORD, DWORD> initialize_sink_writer(OUTPUT_INFO const *const &oip, bool const &is_accelerated, GUID const &output_video_format, IMFMediaType *const &input_video_media_type, IMFMediaType *const &input_audio_media_type)
{
	auto const sink_writer{ make_sink_writer(oip->savefile, is_accelerated, get_suitable_output_video_format_guid(std::filesystem::path(oip->savefile).extension(), GetPrivateProfileInt(_T("mp4"), _T("videoFormat"), 0, configuration_ini_path))) };

	auto const quality{ GetPrivateProfileInt(_T("mp4"), _T("videoQuality"), 70, mfop::configuration_ini_path) };

	auto video_index{ configure_video_stream(sink_writer.get(), { oip->w, oip->h }, { oip->rate, oip->scale }, quality, input_video_media_type, output_video_format) };
	auto audio_index{ configure_audio_stream(sink_writer.get(), oip->audio_ch, oip->audio_rate, GetPrivateProfileInt(_T("mp4"), _T("audioBitRate"), 3, mfop::configuration_ini_path), quality, input_audio_media_type, output_video_format) };

	THROW_IF_FAILED(sink_writer->BeginWriting());

	return { sink_writer, video_index, audio_index };
}
auto write_video_sample(OUTPUT_INFO const *const &oip, IMFSinkWriter *const &sink_writer, int32_t const &f, DWORD const &index, int64_t const &time_stamp, long const &default_stride, bool const &is_accelerated, IMFMediaType *const &input_media_type)
{
	if (oip->func_is_abort()) return false;

	oip->func_rest_time_disp(f, oip->n);

	auto frame_image{ static_cast<uint8_t *>(oip->func_get_video(f, FCC('YUY2'))) };

	auto video_buffer{ wil::com_ptr<IMFMediaBuffer>{} };
	THROW_IF_FAILED(MFCreateMediaBufferFromMediaType(input_media_type, time_stamp, 0, 0, &video_buffer));

	uint8_t *scanline{};
	long stride{};
	uint8_t *buffer_start{};
	DWORD buffer_length{};
	THROW_IF_FAILED(video_buffer.query<IMF2DBuffer2>()->Lock2DSize(MF2DBuffer_LockFlags_Write, &scanline, &stride, &buffer_start, &buffer_length));
	THROW_IF_FAILED(MFCopyImage(scanline, stride, is_accelerated ? yuy2_to_nv12(frame_image, { oip->w, oip->h }).get() : frame_image, default_stride, default_stride, oip->h));
	THROW_IF_FAILED(video_buffer.query<IMF2DBuffer2>()->Unlock2D());
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
auto write_audio_sample(OUTPUT_INFO const *const &oip, IMFSinkWriter *const &sink_writer, int32_t const &n, DWORD const &index, IMFMediaType *const &input_media_type)
{
	if (oip->func_is_abort()) return false;

	oip->func_rest_time_disp(n, oip->audio_n);

	// bytes per audio-frame (block align)
	auto const block_alignment{ get_pcm_block_alignment(static_cast<uint32_t>(oip->audio_ch), audio_bits_per_sample) }; // bytes per audio-frame
	auto const max_samples{ static_cast<int32_t>(block_alignment * oip->audio_rate) }; // bytes per second

	int32_t actual_samples{};
	auto audio_data{ oip->func_get_audio(n, max_samples, &actual_samples, WAVE_FORMAT_PCM) };
	if (!actual_samples) return true;

	// compute number of audio-frames (samples per channel) in buffer
	auto const sample_duration{ static_cast<int64_t>(actual_samples) * 10'000'000LL / max_samples }; // 100-ns units
	auto const sample_time{ static_cast<int64_t>(n) * 10'000'000LL / oip->audio_rate }; // 100-ns units for start
	if (n == 0) GLOBAL::aviutl_logger->info(GLOBAL::aviutl_logger, std::format(_T("audio: duration={}, time={}"), sample_duration, sample_time).c_str());

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

	auto const output_video_format{ get_suitable_output_video_format_guid(std::filesystem::path(oip->savefile).extension(), GetPrivateProfileInt(_T("mp4"), _T("videoFormat"), 0, mfop::configuration_ini_path)) };

	auto const is_accelerated{ GetPrivateProfileInt(_T("general"), _T("useHardware"), BST_UNCHECKED, mfop::configuration_ini_path) == BST_CHECKED };

	long default_stride{};
	THROW_IF_FAILED(MFGetStrideForBitmapInfoHeader(get_suitable_input_video_format_guid(is_accelerated).Data1, oip->w, &default_stride));

	auto const [input_video_media_type, input_audio_media_type] { make_input_media_types(oip, output_video_format, is_accelerated) };

	auto const [sink_writer, video_index, audio_index] { initialize_sink_writer(oip, is_accelerated, output_video_format, input_video_media_type.get(), input_audio_media_type.get()) };

	oip->func_set_buffer_size(16, 16);
	GLOBAL::aviutl_logger->info(GLOBAL::aviutl_logger, _T("Sending video samples to the writer..."));
	for (auto f{ 0 }; f < oip->n; ++f)
		if (!write_video_sample(oip, sink_writer.get(), f, video_index, time_stamp, default_stride, is_accelerated, input_video_media_type.get())) goto abort;
	GLOBAL::aviutl_logger->info(GLOBAL::aviutl_logger, _T("Sending audio samples to the writer..."));
	for (auto n{ 0 }; n < oip->audio_n; n += oip->audio_rate)
		if (!write_audio_sample(oip, sink_writer.get(), n, audio_index, input_audio_media_type.get())) goto abort;

	GLOBAL::aviutl_logger->info(GLOBAL::aviutl_logger, _T("Finalizing. It may take a while..."));
	sink_writer->Finalize();

abort:
	GLOBAL::aviutl_logger->info(GLOBAL::aviutl_logger, _T("Done."));
	return true;
}
intptr_t CALLBACK config_dialog_proc(HWND dialog, uint32_t message, WPARAM w_param, LPARAM)
{
	static TCHAR quality_wchar{};

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

		ComboBox_SetCurSel(GetDlgItem(dialog, IDC_COMBO2), GetPrivateProfileInt(_T("mp4"), _T("videoFormat"), 0, mfop::configuration_ini_path));

		GetPrivateProfileString(_T("mp4"), _T("videoQuality"), _T("70"), &quality_wchar, 3, mfop::configuration_ini_path);
		THROW_IF_WIN32_BOOL_FALSE(SetDlgItemText(dialog, IDC_EDIT1, &quality_wchar));
		ComboBox_SetCurSel(GetDlgItem(dialog, IDC_COMBO1), GetPrivateProfileInt(_T("mp4"), _T("audioBitRate"), 3, mfop::configuration_ini_path));

		Button_SetCheck(GetDlgItem(dialog, IDC_CHECK1), GetPrivateProfileInt(_T("general"), _T("useHardware"), BST_UNCHECKED, mfop::configuration_ini_path));

		return false;
	case WM_COMMAND:
		switch (LOWORD(w_param))
		{
		case IDNO:
			if (MessageBox(dialog, _T("全ての設定を初期化しますか？"), _T("設定値のリセット"), MB_YESNO | MB_ICONWARNING) == IDYES)
			{
				ComboBox_SetCurSel(GetDlgItem(dialog, IDC_COMBO2), 0);
				THROW_IF_WIN32_BOOL_FALSE(SetDlgItemText(dialog, IDC_EDIT1, _T("70")));
				ComboBox_SetCurSel(GetDlgItem(dialog, IDC_COMBO1), 3);
				Button_SetCheck(GetDlgItem(dialog, IDC_CHECK1), BST_UNCHECKED);
			}
			return false;
		case IDOK:
			quality = GetDlgItemInt(dialog, IDC_EDIT1, nullptr, false);
			if (quality > 100 || quality == 0)
			{
				MessageBox(dialog, _T("映像品質は1〜100の範囲で指定してください。"), nullptr, MB_OK | MB_ICONERROR);
				return false;
			}

			_ltot_s(static_cast<long>(ComboBox_GetCurSel(GetDlgItem(dialog, IDC_COMBO2))), video_format_wchar, std::size(video_format_wchar), 10);
			THROW_IF_WIN32_BOOL_FALSE(WritePrivateProfileString(_T("mp4"), _T("videoFormat"), video_format_wchar, mfop::configuration_ini_path));

			GetDlgItemTextW(dialog, IDC_EDIT1, &quality_wchar, 3);
			THROW_IF_WIN32_BOOL_FALSE(WritePrivateProfileString(_T("mp4"), _T("videoQuality"), &quality_wchar, mfop::configuration_ini_path));

			_ltot_s(static_cast<long>(ComboBox_GetCurSel(GetDlgItem(dialog, IDC_COMBO1))), audio_bit_rate_wchar, std::size(audio_bit_rate_wchar), 10);
			THROW_IF_WIN32_BOOL_FALSE(WritePrivateProfileString(_T("mp4"), _T("audioBitRate"), audio_bit_rate_wchar, mfop::configuration_ini_path));

			THROW_IF_WIN32_BOOL_FALSE(WritePrivateProfileString(_T("general"), _T("useHardware"), Button_GetCheck(GetDlgItem(dialog, IDC_CHECK1)) ? _T("1") : _T("0"), mfop::configuration_ini_path));
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
	DialogBox(instance, MAKEINTRESOURCE(IDD_DIALOG1), window, config_dialog_proc);
	return true;
}
auto wil_log_callback(wil::FailureInfo const &failure) noexcept
{
	switch (failure.type)
	{
		using enum wil::FailureType;

	case Exception:
		GLOBAL::aviutl_logger->error(GLOBAL::aviutl_logger, std::format(L"{}. At line {}. ({})", failure.pszMessage, failure.uLineNumber, failure.hr).c_str());
		break;
	case Log:
		GLOBAL::aviutl_logger->log(GLOBAL::aviutl_logger, failure.pszMessage);
		break;
	case Return:
	case FailFast:
	default:
		break;
	}
}
auto APIENTRY DllMain(HMODULE handle, DWORD reason, void *reserved) noexcept
{
	wil::DLLMain(handle, reason, reserved);
	return true;
}
extern "C"
{
	__declspec(dllexport) auto InitializeLogger(LOG_HANDLE *logger) noexcept
	{
		GLOBAL::aviutl_logger = logger;
		wil::SetResultLoggingCallback(wil_log_callback);
	}
	__declspec(dllexport) auto GetOutputPluginTable() noexcept
	{
		static auto constexpr output_plugin_table{ OUTPUT_PLUGIN_TABLE{
			OUTPUT_PLUGIN_TABLE::FLAG_VIDEO | OUTPUT_PLUGIN_TABLE::FLAG_AUDIO, //	フラグ
			_T("Media Foundation 出力"),					// プラグインの名前
			_T("MP4 (*.mp4)\0*.mp4\0Advanced Systems Format (*.wmv)\0*.wmv\0"),					// 出力ファイルのフィルタ
			_T("MFOutput (") _T(__DATE__) _T(") by MonogoiNoobs"),	// プラグインの情報
			func_output,									// 出力時に呼ばれる関数へのポインタ
			func_config,									// 出力設定のダイアログを要求された時に呼ばれる関数へのポインタ (nullptrなら呼ばれません)
			nullptr,							// 出力設定のテキスト情報を取得する時に呼ばれる関数へのポインタ (nullptrなら呼ばれません)
		} };
		return &output_plugin_table;
	}
}
}

#pragma warning(pop)
