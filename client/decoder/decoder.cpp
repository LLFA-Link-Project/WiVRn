/*
 * WiVRn VR streaming
 * Copyright (C) 2025  Patrick Nicolas <patricknicolas@laposte.net>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include "decoder.h"

#ifdef __ANDROID__
#include "decoder/android/android_decoder.h"
#else
#if WIVRN_USE_V4L2
#include "decoder/v4l2/v4l2_decoder.h"
#endif
#if !WIVRN_USE_V4L2 || WIVRN_USE_GSTREAMER
#include "decoder/ffmpeg/ffmpeg_decoder.h"
#endif
#if WIVRN_USE_GSTREAMER
#include "decoder/gstreamer/gstreamer_decoder.h"
#endif
#endif
#include <algorithm>
#include <cstdlib>
#include <spdlog/spdlog.h>
#include <string_view>

#ifndef __ANDROID__
namespace
{
std::string_view selected_backend()
{
	const char * value = std::getenv("WIVRN_DECODER");
	std::string_view selected = value ? value : "auto";
	if (selected != "auto" && selected != "gstreamer" && selected != "v4l2" && selected != "ffmpeg")
		throw std::runtime_error("WIVRN_DECODER must be auto, gstreamer, v4l2, or ffmpeg");
	return selected;
}
} // namespace
#endif
#include "decoder/raw_decoder.h"

wivrn::decoder::~decoder() = default;

std::shared_ptr<wivrn::decoder> wivrn::decoder::make(
        vk::raii::Device & device,
        vk::raii::PhysicalDevice & phys_dev,
        uint32_t vk_queue_family_index,
        const wivrn::to_headset::video_stream_description & description,
        uint8_t stream_index,
        std::weak_ptr<scenes::stream> scene,
        shard_accumulator * acc)
{
	switch (description.codec[stream_index])
	{
		case h264:
		case h265:
		case av1:
#ifdef __ANDROID__
			return std::make_shared<wivrn::android::decoder>(
			        device,
			        phys_dev,
			        description,
			        stream_index,
			        scene,
			        acc);
#else
		{
			auto selected = selected_backend();
#if WIVRN_USE_GSTREAMER
			if (selected == "auto" || selected == "gstreamer")
			{
				try
				{
					return std::make_shared<wivrn::gstreamer::decoder>(device, phys_dev, description, stream_index, scene, acc);
				}
				catch (const std::exception & e)
				{
					if (selected == "gstreamer")
						throw;
					spdlog::warn("GStreamer unavailable: {}; trying another decoder", e.what());
				}
			}
#endif
#if WIVRN_USE_V4L2
			if (selected == "auto" || selected == "v4l2")
			{
				try
				{
					return std::make_shared<wivrn::v4l2::decoder>(device, phys_dev, vk_queue_family_index, description, stream_index, scene, acc);
				}
				catch (const std::exception & e)
				{
					if (selected == "v4l2" || !WIVRN_USE_GSTREAMER)
						throw;
					spdlog::warn("Iris unavailable: {}; trying software decoder", e.what());
				}
			}
#endif
#if !WIVRN_USE_V4L2 || WIVRN_USE_GSTREAMER
			if (selected == "auto" || selected == "ffmpeg")
				return std::make_shared<wivrn::ffmpeg::decoder>(device, phys_dev, description, stream_index, scene, acc);
#endif
			throw std::runtime_error("Requested decoder backend is not compiled into this client");
		}
#endif
		case raw:
			return std::make_shared<wivrn::raw_decoder>(
			        device,
			        phys_dev,
			        vk_queue_family_index,
			        description,
			        stream_index,
			        scene,
			        acc);
	}
	__builtin_unreachable();
}

static std::vector<wivrn::video_codec_capability> supported_codecs_()
{
	std::vector<wivrn::video_codec> codecs;
#ifdef __ANDROID__
	wivrn::android::decoder::supported_codecs(codecs);
#else
	auto selected = selected_backend();
#if WIVRN_USE_GSTREAMER
	if (selected == "gstreamer" || selected == "auto")
		wivrn::gstreamer::decoder::supported_codecs(codecs);
#endif
#if WIVRN_USE_V4L2
	if (selected == "v4l2" || selected == "auto")
		wivrn::v4l2::decoder::supported_codecs(codecs);
#endif
#if !WIVRN_USE_V4L2 || WIVRN_USE_GSTREAMER
	if (selected == "ffmpeg" || selected == "auto")
		wivrn::ffmpeg::decoder::supported_codecs(codecs);
#endif
	if (codecs.empty() && selected != "auto")
		spdlog::warn("Requested decoder backend {} has no supported compressed codecs on this device", selected);
	std::ranges::sort(codecs);
	codecs.erase(std::unique(codecs.begin(), codecs.end()), codecs.end());
#endif
	codecs.push_back(wivrn::video_codec::raw);

	std::vector<wivrn::video_codec_capability> res;
	res.reserve(codecs.size());
	for (auto codec: codecs)
		res.push_back({.codec = codec, .supports_10bit = wivrn::decoder::supports_10bit(codec)});
	return res;
}

const std::vector<wivrn::video_codec_capability> & wivrn::decoder::supported_codecs()
{
	static std::vector<wivrn::video_codec_capability> res = supported_codecs_();
	return res;
}

bool wivrn::decoder::supports_10bit(wivrn::video_codec codec)
{
#ifdef __ANDROID__
	return codec == wivrn::video_codec::h265 or codec == wivrn::video_codec::av1;
#else
	auto selected = selected_backend();
#if WIVRN_USE_GSTREAMER
	// AR24 is an 8-bit output. Auto may select it, so advertise conservatively.
	if (selected == "gstreamer" || selected == "auto")
		return false;
#endif
#if WIVRN_USE_V4L2
	if (selected == "v4l2" || selected == "auto")
		return wivrn::v4l2::decoder::supports_10bit(codec);
#endif
#if !WIVRN_USE_V4L2 || WIVRN_USE_GSTREAMER
	if (selected == "ffmpeg" || selected == "auto")
		return codec == wivrn::video_codec::h265 || codec == wivrn::video_codec::av1;
#endif
	return false;
#endif
}
