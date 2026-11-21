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

#include <EGL/egl.h>
#include <EGL/eglext.h>

#include "video/out/ohos_common.h"
#include "egl_helpers.h"
#include "common/common.h"
#include "context.h"

struct priv {
    struct GL gl;
    EGLDisplay egl_display;
    EGLContext egl_context;
    EGLSurface egl_surface;
    // EGLConfig the context was created with. Kept on the context rather than
    // a local so the surface can be recreated later with the same pixel
    // format; creating it with a provisional config would risk EGL_BAD_MATCH.
    EGLConfig egl_config;
};

static pl_color_space_t ohos_preferred_csp(struct ra_ctx *ctx)
{
    return vo_ohos_preferred_csp(ctx->vo);
}

static void ohos_set_color(struct ra_ctx *ctx, struct mp_image_params *params)
{
    vo_ohos_set_color(ctx->vo, params);
}

static void ohos_swap_buffers(struct ra_ctx *ctx)
{
    struct priv *p = ctx->priv;
    if (eglSwapBuffers(p->egl_display, p->egl_surface))
        return;

    // A failed swap is normally EGL_BAD_SURFACE / EGL_CONTEXT_LOST: the
    // surface went away under us (XComponent recreated, compositor dropped
    // the consumer). Ignoring it used to leave a frozen picture with no
    // diagnostic at all. Rebuild the surface so the next frame has one.
    EGLint err = eglGetError();
    MP_WARN(ctx, "eglSwapBuffers failed: 0x%x, recreating surface\n",
            (unsigned)err);

    if (p->egl_surface != EGL_NO_SURFACE) {
        eglMakeCurrent(p->egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE,
                       EGL_NO_CONTEXT);
        eglDestroySurface(p->egl_display, p->egl_surface);
        p->egl_surface = EGL_NO_SURFACE;
    }

    OHNativeWindow *native_window = vo_ohos_native_window(ctx->vo);
    if (native_window) {
        p->egl_surface = eglCreateWindowSurface(p->egl_display, p->egl_config,
                                        (EGLNativeWindowType)native_window,
                                        NULL);
    }
    if (p->egl_surface == EGL_NO_SURFACE ||
        !eglMakeCurrent(p->egl_display, p->egl_surface, p->egl_surface,
                        p->egl_context)) {
        MP_ERR(ctx, "Could not recreate the EGL surface after a failed "
                    "swap; the picture will not recover until the output is "
                    "reconfigured\n");
    }
}

static void ohos_uninit(struct ra_ctx *ctx)
{
    struct priv *p = ctx->priv;
    ra_gl_ctx_uninit(ctx);

    if (p->egl_surface) {
        eglMakeCurrent(p->egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE,
                       EGL_NO_CONTEXT);
        eglDestroySurface(p->egl_display, p->egl_surface);
    }
    if (p->egl_context)
        eglDestroyContext(p->egl_display, p->egl_context);

    vo_ohos_uninit(ctx->vo);
}

static bool ohos_init(struct ra_ctx *ctx)
{
    struct priv *p = ctx->priv = talloc_zero(ctx, struct priv);

    if (!vo_ohos_init(ctx->vo))
        goto fail;

    p->egl_display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (!eglInitialize(p->egl_display, NULL, NULL)) {
        MP_FATAL(ctx, "EGL failed to initialize.\n");
        goto fail;
    }

    if (!mpegl_create_context(ctx, p->egl_display, &p->egl_context,
                              &p->egl_config))
        goto fail;

    OHNativeWindow *native_window = vo_ohos_native_window(ctx->vo);
    p->egl_surface = eglCreateWindowSurface(p->egl_display, p->egl_config,
                                    (EGLNativeWindowType)native_window, NULL);

    if (p->egl_surface == EGL_NO_SURFACE) {
        MP_FATAL(ctx, "Could not create EGL surface!\n");
        goto fail;
    }

    if (!eglMakeCurrent(p->egl_display, p->egl_surface, p->egl_surface,
                        p->egl_context)) {
        MP_FATAL(ctx, "Failed to set context!\n");
        goto fail;
    }

    mpegl_load_functions(&p->gl, ctx->log);

    struct ra_ctx_params params = {
        .preferred_csp = ohos_preferred_csp,
        .set_color = ohos_set_color,
        .swap_buffers = ohos_swap_buffers,
    };

    if (!ra_gl_ctx_init(ctx, &p->gl, params))
        goto fail;

    return true;
fail:
    ohos_uninit(ctx);
    return false;
}

static bool ohos_reconfig(struct ra_ctx *ctx)
{
    int w, h;
    if (!vo_ohos_ensure_surface_size(ctx->vo, &w, &h))
        return false;

    // vo_ohos_ensure_surface_size() leaves the buffer queue sized to w x h, so
    // render at that size instead of letting the compositor rescale per axis.
    ctx->vo->dwidth = w;
    ctx->vo->dheight = h;
    ra_gl_ctx_resize(ctx->swapchain, w, h, 0);
    vo_ohos_invalidate_color(ctx->vo);
    return true;
}

static int ohos_control(struct ra_ctx *ctx, int *events, int request, void *arg)
{
    switch (request) {
    case VOCTRL_GET_DISPLAY_FPS: {
        double fps = vo_ohos_display_fps(ctx->vo);
        if (fps <= 0)
            return VO_NOTIMPL;
        *(double *)arg = fps;
        return VO_TRUE;
    }
    }
    return VO_NOTIMPL;
}

const struct ra_ctx_fns ra_ctx_ohos = {
    .type           = "opengl",
    .name           = "ohos",
    .description    = "HarmonyOS/EGL",
    .reconfig       = ohos_reconfig,
    .control        = ohos_control,
    .init           = ohos_init,
    .uninit         = ohos_uninit,
};
