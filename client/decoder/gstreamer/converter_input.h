/* WiVRn VR streaming
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once
#include <stdexcept>
#include <string_view>

namespace wivrn::gstreamer
{
inline bool converter_input_copy(const char * value)
{
	if (!value || std::string_view(value) == "dmabuf")
		return false;
	if (std::string_view(value) == "copy")
		return true;
	throw std::runtime_error("WIVRN_GSTREAMER_CONVERTER_INPUT must be dmabuf or copy");
}
} // namespace wivrn::gstreamer
