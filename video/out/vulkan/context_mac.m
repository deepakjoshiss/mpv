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

#import <QuartzCore/QuartzCore.h>
#import <CoreVideo/CoreVideo.h>

#include <libavutil/dovi_meta.h>
#include <libavutil/frame.h>

#include "video/out/gpu/context.h"
#include "video/mp_image.h"
#include "osdep/mac/swift.h"

#include "common.h"
#include "context.h"
#include "context_mac.h"
#include "utils.h"

// What the native layer is showing; decides which attachments a frame gets.
enum native_kind {
    NATIVE_NONE,
    NATIVE_PLAIN,   // SDR, HDR10, HLG
    NATIVE_DV5,
    NATIVE_DV81,
    NATIVE_DV84,
};

static const char *const native_kind_names[] = {
    [NATIVE_NONE]  = "off (rendered by libplacebo)",
    [NATIVE_PLAIN] = "on",
    [NATIVE_DV5]   = "on, Dolby Vision profile 5",
    [NATIVE_DV81]  = "on, Dolby Vision profile 8.1",
    [NATIVE_DV84]  = "on, Dolby Vision profile 8.4",
};

struct priv {
    struct mpvk_ctx vk;
    MacCommon *vo_mac;

    enum native_kind native_kind;
    bool native_logged;
    CVPixelBufferRef native_staged;   // retained until mac_vk_native_flip
    CVPixelBufferRef native_last;     // identity only, with native_last_pts
    double native_last_pts;
};

static void mac_vk_uninit(struct ra_ctx *ctx)
{
    struct priv *p = ctx->priv;

    if (p->native_staged)
        CVPixelBufferRelease(p->native_staged);
    ra_vk_ctx_uninit(ctx);
    mpvk_uninit(&p->vk);
    [p->vo_mac uninit:ctx->vo];
}

static void mac_vk_swap_buffers(struct ra_ctx *ctx)
{
    struct priv *p = ctx->priv;
    [p->vo_mac swapBuffer];
}

static void mac_vk_get_vsync(struct ra_ctx *ctx, struct vo_vsync_info *info)
{
    struct priv *p = ctx->priv;
    [p->vo_mac fillVsyncWithInfo:info];
}

static int mac_vk_color_depth(struct ra_ctx *ctx)
{
    return 0;
}

static bool mac_vk_check_visible(struct ra_ctx *ctx)
{
    struct priv *p = ctx->priv;
    return [p->vo_mac isVisible];
}

static bool mac_vk_init(struct ra_ctx *ctx)
{
    struct priv *p = ctx->priv = talloc_zero(ctx, struct priv);
    struct mpvk_ctx *vk = &p->vk;
    int msgl = ctx->opts.probing ? MSGL_V : MSGL_ERR;

    if (!NSApp) {
        MP_ERR(ctx, "Failed to initialize macvk context, no NSApplication initialized.\n");
        goto error;
    }

    if (!mpvk_init(vk, ctx, VK_EXT_METAL_SURFACE_EXTENSION_NAME))
        goto error;

    p->vo_mac = [[MacCommon alloc] init:ctx->vo];
    if (!p->vo_mac)
        goto error;

    VkMetalSurfaceCreateInfoEXT mac_info = {
        .sType = VK_STRUCTURE_TYPE_METAL_SURFACE_CREATE_INFO_EXT,
        .pNext = NULL,
        .flags = 0,
        .pLayer = p->vo_mac.layer,
    };

    struct ra_ctx_params params = {
        .swap_buffers = mac_vk_swap_buffers,
        .get_vsync = mac_vk_get_vsync,
        .color_depth = mac_vk_color_depth,
        .check_visible = mac_vk_check_visible,
    };

    VkInstance inst = vk->vkinst->instance;
    VkResult res = vkCreateMetalSurfaceEXT(inst, &mac_info, NULL, &vk->surface);
    if (res != VK_SUCCESS) {
        MP_MSG(ctx, msgl, "Failed creating metal surface\n");
        goto error;
    }

    if (!ra_vk_ctx_init(ctx, vk, params, VK_PRESENT_MODE_FIFO_KHR))
        goto error;

    return true;
error:
    if (p->vo_mac)
        [p->vo_mac uninit:ctx->vo];
    return false;
}

static bool resize(struct ra_ctx *ctx)
{
    struct priv *p = ctx->priv;

    if (!p->vo_mac.window) {
        return false;
    }
    CGSize size = p->vo_mac.window.framePixel.size;

    return ra_vk_ctx_resize(ctx, (int)size.width, (int)size.height);
}

static bool mac_vk_reconfig(struct ra_ctx *ctx)
{
    struct priv *p = ctx->priv;
    if (![p->vo_mac config:ctx->vo])
        return false;

    [p->vo_mac updateWithAlpha:ctx->opts.want_alpha];

    return true;
}

static int mac_vk_control(struct ra_ctx *ctx, int *events, int request, void *arg)
{
    struct priv *p = ctx->priv;
    int ret = [p->vo_mac control:ctx->vo events:events request:request data:arg];

    if (*events & VO_EVENT_RESIZE) {
        if (!resize(ctx))
            return VO_ERROR;
    }

    return ret;
}

static void mac_vk_update_render_opts(struct ra_ctx *ctx)
{
    struct priv *p = ctx->priv;
    [p->vo_mac updateWithAlpha:ctx->opts.want_alpha];
}

const struct ra_ctx_fns ra_ctx_vulkan_mac = {
    .type               = "vulkan",
    .name               = "macvk",
    .description        = "mac/Vulkan (via Metal)",
    .reconfig           = mac_vk_reconfig,
    .control            = mac_vk_control,
    .update_render_opts = mac_vk_update_render_opts,
    .init               = mac_vk_init,
    .uninit             = mac_vk_uninit,
};

// --- --macos-native-video ---------------------------------------------------------------------
// Frames are tagged exactly the way Apple's own decoder tags them (checked attachment by attachment,
// CF types included, against AVAssetReader output for P5, P8.1 and P8.4). Do not loosen this:
// WindowServer's HDRProcessing aborts on an unexpected attachment type and takes the login session
// with it (seen with a CFNumber DolbyCompatibilityID).

static CFStringRef cv_primaries(enum pl_color_primaries prim)
{
    switch (prim) {
    case PL_COLOR_PRIM_BT_709:     return kCVImageBufferColorPrimaries_ITU_R_709_2;
    case PL_COLOR_PRIM_BT_601_525: return kCVImageBufferColorPrimaries_SMPTE_C;
    case PL_COLOR_PRIM_BT_601_625: return kCVImageBufferColorPrimaries_EBU_3213;
    case PL_COLOR_PRIM_BT_2020:    return kCVImageBufferColorPrimaries_ITU_R_2020;
    case PL_COLOR_PRIM_DISPLAY_P3: return kCVImageBufferColorPrimaries_P3_D65;
    case PL_COLOR_PRIM_DCI_P3:     return kCVImageBufferColorPrimaries_DCI_P3;
    default:                       return NULL;
    }
}

static CFStringRef cv_transfer(enum pl_color_transfer trc)
{
    switch (trc) {
    case PL_COLOR_TRC_BT_1886: return kCVImageBufferTransferFunction_ITU_R_709_2;
    case PL_COLOR_TRC_SRGB:    return kCVImageBufferTransferFunction_sRGB;
    case PL_COLOR_TRC_LINEAR:  return kCVImageBufferTransferFunction_Linear;
    case PL_COLOR_TRC_PQ:      return kCVImageBufferTransferFunction_SMPTE_ST_2084_PQ;
    case PL_COLOR_TRC_HLG:     return kCVImageBufferTransferFunction_ITU_R_2100_HLG;
    default:                   return NULL;
    }
}

static CFStringRef cv_matrix(enum pl_color_system sys)
{
    switch (sys) {
    case PL_COLOR_SYSTEM_BT_601:    return kCVImageBufferYCbCrMatrix_ITU_R_601_4;
    case PL_COLOR_SYSTEM_BT_709:    return kCVImageBufferYCbCrMatrix_ITU_R_709_2;
    case PL_COLOR_SYSTEM_SMPTE_240M: return kCVImageBufferYCbCrMatrix_SMPTE_240M_1995;
    case PL_COLOR_SYSTEM_BT_2020_NC: return kCVImageBufferYCbCrMatrix_ITU_R_2020;
    default:                        return NULL;
    }
}

static AVBufferRef *native_side_data(struct mp_image *mpi, int type)
{
    for (int n = 0; n < mpi->num_ff_side_data; n++) {
        if (mpi->ff_side_data[n].type == type)
            return mpi->ff_side_data[n].buf;
    }
    return NULL;
}

static enum native_kind native_classify(struct mp_image *mpi, AVBufferRef **rpu)
{
    if (!mpi || mpi->imgfmt != IMGFMT_VIDEOTOOLBOX || !mpi->planes[3] || mpi->enhancement_layer ||
        mpi->params.rotate || mpi->params.vflip || mpi->params.stereo3d)
        return NATIVE_NONE;

    if (mpi->params.repr.sys != PL_COLOR_SYSTEM_DOLBYVISION)
        return cv_matrix(mpi->params.repr.sys) ? NATIVE_PLAIN : NATIVE_NONE;

    AVBufferRef *md = native_side_data(mpi, AV_FRAME_DATA_DOVI_METADATA);
    *rpu = native_side_data(mpi, AV_FRAME_DATA_DOVI_RPU_BUFFER);
    if (!md || !*rpu)
        return NATIVE_NONE;

    // Profile from the RPU itself, the same rules as FFmpeg's ff_dovi_guess_profile_hevc().
    const AVDOVIRpuDataHeader *hdr = av_dovi_get_header((const AVDOVIMetadata *)md->data);
    if (hdr->vdr_rpu_profile == 0 && hdr->bl_video_full_range_flag)
        return NATIVE_DV5;
    // Profile 7 (and 4) carry an enhancement layer residual; their RPU needs converting first.
    if (hdr->vdr_rpu_profile != 1 ||
        (hdr->el_spatial_resampling_filter_flag && !hdr->disable_residual_flag))
        return NATIVE_NONE;

    // Profile 8: the base layer's own transfer tells 8.1 (HDR10) from 8.4 (HLG).
    switch (mpi->params.transfer_orig) {
    case PL_COLOR_TRC_PQ:  return NATIVE_DV81;
    case PL_COLOR_TRC_HLG: return NATIVE_DV84;
    default:               return NATIVE_NONE;   // 8.2 (SDR) not verified
    }
}

static void native_put(CVPixelBufferRef pb, CFStringRef key, CFTypeRef value)
{
    if (value) {
        CVBufferSetAttachment(pb, key, value, kCVAttachmentMode_ShouldPropagate);
    } else {
        CVBufferRemoveAttachment(pb, key);
    }
}

static void native_tag(CVPixelBufferRef pb, struct mp_image *mpi, enum native_kind kind,
                       AVBufferRef *rpu)
{
    const struct mp_image_params *par = &mpi->params;
    CFStringRef bt2020 = kCVImageBufferColorPrimaries_ITU_R_2020;

    switch (kind) {
    case NATIVE_DV5:    // Apple tags profile 5 as PQ only: no primaries, no matrix, no colorspace
        native_put(pb, kCVImageBufferColorPrimariesKey, NULL);
        native_put(pb, kCVImageBufferTransferFunctionKey, kCVImageBufferTransferFunction_SMPTE_ST_2084_PQ);
        native_put(pb, kCVImageBufferYCbCrMatrixKey, NULL);
        break;
    case NATIVE_DV81:
    case NATIVE_DV84:
        native_put(pb, kCVImageBufferColorPrimariesKey, bt2020);
        native_put(pb, kCVImageBufferTransferFunctionKey, kind == NATIVE_DV81
                   ? kCVImageBufferTransferFunction_SMPTE_ST_2084_PQ
                   : kCVImageBufferTransferFunction_ITU_R_2100_HLG);
        native_put(pb, kCVImageBufferYCbCrMatrixKey, kCVImageBufferYCbCrMatrix_ITU_R_2020);
        break;
    default: {          // mpv's view of the colors wins (it also knows container tags); keep VT's otherwise
        CFStringRef prim = cv_primaries(par->color.primaries);
        CFStringRef trc = cv_transfer(par->color.transfer);
        CFStringRef mat = cv_matrix(par->repr.sys);
        if (prim)
            native_put(pb, kCVImageBufferColorPrimariesKey, prim);
        if (trc)
            native_put(pb, kCVImageBufferTransferFunctionKey, trc);
        if (mat)
            native_put(pb, kCVImageBufferYCbCrMatrixKey, mat);
    }
    }

    // Apple's Dolby Vision frames carry no gamma level (FFmpeg adds one for gamma-curve sources).
    if (kind >= NATIVE_DV5)
        CVBufferRemoveAttachment(pb, kCVImageBufferGammaLevelKey);
    // VideoToolbox guesses BT.709 for untagged streams; the colorspace must follow the final tags.
    CVBufferRemoveAttachment(pb, CFSTR("ColorInfoGuessedBy"));
    CVBufferRemoveAttachment(pb, kCVImageBufferCGColorSpaceKey);
    if (kind != NATIVE_DV5) {
        CFDictionaryRef att = CVBufferCopyAttachments(pb, kCVAttachmentMode_ShouldPropagate);
        CGColorSpaceRef cs = att ? CVImageBufferCreateColorSpaceFromAttachments(att) : NULL;
        if (cs) {
            CVBufferSetAttachment(pb, kCVImageBufferCGColorSpaceKey, cs, kCVAttachmentMode_ShouldPropagate);
            CGColorSpaceRelease(cs);
        }
        if (att)
            CFRelease(att);
    }

    // The RPU is FFmpeg's AV_FRAME_DATA_DOVI_RPU_BUFFER as is: byte-identical to Apple's attachment.
    CFDataRef rpu_data = kind >= NATIVE_DV5 ? CFDataCreate(NULL, rpu->data, rpu->size) : NULL;
    native_put(pb, CFSTR("DolbyVisionRPUData"), rpu_data);
    if (rpu_data)
        CFRelease(rpu_data);
    // Must be a CFString, never a CFNumber (see above). Apple attaches it for 8.1 only.
    native_put(pb, CFSTR("DolbyCompatibilityID"), kind == NATIVE_DV81 ? CFSTR("1") : NULL);
}

static void native_log_attachments(struct ra_ctx *ctx, CVPixelBufferRef pb)
{
    NSDictionary *att = (NSDictionary *)CVBufferCopyAttachments(pb, kCVAttachmentMode_ShouldPropagate);
    for (NSString *key in [[att allKeys] sortedArrayUsingSelector:@selector(compare:)]) {
        id value = att[key];
        NSString *desc = [value isKindOfClass:[NSData class]]
            ? [NSString stringWithFormat:@"%lu bytes", (unsigned long)[(NSData *)value length]]
            : [[value description] stringByReplacingOccurrencesOfString:@"\n" withString:@" "];
        MP_VERBOSE(ctx, "  %s = %s\n", [key UTF8String], [desc UTF8String]);
    }
    [att release];
}

static struct priv *native_priv(struct ra_ctx *ctx)
{
    if (ctx->fns != &ra_ctx_vulkan_mac)
        return NULL;
    struct priv *p = ctx->priv;
    return p && p->vo_mac.nativeVideo ? p : NULL;
}

bool mac_vk_native_enabled(struct ra_ctx *ctx)
{
    return native_priv(ctx) != NULL;
}

bool mac_vk_native_frame(struct ra_ctx *ctx, struct mp_image *mpi)
{
    struct priv *p = native_priv(ctx);
    if (!p)
        return false;

    AVBufferRef *rpu = NULL;
    enum native_kind kind = native_classify(mpi, &rpu);
    if (kind != p->native_kind) {
        MP_VERBOSE(ctx, "Native video: %s\n", native_kind_names[kind]);
        p->native_kind = kind;
        p->native_logged = false;
    }

    if (kind == NATIVE_NONE) {
        if (p->native_staged)
            CVPixelBufferRelease(p->native_staged);
        p->native_staged = p->native_last = NULL;
        [p->vo_mac.nativeVideo hide];
        return false;
    }

    // Redraws (pause, OSD changes, resizes) of the frame on screen are not shown again.
    CVPixelBufferRef pb = (CVPixelBufferRef)mpi->planes[3];
    if (pb != p->native_last || mpi->pts != p->native_last_pts) {
        native_tag(pb, mpi, kind, rpu);
        if (!p->native_logged) {
            MP_VERBOSE(ctx, "Native video frame attachments:\n");
            native_log_attachments(ctx, pb);
            p->native_logged = true;
        }
        CVPixelBufferRetain(pb);
        if (p->native_staged)
            CVPixelBufferRelease(p->native_staged);
        p->native_staged = pb;
        p->native_last = pb;
        p->native_last_pts = mpi->pts;
    }
    return true;
}

void mac_vk_native_rect(struct ra_ctx *ctx, struct mp_rect src, struct mp_rect dst,
                        int w, int h, int fb_w, int fb_h)
{
    struct priv *p = native_priv(ctx);
    if (!p || mp_rect_w(src) <= 0 || mp_rect_h(src) <= 0 || fb_w <= 0 || fb_h <= 0)
        return;

    // The layer shows the whole image; extend dst by the cropped-away part, the container clips it.
    double sx = mp_rect_w(dst) / (double)mp_rect_w(src);
    double sy = mp_rect_h(dst) / (double)mp_rect_h(src);
    double x0 = dst.x0 - src.x0 * sx;
    double y0 = dst.y0 - src.y0 * sy;
    MP_TRACE(ctx, "Native video rect: src %d,%d-%d,%d dst %d,%d-%d,%d image %dx%d fb %dx%d\n",
             src.x0, src.y0, src.x1, src.y1, dst.x0, dst.y0, dst.x1, dst.y1, w, h, fb_w, fb_h);
    [p->vo_mac.nativeVideo setVideoRectWithX:x0 / fb_w y:y0 / fb_h w:w * sx / fb_w h:h * sy / fb_h];
}

void mac_vk_native_flip(struct ra_ctx *ctx)
{
    struct priv *p = native_priv(ctx);
    if (!p || !p->native_staged)
        return;
    [p->vo_mac.nativeVideo enqueue:p->native_staged];
    CVPixelBufferRelease(p->native_staged);
    p->native_staged = NULL;
}
