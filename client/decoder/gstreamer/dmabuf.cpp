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
dmabuf_frame describe_dmabuf(GstSample * sample, bool nv12)
{
	GstVideoInfoDmaDrm info;
	gst_video_info_dma_drm_init(&info);
	auto caps = gst_sample_get_caps(sample);
	auto buffer = gst_sample_get_buffer(sample);
	if (!caps || !buffer || !gst_video_is_dma_drm_caps(caps) || !gst_video_info_dma_drm_from_caps(&info, caps))
		throw std::runtime_error("GStreamer output is not negotiated DMA_DRM memory:DMABuf");
	if (info.drm_fourcc != (nv12 ? DRM_FORMAT_NV12 : DRM_FORMAT_ARGB8888) || info.drm_modifier == DRM_FORMAT_MOD_INVALID)
		throw std::runtime_error("GStreamer output fourcc/modifier does not match selected 8-bit import format");
	// VideoMeta describes the actual allocation, including padding; caps strides do not.
	auto meta = gst_buffer_get_video_meta(buffer);
	if (!meta || meta->n_planes != (nv12 ? 2u : 1u) || !meta->width || !meta->height ||
	    meta->format != (nv12 ? GST_VIDEO_FORMAT_NV12 : GST_VIDEO_FORMAT_BGRA) ||
	    meta->width != uint32_t(info.vinfo.width) || meta->height != uint32_t(info.vinfo.height))
		throw std::runtime_error("DMA-BUF requires matching GstVideoMeta dimensions and plane count");
	dmabuf_frame result{.fd = -1, .fourcc = info.drm_fourcc, .modifier = info.drm_modifier, .width = meta->width, .height = meta->height, .offset = 0, .stride = 0, .plane_count = meta->n_planes};
	uint64_t luma_end = 0;
	for (uint32_t plane = 0; plane < meta->n_planes; ++plane)
	{
		if (meta->stride[plane] <= 0)
			throw std::runtime_error("DMA-BUF plane has invalid stride");
		guint index, length;
		gsize skip;
		if (!gst_buffer_find_memory(buffer, meta->offset[plane], 1, &index, &length, &skip) || length != 1)
			throw std::runtime_error("Cannot locate DMA-BUF plane");
		auto memory = gst_buffer_peek_memory(buffer, index);
		if (!gst_is_dmabuf_memory(memory))
			throw std::runtime_error("GStreamer output memory is not a DMA-BUF");
		gsize memory_offset, max_size;
		auto memory_size = gst_memory_get_sizes(memory, &memory_offset, &max_size);
		if (memory_offset > max_size || skip >= max_size - memory_offset)
			throw std::runtime_error("Invalid DMA-BUF plane offset");
		if (info.drm_modifier == DRM_FORMAT_MOD_LINEAR)
		{
			uint64_t row_bytes = nv12 ? (plane ? ((uint64_t(meta->width) + 1) / 2) * 2 : meta->width) : uint64_t(meta->width) * 4;
			uint64_t rows = plane ? (uint64_t(meta->height) + 1) / 2 : meta->height;
			uint64_t required = (rows - 1) * uint64_t(meta->stride[plane]) + row_bytes;
			if (uint64_t(meta->stride[plane]) < row_bytes || skip > memory_size || required > memory_size - skip)
				throw std::runtime_error("Linear plane extends beyond its DMA-BUF memory");
			if (plane == 0)
				luma_end = memory_offset + skip + required;
			else if (memory_offset + skip < luma_end)
				throw std::runtime_error("NV12 linear chroma plane overlaps luma");
		}
		auto fd = gst_dmabuf_memory_get_fd(memory);
		if (result.fd >= 0 && result.fd != fd)
			throw std::runtime_error("NV12 direct import currently requires both planes in the same DMA-BUF fd");
		result.fd = fd;
		result.offsets[plane] = memory_offset + skip;
		result.strides[plane] = meta->stride[plane];
	}
	result.offset = result.offsets[0];
	result.stride = result.strides[0];
	return result;
}
} // namespace wivrn::gstreamer
