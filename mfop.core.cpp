/**
 * \copyright	SPDX-License-Identifier: MIT
 * \year		2025-2026
 * \author		Shion Yorigami <62567343+MonogoiNoobs@users.noreply.github.com>
 */

module;

#define STRICT
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <wil/com.h>
#include <d3d11_4.h>
#include <mfd3d12.h>
#include <mfapi.h>
#include <mfreadwrite.h>
#include <codecapi.h>
#include <wmcodecdsp.h>
#include "aviutl2_sdk/output2.h"
#include "aviutl2_sdk/logger2.h"

#pragma comment(lib, "Mfplat")
#pragma comment(lib, "Mfreadwrite")
#pragma comment(lib, "Mfuuid")
#pragma comment(lib, "d3d11")
#pragma comment(lib, "d3d12")

#pragma warning(default: 4557 5266)

#define UNEXPECT_IF_FAILED(hr) do { \
	HRESULT const __mfop_cond{ hr }; \
	if (FAILED(__mfop_cond)) \
	{ \
		return std::unexpected \
		{ \
			mfop::error \
			{ \
				.code = __mfop_cond, \
				.where = __func__ \
			} \
		}; \
	} \
} while (0)

module mfop.core;

import std;

using namespace std;
using namespace wil;

static LOG_HANDLE *aviutl_logger;

namespace mfop
{
	auto const constinit audio_bits_per_sample{ 16 };

	using nv12_ptr = unique_ptr<uint8_t[]>;
	using resolution_t = pair<int32_t const, int32_t const>;
	using fps_t = pair<int32_t const, int32_t const>;
	using stream_indices_t = pair<DWORD const, DWORD const>;
	using IMFMediaTypes = pair<com_ptr<IMFMediaType>, com_ptr<IMFMediaType>>;
	using sink_writer_with_indices_t = pair<com_ptr<IMFSinkWriter>, stream_indices_t const>;

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

	auto constexpr get_pcm_block_alignment(int32_t const &audio_ch, uint32_t &&bit) noexcept
	{
		return (audio_ch * bit) / 8;
	}

	auto constexpr get_suitable_input_video_format_guid(bool const &is_accelerated) noexcept
	{
		return is_accelerated ? MFVideoFormat_NV12 : MFVideoFormat_YUY2;
	}

	auto convert_frame_rate_to_average_time_per_frame(IMFMediaType &media_type) noexcept
	{
		uint32_t rate{}, scale{};
		MFGetAttributeRatio(&media_type, MF_MT_FRAME_RATE, &rate, &scale);
		uint64_t result{};
		MFFrameRateToAverageTimePerFrame(rate, scale, &result);
		return result;
	}

	auto get_suitable_output_video_format_guid(filesystem::path &&extension, bool const &is_hevc_preferable) noexcept
	{
		if (extension == L".mp4") return is_hevc_preferable ? MFVideoFormat_HEVC : MFVideoFormat_H264;
		if (extension == L".wmv") return MFVideoFormat_WVC1;
		return MFVideoFormat_H264;
	}

	auto add_windows_media_qvba_activation_media_attributes(IMFAttributes &attributes, uint32_t const &quality) noexcept
	{
		attributes.SetUINT32(MFPKEY_VBRENABLED.fmtid, true);
		attributes.SetUINT32(MFPKEY_CONSTRAIN_ENUMERATED_VBRQUALITY.fmtid, true);
		attributes.SetUINT32(MFPKEY_DESIRED_VBRQUALITY.fmtid, quality);
	}

	auto set_color_space_media_types(IMFMediaType &media_type, int32_t const &height) noexcept
	{
		media_type.SetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, MFNominalRange_16_235);
		media_type.SetUINT32(MF_MT_VIDEO_CHROMA_SITING, MFVideoChromaSubsampling_MPEG2);
		if (height <= 720)
		{
			aviutl_logger->info(aviutl_logger, L"Color space desires BT.601.");
			media_type.SetUINT32(MF_MT_VIDEO_PRIMARIES, MFVideoPrimaries_SMPTE170M);
			media_type.SetUINT32(MF_MT_YUV_MATRIX, MFVideoTransferMatrix_BT601);
			media_type.SetUINT32(MF_MT_TRANSFER_FUNCTION, MFVideoTransFunc_709);
		}
		else if (height >= 2160)
		{
			aviutl_logger->info(aviutl_logger, L"Color space desires BT.2020 (12bit).");
			media_type.SetUINT32(MF_MT_VIDEO_PRIMARIES, MFVideoPrimaries_BT2020);
			media_type.SetUINT32(MF_MT_YUV_MATRIX, MFVideoTransferMatrix_BT2020_12);
			media_type.SetUINT32(MF_MT_TRANSFER_FUNCTION, MFVideoTransFunc_2020);
		}
		else
		{
			aviutl_logger->info(aviutl_logger, L"Color space desires BT.709.");
			media_type.SetUINT32(MF_MT_VIDEO_PRIMARIES, MFVideoPrimaries_BT709);
			media_type.SetUINT32(MF_MT_YUV_MATRIX, MFVideoTransferMatrix_BT709);
			media_type.SetUINT32(MF_MT_TRANSFER_FUNCTION, MFVideoTransFunc_709);
		}
	}

	auto make_input_video_media_type(resolution_t &&resolution, fps_t &&fps, bool const &is_accelerated) noexcept
	{
		auto const [width, height] { resolution };
		auto const [rate, scale] { fps };

		auto const video_format{ get_suitable_input_video_format_guid(is_accelerated) };

		uint32_t image_size{};
		MFCalculateImageSize(video_format, width, height, &image_size);

		long default_stride{};
		MFGetStrideForBitmapInfoHeader(video_format.Data1, width, &default_stride);

		com_ptr_nothrow<IMFMediaType> input_video_media_type{};
		MFCreateMediaType(out_ptr(input_video_media_type));
		input_video_media_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
		input_video_media_type->SetGUID(MF_MT_SUBTYPE, video_format);
		input_video_media_type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
		input_video_media_type->SetUINT32(MF_MT_DEFAULT_STRIDE, default_stride);
		input_video_media_type->SetUINT32(MF_MT_SAMPLE_SIZE, image_size);
		input_video_media_type->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, true);
		MFSetAttributeSize(input_video_media_type.get(), MF_MT_FRAME_SIZE, width, height);
		MFSetAttributeRatio(input_video_media_type.get(), MF_MT_FRAME_RATE, rate, scale);
		MFSetAttributeRatio(input_video_media_type.get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);

		if (is_accelerated) input_video_media_type->SetUINT32(MF_MT_D3D_RESOURCE_VERSION, MF_D3D11_RESOURCE);

		return input_video_media_type;
	}

	auto make_input_audio_media_type(int32_t const &channel_count, int32_t const &sampling_rate, GUID const &output_video_format)
	{
		com_ptr_nothrow<IMFMediaType> input_audio_media_type{};
		MFCreateMediaType(out_ptr(input_audio_media_type));
		input_audio_media_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
		input_audio_media_type->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
		input_audio_media_type->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, audio_bits_per_sample);
		input_audio_media_type->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, sampling_rate);
		input_audio_media_type->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, channel_count);
		input_audio_media_type->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, true);

		if (output_video_format == MFVideoFormat_WVC1)
		{
			auto const block_alignment{ get_pcm_block_alignment(channel_count, audio_bits_per_sample) };
			input_audio_media_type->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, block_alignment);
			input_audio_media_type->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, block_alignment * sampling_rate);
			input_audio_media_type->SetUINT32(MF_MT_AVG_BITRATE, sampling_rate * audio_bits_per_sample * channel_count);
		}
		return input_audio_media_type;
	}

	auto make_input_media_types(OUTPUT_INFO const &oip, GUID const &output_video_format, bool const &is_accelerated) noexcept
	{
		return IMFMediaTypes
		{
			make_input_video_media_type({ oip.w, oip.h }, { oip.rate, oip.scale }, is_accelerated),
			make_input_audio_media_type(oip.audio_ch, oip.audio_rate, output_video_format)
		};
	}

	auto write_sample_to_sink_writer(IMFSinkWriter &sink_writer, DWORD const &index, IMFMediaBuffer &buffer, int64_t const &time, int64_t const &duration) noexcept
	{
		auto sample{ com_ptr_nothrow<IMFSample>{} };
		RETURN_IF_FAILED(MFCreateSample(out_ptr(sample)));
		RETURN_IF_FAILED(sample->AddBuffer(&buffer));
		RETURN_IF_FAILED(sample->SetSampleTime(time));
		RETURN_IF_FAILED(sample->SetSampleDuration(duration));
		RETURN_IF_FAILED(sink_writer.WriteSample(index, sample.get()));

		return S_OK;
	}

	expected<com_ptr_nothrow<IMFDXGIDeviceManager>, error> make_dxgi_device_manager_ptr() noexcept
	{
		static auto const constinit d3d_feature_levels{ to_array(
		{
			D3D_FEATURE_LEVEL_11_1,
			D3D_FEATURE_LEVEL_11_0,
			D3D_FEATURE_LEVEL_10_1,
			D3D_FEATURE_LEVEL_10_0,
			D3D_FEATURE_LEVEL_9_3,
			D3D_FEATURE_LEVEL_9_2,
			D3D_FEATURE_LEVEL_9_1
		}) };

		aviutl_logger->info(aviutl_logger, L"Preparing DirectX Video Acceleration...");
		com_ptr_nothrow<ID3D11Device> directx_device{};

		UNEXPECT_IF_FAILED(D3D11CreateDevice
		(
			nullptr,
			D3D_DRIVER_TYPE_HARDWARE,
			nullptr,
			D3D11_CREATE_DEVICE_VIDEO_SUPPORT,
			d3d_feature_levels.data(),
			static_cast<uint32_t>(d3d_feature_levels.size()),
			D3D11_SDK_VERSION,
			&directx_device,
			nullptr,
			nullptr
		));

		com_ptr_nothrow<IMFDXGIDeviceManager> dxgi_device_manager{};
		uint32_t reset_token{};
		MFCreateDXGIDeviceManager(&reset_token, out_ptr(dxgi_device_manager));
		UNEXPECT_IF_FAILED(dxgi_device_manager->ResetDevice(directx_device.get(), reset_token));

		return dxgi_device_manager;
	}

	expected<com_ptr_nothrow<IMFSinkWriter>, error> make_sink_writer(wstring_view output_name, IMFAttributes &media_type, GUID const &output_video_format) noexcept
	{
		auto sink_writer_attributes{ com_ptr_nothrow<IMFAttributes>{} };
		MFCreateAttributes(out_ptr(sink_writer_attributes), 3);

		sink_writer_attributes->SetUINT32(MF_SINK_WRITER_DISABLE_THROTTLING, true);

		if (output_video_format == MFVideoFormat_H264)
		{
			sink_writer_attributes->SetGUID(MF_TRANSCODE_CONTAINERTYPE, MFTranscodeContainerType_FMPEG4);
			sink_writer_attributes->SetUINT32(MF_MPEG4SINK_MOOV_BEFORE_MDAT, true);
		}

		uint32_t d3d_resource_version{};
		if (SUCCEEDED(media_type.GetUINT32(MF_MT_D3D_RESOURCE_VERSION, &d3d_resource_version)))
		{
			auto const d3d_manager{ make_dxgi_device_manager_ptr() };
			if (!d3d_manager) return unexpected{ d3d_manager.error() };

			sink_writer_attributes->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, true);
			sink_writer_attributes->SetUnknown(MF_SINK_WRITER_D3D_MANAGER, d3d_manager->get());
		}

		auto sink_writer{ com_ptr_nothrow<IMFSinkWriter>{} };
		UNEXPECT_IF_FAILED(MFCreateSinkWriterFromURL(output_name.data(), nullptr, sink_writer_attributes.get(), out_ptr(sink_writer)));

		return sink_writer;
	}

	expected<DWORD, error> configure_video_output(IMFSinkWriter &sink_writer, IMFMediaType &input_media_type, GUID const &output_video_format) noexcept
	{
		uint32_t width, height;
		MFGetAttributeSize(&input_media_type, MF_MT_FRAME_SIZE, &width, &height);

		uint32_t rate, scale;
		MFGetAttributeRatio(&input_media_type, MF_MT_FRAME_RATE, &rate, &scale);

		auto output_video_media_type{ com_ptr_nothrow<IMFMediaType>{} };
		MFCreateMediaType(out_ptr(output_video_media_type));

		output_video_media_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
		output_video_media_type->SetGUID(MF_MT_SUBTYPE, output_video_format);
		MFSetAttributeSize(output_video_media_type.get(), MF_MT_FRAME_SIZE, width, height);
		MFSetAttributeRatio(output_video_media_type.get(), MF_MT_FRAME_RATE, rate, scale);
		output_video_media_type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
		set_color_space_media_types(*output_video_media_type, height);

		switch (output_video_format.Data1)
		{
		case FCC('H264'):
			output_video_media_type->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_High);
			break;
		case FCC('HEVC'):
			output_video_media_type->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH265VProfile_Main_420_8);
			output_video_media_type->SetUINT32(MF_MT_MPEG2_LEVEL, eAVEncH265VLevel5_1);
			[[fallthrough]];
		default:
			output_video_media_type->SetUINT32(MF_MT_AVG_BITRATE, 12000000);
			break;
		}

		DWORD video_index{};
		UNEXPECT_IF_FAILED(sink_writer.AddStream(output_video_media_type.get(), &video_index));

		return video_index;
	}

	expected<DWORD, error> configure_audio_output(IMFSinkWriter &sink_writer, IMFMediaType &input_media_type, uint32_t const &output_bit_rate, GUID const &output_video_format) noexcept
	{
		__assume(output_bit_rate <= 3);

		auto output_audio_media_type{ com_ptr_nothrow<IMFMediaType>{} };
		MFCreateMediaType(out_ptr(output_audio_media_type));

		output_audio_media_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
		output_audio_media_type->SetGUID(MF_MT_SUBTYPE, output_video_format == MFVideoFormat_WVC1 ? MFAudioFormat_WMAudioV9 : MFAudioFormat_AAC);
		output_audio_media_type->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, MFGetAttributeUINT32(&input_media_type, MF_MT_AUDIO_NUM_CHANNELS, 2));
		output_audio_media_type->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, (3 + output_bit_rate) * 4000);
		output_audio_media_type->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, MFGetAttributeUINT32(&input_media_type, MF_MT_AUDIO_SAMPLES_PER_SECOND, 48000));
		output_audio_media_type->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, audio_bits_per_sample);

		DWORD audio_index{};
		UNEXPECT_IF_FAILED(sink_writer.AddStream(output_audio_media_type.get(), &audio_index));

		return audio_index;
	}

	expected<HRESULT, error> configure_video_input(IMFSinkWriter &sink_writer, DWORD const &index, uint32_t const &quality, GUID const &output_video_format, IMFMediaType &input_media_type) noexcept
	{
		__assume(quality <= 100);

		auto encoder_attributes{ com_ptr_nothrow<IMFAttributes>{} };
		MFCreateAttributes(out_ptr(encoder_attributes), 5);

		switch (output_video_format.Data1)
		{
		default:
		case FCC('H264'):
			encoder_attributes->SetUINT32(CODECAPI_AVEncH264CABACEnable, true);
			[[fallthrough]];
		case FCC('HEVC'):
			encoder_attributes->SetUINT32(CODECAPI_AVEncMPVDefaultBPictureCount, 2);
			encoder_attributes->SetUINT32(CODECAPI_AVEncCommonRateControlMode, eAVEncCommonRateControlMode_Quality);
			encoder_attributes->SetUINT32(CODECAPI_AVEncCommonQuality, quality);
			encoder_attributes->SetUINT32(CODECAPI_AVEncNumWorkerThreads, 0);
			break;
		case FCC('WVC1'):
			encoder_attributes->SetUINT32(MFPKEY_COMPRESSIONOPTIMIZATIONTYPE.fmtid, 1);
			add_windows_media_qvba_activation_media_attributes(*encoder_attributes, quality);
			break;
		}

		UNEXPECT_IF_FAILED(sink_writer.SetInputMediaType(index, &input_media_type, encoder_attributes.get()));

		return S_OK;
	}

	expected<HRESULT, error> configure_audio_input(IMFSinkWriter &sink_writer, DWORD const &index, uint32_t const &quality, GUID const &output_video_format, IMFMediaType &input_media_type) noexcept
	{
		auto encoder_attributes{ com_ptr_nothrow<IMFAttributes>{} };

		if (output_video_format == MFVideoFormat_WVC1)
		{
			MFCreateAttributes(out_ptr(encoder_attributes), 3);
			add_windows_media_qvba_activation_media_attributes(*encoder_attributes, quality);
		}

		UNEXPECT_IF_FAILED(sink_writer.SetInputMediaType(index, &input_media_type, encoder_attributes.get()));

		return S_OK;
	}

	expected<DWORD, error> configure_video_stream(IMFSinkWriter &sink_writer, uint32_t const &quality, IMFMediaType &input_media_type, GUID const &output_video_format) noexcept
	{
		auto const index{ configure_video_output(sink_writer, input_media_type, output_video_format) };
		if (!index) return unexpected{ index.error() };
		auto const result{ configure_video_input(sink_writer, *index, quality, output_video_format, input_media_type) };
		if (!result) return unexpected{ result.error() };
		return *index;
	}

	expected<DWORD, error> configure_audio_stream(IMFSinkWriter &sink_writer, int32_t const &output_bit_rate, uint32_t const &quality, IMFMediaType &input_media_type, GUID const &output_video_format) noexcept
	{
		auto const index{ configure_audio_output(sink_writer, input_media_type, output_bit_rate, output_video_format) };
		if (!index) return unexpected{ index.error() };
		auto const result{ configure_audio_input(sink_writer, *index, quality, output_video_format, input_media_type) };
		if (!result) return unexpected{ result.error() };
		return *index;
	}

	expected<stream_indices_t, error> configure_streams(IMFSinkWriter &sink_writer, uint32_t const &quality, uint32_t const &output_bit_rate, IMFMediaTypes const &input_media_types, GUID const &output_video_format) noexcept
	{
		auto const video_index{ configure_video_stream(sink_writer, quality, *input_media_types.first, output_video_format) };
		if (!video_index) return unexpected{ video_index.error() };

		auto const audio_index{ configure_audio_stream(sink_writer, output_bit_rate, quality, *input_media_types.second, output_video_format) };
		if (!audio_index) return unexpected{ audio_index.error() };

		return stream_indices_t{ move(*video_index), move(*audio_index) };
	}

	expected<sink_writer_with_indices_t, error> make_initialized_sink_writer(OUTPUT_INFO const &oip, GUID const &output_video_format, uint32_t const &video_quality, uint32_t const &audio_bit_rate, IMFMediaTypes const &media_types) noexcept
	{
		auto const sink_writer{ make_sink_writer(oip.savefile, *media_types.first, output_video_format) };
		if (!sink_writer) return unexpected{ sink_writer.error() };
		auto const indices{ configure_streams(**sink_writer, video_quality, audio_bit_rate, media_types, output_video_format) };
		if (!indices) return unexpected{ indices.error() };

		UNEXPECT_IF_FAILED((*sink_writer)->BeginWriting());

		return sink_writer_with_indices_t{ move(*sink_writer), move(*indices) };
	}

	auto write_video_sample(OUTPUT_INFO const &oip, IMFSinkWriter &sink_writer, int32_t const &f, DWORD const &index, IMFMediaType &input_media_type, bool const &is_accelerated, int64_t const &time_stamp) noexcept
	{
		if (oip.func_is_abort()) return E_ABORT;

		oip.func_rest_time_disp(f, oip.n);

		auto const frame_image{ static_cast<uint8_t *>(oip.func_get_video(f, FCC('YUY2'))) };

		com_ptr_nothrow<IMFMediaBuffer> video_buffer{};
		RETURN_IF_FAILED(MFCreateMediaBufferFromMediaType(&input_media_type, time_stamp, 0, 0, out_ptr(video_buffer)));

		com_ptr_nothrow<IMF2DBuffer2> video_2d_buffer{};
		RETURN_IF_FAILED(video_buffer.query_to(&video_2d_buffer));

		uint8_t *scanline{};
		long stride{};
		uint8_t *buffer_begin{};
		DWORD buffer_size{};
		video_2d_buffer->Lock2DSize(MF2DBuffer_LockFlags_Write, &scanline, &stride, &buffer_begin, &buffer_size);
		RETURN_IF_FAILED(MFCopyImage(scanline, stride, is_accelerated ? yuy2_to_nv12(frame_image, { oip.w, oip.h }).get() : frame_image, stride, stride, oip.h));
		video_2d_buffer->Unlock2D();

		DWORD contiguous_length{};
		video_2d_buffer->GetContiguousLength(&contiguous_length);
		video_buffer->SetCurrentLength(contiguous_length);

		RETURN_IF_FAILED(write_sample_to_sink_writer(sink_writer, index, *video_buffer, time_stamp * f, time_stamp));

		return S_OK;
	}

	auto write_audio_sample(OUTPUT_INFO const &oip, IMFSinkWriter &sink_writer, int32_t const &n, DWORD const &index, IMFMediaType &input_media_type, int32_t const &max_samples) noexcept
	{
		if (oip.func_is_abort()) return E_ABORT;

		oip.func_rest_time_disp(n, oip.audio_n);

		int32_t actual_samples{};
		auto const audio_data{ oip.func_get_audio(n, max_samples, &actual_samples, WAVE_FORMAT_PCM) };
		if (!actual_samples) return S_FALSE;

		auto const sample_duration{ static_cast<int64_t>(actual_samples) * 10'000'000LL / max_samples };
		auto const sample_time{ static_cast<int64_t>(n) * 10'000'000LL / oip.audio_rate };

		com_ptr_nothrow<IMFMediaBuffer> audio_buffer{};
		RETURN_IF_FAILED(MFCreateMediaBufferFromMediaType(&input_media_type, sample_duration, static_cast<DWORD>(actual_samples), 0, out_ptr(audio_buffer)));

		uint8_t *media_data{};
		DWORD media_data_max_length{};
		audio_buffer->Lock(&media_data, &media_data_max_length, nullptr);
		if (memmove_s(media_data, media_data_max_length, audio_data, static_cast<size_t>(actual_samples)) != 0) return E_OUTOFMEMORY;
		audio_buffer->Unlock();

		audio_buffer->SetCurrentLength(static_cast<DWORD>(actual_samples));

		RETURN_IF_FAILED(write_sample_to_sink_writer(sink_writer, index, *audio_buffer, sample_time, sample_duration));

		return S_OK;
	}

	using unique_mfshutdown_call = unique_call<decltype(&::MFShutdown), ::MFShutdown>;
	[[nodiscard]] inline unique_mfshutdown_call MFStartup(DWORD &&flags = MFSTARTUP_FULL)
	{
		FAIL_FAST_IF_FAILED(::MFStartup(MF_VERSION, flags));
		return {};
	}

	expected<HRESULT, error> output_file(OUTPUT_INFO const &oip, output_configuration &&configuration, LOG_HANDLE &logger)
	{
		auto hr{ S_OK };

		auto const com_cleanup{ CoInitializeEx_failfast() };
		auto const mf_cleanup{ MFStartup() };

		aviutl_logger = &logger;

		auto const output_video_format{ get_suitable_output_video_format_guid(filesystem::path(oip.savefile).extension(), configuration.is_hevc_preferable) };

		auto const input_media_types{ make_input_media_types(oip, output_video_format, configuration.is_accelerated) };

		auto sink_writer_with_indices{ make_initialized_sink_writer(oip, output_video_format, configuration.video_quality, configuration.audio_bit_rate, input_media_types) };
		if (!sink_writer_with_indices) return unexpected{ sink_writer_with_indices.error() };

		auto const [sink_writer, indices] { *sink_writer_with_indices };
		auto const [video_index, audio_index] { indices };

		oip.func_set_buffer_size(8, 8);

		aviutl_logger->info(aviutl_logger, L"Sending video samples to the writer...");

		uint32_t d3d_resource_version{};
		auto const is_accelerated{ SUCCEEDED(input_media_types.first->GetUINT32(MF_MT_D3D_RESOURCE_VERSION, &d3d_resource_version)) };

		auto const video_time_stamp{ convert_frame_rate_to_average_time_per_frame(*input_media_types.first) };

		for (auto f{ 0 }; f < oip.n; ++f)
			if ((hr = write_video_sample(oip, *sink_writer, f, video_index, *input_media_types.first, is_accelerated, video_time_stamp)) < 0)
				return unexpected{ error{ hr, "write_video_sample" } };

		aviutl_logger->info(aviutl_logger, L"Sending audio samples to the writer...");

		auto const audio_max_samples{ static_cast<int32_t>(get_pcm_block_alignment(oip.audio_ch, audio_bits_per_sample) * oip.audio_rate) };

		for (auto n{ 0 }; n < oip.audio_n; n += oip.audio_rate)
			if ((hr = write_audio_sample(oip, *sink_writer, n, audio_index, *input_media_types.second, audio_max_samples)) < 0)
				return unexpected{ error{ hr, "write_audio_sample" } };

		aviutl_logger->info(aviutl_logger, L"Finalizing. It may take a while...");
		if ((hr = sink_writer->Finalize()) < 0)
			return unexpected{ error{ hr, "Finalize" } };

		return S_OK;
	}
}