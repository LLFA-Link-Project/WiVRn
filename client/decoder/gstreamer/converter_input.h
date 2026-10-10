/* WiVRn VR streaming
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once
#include <stdexcept>
#include <string_view>

namespace wivrn::gstreamer
{
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
