/* WiVRn VR streaming
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once
#include <gst/video/video-info.h>
#include <stdexcept>
#include <string_view>
#include <vulkan/vulkan.hpp>

namespace wivrn::gstreamer
{
inline vk::SamplerYcbcrConversionCreateInfo nv12_conversion_info(const GstVideoInfo & info, vk::FormatFeatureFlags features)
{
	// The shared reprojection shader decodes sRGB; do not silently reinterpret
	// HDR/PQ, other primaries, or unknown chroma location as this 8-bit SDR path.
	if (info.colorimetry.transfer != GST_VIDEO_TRANSFER_SRGB || info.colorimetry.primaries != GST_VIDEO_COLOR_PRIMARIES_BT709)
		throw std::runtime_error("NV12 import requires explicit sRGB transfer and BT.709 primaries (colorimetry override available)");
	vk::SamplerYcbcrModelConversion model;
	switch (info.colorimetry.matrix)
	{
		case GST_VIDEO_COLOR_MATRIX_BT709:
			model = vk::SamplerYcbcrModelConversion::eYcbcr709;
			break;
		case GST_VIDEO_COLOR_MATRIX_BT601:
			model = vk::SamplerYcbcrModelConversion::eYcbcr601;
			break;
		default:
			throw std::runtime_error("NV12 import requires a supported YUV matrix");
	}
	if (info.colorimetry.range != GST_VIDEO_COLOR_RANGE_0_255 && info.colorimetry.range != GST_VIDEO_COLOR_RANGE_16_235)
		throw std::runtime_error("NV12 import requires explicit quantization range");
	if (info.chroma_site != GST_VIDEO_CHROMA_SITE_JPEG && info.chroma_site != GST_VIDEO_CHROMA_SITE_MPEG2 && info.chroma_site != GST_VIDEO_CHROMA_SITE_COSITED)
		throw std::runtime_error("NV12 import requires explicit chroma-site (set WIVRN_GSTREAMER_CHROMA_SITE=mpeg2 for the WiVRn H.264 encoder)");
	auto location = [features](bool cosited) {
		auto required = cosited ? vk::FormatFeatureFlagBits::eCositedChromaSamples : vk::FormatFeatureFlagBits::eMidpointChromaSamples;
		if (!(features & required))
			throw std::runtime_error("NV12 DRM modifier does not support negotiated chroma location");
		return cosited ? vk::ChromaLocation::eCositedEven : vk::ChromaLocation::eMidpoint;
	};
	bool linear = bool(features & vk::FormatFeatureFlagBits::eSampledImageFilterLinear) && bool(features & vk::FormatFeatureFlagBits::eSampledImageYcbcrConversionLinearFilter);
	vk::SamplerYcbcrConversionCreateInfo result{};
	result.format = vk::Format::eG8B8R82Plane420Unorm;
	result.ycbcrModel = model;
	result.ycbcrRange = info.colorimetry.range == GST_VIDEO_COLOR_RANGE_0_255 ? vk::SamplerYcbcrRange::eItuFull : vk::SamplerYcbcrRange::eItuNarrow;
	result.xChromaOffset = location(info.chroma_site & GST_VIDEO_CHROMA_SITE_H_COSITED);
	result.yChromaOffset = location(info.chroma_site & GST_VIDEO_CHROMA_SITE_V_COSITED);
	result.chromaFilter = linear ? vk::Filter::eLinear : vk::Filter::eNearest;
	return result;
}
inline vk::Format rgb_import_format(bool shader_srgb_conversion, const char * override_format)
{
	if (override_format && std::string_view(override_format) != "auto")
	{
		if (std::string_view(override_format) == "srgb")
			return vk::Format::eB8G8R8A8Srgb;
		if (std::string_view(override_format) == "unorm")
			return vk::Format::eB8G8R8A8Unorm;
		throw std::runtime_error("WIVRN_GSTREAMER_RGB_FORMAT must be auto, unorm or srgb");
	}
	// RGA outputs nonlinear RGB bytes. Convert them to linear exactly once:
	// either in the reprojection shader or through sRGB texture sampling.
	return shader_srgb_conversion ? vk::Format::eB8G8R8A8Unorm : vk::Format::eB8G8R8A8Srgb;
}
} // namespace wivrn::gstreamer
