/* WiVRn VR streaming
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once
#include <stdexcept>
#include <string_view>
#include <vulkan/vulkan.hpp>

namespace wivrn::gstreamer
{
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
