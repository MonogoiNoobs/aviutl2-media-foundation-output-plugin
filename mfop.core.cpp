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
	if (FAILED(__mfop_cond)) return std::unexpected{ mfop_cond }; \
} while (0)

module mfop.core;

import std;

using namespace std;
using namespace wil;

static LOG_HANDLE *aviutl_logger;

namespace mfop
{
	auto const constinit audio_bits_per_sample{ 16 };
	auto const constinit d3d_feature_levels{ to_array({
		D3D_FEATURE_LEVEL_11_1,
		D3D_FEATURE_LEVEL_11_0,
		D3D_FEATURE_LEVEL_10_1,
		D3D_FEATURE_LEVEL_10_0,
		D3D_FEATURE_LEVEL_9_3,
		D3D_FEATURE_LEVEL_9_2,
		D3D_FEATURE_LEVEL_9_1
	}) };

	using nv12_ptr = unique_ptr<uint8_t[]>;
	using resolution_t = pair<int32_t const, int32_t const>;
	using fps_t = pair<int32_t const, int32_t const>;
	using stream_indices_t = pair<DWORD const, DWORD const>;
	using IMFMediaTypes = pair<com_ptr<IMFMediaType>, com_ptr<IMFMediaType>>;
	using sink_writer_with_indices_t = pair<com_ptr<IMFSinkWriter>, stream_indices_t const>;
	template<typename Expected> using expected_win32 = expected<Expected, HRESULT>;

	auto yuy2_to_nv12(span<uint8_t> yuy2, resolution_t &&resolution)
	{
		__assume(resolution.first % 2 == 0 && resolution.second % 2 == 0);

		auto const [width, height] { resolution };

		nv12_ptr output{ make_unique_for_overwrite<uint8_t[]>(static_cast<size_t>(3 * width * height) / 2) };

		auto const stride{ width * 2 };
		auto const image_size{ static_cast<size_t>(width * height) };

#pragma omp parallel
		{
#pragma omp for nowait
			for (auto i{ 0 }; i < yuy2.size_bytes(); i += 2)
				_mm256_store_si256
				(
					reinterpret_cast<__m256i *>(&output[i / 2]),
					_mm256_load_si256(reinterpret_cast<__m256i const *>(&yuy2[i]))
				);

			for (auto current_height{ 0uz }; current_height < height; current_height += 2)
#pragma omp for nowait
				for (auto i{ 0 }; i < stride; i += 4)
					_mm256_store_si256(reinterpret_cast<__m256i *>(&output[image_size + (width * current_height / 2) + 0]), _mm256_load_si256(reinterpret_cast<__m256i const *>(&yuy2[stride * current_height + i + 1]))),
					_mm256_store_si256(reinterpret_cast<__m256i *>(&output[image_size + (width * current_height / 2) + 1]), _mm256_load_si256(reinterpret_cast<__m256i const *>(&yuy2[stride * current_height + i + 3])));
		}

		return output;
	}

	template<typename D3D12DeviceVersion = ID3D12Device>
	auto is_dx12_available(D3D_FEATURE_LEVEL &&minimum_feature_level = D3D_FEATURE_LEVEL_11_1) noexcept
	{
		return SUCCEEDED(D3D12CreateDevice(nullptr, minimum_feature_level, __uuidof(D3D12DeviceVersion), nullptr));
	}

	auto constexpr get_pcm_block_alignment(uint32_t &&audio_ch, uint32_t &&bit) noexcept
	{
		return (audio_ch * bit) / 8;
	}

	auto constexpr get_suitable_input_video_format_guid(bool const &is_accelerated) noexcept
	{
		return is_accelerated ? MFVideoFormat_NV12 : MFVideoFormat_YUY2;
	}

	auto convert_frame_rate_to_average_time_per_frame(IMFMediaType &media_type)
	{
		uint32_t rate{}, scale{};
		THROW_IF_FAILED(MFGetAttributeRatio(&media_type, MF_MT_FRAME_RATE, &rate, &scale));
		uint64_t result{};
		THROW_IF_FAILED(MFFrameRateToAverageTimePerFrame(rate, scale, &result));
		return result;
	}

	auto get_suitable_output_video_format_guid(filesystem::path &&extension, bool const &is_hevc_preferable) noexcept
	{
		if (extension == L".mp4") return is_hevc_preferable ? MFVideoFormat_HEVC : MFVideoFormat_H264;
		if (extension == L".wmv") return MFVideoFormat_WVC1;
		return MFVideoFormat_H264;
	}

	auto add_windows_media_qvba_activation_media_attributes(IMFAttributes &attributes, uint32_t const &quality)
	{
		THROW_IF_FAILED(attributes.SetUINT32(MFPKEY_VBRENABLED.fmtid, true));
		THROW_IF_FAILED(attributes.SetUINT32(MFPKEY_CONSTRAIN_ENUMERATED_VBRQUALITY.fmtid, true));
		THROW_IF_FAILED(attributes.SetUINT32(MFPKEY_DESIRED_VBRQUALITY.fmtid, quality));
	}

	auto set_color_space_media_types(IMFMediaType &media_type, int32_t const &height)
	{
		THROW_IF_FAILED(media_type.SetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, MFNominalRange_16_235));
		THROW_IF_FAILED(media_type.SetUINT32(MF_MT_VIDEO_CHROMA_SITING, MFVideoChromaSubsampling_MPEG2));
		if (height <= 720)
		{
			aviutl_logger->info(aviutl_logger, L"Color space desires BT.601.");
			THROW_IF_FAILED(media_type.SetUINT32(MF_MT_VIDEO_PRIMARIES, MFVideoPrimaries_SMPTE170M));
			THROW_IF_FAILED(media_type.SetUINT32(MF_MT_YUV_MATRIX, MFVideoTransferMatrix_BT601));
			THROW_IF_FAILED(media_type.SetUINT32(MF_MT_TRANSFER_FUNCTION, MFVideoTransFunc_709));
		}
		else if (height >= 2160)
		{
			aviutl_logger->info(aviutl_logger, L"Color space desires BT.2020 (12bit).");
			THROW_IF_FAILED(media_type.SetUINT32(MF_MT_VIDEO_PRIMARIES, MFVideoPrimaries_BT2020));
			THROW_IF_FAILED(media_type.SetUINT32(MF_MT_YUV_MATRIX, MFVideoTransferMatrix_BT2020_12));
			THROW_IF_FAILED(media_type.SetUINT32(MF_MT_TRANSFER_FUNCTION, MFVideoTransFunc_2020));
		}
		else
		{
			aviutl_logger->info(aviutl_logger, L"Color space desires BT.709.");
			THROW_IF_FAILED(media_type.SetUINT32(MF_MT_VIDEO_PRIMARIES, MFVideoPrimaries_BT709));
			THROW_IF_FAILED(media_type.SetUINT32(MF_MT_YUV_MATRIX, MFVideoTransferMatrix_BT709));
			THROW_IF_FAILED(media_type.SetUINT32(MF_MT_TRANSFER_FUNCTION, MFVideoTransFunc_709));
		}
	}

	auto make_input_video_media_type(resolution_t &&resolution, fps_t &&fps, bool const &is_accelerated)
	{
		auto const [width, height] { resolution };
		auto const [rate, scale] { fps };

		auto const video_format{ get_suitable_input_video_format_guid(is_accelerated) };

		uint32_t image_size{};
		THROW_IF_FAILED(MFCalculateImageSize(video_format, width, height, &image_size));

		long default_stride{};
		THROW_IF_FAILED(MFGetStrideForBitmapInfoHeader(video_format.Data1, width, &default_stride));

		auto input_video_media_type{ com_ptr<IMFMediaType>{} };
		THROW_IF_FAILED(MFCreateMediaType(out_ptr(input_video_media_type)));
		THROW_IF_FAILED(input_video_media_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video));
		THROW_IF_FAILED(input_video_media_type->SetGUID(MF_MT_SUBTYPE, video_format));
		THROW_IF_FAILED(input_video_media_type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive));
		THROW_IF_FAILED(input_video_media_type->SetUINT32(MF_MT_DEFAULT_STRIDE, default_stride));
		THROW_IF_FAILED(input_video_media_type->SetUINT32(MF_MT_SAMPLE_SIZE, image_size));
		THROW_IF_FAILED(input_video_media_type->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, true));
		THROW_IF_FAILED(input_video_media_type->SetUINT32(MF_MT_FIXED_SIZE_SAMPLES, true));
		THROW_IF_FAILED(MFSetAttributeSize(input_video_media_type.get(), MF_MT_FRAME_SIZE, width, height));
		THROW_IF_FAILED(MFSetAttributeRatio(input_video_media_type.get(), MF_MT_FRAME_RATE, rate, scale));
		THROW_IF_FAILED(MFSetAttributeRatio(input_video_media_type.get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1));

		if (is_accelerated) THROW_IF_FAILED(input_video_media_type->SetUINT32(MF_MT_D3D_RESOURCE_VERSION, is_dx12_available() ? MF_D3D12_RESOURCE : MF_D3D11_RESOURCE));

		return input_video_media_type;
	}

	auto make_input_audio_media_type(int32_t const &channel_count, int32_t const &sampling_rate, GUID const &output_video_format)
	{
		auto input_audio_media_type{ com_ptr<IMFMediaType>{} };
		THROW_IF_FAILED(MFCreateMediaType(out_ptr(input_audio_media_type)));
		THROW_IF_FAILED(input_audio_media_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio));
		THROW_IF_FAILED(input_audio_media_type->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM));
		THROW_IF_FAILED(input_audio_media_type->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, audio_bits_per_sample));
		THROW_IF_FAILED(input_audio_media_type->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, sampling_rate));
		THROW_IF_FAILED(input_audio_media_type->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, channel_count));
		THROW_IF_FAILED(input_audio_media_type->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, true));

		if (output_video_format == MFVideoFormat_WVC1)
		{
			auto const block_alignment{ get_pcm_block_alignment(static_cast<uint32_t>(channel_count), audio_bits_per_sample) };
			THROW_IF_FAILED(input_audio_media_type->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, block_alignment));
			THROW_IF_FAILED(input_audio_media_type->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, block_alignment * sampling_rate));
			THROW_IF_FAILED(input_audio_media_type->SetUINT32(MF_MT_AVG_BITRATE, sampling_rate * audio_bits_per_sample * channel_count));
		}
		return input_audio_media_type;
	}

	auto make_input_media_types(OUTPUT_INFO const &oip, GUID const &output_video_format, bool const &is_accelerated)
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

	auto make_dxgi_device_manager_ptr(uint32_t const &d3d_resource_version = MF_D3D11_RESOURCE)
	{
		aviutl_logger->info(aviutl_logger, L"Preparing DirectX Video Acceleration...");
		com_ptr<IUnknown> directx_device{};
		switch (d3d_resource_version)
		{
		default:
		case MF_D3D11_RESOURCE:
			THROW_IF_FAILED(D3D11CreateDevice
			(
				nullptr,
				D3D_DRIVER_TYPE_HARDWARE,
				nullptr,
				D3D11_CREATE_DEVICE_VIDEO_SUPPORT,
				d3d_feature_levels.data(),
				static_cast<uint32_t>(d3d_feature_levels.size()),
				D3D11_SDK_VERSION,
				reinterpret_cast<ID3D11Device **>(&directx_device),
				nullptr,
				nullptr
			));
			break;

		case MF_D3D12_RESOURCE:
			aviutl_logger->info(aviutl_logger, L"DirectX 12 is available so going to be used.");
			THROW_IF_FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_1, __uuidof(ID3D12Device), out_ptr(directx_device)));
			break;
		}
		com_ptr<IMFDXGIDeviceManager> dxgi_device_manager{};
		uint32_t reset_token{};
		THROW_IF_FAILED(MFCreateDXGIDeviceManager(&reset_token, out_ptr(dxgi_device_manager)));
		THROW_IF_FAILED(dxgi_device_manager->ResetDevice(directx_device.get(), reset_token));

		return dxgi_device_manager;
	}

	[[nodiscard]] auto make_sink_writer(wstring_view output_name, IMFAttributes &media_type, GUID const &output_video_format)
	{
		auto sink_writer_attributes{ com_ptr<IMFAttributes>{} };
		THROW_IF_FAILED(MFCreateAttributes(out_ptr(sink_writer_attributes), 3));

		THROW_IF_FAILED(sink_writer_attributes->SetUINT32(MF_SINK_WRITER_DISABLE_THROTTLING, true));

		if (output_video_format == MFVideoFormat_H264)
		{
			THROW_IF_FAILED(sink_writer_attributes->SetGUID(MF_TRANSCODE_CONTAINERTYPE, MFTranscodeContainerType_FMPEG4));
			THROW_IF_FAILED(sink_writer_attributes->SetUINT32(MF_MPEG4SINK_MOOV_BEFORE_MDAT, true));
		}

		uint32_t d3d_resource_version{};
		if (SUCCEEDED(media_type.GetUINT32(MF_MT_D3D_RESOURCE_VERSION, &d3d_resource_version)))
		{
			try
			{
				THROW_IF_FAILED(sink_writer_attributes->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, true));
				THROW_IF_FAILED(sink_writer_attributes->SetUnknown(MF_SINK_WRITER_D3D_MANAGER, make_dxgi_device_manager_ptr(d3d_resource_version).get()));
			}
			catch (HRESULT const &)
			{
				aviutl_logger->warn(aviutl_logger, L"Failed to initialize hardware acceleration. Falling back to software encoding.");
			}
		}

		auto sink_writer{ com_ptr<IMFSinkWriter>{} };
		THROW_IF_FAILED(MFCreateSinkWriterFromURL(output_name.data(), nullptr, sink_writer_attributes.get(), out_ptr(sink_writer)));

		return sink_writer;
	}

	[[nodiscard]] auto configure_video_output(IMFSinkWriter &sink_writer, IMFMediaType &input_media_type, GUID const &output_video_format)
	{
		uint32_t width, height;
		THROW_IF_FAILED(MFGetAttributeSize(&input_media_type, MF_MT_FRAME_SIZE, &width, &height));

		uint32_t rate, scale;
		THROW_IF_FAILED(MFGetAttributeRatio(&input_media_type, MF_MT_FRAME_RATE, &rate, &scale));

		auto output_video_media_type{ com_ptr<IMFMediaType>{} };
		THROW_IF_FAILED(MFCreateMediaType(out_ptr(output_video_media_type)));

		THROW_IF_FAILED(output_video_media_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video));
		THROW_IF_FAILED(output_video_media_type->SetGUID(MF_MT_SUBTYPE, output_video_format));
		THROW_IF_FAILED(MFSetAttributeSize(output_video_media_type.get(), MF_MT_FRAME_SIZE, width, height));
		THROW_IF_FAILED(MFSetAttributeRatio(output_video_media_type.get(), MF_MT_FRAME_RATE, rate, scale));
		THROW_IF_FAILED(output_video_media_type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive));
		set_color_space_media_types(*output_video_media_type, height);

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
		THROW_IF_FAILED(sink_writer.AddStream(output_video_media_type.get(), &video_index));

		return video_index;
	}

	[[nodiscard]] auto configure_audio_output(IMFSinkWriter &sink_writer, IMFMediaType &input_media_type, uint32_t const &output_bit_rate, GUID const &output_video_format)
	{
		__assume(output_bit_rate <= 3);

		auto output_audio_media_type{ com_ptr<IMFMediaType>{} };
		THROW_IF_FAILED(MFCreateMediaType(out_ptr(output_audio_media_type)));

		THROW_IF_FAILED(output_audio_media_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio));
		THROW_IF_FAILED(output_audio_media_type->SetGUID(MF_MT_SUBTYPE, output_video_format == MFVideoFormat_WVC1 ? MFAudioFormat_WMAudioV9 : MFAudioFormat_AAC));
		THROW_IF_FAILED(output_audio_media_type->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, MFGetAttributeUINT32(&input_media_type, MF_MT_AUDIO_NUM_CHANNELS, 2)));
		THROW_IF_FAILED(output_audio_media_type->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, (3 + output_bit_rate) * 4000));
		THROW_IF_FAILED(output_audio_media_type->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, MFGetAttributeUINT32(&input_media_type, MF_MT_AUDIO_SAMPLES_PER_SECOND, 48000)));
		THROW_IF_FAILED(output_audio_media_type->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, audio_bits_per_sample));

		DWORD audio_index{};
		THROW_IF_FAILED(sink_writer.AddStream(output_audio_media_type.get(), &audio_index));

		return audio_index;
	}

	auto configure_video_input(IMFSinkWriter &sink_writer, DWORD const &index, uint32_t const &quality, GUID const &output_video_format, IMFMediaType &input_media_type)
	{
		__assume(quality <= 100);

		auto encoder_attributes{ com_ptr<IMFAttributes>{} };
		THROW_IF_FAILED(MFCreateAttributes(out_ptr(encoder_attributes), 5));

		switch (output_video_format.Data1)
		{
		default:
		case FCC('H264'):
			THROW_IF_FAILED(encoder_attributes->SetUINT32(CODECAPI_AVEncH264CABACEnable, true));
			[[fallthrough]];
		case FCC('HEVC'):
			THROW_IF_FAILED(encoder_attributes->SetUINT32(CODECAPI_AVEncMPVDefaultBPictureCount, 2));
			THROW_IF_FAILED(encoder_attributes->SetUINT32(CODECAPI_AVEncCommonRateControlMode, eAVEncCommonRateControlMode_Quality));
			THROW_IF_FAILED(encoder_attributes->SetUINT32(CODECAPI_AVEncCommonQuality, quality));
			THROW_IF_FAILED(encoder_attributes->SetUINT32(CODECAPI_AVEncNumWorkerThreads, 0));
			break;
		case FCC('WVC1'):
			THROW_IF_FAILED(encoder_attributes->SetUINT32(MFPKEY_COMPRESSIONOPTIMIZATIONTYPE.fmtid, 1));
			add_windows_media_qvba_activation_media_attributes(*encoder_attributes, quality);
			break;
		}

		THROW_IF_FAILED(sink_writer.SetInputMediaType(index, &input_media_type, encoder_attributes.get()));
	}

	auto configure_audio_input(IMFSinkWriter &sink_writer, DWORD const &index, uint32_t const &quality, GUID const &output_video_format, IMFMediaType &input_media_type)
	{
		auto encoder_attributes{ com_ptr<IMFAttributes>{} };

		if (output_video_format == MFVideoFormat_WVC1)
		{
			THROW_IF_FAILED(MFCreateAttributes(out_ptr(encoder_attributes), 3));
			add_windows_media_qvba_activation_media_attributes(*encoder_attributes, quality);
		}

		THROW_IF_FAILED(sink_writer.SetInputMediaType(index, &input_media_type, encoder_attributes.get()));
	}

	auto configure_video_stream(IMFSinkWriter &sink_writer, uint32_t const &quality, IMFMediaType &input_media_type, GUID const &output_video_format)
	{
		auto index{ configure_video_output(sink_writer, input_media_type, output_video_format) };
		configure_video_input(sink_writer, index, quality, output_video_format, input_media_type);
		return index;
	}

	auto configure_audio_stream(IMFSinkWriter &sink_writer, int32_t const &output_bit_rate, uint32_t const &quality, IMFMediaType &input_media_type, GUID const &output_video_format)
	{
		auto const index{ configure_audio_output(sink_writer, input_media_type, output_bit_rate, output_video_format) };
		configure_audio_input(sink_writer, index, quality, output_video_format, input_media_type);
		return index;
	}

	auto configure_streams(IMFSinkWriter &sink_writer, uint32_t const &quality, uint32_t const &output_bit_rate, IMFMediaTypes const &input_media_types, GUID const &output_video_format)
	{
		return stream_indices_t{
			configure_video_stream(sink_writer, quality, *input_media_types.first, output_video_format),
			configure_audio_stream(sink_writer, output_bit_rate, quality, *input_media_types.second, output_video_format)
		};
	}

	auto make_initialized_sink_writer(OUTPUT_INFO const &oip, GUID const &output_video_format, uint32_t const &video_quality, uint32_t const &audio_bit_rate, IMFMediaTypes const &media_types)
	{
		auto const sink_writer{ make_sink_writer(oip.savefile, *media_types.first, output_video_format) };
		auto const indices{ configure_streams(*sink_writer, video_quality, audio_bit_rate, media_types, output_video_format) };

		THROW_IF_FAILED(sink_writer->BeginWriting());

		return sink_writer_with_indices_t{ move(sink_writer), move(indices) };
	}

	auto write_video_sample(OUTPUT_INFO const &oip, IMFSinkWriter &sink_writer, int32_t const &f, DWORD const &index, IMFMediaType &input_media_type) noexcept
	{
		static uint32_t d3d_resource_version{};
		static auto const is_accelerated{ SUCCEEDED(input_media_type.GetUINT32(MF_MT_D3D_RESOURCE_VERSION, &d3d_resource_version)) };
		static auto const time_stamp{ convert_frame_rate_to_average_time_per_frame(input_media_type) };

		if (oip.func_is_abort()) return E_ABORT;

		oip.func_rest_time_disp(f, oip.n);

		auto const frame_image{ static_cast<uint8_t *>(oip.func_get_video(f, FCC('YUY2'))) };

		auto video_buffer{ com_ptr_nothrow<IMFMediaBuffer>{} };
		RETURN_IF_FAILED(MFCreateMediaBufferFromMediaType(&input_media_type, time_stamp, 0, 0, out_ptr(video_buffer)));

		com_ptr_nothrow<IMF2DBuffer2> video_2d_buffer{};
		RETURN_IF_FAILED(video_buffer.query_to(&video_2d_buffer));

		uint8_t *scanline{};
		long stride{};
		uint8_t *buffer_begin{};
		DWORD buffer_size{};
		RETURN_IF_FAILED(video_2d_buffer->Lock2DSize(MF2DBuffer_LockFlags_Write, &scanline, &stride, &buffer_begin, &buffer_size));
		RETURN_IF_FAILED(MFCopyImage(scanline, stride, is_accelerated ? yuy2_to_nv12(span{ frame_image, buffer_size }, { oip.w, oip.h }).get() : frame_image, stride, stride, oip.h));
		RETURN_IF_FAILED(video_2d_buffer->Unlock2D());

		DWORD contiguous_length{};
		RETURN_IF_FAILED(video_2d_buffer->GetContiguousLength(&contiguous_length));
		RETURN_IF_FAILED(video_buffer->SetCurrentLength(contiguous_length));

		RETURN_IF_FAILED(write_sample_to_sink_writer(sink_writer, index, *video_buffer, time_stamp * f, time_stamp));

		return S_OK;
	}

	auto write_audio_sample(OUTPUT_INFO const &oip, IMFSinkWriter &sink_writer, int32_t const &n, DWORD const &index, IMFMediaType &input_media_type) noexcept
	{
		static auto const block_alignment{ get_pcm_block_alignment(static_cast<uint32_t>(oip.audio_ch), audio_bits_per_sample) };
		static auto const max_samples{ static_cast<int32_t>(block_alignment * oip.audio_rate) };

		if (oip.func_is_abort()) return E_ABORT;

		oip.func_rest_time_disp(n, oip.audio_n);

		int32_t actual_samples{};
		auto const audio_data{ oip.func_get_audio(n, max_samples, &actual_samples, WAVE_FORMAT_PCM) };
		if (!actual_samples) return S_FALSE;

		auto const sample_duration{ static_cast<int64_t>(actual_samples) * 10'000'000LL / max_samples };
		auto const sample_time{ static_cast<int64_t>(n) * 10'000'000LL / oip.audio_rate };

		auto audio_buffer{ wil::com_ptr_nothrow<IMFMediaBuffer>{} };
		RETURN_IF_FAILED(MFCreateMediaBufferFromMediaType(&input_media_type, sample_duration, static_cast<DWORD>(actual_samples), 0, out_ptr(audio_buffer)));
		uint8_t *media_data{};
		DWORD media_data_max_length{};
		RETURN_IF_FAILED(audio_buffer->Lock(&media_data, &media_data_max_length, nullptr));
		memmove_s(media_data, media_data_max_length, audio_data, static_cast<size_t>(actual_samples));
		RETURN_IF_FAILED(audio_buffer->Unlock());
		RETURN_IF_FAILED(audio_buffer->SetCurrentLength(static_cast<DWORD>(actual_samples)));

		RETURN_IF_FAILED(write_sample_to_sink_writer(sink_writer, index, *audio_buffer, sample_time, sample_duration));

		return S_OK;
	}

	auto write_samples(OUTPUT_INFO const &oip, GUID const &output_video_format, uint32_t const &video_quality, uint32_t const &audio_bit_rate, IMFMediaTypes const &input_media_types)
	{
		auto is_aborted{ true };

		auto const [sink_writer, indices] { make_initialized_sink_writer(oip, output_video_format, video_quality, audio_bit_rate, input_media_types) };
		auto const [video_index, audio_index] { indices };
		auto const [input_video_media_type, input_audio_media_type] { input_media_types };

		oip.func_set_buffer_size(8, 8);
		aviutl_logger->info(aviutl_logger, L"Sending video samples to the writer...");
		for (auto f{ 0 }; f < oip.n; ++f)
			if (FAILED(write_video_sample(oip, *sink_writer, f, video_index, *input_video_media_type)))
				goto abort;
		aviutl_logger->info(aviutl_logger, L"Sending audio samples to the writer...");
		for (auto n{ 0 }; n < oip.audio_n; n += oip.audio_rate)
			if (FAILED(write_audio_sample(oip, *sink_writer, n, audio_index, *input_audio_media_type)))
				goto abort;

		aviutl_logger->info(aviutl_logger, L"Finalizing. It may take a while...");
		sink_writer->Finalize();
		is_aborted = false;

	abort:
		aviutl_logger->info(aviutl_logger, is_aborted ? L"Aborted." : L"Done.");
		return;
	}

	using unique_mfshutdown_call = unique_call<decltype(&::MFShutdown), ::MFShutdown>;
	[[nodiscard]] inline auto MFStartup(DWORD &&flags = MFSTARTUP_FULL)
	{
		THROW_IF_FAILED(::MFStartup(MF_VERSION, flags));
		return unique_mfshutdown_call();
	}

	void output_file(OUTPUT_INFO const &oip, std::uint32_t &&video_quality, std::uint32_t &&audio_bit_rate, bool &&is_hevc_preferable, bool &&is_accelerated, LOG_HANDLE &logger)
	{
		auto const com_cleanup{ CoInitializeEx() };
		auto const mf_cleanup{ MFStartup() };

		aviutl_logger = &logger;

		auto const output_video_format{ get_suitable_output_video_format_guid(filesystem::path(oip.savefile).extension(), is_hevc_preferable) };
		auto const input_media_types{ make_input_media_types(oip, output_video_format, is_accelerated) };

		write_samples
		(
			oip,
			output_video_format,
			video_quality,
			audio_bit_rate,
			input_media_types
		);
	}
}