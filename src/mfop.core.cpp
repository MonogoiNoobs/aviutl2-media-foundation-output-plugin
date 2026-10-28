/**
 * \copyright	SPDX-License-Identifier: MIT
 * \year		2025-2026
 * \author		Shion Yorigami <62567343+MonogoiNoobs@users.noreply.github.com>
 */

module;

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

#include <ranges>

#pragma comment(lib, "Mfplat")
#pragma comment(lib, "Mfreadwrite")
#pragma comment(lib, "Mfuuid")
#pragma comment(lib, "d3d11")
#pragma comment(lib, "d3d12")

#pragma warning(default: 4557 5266)

#define UNEXPECT_IF_FAILED(hr) do { \
	if (HRESULT const __mfop_hr{ hr }) \
		if (FAILED(__mfop_hr)) [[unlikely]] return std::unexpected{ mfop::error{ __mfop_hr, #hr } }; \
} while (0)

#define RETURN_IF_UNEXPECTED(__mfop_expected) do { \
	if (not __mfop_expected) [[unlikely]] return unexpected{ __mfop_expected.error() }; \
} while (0)

module mfop;

import :core;

import std;

using namespace std;
using namespace wil;

static LOG_HANDLE *aviutl_logger;

namespace mfop
{
	auto const constinit audio_bits_per_sample{ 16u };

	auto const constinit null_stream_index{ -1 };

	using nv12_t = vector<std::byte>;
	using resolution_t = pair<int32_t const, int32_t const>;
	using fps_t = pair<int32_t const, int32_t const>;
	using stream_indices_t = pair<DWORD const, DWORD const>;
	using IMFMediaTypes = pair<com_ptr_nothrow<IMFMediaType>, com_ptr_nothrow<IMFMediaType>>;
	using sink_writer_with_indices_and_media_types_t = tuple<com_ptr_nothrow<IMFSinkWriter>, stream_indices_t const, IMFMediaTypes const>;

	auto constexpr yuy2_to_nv12(ranges::constant_range auto &&yuy2, unsigned_integral auto const &yuy2_stride)
	{
		static auto const constinit take_y{ views::stride(2) };
		static auto constexpr take_necessary_uv{ [](unsigned_integral auto const &yuy2_stride) {
			return views::chunk(yuy2_stride)
				| views::stride(2)
				| views::join
				| views::drop(1)
				| views::stride(2);
		} };

		auto const image_size{ yuy2.size() / 2 };

		nv12_t nv12{};
		nv12.reserve(image_size * 3 / 2);

		nv12.append_range(yuy2 | take_y);
		nv12.append_range(yuy2 | take_necessary_uv(yuy2_stride));

		return nv12;
	}

	unsigned_integral auto constexpr get_pcm_block_alignment(integral auto const &audio_ch, uint32_t const &bit = audio_bits_per_sample) noexcept
	{
		return (audio_ch * bit) / 8;
	}

	auto constexpr get_suitable_input_video_format_guid(bool const &is_accelerated) noexcept
	{
		return is_accelerated ? MFVideoFormat_NV12 : MFVideoFormat_YUY2;
	}

	template<typename D3D12DeviceVersion = ID3D12Device>
	auto is_dx12_available(D3D_FEATURE_LEVEL &&minimum_feature_level = D3D_FEATURE_LEVEL_11_1) noexcept
	{
		return SUCCEEDED(D3D12CreateDevice(nullptr, minimum_feature_level, __uuidof(D3D12DeviceVersion), nullptr));
	}

	unsigned_integral auto get_average_time_per_frame(IMFMediaType &media_type) noexcept
	{
		uint32_t rate{}, scale{};
		ignore = MFGetAttributeRatio(&media_type, MF_MT_FRAME_RATE, &rate, &scale);

		uint64_t result{};
		ignore = MFFrameRateToAverageTimePerFrame(rate, scale, &result);

		return result;
	}

	auto get_suitable_output_video_format_guid(filesystem::path const &extension, bool const &is_hevc_preferable) noexcept
	{
		if (extension == ".mp4") [[likely]] return is_hevc_preferable ? MFVideoFormat_HEVC : MFVideoFormat_H264;
		if (extension == ".wmv") return MFVideoFormat_WVC1;
		return MFVideoFormat_H264;
	}

	auto add_windows_media_qvba_activation_media_attributes(IMFAttributes &attributes, uint32_t const &quality) noexcept
	{
		ignore = attributes.SetUINT32(MFPKEY_VBRENABLED.fmtid, true);
		ignore = attributes.SetUINT32(MFPKEY_CONSTRAIN_ENUMERATED_VBRQUALITY.fmtid, true);
		ignore = attributes.SetUINT32(MFPKEY_DESIRED_VBRQUALITY.fmtid, quality);
	}

	auto set_color_space_media_types(IMFMediaType &media_type, int32_t const &height) noexcept
	{
		ignore = media_type.SetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, MFNominalRange_16_235);

		if (cmp_less_equal(height, 720)) [[unlikely]]
		{
			aviutl_logger->info(aviutl_logger, L"Color space desires BT.601.");
			ignore = media_type.SetUINT32(MF_MT_VIDEO_PRIMARIES, MFVideoPrimaries_SMPTE170M);
			ignore = media_type.SetUINT32(MF_MT_YUV_MATRIX, MFVideoTransferMatrix_BT601);
			ignore = media_type.SetUINT32(MF_MT_TRANSFER_FUNCTION, MFVideoTransFunc_709);
		}
		else if (cmp_greater_equal(height, 2160))
		{
			aviutl_logger->info(aviutl_logger, L"Color space desires BT.2020 (12bit).");
			ignore = media_type.SetUINT32(MF_MT_VIDEO_PRIMARIES, MFVideoPrimaries_BT2020);
			ignore = media_type.SetUINT32(MF_MT_YUV_MATRIX, MFVideoTransferMatrix_BT2020_12);
			ignore = media_type.SetUINT32(MF_MT_TRANSFER_FUNCTION, MFVideoTransFunc_2020);
		}
		else
		{
			aviutl_logger->info(aviutl_logger, L"Color space desires BT.709.");
			ignore = media_type.SetUINT32(MF_MT_VIDEO_PRIMARIES, MFVideoPrimaries_BT709);
			ignore = media_type.SetUINT32(MF_MT_YUV_MATRIX, MFVideoTransferMatrix_BT709);
			ignore = media_type.SetUINT32(MF_MT_TRANSFER_FUNCTION, MFVideoTransFunc_709);
		}
	}

	auto make_input_video_media_type(resolution_t &&resolution, fps_t &&fps, bool const &is_accelerated) noexcept
	{
		auto const [width, height] { resolution };
		auto const [rate, scale] { fps };

		auto const video_format{ get_suitable_input_video_format_guid(is_accelerated) };

		com_ptr_nothrow<IMFMediaType> input_video_media_type{};
		ignore = MFCreateMediaType(out_ptr(input_video_media_type));
		ignore = input_video_media_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
		ignore = input_video_media_type->SetGUID(MF_MT_SUBTYPE, video_format);
		ignore = input_video_media_type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
		ignore = input_video_media_type->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, true);
		ignore = MFSetAttributeSize(input_video_media_type.get(), MF_MT_FRAME_SIZE, width, height);
		ignore = MFSetAttributeRatio(input_video_media_type.get(), MF_MT_FRAME_RATE, rate, scale);
		ignore = MFSetAttributeRatio(input_video_media_type.get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);

		if (is_accelerated) ignore = input_video_media_type->SetUINT32(MF_MT_D3D_RESOURCE_VERSION, is_dx12_available() ? MF_D3D12_RESOURCE : MF_D3D11_RESOURCE);

		return input_video_media_type;
	}

	auto make_input_audio_media_type(int32_t const &channel_count, int32_t const &sampling_rate, bool const &is_asf) noexcept
	{
		com_ptr_nothrow<IMFMediaType> input_audio_media_type{};
		ignore = MFCreateMediaType(out_ptr(input_audio_media_type));
		ignore = input_audio_media_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
		ignore = input_audio_media_type->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
		ignore = input_audio_media_type->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, audio_bits_per_sample);
		ignore = input_audio_media_type->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, sampling_rate);
		ignore = input_audio_media_type->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, channel_count);
		ignore = input_audio_media_type->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, true);

		if (is_asf)
		{
			auto const block_alignment{ get_pcm_block_alignment(channel_count) };
			ignore = input_audio_media_type->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, block_alignment);
			ignore = input_audio_media_type->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, block_alignment * sampling_rate);
			ignore = input_audio_media_type->SetUINT32(MF_MT_AVG_BITRATE, sampling_rate * audio_bits_per_sample * channel_count);
		}

		return input_audio_media_type;
	}

	auto make_input_media_types(OUTPUT_INFO const &oip, GUID const &output_video_format = GUID_NULL, bool const &is_accelerated = false) noexcept
	{
		return IMFMediaTypes
		{
			(oip.flag bitand oip.FLAG_VIDEO) and (output_video_format not_eq GUID_NULL) ? make_input_video_media_type({ oip.w, oip.h },{ oip.rate, oip.scale }, is_accelerated) : nullptr,
			oip.flag bitand oip.FLAG_AUDIO ? make_input_audio_media_type(oip.audio_ch, oip.audio_rate, cmp_equal(output_video_format.Data1, FCC('WVC1'))) : nullptr
		};
	}

	auto write_sample_to_sink_writer(IMFSinkWriter &sink_writer, DWORD const &index, IMFMediaBuffer &buffer, int64_t const &time, int64_t const &duration) noexcept
	{
		com_ptr_nothrow<IMFSample> sample{};
		ignore = MFCreateSample(out_ptr(sample));
		ignore = sample->AddBuffer(&buffer);
		ignore = sample->SetSampleTime(time);
		ignore = sample->SetSampleDuration(duration);
		return sink_writer.WriteSample(index, sample.get());
	}

	expected<com_ptr_nothrow<IMFDXGIDeviceManager>, error> make_dxgi_device_manager(bool &&is_dx12_preferable) noexcept
	{
		aviutl_logger->info(aviutl_logger, L"Preparing DirectX Video Acceleration...");
		com_ptr_nothrow<IUnknown> directx_device{};

		if (is_dx12_preferable)
		{
			aviutl_logger->info(aviutl_logger, L"DirectX 12 is available so will be used.");
			UNEXPECT_IF_FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_1, IID_PPV_ARGS(&directx_device)));
		}
		else
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

			auto d3d11_device{ &dynamic_cast<ID3D11Device &>(*directx_device) };

			UNEXPECT_IF_FAILED(D3D11CreateDevice
			(
				nullptr,
				D3D_DRIVER_TYPE_HARDWARE,
				nullptr,
				D3D11_CREATE_DEVICE_VIDEO_SUPPORT,
				d3d_feature_levels.data(),
				static_cast<uint32_t>(d3d_feature_levels.size()),
				D3D11_SDK_VERSION,
				&d3d11_device,
				nullptr,
				nullptr
			));
		}

		com_ptr_nothrow<IMFDXGIDeviceManager> dxgi_device_manager{};
		uint32_t reset_token{};
		ignore = MFCreateDXGIDeviceManager(&reset_token, out_ptr(dxgi_device_manager));
		UNEXPECT_IF_FAILED(dxgi_device_manager->ResetDevice(directx_device.get(), reset_token));

		return dxgi_device_manager;
	}

	expected<com_ptr_nothrow<IMFSinkWriter>, error> make_sink_writer(wstring_view output_name, bool const &is_accelerated, GUID const &output_video_format) noexcept
	{
		com_ptr_nothrow<IMFAttributes> sink_writer_attributes{};
		ignore = MFCreateAttributes(out_ptr(sink_writer_attributes), 3);

		ignore = sink_writer_attributes->SetUINT32(MF_SINK_WRITER_DISABLE_THROTTLING, true);

		if (cmp_equal(output_video_format.Data1, FCC('H264'))) [[likely]]
		{
			ignore = sink_writer_attributes->SetGUID(MF_TRANSCODE_CONTAINERTYPE, MFTranscodeContainerType_FMPEG4);
			ignore = sink_writer_attributes->SetUINT32(MF_MPEG4SINK_MOOV_BEFORE_MDAT, true);
		}

		if (is_accelerated)
			if (auto const d3d_manager{ make_dxgi_device_manager(is_dx12_available()) })
			{
				ignore = sink_writer_attributes->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, true);
				ignore = sink_writer_attributes->SetUnknown(MF_SINK_WRITER_D3D_MANAGER, d3d_manager->get());
			}

		auto sink_writer{ com_ptr_nothrow<IMFSinkWriter>{} };
		UNEXPECT_IF_FAILED(MFCreateSinkWriterFromURL(output_name.data(), nullptr, sink_writer_attributes.get(), out_ptr(sink_writer)));

		return sink_writer;
	}

	expected<DWORD, error> configure_video_output(IMFSinkWriter &sink_writer, IMFMediaType &input_media_type, GUID const &output_video_format) noexcept
	{
		uint32_t width, height;
		ignore = MFGetAttributeSize(&input_media_type, MF_MT_FRAME_SIZE, &width, &height);

		uint32_t rate, scale;
		ignore = MFGetAttributeRatio(&input_media_type, MF_MT_FRAME_RATE, &rate, &scale);

		auto output_video_media_type{ com_ptr_nothrow<IMFMediaType>{} };
		ignore = MFCreateMediaType(out_ptr(output_video_media_type));

		ignore = output_video_media_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
		ignore = output_video_media_type->SetGUID(MF_MT_SUBTYPE, output_video_format);
		ignore = MFSetAttributeSize(output_video_media_type.get(), MF_MT_FRAME_SIZE, width, height);
		ignore = MFSetAttributeRatio(output_video_media_type.get(), MF_MT_FRAME_RATE, rate, scale);
		ignore = output_video_media_type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
		set_color_space_media_types(*output_video_media_type, height);

		switch (output_video_format.Data1)
		{
		[[likely]] case FCC('H264'):
			ignore = output_video_media_type->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_High);
			break;
		case FCC('HEVC'):
			ignore = output_video_media_type->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH265VProfile_Main_420_8);
			ignore = output_video_media_type->SetUINT32(MF_MT_MPEG2_LEVEL, eAVEncH265VLevel5_1);
			[[fallthrough]];
		default:
			ignore = output_video_media_type->SetUINT32(MF_MT_AVG_BITRATE, 12000000);
			break;
		}

		DWORD video_index{};
		UNEXPECT_IF_FAILED(sink_writer.AddStream(output_video_media_type.get(), &video_index));

		return video_index;
	}

	expected<DWORD, error> configure_audio_output(IMFSinkWriter &sink_writer, IMFMediaType &input_media_type, uint32_t const &output_bit_rate, GUID const &output_video_format) noexcept
	{
		__assume(cmp_less_equal(output_bit_rate, 3));

		auto output_audio_media_type{ com_ptr_nothrow<IMFMediaType>{} };
		ignore = MFCreateMediaType(out_ptr(output_audio_media_type));

		ignore = output_audio_media_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
		ignore = output_audio_media_type->SetGUID(MF_MT_SUBTYPE, cmp_equal(output_video_format.Data1, FCC('WVC1')) ? MFAudioFormat_WMAudioV9 : MFAudioFormat_AAC);
		ignore = output_audio_media_type->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, MFGetAttributeUINT32(&input_media_type, MF_MT_AUDIO_NUM_CHANNELS, 2));
		ignore = output_audio_media_type->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, (3 + output_bit_rate) * 4000);
		ignore = output_audio_media_type->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, MFGetAttributeUINT32(&input_media_type, MF_MT_AUDIO_SAMPLES_PER_SECOND, 48000));
		ignore = output_audio_media_type->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, audio_bits_per_sample);

		DWORD audio_index{};
		UNEXPECT_IF_FAILED(sink_writer.AddStream(output_audio_media_type.get(), &audio_index));

		return audio_index;
	}

	expected<HRESULT, error> configure_video_input(IMFSinkWriter &sink_writer, DWORD const &index, uint32_t const &quality, GUID const &output_video_format, IMFMediaType &input_video_media_type) noexcept
	{
		__assume(cmp_less_equal(quality, 100));

		com_ptr_nothrow<IMFAttributes> encoder_attributes{};
		ignore = MFCreateAttributes(out_ptr(encoder_attributes), 5);

		switch (output_video_format.Data1)
		{
		default:
		[[likely]] case FCC('H264'):
			ignore = encoder_attributes->SetUINT32(CODECAPI_AVEncH264CABACEnable, true);
			[[fallthrough]];
		case FCC('HEVC'):
			ignore = encoder_attributes->SetUINT32(CODECAPI_AVEncMPVDefaultBPictureCount, 2);
			ignore = encoder_attributes->SetUINT32(CODECAPI_AVEncCommonRateControlMode, eAVEncCommonRateControlMode_Quality);
			ignore = encoder_attributes->SetUINT32(CODECAPI_AVEncCommonQuality, quality);
			ignore = encoder_attributes->SetUINT32(CODECAPI_AVEncNumWorkerThreads, 0);
			break;
		case FCC('WVC1'):
			ignore = encoder_attributes->SetUINT32(MFPKEY_COMPRESSIONOPTIMIZATIONTYPE.fmtid, 1);
			add_windows_media_qvba_activation_media_attributes(*encoder_attributes, quality);
			break;
		}

		UNEXPECT_IF_FAILED(sink_writer.SetInputMediaType(index, &input_video_media_type, encoder_attributes.get()));

		return S_OK;
	}

	expected<HRESULT, error> configure_audio_input(IMFSinkWriter &sink_writer, DWORD const &index, uint32_t const &quality, GUID const &output_video_format, IMFMediaType &input_audio_media_type) noexcept
	{
		com_ptr_nothrow<IMFAttributes> encoder_attributes{};

		if (cmp_equal(output_video_format.Data1, FCC('WVC1')))
		{
			ignore = MFCreateAttributes(out_ptr(encoder_attributes), 3);
			add_windows_media_qvba_activation_media_attributes(*encoder_attributes, quality);
		}

		UNEXPECT_IF_FAILED(sink_writer.SetInputMediaType(index, &input_audio_media_type, encoder_attributes.get()));

		return S_OK;
	}

	expected<DWORD, error> configure_video_stream(IMFSinkWriter &sink_writer, uint32_t const &quality, IMFMediaType &input_media_type, GUID const &output_video_format) noexcept
	{
		auto const index{ configure_video_output(sink_writer, input_media_type, output_video_format) };
		RETURN_IF_UNEXPECTED(index);

		auto const result{ configure_video_input(sink_writer, *index, quality, output_video_format, input_media_type) };
		RETURN_IF_UNEXPECTED(result);

		return *index;
	}

	expected<DWORD, error> configure_audio_stream(IMFSinkWriter &sink_writer, int32_t const &output_bit_rate, uint32_t const &quality, IMFMediaType &input_media_type, GUID const &output_video_format) noexcept
	{
		auto const index{ configure_audio_output(sink_writer, input_media_type, output_bit_rate, output_video_format) };
		RETURN_IF_UNEXPECTED(index);

		auto const result{ configure_audio_input(sink_writer, *index, quality, output_video_format, input_media_type) };
		RETURN_IF_UNEXPECTED(result);

		return *index;
	}

	expected<stream_indices_t, error> configure_streams(IMFSinkWriter &sink_writer, uint32_t const &quality, uint32_t const &output_bit_rate, IMFMediaTypes const &input_media_types, GUID const &output_video_format) noexcept
	{
		expected<DWORD, error> video_index{ null_stream_index }, audio_index{ null_stream_index };

		if (input_media_types.first) [[likely]]
		{
			video_index = configure_video_stream(sink_writer, quality, *input_media_types.first, output_video_format);
			RETURN_IF_UNEXPECTED(video_index);
		}

		if (input_media_types.second) [[likely]]
		{
			audio_index = configure_audio_stream(sink_writer, output_bit_rate, quality, *input_media_types.second, output_video_format);
			RETURN_IF_UNEXPECTED(audio_index);
		}

		return stream_indices_t{ move_if_noexcept(*video_index), move_if_noexcept(*audio_index) };
	}

	expected<sink_writer_with_indices_and_media_types_t, error> make_initialized_sink_writer(OUTPUT_INFO const &oip, output_configuration const &configuration) noexcept
	{
		auto output_video_format{ GUID_NULL };

		auto const extension{ filesystem::path{ oip.savefile }.extension() };

		if ((oip.flag bitand oip.FLAG_VIDEO) and not (extension == ".wma" or extension == ".m4a"))
			output_video_format = get_suitable_output_video_format_guid(extension, configuration.is_hevc_preferable);

		auto const media_types{ make_input_media_types(oip, output_video_format, configuration.is_accelerated) };

		auto const sink_writer{ make_sink_writer(oip.savefile, configuration.is_accelerated, output_video_format) };
		RETURN_IF_UNEXPECTED(sink_writer);

		auto const indices{ configure_streams(**sink_writer, configuration.video_quality, configuration.audio_bit_rate, media_types, output_video_format) };
		RETURN_IF_UNEXPECTED(indices);

		UNEXPECT_IF_FAILED((*sink_writer)->BeginWriting());

		return sink_writer_with_indices_and_media_types_t{ move_if_noexcept(*sink_writer), move_if_noexcept(*indices), move_if_noexcept(media_types) };
	}

	auto write_video_sample(OUTPUT_INFO const &oip, IMFSinkWriter &sink_writer, int32_t const &f, DWORD const &index, IMFMediaType &input_media_type, bool const &is_accelerated, int64_t const &time_stamp) noexcept
	{
		if (oip.func_is_abort()) return E_ABORT;

		oip.func_rest_time_disp(f, oip.n);

		auto const frame_image{ as_bytes(span{ static_cast<uint8_t *>(oip.func_get_video(f, FCC('YUY2'))), static_cast<size_t>(oip.w * oip.h * 2) }) };

		com_ptr_nothrow<IMFMediaBuffer> video_buffer{};
		RETURN_IF_FAILED(MFCreateMediaBufferFromMediaType(&input_media_type, time_stamp, 0, 0, out_ptr(video_buffer)));

		com_ptr_nothrow<IMF2DBuffer2> video_2d_buffer{};
		RETURN_IF_FAILED(video_buffer.query_to(&video_2d_buffer));

		// Neither buffer_begin nor buffer_size used, but do NOT give Lock2DSize's 4-5th args nullptr or you'll be cooked.
		uint8_t *scanline{}, *buffer_begin{};
		long stride{};
		DWORD buffer_size{};
		ignore = video_2d_buffer->Lock2DSize(MF2DBuffer_LockFlags_Write, &scanline, &stride, &buffer_begin, &buffer_size);
		ignore = MFCopyImage
		(
			scanline,
			stride,
			is_accelerated ? reinterpret_cast<uint8_t const *>(yuy2_to_nv12(frame_image, static_cast<size_t>(oip.w) * 2).data()) : reinterpret_cast<uint8_t const *>(frame_image.data()),
			stride,
			stride,
			is_accelerated ? oip.h * 3 / 2 : oip.h
		);
		ignore = video_2d_buffer->Unlock2D();

		// MSDN's a bald-faced liar and seriously values redundancy.
		DWORD contiguous_length{};
		ignore = video_2d_buffer->GetContiguousLength(&contiguous_length);
		ignore = video_buffer->SetCurrentLength(contiguous_length);

		return write_sample_to_sink_writer(sink_writer, index, *video_buffer, time_stamp * f, time_stamp);
	}

	auto write_audio_sample(OUTPUT_INFO const &oip, IMFSinkWriter &sink_writer, int32_t const &n, DWORD const &index, IMFMediaType &input_media_type, int32_t const &max_samples) noexcept
	{
		if (oip.func_is_abort()) return E_ABORT;

		oip.func_rest_time_disp(n, oip.audio_n);

		int32_t actual_samples{};
		auto const audio_data{ static_cast<uint8_t *>(oip.func_get_audio(n, max_samples, &actual_samples, MFAudioFormat_PCM.Data1)) };
		if (not actual_samples) return S_FALSE;

		auto const sample_duration{ static_cast<int64_t>(actual_samples) * 10'000'000LL / max_samples };
		auto const sample_time{ static_cast<int64_t>(n) * 10'000'000LL / oip.audio_rate };

		com_ptr_nothrow<IMFMediaBuffer> audio_buffer{};
		RETURN_IF_FAILED(MFCreateMediaBufferFromMediaType(&input_media_type, sample_duration, static_cast<DWORD>(actual_samples), 0, out_ptr(audio_buffer)));

		uint8_t *media_data{};
		DWORD media_data_max_length{};
		ignore = audio_buffer->Lock(&media_data, &media_data_max_length, nullptr); // Despite Lock2DSize, the 3rd arg set null, but MSDN says it's OK.
		ignore = copy_n(audio_data, static_cast<size_t>(actual_samples), media_data);
		ignore = audio_buffer->Unlock();

		ignore = audio_buffer->SetCurrentLength(static_cast<DWORD>(actual_samples));

		return write_sample_to_sink_writer(sink_writer, index, *audio_buffer, sample_time, sample_duration);
	}

	[[nodiscard]] inline auto MFStartup(DWORD &&flags = MFSTARTUP_FULL)
	{
		FAIL_FAST_IF_FAILED(::MFStartup(MF_VERSION, flags));
		return unique_call<decltype(&::MFShutdown), ::MFShutdown>{};
	}

	expected<HRESULT, error> output_file(OUTPUT_INFO const &oip, output_configuration &&configuration, LOG_HANDLE &logger)
	{
		if (not oip.flag) [[unlikely]] return unexpected{ error{ E_INVALIDARG, "this session: no input found" } };
		if (auto extension{ filesystem::path{ oip.savefile }.extension() }; not (oip.flag bitand oip.FLAG_AUDIO) and (extension == ".wma" or extension == ".m4a"))
			[[unlikely]] return unexpected{ error{ E_INVALIDARG, "this session: you've just tried to output audio WITHOUT audio" } };

		auto const com_cleanup{ CoInitializeEx_failfast() };
		auto const mf_cleanup{ MFStartup() };

		aviutl_logger = &logger;

		auto sink_writer_with_indices{ make_initialized_sink_writer(oip, configuration) };
		RETURN_IF_UNEXPECTED(sink_writer_with_indices);

		auto const [sink_writer, indices, input_media_types] { *sink_writer_with_indices };

		oip.func_set_buffer_size(8, 8);

		auto aeternum{ S_OK };

		if (cmp_not_equal(indices.first, null_stream_index)) [[likely]]
		{
			aviutl_logger->info(aviutl_logger, L"Video stream found; sending samples to the writer...");

			auto const video_time_stamp{ get_average_time_per_frame(*input_media_types.first) };

			for (auto &&f : views::iota(0, oip.n))
				if (cmp_less((aeternum = write_video_sample(oip, *sink_writer, f, indices.first, *input_media_types.first, configuration.is_accelerated, video_time_stamp)), 0))
					goto abort;
		}

		if (cmp_not_equal(indices.second, null_stream_index)) [[likely]]
		{
			aviutl_logger->info(aviutl_logger, L"Audio stream found; sending samples to the writer...");

			auto const audio_max_samples{ static_cast<int32_t>(get_pcm_block_alignment(oip.audio_ch) * oip.audio_rate) };

			for (auto &&n : views::iota(0, oip.audio_n / oip.audio_rate + 1) | views::transform([&](integral auto &&i) { return i * oip.audio_rate; }))
				if (cmp_less((aeternum = write_audio_sample(oip, *sink_writer, n, indices.second, *input_media_types.second, audio_max_samples)), 0))
					break;
		}

	abort:
		aviutl_logger->info(aviutl_logger, SUCCEEDED(aeternum) ? L"Finalizing. It may take a while..." : L"Aborting...");
		UNEXPECT_IF_FAILED(sink_writer->Finalize());

		UNEXPECT_IF_FAILED(aeternum);

		return S_OK;
	}
}
