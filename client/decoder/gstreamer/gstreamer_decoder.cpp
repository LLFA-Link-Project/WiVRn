/* WiVRn VR streaming
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "gstreamer_decoder.h"
#include "application.h"
#include "dmabuf.h"
#include "converter_input.h"
#include "pipeline_diagnostics.h"
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <gst/video/gstvideometa.h>
#include <gst/video/video-info-dma.h>
#include <linux/dma-buf.h>
#include <poll.h>
#include <spdlog/spdlog.h>
#include <sstream>
#include <stdexcept>
#include <sys/ioctl.h>
#include <system_error>
#include <unistd.h>
#include <utility>

namespace wivrn::gstreamer
{
namespace
{
void initialize()
{
	static const bool initialized = [] {
		GError * error = nullptr;
		if (!gst_init_check(nullptr, nullptr, &error))
		{
			std::string message = error ? error->message : "unknown error";
			g_clear_error(&error);
			throw std::runtime_error("GStreamer initialization: " + message);
		}
		return true;
	}();
	(void)initialized;
}
bool factory_available(const char * name)
{
	auto factory = gst_element_factory_find(name);
	if (!factory)
		return false;
	gst_object_unref(factory);
	return true;
}
struct codec_description
{
	video_codec codec;
	const char * name;
	const char * parser;
	const char * decoder;
	const char * caps;
};
constexpr codec_description codec_descriptions[] = {
        {h264, "h264", "h264parse", "v4l2slh264dec", "video/x-h264,stream-format=byte-stream,alignment=au"},
        {h265, "h265", "h265parse", "v4l2slh265dec", "video/x-h265,stream-format=byte-stream,alignment=au"},
        {av1, "av1", "av1parse", "v4l2slav1dec", "video/x-av1,stream-format=obu-stream,alignment=frame"},
};

bool verified_codec(const char * name)
{
	// H.264 decode/conversion was verified by the operator. Other codecs require
	// explicit attestation after testing their hardware pipeline on this device.
	const char * configured = std::getenv("WIVRN_GSTREAMER_CODECS");
	std::istringstream list(configured ? configured : "h264");
	std::string entry;
	while (std::getline(list, entry, ','))
		if (entry == name)
			return true;
	return false;
}

struct owned_fd
{
	int fd = -1;
	~owned_fd()
	{
		if (fd >= 0)
			close(fd);
	}
};
struct imported_frame
{
	// Declaration order matters: destroy view, image, memory, then sample.
	std::unique_ptr<GstSample, decltype(&gst_sample_unref)> sample;
	vk::raii::DeviceMemory memory = nullptr;
	vk::raii::Image image = nullptr;
	vk::raii::ImageView view = nullptr;
	vk::ImageLayout layout = vk::ImageLayout::eGeneral;
	imported_frame(std::unique_ptr<GstSample, decltype(&gst_sample_unref)> s) : sample(std::move(s)) {}
};
struct frame_handle : wivrn::decoder::blit_handle
{
	std::shared_ptr<imported_frame> storage;
	std::chrono::steady_clock::time_point ready_at = std::chrono::steady_clock::now();
	frame_handle(std::shared_ptr<imported_frame> frame, const dmabuf_frame & dma, const from_headset::feedback & feedback, const to_headset::video_stream_data_shard::view_info_t & view_info) :
	        blit_handle{.feedback = feedback, .view_info = view_info, .image_view = *frame->view, .image = *frame->image, .extent = {dma.width, dma.height}, .current_layout = frame->layout, .foreign_queue_family = VK_QUEUE_FAMILY_FOREIGN_EXT}, storage(std::move(frame)) {}
};

// Export the producer's reservation fence rather than assuming that importing memory
// or retaining GstBuffer also synchronizes writes. No CPU pixel read or conversion.
void wait_producer(int fd, std::stop_token stop)
{
	dma_buf_export_sync_file export_info{.flags = DMA_BUF_SYNC_READ, .fd = -1};
	if (ioctl(fd, DMA_BUF_IOCTL_EXPORT_SYNC_FILE, &export_info) < 0)
		throw std::system_error(errno, std::generic_category(), "DMA-BUF export producer sync_file");
	owned_fd fence{export_info.fd};
	pollfd poll_info{fence.fd, POLLIN, 0};
	auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
	while (!stop.stop_requested())
	{
		int result = poll(&poll_info, 1, 50);
		if (result > 0)
		{
			if (poll_info.revents & POLLIN)
				return;
			throw std::runtime_error("DMA-BUF producer fence failed");
		}
		if (result < 0 && errno != EINTR)
			throw std::system_error(errno, std::generic_category(), "DMA-BUF producer fence poll");
		if (std::chrono::steady_clock::now() >= deadline)
			throw std::runtime_error("DMA-BUF producer fence timed out");
	}
	throw std::runtime_error("GStreamer decoder stopped");
}

void import_image(imported_frame & frame, const dmabuf_frame & dma, vk::raii::Device & device, vk::raii::PhysicalDevice & physical)
{
	constexpr auto format = vk::Format::eB8G8R8A8Srgb;
	vk::StructureChain query{
	        vk::PhysicalDeviceImageFormatInfo2{.format = format, .type = vk::ImageType::e2D, .tiling = vk::ImageTiling::eDrmFormatModifierEXT, .usage = vk::ImageUsageFlagBits::eSampled},
	        vk::PhysicalDeviceImageDrmFormatModifierInfoEXT{.drmFormatModifier = dma.modifier,
		                                                .sharingMode = vk::SharingMode::eExclusive},
	        vk::PhysicalDeviceExternalImageFormatInfo{.handleType = vk::ExternalMemoryHandleTypeFlagBits::eDmaBufEXT}};
	auto properties = physical.getImageFormatProperties2<vk::ImageFormatProperties2, vk::ExternalImageFormatProperties>(query.get());
	if (!(properties.get<vk::ExternalImageFormatProperties>().externalMemoryProperties.externalMemoryFeatures & vk::ExternalMemoryFeatureFlagBits::eImportable))
		throw std::runtime_error("Vulkan cannot import AR24 with negotiated DRM modifier");
	auto limits = properties.get<vk::ImageFormatProperties2>().imageFormatProperties;
	if (dma.width > limits.maxExtent.width || dma.height > limits.maxExtent.height)
		throw std::runtime_error("DMA-BUF dimensions exceed Vulkan import limits");
	// Modifier plane count must also match the one-plane AR24 allocation.
	vk::StructureChain format_properties{vk::FormatProperties2{}, vk::DrmFormatModifierPropertiesListEXT{}};
	// Older Vulkan-Hpp RAII wrappers expose only the value-returning overload.
	// Call the core API through the RAII dispatcher to preserve our populated pNext
	// chain during both passes of the modifier-list query.
	auto query_format_properties = [&] {
		physical.getDispatcher()->vkGetPhysicalDeviceFormatProperties2(
		        static_cast<VkPhysicalDevice>(*physical), static_cast<VkFormat>(format), reinterpret_cast<VkFormatProperties2 *>(&format_properties.get()));
	};
	query_format_properties();
	std::vector<vk::DrmFormatModifierPropertiesEXT> modifiers(format_properties.get<vk::DrmFormatModifierPropertiesListEXT>().drmFormatModifierCount);
	format_properties.get<vk::DrmFormatModifierPropertiesListEXT>().pDrmFormatModifierProperties = modifiers.data();
	query_format_properties();
	auto modifier = std::ranges::find(modifiers, dma.modifier, &vk::DrmFormatModifierPropertiesEXT::drmFormatModifier);
	if (modifier == modifiers.end() || modifier->drmFormatModifierPlaneCount != 1 ||
	    !(modifier->drmFormatModifierTilingFeatures & vk::FormatFeatureFlagBits::eSampledImageFilterLinear))
		throw std::runtime_error("AR24 modifier requires unsupported plane layout or filtering");
	vk::SubresourceLayout plane{.offset = dma.offset, .rowPitch = dma.stride};
	vk::StructureChain image_info{
	        vk::ImageCreateInfo{.imageType = vk::ImageType::e2D, .format = format, .extent = {dma.width, dma.height, 1}, .mipLevels = 1, .arrayLayers = 1, .samples = vk::SampleCountFlagBits::e1, .tiling = vk::ImageTiling::eDrmFormatModifierEXT, .usage = vk::ImageUsageFlagBits::eSampled, .sharingMode = vk::SharingMode::eExclusive, .initialLayout = vk::ImageLayout::eUndefined},
	        vk::ExternalMemoryImageCreateInfo{.handleTypes = vk::ExternalMemoryHandleTypeFlagBits::eDmaBufEXT},
	        vk::ImageDrmFormatModifierExplicitCreateInfoEXT{.drmFormatModifier = dma.modifier,
		                                                .drmFormatModifierPlaneCount = 1,
		                                                .pPlaneLayouts = &plane}};
	frame.image = vk::raii::Image(device, image_info.get());
	auto requirements = device.getImageMemoryRequirements2<vk::MemoryRequirements2, vk::MemoryDedicatedRequirements>({.image = *frame.image});
	owned_fd fd{fcntl(dma.fd, F_DUPFD_CLOEXEC, 0)};
	if (fd.fd < 0)
		throw std::system_error(errno, std::generic_category(), "Duplicate DMA-BUF fd");
	auto bits = device.getMemoryFdPropertiesKHR(vk::ExternalMemoryHandleTypeFlagBits::eDmaBufEXT, fd.fd).memoryTypeBits &
	            requirements.get<vk::MemoryRequirements2>().memoryRequirements.memoryTypeBits;
	if (!bits)
		throw std::runtime_error("DMA-BUF has no compatible Vulkan memory type");
	uint32_t type = 0;
	while (!(bits & (1u << type)))
		++type;
	vk::StructureChain allocation{
	        vk::MemoryAllocateInfo{.allocationSize = requirements.get<vk::MemoryRequirements2>().memoryRequirements.size, .memoryTypeIndex = type},
	        vk::ImportMemoryFdInfoKHR{.handleType = vk::ExternalMemoryHandleTypeFlagBits::eDmaBufEXT, .fd = fd.fd},
	        vk::MemoryDedicatedAllocateInfo{.image = *frame.image}};
	frame.memory = vk::raii::DeviceMemory(device, allocation.get());
	fd.fd = -1; // Vulkan consumed the duplicate on successful allocation.
	frame.image.bindMemory(*frame.memory, 0);
	frame.view = vk::raii::ImageView(device, vk::ImageViewCreateInfo{.image = *frame.image, .viewType = vk::ImageViewType::e2D, .format = format, .subresourceRange = {.aspectMask = vk::ImageAspectFlagBits::eColor, .levelCount = 1, .layerCount = 1}});
	// DMA-BUF external access implies GENERAL. The renderer acquires from FOREIGN;
	// an UNDEFINED transition here would discard the already decoded contents.
}

// Log every diagnostic at emission time, including warnings which a filtered
// bus pop would discard and subsequent errors hidden by the first exception.
GstBusSyncReply log_bus_message(GstBus *, GstMessage * message, gpointer data)
{
	try
	{
		auto index = GPOINTER_TO_UINT(data);
		if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_ERROR)
			spdlog::error("GStreamer stream {}: {}", index, describe_bus_message(message));
		else if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_WARNING)
			spdlog::warn("GStreamer stream {}: {}", index, describe_bus_message(message));
	}
	catch (...)
	{} // C callbacks must not propagate C++ exceptions.
	return GST_BUS_PASS;
}

struct stage_probe
{
	guint stream;
	const char * stage;
	bool saw_buffer = false;
};
GstPadProbeReturn log_stage(GstPad *, GstPadProbeInfo * info, gpointer data)
{
	auto & stage = *static_cast<stage_probe *>(data);
	try
	{
		if (GST_PAD_PROBE_INFO_TYPE(info) & GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM)
		{
			auto event = GST_PAD_PROBE_INFO_EVENT(info);
			if (GST_EVENT_TYPE(event) == GST_EVENT_CAPS)
			{
				GstCaps * caps = nullptr;
				gst_event_parse_caps(event, &caps);
				auto text = gst_caps_to_string(caps);
				std::string value = text ? text : "unknown caps";
				g_free(text);
				spdlog::info("GStreamer stream {} {} caps: {}", stage.stream, stage.stage, value);
			}
		}
		if ((GST_PAD_PROBE_INFO_TYPE(info) & GST_PAD_PROBE_TYPE_BUFFER) && !stage.saw_buffer)
		{
			stage.saw_buffer = true;
			auto buffer = GST_PAD_PROBE_INFO_BUFFER(info);
			spdlog::info("GStreamer stream {} {} first buffer: bytes={}, memories={}, pts={}", stage.stream, stage.stage, gst_buffer_get_size(buffer), gst_buffer_n_memory(buffer), GST_BUFFER_PTS(buffer));
		}
	}
	catch (...)
	{}
	return GST_PAD_PROBE_OK;
}
void observe_stage(GstElement * element, guint index, const char * stage)
{
	auto pad = gst_element_get_static_pad(element, "src");
	gst_pad_add_probe(pad, GstPadProbeType(GST_PAD_PROBE_TYPE_BUFFER | GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM), log_stage, new stage_probe{index, stage}, [](gpointer data) { delete static_cast<stage_probe *>(data); });
	gst_object_unref(pad);
}

gboolean propose_allocation(GstAppSink *, GstQuery * query, gpointer)
{
	GstCaps * caps = nullptr;
	gst_query_parse_allocation(query, &caps, nullptr);
	GstVideoInfoDmaDrm info;
	gst_video_info_dma_drm_init(&info);
	if (caps && gst_video_info_dma_drm_from_caps(&info, caps))
		gst_query_add_allocation_pool(query, nullptr, info.vinfo.size, 12, 0);
	gst_query_add_allocation_meta(query, GST_VIDEO_META_API_TYPE, nullptr);
	return TRUE;
}
} // namespace

void decoder::supported_codecs(std::vector<video_codec> & codecs)
{
	try
	{
		initialize();
	}
	catch (const std::exception & e)
	{
		spdlog::warn("GStreamer capability discovery failed: {}", e.what());
		return; // Auto selection can still advertise its software backend.
	}
	if (!factory_available("v4l2convert") || !factory_available("appsrc") || !factory_available("appsink"))
		return;
	// Capability discovery runs after application's Vulkan device initialization.
	// Do not advertise a hardware path if its mandatory import extensions are absent.
	for (auto extension: {VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME, VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME, VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME, VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME})
		if (std::ranges::none_of(application::get_vk_device_extensions(), [extension](auto name) { return !strcmp(name, extension); }))
			return;
	for (const auto & entry: codec_descriptions)
		if (verified_codec(entry.name) && factory_available(entry.parser) && factory_available(entry.decoder))
			codecs.push_back(entry.codec);
}

decoder::decoder(vk::raii::Device & device, vk::raii::PhysicalDevice & physical, const to_headset::video_stream_description & description, uint8_t index, std::weak_ptr<scenes::stream>, shard_accumulator *) :
        device(device), physical_device(physical), duration(GstClockTime(GST_SECOND / std::max(1.f, description.frame_rate)))
{
	initialize();
	profile_enabled = std::getenv("WIVRN_GSTREAMER_PROFILE") && !strcmp(std::getenv("WIVRN_GSTREAMER_PROFILE"), "1");
	for (auto extension: {VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME, VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME, VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME, VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME})
		if (std::ranges::none_of(application::get_vk_device_extensions(), [extension](auto name) { return !strcmp(name, extension); }))
			throw std::runtime_error(std::string("GStreamer DMA-BUF needs enabled Vulkan extension ") + extension);
	auto codec = description.codec[index];
	input_codec = codec;
	std::vector<video_codec> supported;
	supported_codecs(supported);
	if (std::ranges::find(supported, codec) == supported.end())
		throw std::runtime_error("GStreamer V4L2 Stateless decoder/parser or v4l2convert unavailable for requested codec");
	rgb_sampler = vk::raii::Sampler(device, vk::SamplerCreateInfo{.magFilter = vk::Filter::eLinear, .minFilter = vk::Filter::eLinear, .mipmapMode = vk::SamplerMipmapMode::eNearest, .addressModeU = vk::SamplerAddressMode::eClampToEdge, .addressModeV = vk::SamplerAddressMode::eClampToEdge, .addressModeW = vk::SamplerAddressMode::eClampToEdge, .maxLod = VK_LOD_CLAMP_NONE});
	const auto & entry = *std::ranges::find(codec_descriptions, codec, &codec_description::codec);
	// Construct-only device properties must be set when creating the element.
	// With no overrides, the dynamically registered factory selects its device.
	const char * device_names[2];
	GValue device_values[2] = {};
	guint device_count = 0;
	for (auto property: {std::pair{"video-device", "WIVRN_GSTREAMER_VIDEO_DEVICE"}, std::pair{"media-device", "WIVRN_GSTREAMER_MEDIA_DEVICE"}})
		if (auto value = std::getenv(property.second); value && *value)
		{
			device_names[device_count] = property.first;
			g_value_init(&device_values[device_count], G_TYPE_STRING);
			g_value_set_string(&device_values[device_count++], value);
		}
	auto hardware = gst_element_factory_make_with_properties(entry.decoder, device_count, device_names, device_values);
	for (guint i = 0; i < device_count; ++i)
		g_value_unset(&device_values[i]);
	if (!hardware)
		throw std::runtime_error("Cannot construct GStreamer hardware decoder");
	pipeline = gst_pipeline_new(nullptr);
	auto parser = gst_element_factory_make(entry.parser, nullptr);
	auto decoded_caps = gst_element_factory_make("capsfilter", nullptr);
	auto converter = gst_element_factory_make("v4l2convert", nullptr);
	auto output_caps = gst_element_factory_make("capsfilter", nullptr);
	source = GST_APP_SRC(gst_element_factory_make("appsrc", nullptr));
	sink = GST_APP_SINK(gst_element_factory_make("appsink", nullptr));
	if (!pipeline || !parser || !decoded_caps || !converter || !output_caps || !source || !sink)
	{
		for (auto element: {hardware, parser, decoded_caps, converter, output_caps, GST_ELEMENT(source), GST_ELEMENT(sink), pipeline})
			if (element)
				gst_object_unref(element);
		throw std::runtime_error("Cannot construct GStreamer pipeline elements");
	}
	gst_bin_add_many(GST_BIN(pipeline), GST_ELEMENT(source), parser, hardware, decoded_caps, converter, output_caps, GST_ELEMENT(sink), nullptr);

	try
	{
		// The strict import path cannot bridge one-memory NV12 to an NM12
		// importer in GStreamer 1.26. MMAP explicitly allows an NV12 CPU copy
		// into the converter's pool; conversion and RGB export remain hardware.
		const bool copy_input = converter_input_copy(std::getenv("WIVRN_GSTREAMER_CONVERTER_INPUT"));
		// GstV4l2IOMode: MMAP = 2, DMABUF_IMPORT = 5, DMABUF = 4.
		g_object_set(converter, "output-io-mode", copy_input ? 2 : 5, "capture-io-mode", 4, nullptr);
		if (auto path = std::getenv("WIVRN_GSTREAMER_CONVERTER_DEVICE"); path && *path)
			g_object_set(converter, "device", path, nullptr);
		auto decoded = gst_caps_from_string(converter_input_caps(copy_input));
		try
		{
			set_converter_colorimetry(decoded, std::getenv("WIVRN_GSTREAMER_COLORIMETRY"));
		}
		catch (...)
		{
			gst_caps_unref(decoded);
			throw;
		}
		auto output = gst_caps_from_string("video/x-raw(memory:DMABuf),format=DMA_DRM,drm-format=AR24");
		g_object_set(decoded_caps, "caps", decoded, nullptr);
		g_object_set(output_caps, "caps", output, nullptr);
		gst_caps_unref(decoded);
		gst_caps_unref(output);
		if (!gst_element_link_many(GST_ELEMENT(source), parser, hardware, decoded_caps, converter, output_caps, GST_ELEMENT(sink), nullptr))
			throw std::runtime_error("Cannot link GStreamer 8-bit DMA-BUF pipeline");
		bus = gst_element_get_bus(pipeline);
		gst_bus_set_sync_handler(bus, log_bus_message, GUINT_TO_POINTER(index), nullptr);
		observe_stage(parser, index, "parsed");
		observe_stage(hardware, index, "decoded");
		// The hardware src caps precede the optional colorimetry constraint.
		// This pad reports the actual negotiated input passed to v4l2convert.
		observe_stage(decoded_caps, index, "converter-input");
		observe_stage(converter, index, "converted");
		auto caps = gst_caps_from_string(entry.caps);
		gst_app_src_set_caps(source, caps);
		gst_caps_unref(caps);
		g_object_set(source, "format", GST_FORMAT_TIME, "is-live", TRUE, "block", FALSE, "max-buffers", guint64(32), "max-bytes", guint64(32 * 1024 * 1024), nullptr);
		g_object_set(sink, "sync", FALSE, "async", FALSE, "max-buffers", guint(2), "drop", TRUE, "enable-last-sample", FALSE, "emit-signals", TRUE, "wait-on-eos", FALSE, nullptr);
		g_signal_connect(sink, "propose-allocation", G_CALLBACK(propose_allocation), nullptr);
		if (gst_element_set_state(pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE)
			throw std::runtime_error("Cannot start GStreamer V4L2 Stateless pipeline");
		worker = std::jthread([this](std::stop_token stop) { receive(stop); });
		spdlog::info("GStreamer V4L2 Stateless stream {}: {} -> v4l2convert -> AR24 DMA-BUF", index, entry.name);
		spdlog::info("GStreamer stream {} converter input: {}", index, copy_input ? "MMAP (CPU NV12 copy enabled; RGA color conversion)" : "DMA-BUF import (no CPU pixel copy)");
	}
	catch (...)
	{
		worker.request_stop();
		if (worker.joinable())
			worker.join();
		gst_element_set_state(pipeline, GST_STATE_NULL);
		if (bus)
			gst_object_unref(bus);
		gst_object_unref(pipeline);
		throw;
	}
}

decoder::~decoder()
{
	worker.request_stop();
	// Worker never acquires scene locks; joining is safe during setup.
	if (worker.joinable())
		worker.join();
	gst_element_set_state(pipeline, GST_STATE_NULL);
	gst_object_unref(bus);
	gst_object_unref(pipeline);
}

std::vector<std::shared_ptr<wivrn::decoder::blit_handle>> decoder::drain_frames()
{
	std::lock_guard lock(mutex);
	if (!failure.empty())
		throw std::runtime_error(failure);
	std::vector<std::shared_ptr<blit_handle>> frames;
	while (!ready.empty())
	{
		if (profile_enabled)
		{
			auto now = std::chrono::steady_clock::now();
			if (delivery_started == std::chrono::steady_clock::time_point{})
				delivery_started = now;
			auto age = std::chrono::duration<double, std::milli>(now - static_cast<frame_handle *>(ready.front().get())->ready_at).count();
			++delivery_count;
			delivery_age_sum += age;
			delivery_age_max = std::max(delivery_age_max, age);
			if (now - delivery_started >= std::chrono::seconds(5))
			{
				spdlog::info("GStreamer delivery: stream={} frame={} delivered_fps={:.2f} ready_to_render_thread_ms[mean/max]={:.2f}/{:.2f}",
				             ready.front()->feedback.stream_index, ready.front()->feedback.frame_index,
				             delivery_count / std::chrono::duration<double>(now - delivery_started).count(), delivery_age_sum / delivery_count, delivery_age_max);
				delivery_started = now;
				delivery_count = 0;
				delivery_age_sum = delivery_age_max = 0;
			}
		}
		frames.push_back(std::move(ready.front()));
		ready.pop_front();
	}
	return frames;
}

void decoder::check_failure()
{
	std::lock_guard lock(mutex);
	if (!failure.empty())
		throw std::runtime_error(failure);
}

void decoder::push_data(std::span<std::span<const uint8_t>> data, uint64_t index, bool)
{
	check_failure();
	if (index != frame_index)
	{
		packet.clear();
		frame_index = index;
	}
	for (auto shard: data)
	{
		if (shard.size() > 32 * 1024 * 1024 - packet.size())
			throw std::runtime_error("GStreamer access unit exceeds 32 MiB");
		packet.insert(packet.end(), shard.begin(), shard.end());
	}
}

void decoder::frame_completed(const from_headset::feedback & feedback,
                              const to_headset::video_stream_data_shard::view_info_t & view_info)
{
	check_failure();
	if (packet.empty() || frame_index != feedback.frame_index)
		return;
	// WiVRn encoders emit start-code-prefixed NAL units, not length-prefixed AVC.
	if (input_codec != av1 && (packet.size() < 4 || packet[0] || packet[1] || (packet[2] != 1 && (packet[2] || packet[3] != 1))))
		throw std::runtime_error("GStreamer expects Annex B access units");
	// Pool allocation starts on the first access unit, rather than PLAYING.
	// Allow bounded startup buffering so 8 frames at 60 Hz (133 ms) do not
	// terminate the session before the media engines have allocated their pools.
	auto now = std::chrono::steady_clock::now();
	if (input_started == std::chrono::steady_clock::time_point{})
		input_started = now;
	const guint64 queue_limit = now - input_started < std::chrono::seconds(2) ? 32 : 8;
	if (gst_app_src_get_current_level_buffers(source) >= queue_limit ||
	    gst_app_src_get_current_level_bytes(source) > 32 * 1024 * 1024 - packet.size())
		throw std::runtime_error("GStreamer encoded queue stalled; reconnect instead of dropping reference pictures");
	auto buffer = gst_buffer_new_allocate(nullptr, packet.size(), nullptr);
	if (!buffer)
		throw std::bad_alloc();
	gst_buffer_fill(buffer, 0, packet.data(), packet.size());
	timestamp += duration;
	GST_BUFFER_PTS(buffer) = timestamp;
	GST_BUFFER_DTS(buffer) = GST_CLOCK_TIME_NONE;
	GST_BUFFER_DURATION(buffer) = duration;
	GST_BUFFER_OFFSET(buffer) = feedback.frame_index;
	{
		std::lock_guard lock(mutex);
		// Parsers can suppress corrupt frames. Bound metadata even in that case.
		while (pending.size() >= 32)
			pending.erase(pending.begin());
		pending.emplace(timestamp, metadata{feedback, view_info, now});
	}
	packet.clear();
	if (gst_app_src_push_buffer(source, buffer) != GST_FLOW_OK)
		throw std::runtime_error("GStreamer appsrc rejected an access unit");
}

void decoder::receive(std::stop_token stop)
{
	bool logged_import = false;
	bool logged_missing_pts = false;
	auto report_started = std::chrono::steady_clock::time_point{};
	uint64_t output_count = 0, ready_dropped = 0;
	double age_sum = 0, age_max = 0, fence_sum = 0, import_sum = 0;
	auto milliseconds = [](auto elapsed) { return std::chrono::duration<double, std::milli>(elapsed).count(); };
	try
	{
		while (!stop.stop_requested())
		{
			auto message = gst_bus_pop_filtered(bus, GstMessageType(GST_MESSAGE_ERROR | GST_MESSAGE_EOS));
			if (message)
			{
				std::string reason = "unexpected EOS";
				if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_ERROR)
				{
					reason = describe_bus_message(message);
				}
				gst_message_unref(message);
				throw std::runtime_error("GStreamer: " + reason);
			}
			auto sample = gst_app_sink_try_pull_sample(sink, 50 * GST_MSECOND);
			if (!sample)
				continue;
			std::unique_ptr<GstSample, decltype(&gst_sample_unref)> sample_owner(sample, gst_sample_unref);
			auto frame = std::make_shared<imported_frame>(std::move(sample_owner));
			metadata meta;
			{
				std::lock_guard lock(mutex);
				auto it = pending.find(GST_BUFFER_PTS(gst_sample_get_buffer(sample)));
				if (it == pending.end())
				{
					if (!logged_missing_pts)
					{
						spdlog::warn("GStreamer output has no matching input PTS: pts={}, pending={}; discarding frame", GST_BUFFER_PTS(gst_sample_get_buffer(sample)), pending.size());
						logged_missing_pts = true;
					}
					continue; // stale/missing metadata must never be guessed
				}
				meta = it->second;
				pending.erase(it);
			}
			auto dma = describe_dmabuf(sample);
			auto fence_started = std::chrono::steady_clock::now();
			wait_producer(dma.fd, stop);
			auto import_started = std::chrono::steady_clock::now();
			try
			{
				import_image(*frame, dma, device, physical_device);
			}
			catch (const std::exception & e)
			{
				throw std::runtime_error(fmt::format("AR24 {}x{}, modifier {:#x}, offset {}, stride {}: {}",
				                                     dma.width,
				                                     dma.height,
				                                     dma.modifier,
				                                     dma.offset,
				                                     dma.stride,
				                                     e.what()));
			}
			if (!logged_import)
			{
				spdlog::info("GStreamer DMA-BUF imported: AR24 {}x{}, modifier {:#x}, offset {}, stride {}, stream {}, frame {}",
				             dma.width,
				             dma.height,
				             dma.modifier,
				             dma.offset,
				             dma.stride,
				             meta.feedback.stream_index,
				             meta.feedback.frame_index);
				logged_import = true;
			}
			{
				std::lock_guard lock(mutex);
				while (ready.size() >= 2)
				{
					ready.pop_front();
					++ready_dropped;
				}
				ready.push_back(std::make_shared<frame_handle>(frame, dma, meta.feedback, meta.view_info));
			}
			if (profile_enabled)
			{
				auto completed = std::chrono::steady_clock::now();
				if (report_started == std::chrono::steady_clock::time_point{})
					report_started = completed;
				auto age = milliseconds(completed - meta.submitted);
				++output_count;
				age_sum += age;
				age_max = std::max(age_max, age);
				fence_sum += milliseconds(import_started - fence_started);
				import_sum += milliseconds(completed - import_started);
				if (completed - report_started >= std::chrono::seconds(5))
				{
					size_t metadata_count, ready_count;
					{
						std::lock_guard lock(mutex);
						metadata_count = pending.size();
						ready_count = ready.size();
					}
					spdlog::info("GStreamer profile: stream={} frame={} output_fps={:.2f} input_to_ready_ms[mean/max]={:.2f}/{:.2f} producer_wait_ms={:.3f} import_and_enqueue_ms={:.3f} encoded_queue={} pending={} ready={} ready_dropped={}",
					             meta.feedback.stream_index, meta.feedback.frame_index, output_count * 1000. / milliseconds(completed - report_started),
					             age_sum / output_count, age_max, fence_sum / output_count, import_sum / output_count,
					             gst_app_src_get_current_level_buffers(source), metadata_count, ready_count, ready_dropped);
					report_started = completed;
					output_count = ready_dropped = 0;
					age_sum = age_max = fence_sum = import_sum = 0;
				}
			}
		}
	}
	catch (const std::exception & e)
	{
		if (!stop.stop_requested())
		{
			{
				std::lock_guard lock(mutex);
				failure = e.what();
			}
			spdlog::error("GStreamer decoder: {}", e.what());
			// The render/network thread observes failure and ends only the stream session.
		}
	}
}
} // namespace wivrn::gstreamer
