/* WiVRn VR streaming
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once
#include <gst/gst.h>
#include <gst/video/video-color.h>
#include <stdexcept>
#include <string_view>

namespace wivrn::gstreamer
{
inline bool direct_nv12_output(const char * value)
{
	if (!value || std::string_view(value) == "ar24")
		return false;
	if (std::string_view(value) == "nv12")
		return true;
	throw std::runtime_error("WIVRN_GSTREAMER_OUTPUT must be ar24 or nv12");
}
inline void set_converter_colorimetry(GstCaps * caps, const char * value)
{
	if (!value || !*value)
		return;
	// Explicit metadata override for streams whose decoder omits colorimetry.
	// This does not convert pixels or change their packing.
	GstVideoColorimetry color{};
	if (!gst_video_colorimetry_from_string(&color, value) ||
	    color.range == GST_VIDEO_COLOR_RANGE_UNKNOWN || color.matrix == GST_VIDEO_COLOR_MATRIX_UNKNOWN ||
	    color.transfer == GST_VIDEO_TRANSFER_UNKNOWN || color.primaries == GST_VIDEO_COLOR_PRIMARIES_UNKNOWN)
		throw std::runtime_error("WIVRN_GSTREAMER_COLORIMETRY must specify a complete GStreamer colorimetry");
	if (color.matrix == GST_VIDEO_COLOR_MATRIX_RGB)
		throw std::runtime_error("NV12 converter input requires a YUV color matrix");
	gst_caps_set_simple(caps, "colorimetry", G_TYPE_STRING, value, nullptr);
}
inline const char * converter_input_caps(bool copy)
{
	// MMAP input advertises regular raw video caps, not memory:DMABuf.
	return copy ? "video/x-raw,format=NV12" : "video/x-raw(memory:DMABuf),format=DMA_DRM,drm-format=NV12";
}
inline bool converter_input_copy(const char * value)
{
	if (!value || std::string_view(value) == "dmabuf")
		return false;
	if (std::string_view(value) == "copy")
		return true;
	throw std::runtime_error("WIVRN_GSTREAMER_CONVERTER_INPUT must be dmabuf or copy");
}
} // namespace wivrn::gstreamer
