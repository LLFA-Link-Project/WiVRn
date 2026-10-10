/* WiVRn VR streaming
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "dmabuf.h"
#include <drm_fourcc.h>
#include <gst/allocators/gstdmabuf.h>
#include <gst/video/gstvideometa.h>
#include <gst/video/video-info-dma.h>
#include <stdexcept>

namespace wivrn::gstreamer
{
dmabuf_frame describe_dmabuf(GstSample * sample)
{
	GstVideoInfoDmaDrm info;
	gst_video_info_dma_drm_init(&info);
	auto caps = gst_sample_get_caps(sample);
	auto buffer = gst_sample_get_buffer(sample);
	if (!caps || !buffer || !gst_video_is_dma_drm_caps(caps) || !gst_video_info_dma_drm_from_caps(&info, caps))
		throw std::runtime_error("GStreamer output is not negotiated DMA_DRM memory:DMABuf");
	if (info.drm_fourcc != DRM_FORMAT_ARGB8888 || info.drm_modifier == DRM_FORMAT_MOD_INVALID)
		throw std::runtime_error("GStreamer requires AR24 with an explicit, valid DRM modifier");
	// VideoMeta describes the actual allocation, including padding; caps strides do not.
	auto meta = gst_buffer_get_video_meta(buffer);
	if (!meta || meta->n_planes != 1 || meta->stride[0] <= 0 || !meta->width || !meta->height ||
	    meta->width != uint32_t(info.vinfo.width) || meta->height != uint32_t(info.vinfo.height))
		throw std::runtime_error("AR24 DMA-BUF requires matching single-plane GstVideoMeta");
	guint index, length;
	gsize skip;
	if (!gst_buffer_find_memory(buffer, meta->offset[0], 1, &index, &length, &skip) || length != 1)
		throw std::runtime_error("Cannot locate AR24 DMA-BUF plane");
	auto memory = gst_buffer_peek_memory(buffer, index);
	if (!gst_is_dmabuf_memory(memory))
		throw std::runtime_error("GStreamer output memory is not a DMA-BUF");
	gsize memory_offset, max_size;
	auto memory_size = gst_memory_get_sizes(memory, &memory_offset, &max_size);
	if (memory_offset > max_size || skip >= max_size - memory_offset)
		throw std::runtime_error("Invalid DMA-BUF plane offset");
	if (info.drm_modifier == DRM_FORMAT_MOD_LINEAR)
	{
		uint64_t row_bytes = uint64_t(meta->width) * 4;
		uint64_t required = uint64_t(meta->height - 1) * uint64_t(meta->stride[0]) + row_bytes;
		if (uint64_t(meta->stride[0]) < row_bytes || skip > memory_size || required > memory_size - skip)
			throw std::runtime_error("AR24 linear plane extends beyond its DMA-BUF memory");
	}
	return {gst_dmabuf_memory_get_fd(memory), info.drm_fourcc, info.drm_modifier, meta->width, meta->height, memory_offset + skip, uint64_t(meta->stride[0])};
}
} // namespace wivrn::gstreamer
