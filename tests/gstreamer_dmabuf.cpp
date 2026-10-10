/* WiVRn VR streaming
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "decoder/gstreamer/dmabuf.h"
#include "decoder/gstreamer/converter_input.h"
#include "decoder/gstreamer/pipeline_diagnostics.h"
#include <array>
#include <drm_fourcc.h>
#include <fcntl.h>
#include <gst/allocators/gstdmabuf.h>
#include <gst/app/gstappsink.h>
#include <gst/app/gstappsrc.h>
#include <gst/video/gstvideometa.h>
#include <gst/video/video-frame.h>
#include <gst/video/video-info-dma.h>
#include <iostream>
#include <span>
#include <stdexcept>
#include <sys/mman.h>
#include <unistd.h>

void require(bool result, const char * message)
{
	if (!result)
		throw std::runtime_error(message);
}
void test_nv12_copy()
{
	// Exercise the GstVideoFrame path used by v4l2convert's MMAP input pool.
	// One source DMA-BUF, two destination memories, and different padded strides.
	// memfd establishes layout/copy correctness, not hardware DMA capability.
	int fd = memfd_create("wivrn-nv12-copy", MFD_CLOEXEC);
	require(fd >= 0 && !ftruncate(fd, 1600), "NV12 source fd");
	auto allocator = gst_dmabuf_allocator_new();
	auto source = gst_buffer_new();
	gst_buffer_append_memory(source, gst_dmabuf_allocator_alloc(allocator, fd, 1600));
	gst_object_unref(allocator);
	gsize source_offsets[GST_VIDEO_MAX_PLANES] = {64, 1088};
	gint source_strides[GST_VIDEO_MAX_PLANES] = {64, 64};
	gst_buffer_add_video_meta_full(source, GST_VIDEO_FRAME_FLAG_NONE, GST_VIDEO_FORMAT_NV12, 32, 16, 2, source_offsets, source_strides);
	auto destination = gst_buffer_new();
	gst_buffer_append_memory(destination, gst_allocator_alloc(nullptr, 768, nullptr));
	gst_buffer_append_memory(destination, gst_allocator_alloc(nullptr, 384, nullptr));
	gsize destination_offsets[GST_VIDEO_MAX_PLANES] = {0, 768};
	gint destination_strides[GST_VIDEO_MAX_PLANES] = {48, 48};
	gst_buffer_add_video_meta_full(destination, GST_VIDEO_FRAME_FLAG_NONE, GST_VIDEO_FORMAT_NV12, 32, 16, 2, destination_offsets, destination_strides);
	std::string caps_text = wivrn::gstreamer::converter_input_caps(true);
	caps_text += ",width=32,height=16,framerate=60/1";
	auto caps = gst_caps_from_string(caps_text.c_str());
	auto strict_caps = gst_caps_from_string(wivrn::gstreamer::converter_input_caps(false));
	auto ten_bit_caps = gst_caps_from_string("video/x-raw,format=P010_10LE,width=32,height=16,framerate=60/1");
	require(gst_video_is_dma_drm_caps(strict_caps) && !gst_caps_can_intersect(caps, strict_caps), "strict DMA-BUF and MMAP caps stay distinct");
	require(!gst_caps_can_intersect(caps, ten_bit_caps), "copy fallback must reject 10-bit output");
	gst_caps_unref(strict_caps);
	gst_caps_unref(ten_bit_caps);
	GstVideoInfoDmaDrm info;
	gst_video_info_dma_drm_init(&info);
	require(!gst_video_is_dma_drm_caps(caps) && gst_video_info_from_caps(&info.vinfo, caps), "MMAP input must negotiate ordinary NV12 caps");
	GstVideoFrame input{}, output{};
	require(gst_video_frame_map(&input, &info.vinfo, source, GST_MAP_WRITE), "map padded single DMA-BUF NV12");
	require(gst_video_frame_map(&output, &info.vinfo, destination, GST_MAP_WRITE), "map separate destination planes");
	for (int plane = 0; plane < 2; ++plane)
		for (int row = 0; row < (plane ? 8 : 16); ++row)
			for (int column = 0; column < 32; ++column)
				static_cast<uint8_t *>(GST_VIDEO_FRAME_PLANE_DATA(&input, plane))[row * GST_VIDEO_FRAME_PLANE_STRIDE(&input, plane) + column] = plane * 100 + row + column;
	require(gst_video_frame_copy(&output, &input), "NV12 copy between memory layouts");
	for (int plane = 0; plane < 2; ++plane)
		for (int row = 0; row < (plane ? 8 : 16); ++row)
			for (int column = 0; column < 32; ++column)
				require(static_cast<uint8_t *>(GST_VIDEO_FRAME_PLANE_DATA(&output, plane))[row * GST_VIDEO_FRAME_PLANE_STRIDE(&output, plane) + column] == plane * 100 + row + column, "NV12 Y/UV content and strides");
	gst_video_frame_unmap(&output);
	gst_video_frame_unmap(&input);
	gst_buffer_unref(destination);
	gst_buffer_unref(source);
	gst_caps_unref(caps);
	require(fcntl(fd, F_GETFD) == -1, "NV12 copy releases source fd");
}
void test_parser(const char * parser, const char * input_caps, std::span<const uint8_t> data)
{
	auto factory = gst_element_factory_find(parser);
	if (!factory)
	{
		std::cout << parser << " not installed: parser test skipped\n";
		return;
	}
	gst_object_unref(factory);
	std::string description = std::string("appsrc name=input ! ") + parser + " name=parser ! appsink name=output sync=false async=false";
	auto pipeline = gst_parse_launch(description.c_str(), nullptr);
	require(pipeline != nullptr, "parser pipeline");
	auto source = GST_APP_SRC(gst_bin_get_by_name(GST_BIN(pipeline), "input"));
	auto sink = GST_APP_SINK(gst_bin_get_by_name(GST_BIN(pipeline), "output"));
	auto parse_element = gst_bin_get_by_name(GST_BIN(pipeline), "parser");
	g_object_set(parse_element, "disable-passthrough", TRUE, nullptr);
	gst_object_unref(parse_element);
	auto caps = gst_caps_from_string(input_caps);
	gst_app_src_set_caps(source, caps);
	gst_caps_unref(caps);
	g_object_set(source, "format", GST_FORMAT_TIME, nullptr);
	require(gst_element_set_state(pipeline, GST_STATE_PLAYING) != GST_STATE_CHANGE_FAILURE, "parser startup");
	for (uint64_t i = 1; i <= 3; ++i)
	{
		auto buffer = gst_buffer_new_allocate(nullptr, data.size(), nullptr);
		gst_buffer_fill(buffer, 0, data.data(), data.size());
		GST_BUFFER_PTS(buffer) = i * GST_SECOND;
		GST_BUFFER_DURATION(buffer) = GST_SECOND / 30;
		require(gst_app_src_push_buffer(source, buffer) == GST_FLOW_OK, "parser input");
	}
	gst_app_src_end_of_stream(source);
	for (uint64_t i = 1; i <= 3; ++i)
	{
		auto sample = gst_app_sink_try_pull_sample(sink, GST_SECOND);
		require(sample && GST_BUFFER_PTS(gst_sample_get_buffer(sample)) == i * GST_SECOND, "codec headers/frame parser PTS");
		gst_sample_unref(sample);
	}
	gst_element_set_state(pipeline, GST_STATE_NULL);
	gst_object_unref(source);
	gst_object_unref(sink);
	gst_object_unref(pipeline);
}
int main()
{
	try
	{
		gst_init(nullptr, nullptr);
		test_nv12_copy();
		require(!wivrn::gstreamer::converter_input_copy(nullptr) && !wivrn::gstreamer::converter_input_copy("dmabuf"), "strict converter import default");
		require(wivrn::gstreamer::converter_input_copy("copy"), "explicit NV12 copy fallback");
		bool invalid_input_mode = false;
		try
		{
			wivrn::gstreamer::converter_input_copy("auto");
		}
		catch (const std::runtime_error &)
		{
			invalid_input_mode = true;
		}
		require(invalid_input_mode, "unknown modes must not silently enable CPU copies");
		// Preserve the downstream allocation error instead of only appsrc's flow error.
		auto emitter = gst_element_factory_make("identity", "converter-test");
		auto error = g_error_new_literal(GST_RESOURCE_ERROR, GST_RESOURCE_ERROR_NO_SPACE_LEFT, "capture allocation failed");
		auto diagnostic = gst_message_new_error(GST_OBJECT(emitter), error, "test allocation detail");
		auto message_text = wivrn::gstreamer::describe_bus_message(diagnostic);
		require(message_text.find("converter-test") != std::string::npos && message_text.find("capture allocation failed") != std::string::npos && message_text.find("gst-resource-error-quark") != std::string::npos && message_text.find("test allocation detail") != std::string::npos, "downstream diagnostic source, error domain and details");
		gst_message_unref(diagnostic);
		g_error_free(error);
		gst_object_unref(emitter);
		// memfd is only a stand-in for descriptor extraction; this does not test GPU import.
		int fd = memfd_create("wivrn-layout-test", MFD_CLOEXEC);
		require(fd >= 0 && !ftruncate(fd, 8192), "memfd");
		auto allocator = gst_dmabuf_allocator_new();
		auto memory = gst_dmabuf_allocator_alloc(allocator, fd, 8192);
		gst_object_unref(allocator);
		auto buffer = gst_buffer_new();
		gst_buffer_append_memory(buffer, memory);
		gsize offsets[GST_VIDEO_MAX_PLANES] = {128};
		gint strides[GST_VIDEO_MAX_PLANES] = {256};
		auto meta = gst_buffer_add_video_meta_full(buffer, GST_VIDEO_FRAME_FLAG_NONE, GST_VIDEO_FORMAT_BGRA, 32, 16, 1, offsets, strides);
		auto caps = gst_caps_from_string("video/x-raw(memory:DMABuf),format=DMA_DRM,drm-format=AR24,width=32,height=16,framerate=60/1");
		auto sample = gst_sample_new(buffer, caps, nullptr, nullptr);
		auto layout = wivrn::gstreamer::describe_dmabuf(sample);
		require(layout.fd == fd && layout.offset == 128 && layout.stride == 256 && layout.width == 32 && layout.height == 16 && layout.modifier == DRM_FORMAT_MOD_LINEAR, "padded AR24 layout");
		gst_sample_unref(sample);
		gst_caps_unref(caps);
		caps = gst_caps_from_string("video/x-raw(memory:DMABuf),format=DMA_DRM,drm-format=AR24:0x0100000000000001,width=32,height=16,framerate=60/1");
		sample = gst_sample_new(buffer, caps, nullptr, nullptr);
		layout = wivrn::gstreamer::describe_dmabuf(sample);
		require(layout.modifier == 0x0100000000000001ULL, "nonlinear modifier must survive extraction");
		// GstVideoMeta offsets are relative to the whole buffer, while shared memory
		// has its own offset in the fd. Verify both are accounted for.
		auto shared_buffer = gst_buffer_new();
		gst_buffer_append_memory(shared_buffer, gst_allocator_alloc(nullptr, 48, nullptr));
		auto shared_memory = gst_memory_share(memory, 64, 4096);
		require(shared_memory != nullptr, "DMA-BUF shared memory");
		gst_buffer_append_memory(shared_buffer, shared_memory);
		offsets[0] = 112;
		gst_buffer_add_video_meta_full(shared_buffer, GST_VIDEO_FRAME_FLAG_NONE, GST_VIDEO_FORMAT_BGRA, 32, 16, 1, offsets, strides);
		auto shared_sample = gst_sample_new(shared_buffer, caps, nullptr, nullptr);
		auto shared_layout = wivrn::gstreamer::describe_dmabuf(shared_sample);
		require(shared_layout.fd == fd && shared_layout.offset == 128, "memory index and suballocation offset");
		gst_sample_unref(shared_sample);
		gst_buffer_unref(shared_buffer);
		auto ten_bit_caps = gst_caps_from_string("video/x-raw(memory:DMABuf),format=DMA_DRM,drm-format=P010,width=32,height=16,framerate=60/1");
		auto ten_bit_sample = gst_sample_new(buffer, ten_bit_caps, nullptr, nullptr);
		bool ten_bit_rejected = false;
		try
		{
			wivrn::gstreamer::describe_dmabuf(ten_bit_sample);
		}
		catch (const std::runtime_error &)
		{
			ten_bit_rejected = true;
		}
		require(ten_bit_rejected, "P010 must never be interpreted as AR24");
		gst_sample_unref(ten_bit_sample);
		gst_caps_unref(ten_bit_caps);
		meta->stride[0] = -1;
		bool rejected = false;
		try
		{
			wivrn::gstreamer::describe_dmabuf(sample);
		}
		catch (std::runtime_error &)
		{
			rejected = true;
		}
		require(rejected, "negative stride must be rejected");
		gst_sample_unref(sample);
		gst_caps_unref(caps);
		gst_buffer_unref(buffer);
		require(fcntl(fd, F_GETFD) == -1, "GstBuffer must release its fd");

		// Exercise asynchronous appsrc/appsink PTS propagation and shutdown without hardware.
		auto pipeline = gst_parse_launch("appsrc name=input ! identity ! appsink name=output sync=false async=false", nullptr);
		auto source = GST_APP_SRC(gst_bin_get_by_name(GST_BIN(pipeline), "input"));
		auto sink = GST_APP_SINK(gst_bin_get_by_name(GST_BIN(pipeline), "output"));
		g_object_set(source, "format", GST_FORMAT_TIME, "is-live", TRUE, nullptr);
		require(gst_element_set_state(pipeline, GST_STATE_PLAYING) != GST_STATE_CHANGE_FAILURE, "pipeline startup");
		for (uint64_t i = 1; i <= 3; ++i)
		{
			buffer = gst_buffer_new_allocate(nullptr, 4, nullptr);
			GST_BUFFER_PTS(buffer) = i * GST_SECOND;
			GST_BUFFER_OFFSET(buffer) = 100 + i;
			require(gst_app_src_push_buffer(source, buffer) == GST_FLOW_OK, "appsrc input");
			sample = gst_app_sink_try_pull_sample(sink, GST_SECOND);
			require(sample && GST_BUFFER_PTS(gst_sample_get_buffer(sample)) == i * GST_SECOND && GST_BUFFER_OFFSET(gst_sample_get_buffer(sample)) == 100 + i, "PTS/frame identity");
			gst_sample_unref(sample);
		}
		require(gst_app_src_end_of_stream(source) == GST_FLOW_OK, "EOS");
		gst_element_set_state(pipeline, GST_STATE_NULL);
		gst_object_unref(source);
		gst_object_unref(sink);
		gst_object_unref(pipeline);

		// Generated black 32x32 Annex B access unit, with SPS/PPS/IDR; no external test file.
		constexpr std::array<uint8_t, 49> access_unit = {0, 0, 0, 1, 103, 66, 192, 10, 218, 37, 176, 17, 0, 0, 3, 0, 1, 0, 0, 3, 0, 60, 143, 18, 38, 160, 0, 0, 0, 1, 104, 206, 15, 200, 0, 0, 1, 101, 136, 132, 58, 38, 40, 0, 9, 2, 201, 215, 94};
		auto parser_factory = gst_element_factory_find("h264parse");
		if (parser_factory)
		{
			gst_object_unref(parser_factory);
			pipeline = gst_parse_launch("appsrc name=input ! h264parse disable-passthrough=true ! video/x-h264,stream-format=byte-stream,alignment=au ! appsink name=output sync=false async=false", nullptr);
			source = GST_APP_SRC(gst_bin_get_by_name(GST_BIN(pipeline), "input"));
			sink = GST_APP_SINK(gst_bin_get_by_name(GST_BIN(pipeline), "output"));
			caps = gst_caps_from_string("video/x-h264,stream-format=byte-stream,alignment=au");
			gst_app_src_set_caps(source, caps);
			gst_caps_unref(caps);
			g_object_set(source, "format", GST_FORMAT_TIME, "is-live", TRUE, nullptr);
			require(gst_element_set_state(pipeline, GST_STATE_PLAYING) != GST_STATE_CHANGE_FAILURE, "parser startup");
			for (uint64_t i = 1; i <= 3; ++i)
			{
				buffer = gst_buffer_new_allocate(nullptr, access_unit.size(), nullptr);
				gst_buffer_fill(buffer, 0, access_unit.data(), access_unit.size());
				GST_BUFFER_PTS(buffer) = i * GST_SECOND;
				GST_BUFFER_DURATION(buffer) = GST_SECOND / 30;
				require(gst_app_src_push_buffer(source, buffer) == GST_FLOW_OK, "Annex B push");
			}
			gst_app_src_end_of_stream(source);
			for (uint64_t i = 1; i <= 3; ++i)
			{
				sample = gst_app_sink_try_pull_sample(sink, GST_SECOND);
				require(sample && GST_BUFFER_PTS(gst_sample_get_buffer(sample)) == i * GST_SECOND, "SPS/PPS/IDR parser PTS");
				gst_sample_unref(sample);
			}
			gst_element_set_state(pipeline, GST_STATE_NULL);
			gst_object_unref(source);
			gst_object_unref(sink);
			gst_object_unref(pipeline);
		}
		else
			std::cout << "h264parse not installed: parser test skipped\n";
		// Black 32x32 8-bit frames generated by libx265/libaom, including sequence headers.
		constexpr std::array<uint8_t, 98> hevc_unit = {0, 0, 0, 1, 64, 1, 12, 1, 255, 255, 1, 96, 0, 0, 3, 0, 144, 0, 0, 3, 0, 0, 3, 0, 30, 149, 152, 9, 0, 0, 0, 1, 66, 1, 1, 1, 96, 0, 0, 3, 0, 144, 0, 0, 3, 0, 0, 3, 0, 30, 160, 66, 8, 89, 101, 102, 185, 50, 188, 5, 160, 32, 0, 0, 3, 0, 32, 0, 0, 3, 3, 193, 0, 0, 0, 1, 68, 1, 193, 115, 208, 137, 0, 0, 0, 1, 40, 1, 175, 8, 96, 241, 35, 215, 255, 217, 155, 224};
		test_parser("h265parse", "video/x-h265,stream-format=byte-stream,alignment=au", hevc_unit);
		constexpr std::array<uint8_t, 30> av1_unit = {18, 0, 10, 10, 0, 0, 0, 2, 39, 254, 109, 124, 128, 32, 50, 14, 16, 0, 192, 0, 0, 2, 128, 0, 0, 2, 229, 214, 247, 64};
		test_parser("av1parse", "video/x-av1,stream-format=obu-stream,alignment=frame", av1_unit);
		std::cout << "DMA-BUF layout, ownership, modifier and appsrc PTS tests passed\n";
		return 0;
	}
	catch (const std::exception & e)
	{
		std::cerr << e.what() << '\n';
		return 1;
	}
}
