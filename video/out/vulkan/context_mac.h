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

#pragma once

#include <stdbool.h>

#include "config.h"
#include "common/common.h"

struct ra_ctx;
struct mp_image;

// --macos-native-video: hand VideoToolbox frames to Apple's video layer (see mac/native_video.swift).
#if HAVE_COCOA && HAVE_VULKAN && HAVE_SWIFT
// True if --macos-native-video is on for this context. The Vulkan layer is then non-opaque, so
// every frame must be rendered with alpha (premultiplied), or it composites as transparent.
bool mac_vk_native_enabled(struct ra_ctx *ctx);
// Returns true if `mpi` will be shown by the native layer; the caller then draws only the OSD,
// premultiplied, on a transparent target. Returns false (and hides the native layer) otherwise.
bool mac_vk_native_frame(struct ra_ctx *ctx, struct mp_image *mpi);
// Places the native video: `src` crop of a `w`x`h` image shown at `dst` in a `fb_w`x`fb_h` window
// (the size dst was computed for, i.e. vo->dwidth/dheight).
void mac_vk_native_rect(struct ra_ctx *ctx, struct mp_rect src, struct mp_rect dst,
                        int w, int h, int fb_w, int fb_h);
// Shows the frame staged by mac_vk_native_frame. Call when the OSD frame is presented.
void mac_vk_native_flip(struct ra_ctx *ctx);
#else
static inline bool mac_vk_native_enabled(struct ra_ctx *ctx) { return false; }
static inline bool mac_vk_native_frame(struct ra_ctx *ctx, struct mp_image *mpi) { return false; }
static inline void mac_vk_native_rect(struct ra_ctx *ctx, struct mp_rect src, struct mp_rect dst,
                                      int w, int h, int fb_w, int fb_h) {}
static inline void mac_vk_native_flip(struct ra_ctx *ctx) {}
#endif
