# Linux GStreamer V4L2 Stateless decoder

This optional backend accepts WiVRn H.264/HEVC/AV1 access units and imports the
converter's AR24 DMA-BUF output into Vulkan. It uses the installed GStreamer
V4L2 devices, with no Rockchip device paths or driver names compiled in.
Orange Pi 5 startup logs verify H.264 decoding, RGA AR24 DMA-BUF output, and
PanVK import and headset display in the explicit converter input copy mode.
The user also confirmed correct headset colors after the single sRGB conversion
fix (commit `2189ec84`). The accompanying Orange Pi log verified H.264 8-bit,
1280x1280 per eye, MMAP converter input, AR24 DMA-BUF output, UNORM sampling with
shader conversion, and the `1:3:7:1` colorimetry override. Stable output intervals
were approximately 59 fps. This does not verify other codecs, other colorimetry
settings, end-to-end latency, brief blackouts, or long-term stability; this is
not yet a fully validated hardware path.

For timing diagnostics, set `WIVRN_GSTREAMER_PROFILE=1`. Every five seconds of
output, each decoder reports output FPS, access-unit submission to frame-ready
age (including buffering, decoding, conversion, and import), producer fence wait,
import/enqueue time, and queue occupancy/drop counts. These are CPU wall-clock
measurements, not GPU execution times. Reporting starts at the first output so
connection/setup waiting does not skew the initial FPS. An inactive stream
produces no report; a later interval containing an application switch can still
have low output FPS.

`GStreamer delivery` reports frames collected by the render thread and time spent
in the decoded ready queue. This is not headset presentation FPS or total
motion-to-photon latency. `converter-input caps` reports negotiated caps after the
optional colorimetry constraint; `decoded caps` is recorded before that filter.

`WIVRN_GSTREAMER_COLORIMETRY` explicitly constrains the converter's NV12 input
color metadata for comparison when the decoder omits it. The default leaves
negotiation unchanged. For full-range BT.709 matrix, sRGB transfer, BT.709
primaries (matching the local x264 encoder), use `1:3:7:1`. This only specifies
metadata: it cannot repair an incorrect buffer layout or force a driver to
implement unsupported color conversions. RGB and incomplete colorimetry are
rejected. Do not use this override for a stream with different color properties.

AR24 import selects `B8G8R8A8Unorm` when the existing reprojection shader performs
sRGB-to-linear conversion, and `B8G8R8A8Srgb` otherwise. Importing an sRGB texture
and then applying that shader conversion would convert twice and darken the
image. The DRM fourcc, byte order, and producer buffers are unchanged. Runtime
modifier/import/filter capability checks apply to the selected Vulkan format.
The `RGB sampling` startup log records this choice. For a comparison with the
previous import behavior, set `WIVRN_GSTREAMER_RGB_FORMAT=srgb`; `auto` (the
default) and `unorm` are also accepted. This changes only the GStreamer backend;
other decoders and the shared reprojection shader are unchanged. Corrected
color appearance was verified in the Orange Pi configuration described above.

## Brief blackout comparison

The Orange Pi log's Monado revision `f2222d155` matches the local Monado source.
In that source, `oxr_session_frame_begin` discards the preceding begun frame when
it was not ended. `multi_compositor_discard_frame` also clears the **delivered**
layer slot, and `transfer_layers_locked` omits clients without an active delivered
slot. WiVRn normally leaves a repeated video frame begun but unended, expecting
the runtime to keep time-warping the preceding image. These two behaviors can
remove the displayed content until a new frame is delivered. This is a concrete
candidate for recurring blackouts, not yet a headset-verified cause.

Set `WIVRN_STREAM_DISCARD_FRAMES=0` to disable only that repeated-frame rendering
shortcut. WiVRn then samples its retained decoded images and submits the projection
layer again, using the existing GPU fence and FOREIGN ownership barriers. This
does not disable Monado time warp or change decoding, color conversion, frame
selection, or menu controls. It increases rendering/submission work on repeats.
Unset the variable (or use `1`) for the existing behavior; it never enables
discarding on a headset whose traits already prohibit it.

For either comparison, `WIVRN_STREAM_PROFILE=1` reports every five seconds:
`rendered` counts completed `render_end` calls, `discarded` counts repeated-frame
shortcuts (no `xrEndFrame`), and `empty` counts explicit empty submissions
(`shouldRender` false, missing stereo frames, or stream shutdown). These are
application decisions, not KMS scanout measurements. Reports are disabled by
default. Test both modes with the same VR application and hidden menu for at
least 60 seconds after startup. If disabling discard eliminates blackouts while
`discarded=0` and `empty=0`, that supports this cause. If they persist, continue
investigating decoded pixels, DMA-BUF sampling, and compositor presentation.

## Build and select

Use a separate build directory; preserve the working software-decoder build.
The dependencies are GStreamer >= 1.26 (`gstreamer-1.0`, `gstreamer-app-1.0`,
`gstreamer-video-1.0`, `gstreamer-allocators-1.0`), libdrm headers, and the usual
WiVRn client dependencies including FFmpeg. At runtime, `appsrc`, `appsink`,
a codec parser and V4L2 Stateless decoder, and `v4l2convert` must
be registered. GStreamer registers hardware factories based on available devices;
check permissions and `gst-inspect-1.0` on the machine running the client.

```sh
cmake -S . -B build-client-gstreamer -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DWIVRN_BUILD_CLIENT=ON -DWIVRN_BUILD_SERVER=OFF \
  -DWIVRN_BUILD_SERVER_LIBRARY=OFF -DWIVRN_BUILD_DASHBOARD=OFF \
  -DWIVRN_BUILD_WIVRNCTL=OFF \
  -DWIVRN_USE_GSTREAMER=ON -DWIVRN_USE_V4L2=OFF
cmake --build build-client-gstreamer --parallel 4

WIVRN_DECODER=gstreamer \
XR_RUNTIME_JSON=/home/reina/monado/build/openxr_monado-dev.json \
./build-client-gstreamer/bin/wivrn
```

`WIVRN_USE_GSTREAMER` defaults to OFF and is Linux-only. Android continues to use
its existing backend. `WIVRN_USE_V4L2` still controls the Iris implementation;
both hardware backends can be built together. Enabling GStreamer also builds
FFmpeg so that `WIVRN_DECODER=ffmpeg` always provides an explicit software path.
`WIVRN_DECODER=v4l2` selects Iris; `gstreamer` strictly selects the new backend.
The default `auto` tries GStreamer, then Iris if compiled, then FFmpeg when
constructor failures permit fallback. An asynchronous negotiation/import failure
ends the streaming session and logs the reason; it does **not** silently switch
samplers or decoder formats midstream. Reconnect with `WIVRN_DECODER=ffmpeg`.

## Input and frame identity

The current encoders produce Annex B NAL units (x264's `x264_nal_encode`, NVENC's
SPS/PPS repetition, and the existing decoder interfaces consume those same bytes).
The backend rejects a length-prefixed AVC input instead of guessing its format.
`shard_accumulator` calls `push_data` with partial contiguous shard groups and
`frame_completed` only once a complete access unit and its `view_info` exist.
The backend assembles that access unit, retaining SPS/PPS/VPS and IDR bytes.
GStreamer parsers process those headers; no separate out-of-band codec data is
invented. Server x264 uses the `zerolatency` preset.

A synthetic monotonic PTS identifies each submitted access unit. A bounded map
associates that PTS with the original feedback (including stream/frame index and
server timing) and view information. Output is matched by PTS, not decode count,
FIFO position, or a guessed display timestamp. Missing/stale metadata causes an
output to be discarded. Each decoder/pipeline has its own PTS map, reset on stream
recreation. Resolution changes in samples create fresh Vulkan images with their
actual metadata dimensions; a changed stream description recreates the decoder.

Decoded frames are collected on the render thread both while the lobby waits
for the first stereo frames and while the stream scene is active. The lobby must
poll before checking stream readiness: waiting for stream rendering to collect
the first frames would prevent the scene from ever becoming ready.

## Memory and synchronization

The pipeline is:

```
appsrc -> codec parser -> codec V4L2 Stateless decoder
       -> DMA_DRM NV12 (8-bit) memory:DMABuf
       -> v4l2convert output-io-mode=dmabuf-import capture-io-mode=dmabuf
       -> DMA_DRM AR24 memory:DMABuf -> appsink
```

Decoder-to-converter DMA-BUF caps and explicit converter import/export modes
prevent an automatic system-memory conversion path. In the default strict import
mode there is no `videoconvert`, pixel `gst_buffer_map`, staging image, or CPU
decoded pixel copy in this backend. Encoded input is copied into a GstBuffer. The
converter writes a new RGB allocation; consequently the entire decode/conversion
pipeline is not claimed to involve zero hardware copies.

### Explicit converter input copy fallback

The Orange Pi startup log showed RGA negotiating `NM12` with two V4L2 memory
planes while rkvdec supplied one DMA-BUF containing both NV12 image planes.
GStreamer 1.26's importer rejected this with `Got 1 dmabuf but needed 2`, before
any AR24 sample or Vulkan import. Its importer requires the GstMemory count to
match the V4L2 memory-plane count. This is distinct from the image-plane count.
Simply duplicating the fd is unsafe: the mainline RGA buffer mapping does not
apply each plane's `data_offset` to its MMU descriptors.

An explicit comparison/workaround is available:

```bash
WIVRN_DECODER=gstreamer WIVRN_GSTREAMER_CONVERTER_INPUT=copy \
GST_DEBUG_NO_COLOR=1 GST_DEBUG='2,v4l2*:6,h264parse:5' \
XR_RUNTIME_JSON=/home/reina/monado/build/openxr_monado-dev.json \
./build-client-gstreamer/bin/wivrn > /tmp/wivrn-gst-copy.log 2>&1
```

`copy` selects the converter's MMAP input pool and negotiates ordinary
`video/x-raw,format=NV12` on decoder output. GStreamer 1.26 does not advertise
`memory:DMABuf` input caps for explicit MMAP mode; forcing DMA_DRM there causes
`not-negotiated` before decoding, as observed in `4def2bfd` on Orange Pi.
The decoder's raw NV12 buffers may be mapped or copied; no DMA-BUF-only contract
is imposed on this explicit fallback. GStreamer copies the decoded NV12 pixels
into the converter pool; RGA still converts NV12 to AR24, and AR24
output still requires DMA-BUF export and Vulkan import. This is **not** a
zero-copy decoder-to-RGA path and its hardware operation/performance remain
unverified. The default/unset value and `dmabuf` keep strict DMA-BUF import;
there is no silent copy fallback. Unknown values are rejected. Hardware-driver
or GStreamer negotiation/import support for contiguous NV12 is still needed to
make the strict path work on this device. Other layouts/devices may already work.

Startup logs now include each stage's negotiated caps and first buffer, the
converter input mode, and bus warnings/errors with their emitting element.
These distinguish decoding, conversion, PTS matching and Vulkan import failures.

Sources: [GStreamer 1.26.2 DMA-BUF importer](https://github.com/GStreamer/gstreamer/blob/1.26.2/subprojects/gst-plugins-good/sys/v4l2/gstv4l2allocator.c),
[mainline Linux 6.18 RGA buffer mapping](https://github.com/torvalds/linux/blob/v6.18/drivers/media/platform/rockchip/rga/rga-buf.c).

`GstVideoInfoDmaDrm` reads the negotiated fourcc/modifier, and `GstVideoMeta`
provides the actual width, height, plane offset and stride. Memory suballocation
offsets are added to the plane's offset. Only single-plane AR24 is accepted.
AR24 caps without a modifier mean LINEAR **by the GStreamer caps contract**, not
by an assumption about the hardware. Other modifiers can be described by the
layout helper, but this first pipeline explicitly requests AR24/LINEAR. Vulkan
queries must confirm this modifier supports single-plane sampled BGRA sRGB with
linear filtering and DMA-BUF import. Unsupported layouts fail with the negotiated
modifier, size, stride and offset in the diagnostic.

The Vulkan device must have enabled:

- `VK_KHR_external_memory_fd`
- `VK_EXT_external_memory_dma_buf`
- `VK_EXT_image_drm_format_modifier`
- `VK_EXT_queue_family_foreign`

Producer completion is checked by exporting the DMA-BUF reservation's READ
sync_file (`DMA_BUF_IOCTL_EXPORT_SYNC_FILE`) and polling that fence on the decoder
worker. This is a CPU wait for the producer, not a pixel copy. An unsupported
ioctl or unsignalled fence is an explicit failure, not an implicit assumption of
synchronization. GPU semaphore import for this wait is a future optimization.

Only a `F_DUPFD_CLOEXEC` duplicate is transferred to Vulkan; the original FD
continues to belong to GstMemory. Failed imports close the duplicate. Each frame
owns its image/view/memory and retains the GstSample independently of the decoder.
The worker never acquires scene locks. The render thread drains at most two queued
frames; the scene retains the selected handles through the graphics fence.
Existing FOREIGN-to-graphics acquire and graphics-to-FOREIGN release barriers
surround sampling. Frame destruction destroys the Vulkan view/image/memory before
unreffing the sample, so the GStreamer pool cannot reuse it while GPU reads remain
in flight. Stream recreation and destruction wait for the graphics queue before
releasing decoder samplers. The appsink allocation proposal requests a pool of at
least 12 buffers to accommodate scene retention and media-engine processing.

The existing Iris import remains separate: it preallocates a two-plane NV12/P010
capture pool before V4L2 writes into it, whereas this backend imports completed
single-plane RGB samples. Sharing those allocation/lifetime paths would enlarge
the change substantially. The renderer's external ownership handling is reused.

## Bounds and failures

Appsrc input is limited to 32 queued access units during the first two seconds
after input begins (media-engine pool allocation), then eight; individual units are limited
to 32 MiB. A stalled encoded queue ends the session rather than discarding reference
pictures. Appsink retains two samples and drops older decoded output; the worker's
ready queue also retains at most two frames. Pending metadata retains 32 entries.
The stream's existing three-frame rolling history remains intact. The GStreamer
pipeline contains no additional unbounded queue element.

Bus errors, unexpected EOS, producer fence failure and import failure are caught
on the worker and reported on the render/network threads. The existing stream
shutdown returns to the lobby. Stop requests interrupt sample/fence polling;
pipeline teardown follows worker termination. Samples still retained by graphics
remain referenced after the pipeline stops.

## Validation and remaining hardware work

Local validation uses x86_64, GStreamer 1.28.7 and Vulkan headers 1.4.363, without
`/dev/dri`, RK3588, or a registered `v4l2convert` factory. Local build checks (RelWithDebInfo, client warnings treated as errors):

| GStreamer | Iris | Result |
| --- | --- | --- |
| ON | ON | Full client build and link passed; includes FFmpeg |
| ON | OFF | Full client build and link passed; final `build-gstreamer` configuration |
| OFF | OFF | CMake configuration and syntax compilation of decoder selection, application and stream passed; no full client link in this check |

CTest passed 3/3 (client pose, client gaze, GStreamer DMA-BUF/parser tests).
All three parser factories were present for the local codec fixture tests.
The existing `build-client/bin/wivrn` was not rebuilt or replaced. None of these
checks establish hardware decoder, producer-fence or Vulkan import operation.

 The layout test uses a memfd wrapped
by GstDmaBufAllocator to test descriptor extraction and FD lifetime; this **does
not** establish that the FD is importable by Vulkan. It checks padded offsets,
strides, shared-memory offsets, LINEAR and non-LINEAR metadata, rejection of invalid strides and P010, PTS/frame
identity through appsrc/appsink, H.264 SPS/PPS/IDR, HEVC VPS/SPS/PPS and AV1 sequence-header parsing with PTS preservation, EOS, and pipeline shutdown.
The explicit converter copy path is also checked with GStreamer's actual
`gst_video_frame_map`/`gst_video_frame_copy`: padded NV12 in one memfd-backed
DMA-BUF is copied into two destination memories with different strides. Every
Y/UV pixel and source FD release are verified. This tests memory-layout handling;
it does not run a V4L2 pool, RGA, hardware fences, or Vulkan.

On Orange Pi, first verify plugin registration, select H.264 on the WiVRn server,
and run with `WIVRN_DECODER=gstreamer GST_DEBUG=2`. Confirm the decoder startup log,
actual caps with `GST_DEBUG=GST_CAPS:5`, and successful displayed frames. Compare
against `WIVRN_DECODER=ffmpeg` using identical resolution/server settings. Then
exercise disconnect/reconnect, changed resolution, and a sustained stream while
checking FD counts, memory, frame feedback and visual correctness/3DoF tracking.
Producer-fence support, PanVK modifier import, converter buffer-pool sizing, color
conversion, throughput and end-to-end latency all still require this real device.

NV12 direct import, 10-bit output, import caching, and
hot fallback after asynchronous failure are not implemented. HEVC and AV1 use the same
output path but are untested on hardware, including their server bitstream framing. Automatic selection conservatively
advertises 8-bit output; explicit FFmpeg preserves its existing 10-bit capability.

References: [GStreamer DMA-BUF caps and metadata](https://gstreamer.freedesktop.org/documentation/additional/design/dmabuf.html),
[GstVideoInfoDmaDrm](https://gstreamer.freedesktop.org/documentation/video/video-info-dma-drm.html),
[Linux DMA-BUF synchronization](https://docs.kernel.org/driver-api/dma-buf.html),
[Vulkan resource creation and external image layouts](https://docs.vulkan.org/spec/latest/chapters/resources.html).

## Multi-codec capability policy and device selection

| WiVRn codec | Parser | Stateless decoder | Appsrc input Caps |
| --- | --- | --- | --- |
| H.264 | h264parse | v4l2slh264dec | video/x-h264,stream-format=byte-stream,alignment=au |
| H.265 | h265parse | v4l2slh265dec | video/x-h265,stream-format=byte-stream,alignment=au |
| AV1 | av1parse | v4l2slav1dec | video/x-av1,stream-format=obu-stream,alignment=frame |

AV1 is sent as an OBU stream, not Annex B NAL units. Its frame alignment matches
[the Stateless AV1 decoder contract](https://gstreamer.freedesktop.org/documentation/v4l2codecs/v4l2slav1dec.html).
The real WiVRn AV1 encoder payload and decoder interoperability still need testing.
Only H.264 is enabled by default, based on the operator's verified decode/conversion
pipeline. Factory registration alone does not attest successful playback. After
independently verifying each hardware pipeline, the operator can set
`WIVRN_GSTREAMER_CODECS=h264,h265,av1` to attest those codecs; missing parser/decoder
factories still exclude a codec. An empty value disables all GStreamer capabilities.
Do not enable H.265/AV1 before testing them. Automatic backend selection may still
advertise codecs provided by the existing FFmpeg backend and use FFmpeg for them.
The decoder output caps require NV12 8-bit; P010 and other 10-bit outputs cannot
silently pass this filter or the AR24 descriptor check.

Default selection uses device-aware GStreamer factory registration. Optional
`WIVRN_GSTREAMER_VIDEO_DEVICE` and `WIVRN_GSTREAMER_MEDIA_DEVICE` set the decoder's
construct-only properties at construction; `WIVRN_GSTREAMER_CONVERTER_DEVICE`
sets the converter device. No `/dev/video1`, `/dev/video0`, or Rockchip driver
name is hardcoded. Overrides do not create missing hardware factories; check
`gst-inspect-1.0` first and leave these variables unset for automatic selection.
