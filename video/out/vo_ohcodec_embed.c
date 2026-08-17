/*
 * This file is part of mpv.
 *
 * mpv is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * mpv is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with mpv.  If not, see <http://www.gnu.org/licenses/>.
 */

/* HarmonyOS: direct decode-to-surface passthrough VO.
 *
 * Analogous to vo_mediacodec_embed on Android: the OHCodec hardware decoder
 * renders frames straight into the target OHNativeWindow (in practice the
 * producer window of a Flutter external texture, identified by --wid as a
 * surface ID). mpv does NOT run any rendering pipeline here - no libplacebo,
 * no Vulkan swapchain, no color conversion pass. This removes the extra
 * full-resolution render + double-vsync-domain overhead that otherwise sits
 * between the decoder and the consumer on OHOS.
 *
 * Trade-offs (same as vo_mediacodec_embed):
 *  - Only IMGFMT_OHCODEC frames are accepted; software decoding is not
 *    possible with this VO (query_format rejects everything else).
 *  - mpv GPU capabilities (scalers, shaders, HDR tone mapping, screenshot
 *    via VO) are unavailable. Audio/video sync is handled by the decoder
 *    render timing (RenderOutputBuffer in ohdec.c) plus the normal mpv
 *    A/V sync layer.
 */

#include <libavcodec/ohcodec_buffer.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_oh.h>

#include "common/common.h"
#include "video/hwdec.h"
#include "video/mp_image.h"
#include "vo.h"
#include "ohos_common.h"

struct priv {
    struct mp_image *next_image;
    struct mp_hwdec_ctx hwctx;
};

static AVBufferRef *create_ohcodec_device_ref(struct vo *vo)
{
    if (!vo_ohos_init(vo))
        return NULL;

    AVBufferRef *ref = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_OHCODEC);
    if (!ref)
        return NULL;

    AVHWDeviceContext *device = (AVHWDeviceContext *)ref->data;
    AVOHCodecDeviceContext *oh = device->hwctx;
    oh->native_window = vo_ohos_native_window(vo);
    if (!oh->native_window || av_hwdevice_ctx_init(ref) < 0)
        av_buffer_unref(&ref);

    return ref;
}

static int preinit(struct vo *vo)
{
    struct priv *p = vo->priv;
    vo->hwdec_devs = hwdec_devices_create();
    p->hwctx = (struct mp_hwdec_ctx){
        .driver_name = "ohcodec_embed",
        .av_device_ref = create_ohcodec_device_ref(vo),
        .hw_imgfmt = IMGFMT_OHCODEC,
    };

    if (!p->hwctx.av_device_ref) {
        MP_VERBOSE(vo, "Failed to create OHCodec hwdevice_ctx\n");
        return -1;
    }

    hwdec_devices_add(vo->hwdec_devs, &p->hwctx);
    return 0;
}

static void flip_page(struct vo *vo)
{
    struct priv *p = vo->priv;
    if (!p->next_image)
        return;

    AVOHCodecBuffer *buffer = (AVOHCodecBuffer *)p->next_image->planes[3];
    av_ohcodec_release_buffer(buffer, 1);
    mp_image_unrefp(&p->next_image);
}

static bool draw_frame(struct vo *vo, struct vo_frame *frame)
{
    struct priv *p = vo->priv;

    mp_image_t *mpi = NULL;
    if (!frame->redraw && !frame->repeat)
        mpi = mp_image_new_ref(frame->current);

    talloc_free(p->next_image);
    p->next_image = mpi;
    return VO_TRUE;
}

static int query_format(struct vo *vo, int format)
{
    return format == IMGFMT_OHCODEC;
}

static int control(struct vo *vo, uint32_t request, void *data)
{
    return VO_NOTIMPL;
}

static int reconfig(struct vo *vo, struct mp_image_params *params)
{
    return 0;
}

static void uninit(struct vo *vo)
{
    struct priv *p = vo->priv;
    mp_image_unrefp(&p->next_image);

    hwdec_devices_remove(vo->hwdec_devs, &p->hwctx);
    av_buffer_unref(&p->hwctx.av_device_ref);
    vo_ohos_uninit(vo);
}

const struct vo_driver video_out_ohcodec_embed = {
    .description = "HarmonyOS (Embedded OHCodec Surface)",
    .name = "ohcodec_embed",
    .caps = VO_CAP_NORETAIN,
    .preinit = preinit,
    .query_format = query_format,
    .control = control,
    .draw_frame = draw_frame,
    .flip_page = flip_page,
    .reconfig = reconfig,
    .uninit = uninit,
    .priv_size = sizeof(struct priv),
};
