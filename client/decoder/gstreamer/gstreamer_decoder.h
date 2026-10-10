/* WiVRn VR streaming
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once
#include "decoder/decoder.h"
#include <chrono>
#include <deque>
#include <gst/app/gstappsink.h>
#include <gst/app/gstappsrc.h>
#include <map>
#include <mutex>
#include <thread>
#include <vector>

namespace wivrn::gstreamer
{
class decoder : public wivrn::decoder
{
	struct metadata
	{
		from_headset::feedback feedback;
		to_headset::video_stream_data_shard::view_info_t view_info;
		std::chrono::steady_clock::time_point submitted;
	};
	vk::raii::Device & device;
	vk::raii::PhysicalDevice & physical_device;
	vk::raii::Sampler rgb_sampler = nullptr;
	vk::Format imported_format = vk::Format::eUndefined;
	GstElement * pipeline = nullptr;
	GstAppSrc * source = nullptr; // borrowed from pipeline
	GstAppSink * sink = nullptr;
	GstBus * bus = nullptr;
	std::jthread worker;
	std::mutex mutex;
	std::map<GstClockTime, metadata> pending;
	std::string failure;
	std::deque<std::shared_ptr<blit_handle>> ready;
	std::vector<uint8_t> packet;
	video_codec input_codec = h264;
	uint64_t frame_index = UINT64_MAX;
	GstClockTime timestamp = 0, duration;
	std::chrono::steady_clock::time_point input_started{};
	bool profile_enabled = false;
	std::chrono::steady_clock::time_point delivery_started{};
	uint64_t delivery_count = 0;
	double delivery_age_sum = 0, delivery_age_max = 0;
	void receive(std::stop_token stop);
	void check_failure();

public:
	decoder(vk::raii::Device &, vk::raii::PhysicalDevice &, const to_headset::video_stream_description &, uint8_t, std::weak_ptr<scenes::stream>, shard_accumulator *);
	~decoder() override;
	void push_data(std::span<std::span<const uint8_t>>, uint64_t, bool) override;
	void frame_completed(const from_headset::feedback &, const to_headset::video_stream_data_shard::view_info_t &) override;
	std::vector<std::shared_ptr<blit_handle>> drain_frames() override;
	vk::Sampler sampler() override
	{
		return *rgb_sampler;
	}
	static void supported_codecs(std::vector<video_codec> &);
};
} // namespace wivrn::gstreamer
