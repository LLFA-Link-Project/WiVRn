/* WiVRn VR streaming
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once
#include <cstdint>
#include <gst/gst.h>

namespace wivrn::gstreamer
{
// The borrowed fd remains valid while the sample is referenced. No pixel mapping.
struct dmabuf_frame
{
	int fd;
	uint32_t fourcc;
	uint64_t modifier;
	uint32_t width, height;
	uint64_t offset, stride;
};
dmabuf_frame describe_dmabuf(GstSample * sample);
} // namespace wivrn::gstreamer
