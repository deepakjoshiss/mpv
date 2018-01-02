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

#include <stdlib.h>
#include <inttypes.h>
#include <unistd.h>
#include <string.h>
#include <stdbool.h>
#include <assert.h>
#include <time.h>
#include <math.h>
#include <pthread.h>
#include <sys/types.h>

#include <libavutil/avstring.h>
#include <libavutil/common.h>

#include "config.h"
#include "mpv_talloc.h"
#include "client.h"
#include "common/av_common.h"
#include "common/codecs.h"
#include "common/msg.h"
#include "common/msg_control.h"
#include "command.h"
#include "osdep/timer.h"
#include "common/common.h"
#include "input/input.h"
#include "input/keycodes.h"
#include "stream/stream.h"
#include "demux/demux.h"
#include "demux/stheader.h"
#include "common/playlist.h"
#include "sub/osd.h"
#include "sub/dec_sub.h"
#include "options/m_option.h"
#include "options/m_property.h"
#include "options/m_config.h"
#include "video/filter/vf.h"
#include "video/decode/vd.h"
#include "video/out/vo.h"
#include "video/csputils.h"
#include "audio/aframe.h"
#include "audio/format.h"
#include "audio/out/ao.h"
#include "video/decode/dec_video.h"
#include "audio/decode/dec_audio.h"
#include "video/out/bitmap_packer.h"
#include "options/path.h"
#include "screenshot.h"
#include "misc/node.h"

#include "osdep/io.h"
#include "osdep/subprocess.h"

#include "core.h"

#if HAVE_LIBAF
#include "audio/filter/af.h"
#endif

#ifdef _WIN32
#include <windows.h>
#endif

struct command_ctx {
    // All properties, terminated with a {0} item.
    struct m_property *properties;

    bool is_idle;

    double last_seek_time;
    double last_seek_pts;
    double marked_pts;

    char **warned_deprecated;
    int num_warned_deprecated;

    struct cycle_counter *cycle_counters;
    int num_cycle_counters;

    struct overlay *overlays;
    int num_overlays;
    // One of these is in use by the OSD; the other one exists so that the
    // bitmap list can be manipulated without additional synchronization.
    struct sub_bitmaps overlay_osd[2];
    int overlay_osd_current;
    struct bitmap_packer *overlay_packer;

    struct hook_handler **hooks;
    int num_hooks;
    int64_t hook_seq; // for hook_handler.seq

    struct ao_hotplug *hotplug;

    char *cur_ipc;
    char *cur_ipc_input;

    int silence_option_deprecations;
};

struct overlay {
    struct mp_image *source;
    int x, y;
};

struct hook_handler {
    char *client;   // client API user name
    char *type;     // kind of hook, e.g. "on_load"
    char *user_id;  // numeric user-chosen ID, printed as string
    int priority;   // priority for global hook order
    int64_t seq;    // unique ID (also age -> fixed order for equal priorities)
    bool active;    // hook is currently in progress (only 1 at a time for now)
};

// U+279C HEAVY ROUND-TIPPED RIGHTWARDS ARROW
// U+00A0 NO-BREAK SPACE
#define ARROW_SP "\342\236\234\302\240"

const char list_current[] = OSD_ASS_0 ARROW_SP OSD_ASS_1;
const char list_normal[] = OSD_ASS_0 "{\\alpha&HFF}" ARROW_SP "{\\r}" OSD_ASS_1;

static int edit_filters(struct MPContext *mpctx, struct mp_log *log,
                        enum stream_type mediatype,
                        const char *cmd, const char *arg);
static int set_filters(struct MPContext *mpctx, enum stream_type mediatype,
                       struct m_obj_settings *new_chain);

static int mp_property_do_silent(const char *name, int action, void *val,
                                 struct MPContext *ctx);

static void hook_remove(struct MPContext *mpctx, int index)
{
    struct command_ctx *cmd = mpctx->command_ctx;
    assert(index >= 0 && index < cmd->num_hooks);
    talloc_free(cmd->hooks[index]);
    MP_TARRAY_REMOVE_AT(cmd->hooks, cmd->num_hooks, index);
}

bool mp_hook_test_completion(struct MPContext *mpctx, char *type)
{
    struct command_ctx *cmd = mpctx->command_ctx;
    for (int n = 0; n < cmd->num_hooks; n++) {
        struct hook_handler *h = cmd->hooks[n];
        if (h->active && strcmp(h->type, type) == 0) {
            if (!mp_client_exists(mpctx, h->client)) {
                hook_remove(mpctx, n);
                break;
            }
            return false;
        }
    }
    return true;
}

static bool send_hook_msg(struct MPContext *mpctx, struct hook_handler *h,
                          char *cmd)
{
    mpv_event_client_message *m = talloc_ptrtype(NULL, m);
    *m = (mpv_event_client_message){0};
    MP_TARRAY_APPEND(m, m->args, m->num_args, cmd);
    MP_TARRAY_APPEND(m, m->args, m->num_args, talloc_strdup(m, h->user_id));
    MP_TARRAY_APPEND(m, m->args, m->num_args, talloc_strdup(m, h->type));
    bool r =
        mp_client_send_event(mpctx, h->client, MPV_EVENT_CLIENT_MESSAGE, m) >= 0;
    if (!r)
        MP_WARN(mpctx, "Sending hook command failed.\n");
    return r;
}

// client==NULL means start the hook chain
void mp_hook_run(struct MPContext *mpctx, char *client, char *type)
{
    struct command_ctx *cmd = mpctx->command_ctx;
    bool found_current = !client;
    int index = -1;
    for (int n = 0; n < cmd->num_hooks; n++) {
        struct hook_handler *h = cmd->hooks[n];
        if (!found_current) {
            if (h->active && strcmp(h->type, type) == 0) {
                h->active = false;
                found_current = true;
                mp_wakeup_core(mpctx);
            }
        } else if (strcmp(h->type, type) == 0) {
            index = n;
            break;
        }
    }
    if (index < 0)
        return;
    struct hook_handler *next = cmd->hooks[index];
    MP_VERBOSE(mpctx, "Running hook: %s/%s\n", next->client, type);
    next->active = true;
    if (!send_hook_msg(mpctx, next, "hook_run")) {
        hook_remove(mpctx, index);
        mp_wakeup_core(mpctx); // repeat next iteration to finish
    }
}

static int compare_hook(const void *pa, const void *pb)
{
    struct hook_handler **h1 = (void *)pa;
    struct hook_handler **h2 = (void *)pb;
    if ((*h1)->priority != (*h2)->priority)
        return (*h1)->priority - (*h2)->priority;
    return (*h1)->seq - (*h2)->seq;
}

static void mp_hook_add(struct MPContext *mpctx, char *client, char *name,
                        int id, int pri)
{
    struct command_ctx *cmd = mpctx->command_ctx;
    struct hook_handler *h = talloc_ptrtype(cmd, h);
    int64_t seq = cmd->hook_seq++;
    *h = (struct hook_handler){
        .client = talloc_strdup(h, client),
        .type = talloc_strdup(h, name),
        .user_id = talloc_asprintf(h, "%d", id),
        .priority = pri,
        .seq = seq,
    };
    MP_TARRAY_APPEND(cmd, cmd->hooks, cmd->num_hooks, h);
    qsort(cmd->hooks, cmd->num_hooks, sizeof(cmd->hooks[0]), compare_hook);
}

// Call before a seek, in order to allow revert-seek to undo the seek.
void mark_seek(struct MPContext *mpctx)
{
    struct command_ctx *cmd = mpctx->command_ctx;
    double now = mp_time_sec();
    if (now > cmd->last_seek_time + 2.0 || cmd->last_seek_pts == MP_NOPTS_VALUE)
        cmd->last_seek_pts = get_current_time(mpctx);
    cmd->last_seek_time = now;
}

static char *skip_n_lines(char *text, int lines)
{
    while (text && lines > 0) {
        char *next = strchr(text, '\n');
        text = next ? next + 1 : NULL;
        lines--;
    }
    return text;
}

static int count_lines(char *text)
{
    int count = 0;
    while (text) {
        char *next = strchr(text, '\n');
        if (!next || (next[0] == '\n' && !next[1]))
            break;
        text = next + 1;
        count++;
    }
    return count;
}

// Given a huge string separated by new lines, attempts to cut off text above
// the current line to keep the line visible, and below to keep rendering
// performance up. pos gives the current line (0 for the first line).
// "text" might be returned as is, or it can be freed and a new allocation is
// returned.
// This is only a heuristic - we can't deal with line breaking.
static char *cut_osd_list(struct MPContext *mpctx, char *text, int pos)
{
    int screen_h, font_h;
    osd_get_text_size(mpctx->osd, &screen_h, &font_h);
    int max_lines = screen_h / MPMAX(font_h, 1) - 1;

    if (!text || max_lines < 5)
        return text;

    int count = count_lines(text);
    if (count <= max_lines)
        return text;

    char *new = talloc_strdup(NULL, "");

    int start = pos - max_lines / 2;
    if (start == 1)
        start = 0; // avoid weird transition when pad_h becomes visible
    int pad_h = start > 0;
    if (pad_h)
        new = talloc_strdup_append_buffer(new, "\342\206\221 (hidden items)\n");

    int space = max_lines - pad_h - 1;
    int pad_t = count - start > space;
    if (!pad_t)
        start = count - space;

    char *head = skip_n_lines(text, start);
    if (!head) {
        talloc_free(new);
        return text;
    }

    char *tail = skip_n_lines(head, max_lines - pad_h - pad_t);
    new = talloc_asprintf_append_buffer(new, "%.*s",
                            (int)(tail ? tail - head : strlen(head)), head);
    if (pad_t)
        new = talloc_strdup_append_buffer(new, "\342\206\223 (hidden items)\n");

    talloc_free(text);
    return new;
}

static char *format_file_size(int64_t size)
{
    double s = size;
    if (size < 1024)
        return talloc_asprintf(NULL, "%.0f", s);

    if (size < (1024 * 1024))
        return talloc_asprintf(NULL, "%.3f KiB", s / (1024.0));

    if (size < (1024 * 1024 * 1024))
        return talloc_asprintf(NULL, "%.3f MiB", s / (1024.0 * 1024.0));

    if (size < (1024LL * 1024LL * 1024LL * 1024LL))
        return talloc_asprintf(NULL, "%.3f GiB", s / (1024.0 * 1024.0 * 1024.0));

    return talloc_asprintf(NULL, "%.3f TiB", s / (1024.0 * 1024.0 * 1024.0 * 1024.0));
}

static char *format_delay(double time)
{
    return talloc_asprintf(NULL, "%d ms", (int)lrint(time * 1000));
}

// Option-property bridge. This is used so that setting options via various
// mechanisms (including command line parsing, config files, per-file options)
// updates state associated with them. For that, they have to go through the
// property layer. (Ideally, this would be the other way around, and there
// would be per-option change handlers instead.)
// Note that the property-option bridge sidesteps this, as we'd get infinite
// recursion.
int mp_on_set_option(void *ctx, struct m_config_option *co, void *data, int flags)
{
    struct MPContext *mpctx = ctx;
    struct command_ctx *cmd = mpctx->command_ctx;
    const char *name = co->name;

    // Skip going through mp_property_generic_option (typically), because the
    // property implementation is trivial, and can break some obscure features
    // like --profile and --include if non-trivial flags are involved (which
    // the bridge would drop).
    struct m_property *prop = m_property_list_find(cmd->properties, name);
    if (prop && prop->is_option)
        goto direct_option;

    struct m_option type = {0};

    int r = mp_property_do_silent(name, M_PROPERTY_GET_TYPE, &type, mpctx);
    if (r == M_PROPERTY_UNKNOWN)
        goto direct_option; // not mapped as property
    if (r != M_PROPERTY_OK)
        return M_OPT_INVALID; // shouldn't happen

    assert(type.type == co->opt->type);
    assert(type.max == co->opt->max);
    assert(type.min == co->opt->min);

    r = mp_property_do_silent(name, M_PROPERTY_SET, data, mpctx);
    if (r != M_PROPERTY_OK)
        return M_OPT_INVALID;

    // The flags can't be passed through the property layer correctly.
    m_config_mark_co_flags(co, flags);

    return 0;

direct_option:
    mp_notify_property(mpctx, name);
    return m_config_set_option_raw_direct(mpctx->mconfig, co, data, flags);
}

// Property-option bridge. (Maps the property to the option with the same name.)
static int mp_property_generic_option(void *ctx, struct m_property *prop,
                                      int action, void *arg)
{
    MPContext *mpctx = ctx;
    const char *optname = prop->name;
    int flags = M_SETOPT_RUNTIME;
    struct m_config_option *opt;
    if (mpctx->command_ctx->silence_option_deprecations) {
        // This case is specifically for making --reset-on-next-file=all silent.
        opt = m_config_get_co_raw(mpctx->mconfig, bstr0(optname));
    } else {
        opt = m_config_get_co(mpctx->mconfig, bstr0(optname));
    }

    if (!opt)
        return M_PROPERTY_UNKNOWN;

    switch (action) {
    case M_PROPERTY_GET_TYPE:
        *(struct m_option *)arg = *(opt->opt);
        return M_PROPERTY_OK;
    case M_PROPERTY_GET:
        if (!opt->data)
            return M_PROPERTY_NOT_IMPLEMENTED;
        m_option_copy(opt->opt, arg, opt->data);
        return M_PROPERTY_OK;
    case M_PROPERTY_SET:
        if (m_config_set_option_raw_direct(mpctx->mconfig, opt, arg, flags) < 0)
            return M_PROPERTY_ERROR;
        return M_PROPERTY_OK;
    }
    return M_PROPERTY_NOT_IMPLEMENTED;
}

/// Playback speed (RW)
static int mp_property_playback_speed(void *ctx, struct m_property *prop,
                                      int action, void *arg)
{
    MPContext *mpctx = ctx;
    double speed = mpctx->opts->playback_speed;
    switch (action) {
    case M_PROPERTY_SET: {
        mpctx->opts->playback_speed = *(double *)arg;
        update_playback_speed(mpctx);
        mp_wakeup_core(mpctx);
        return M_PROPERTY_OK;
    }
    case M_PROPERTY_PRINT:
        *(char **)arg = talloc_asprintf(NULL, "%.2f", speed);
        return M_PROPERTY_OK;
    }
    return mp_property_generic_option(mpctx, prop, action, arg);
}

static int mp_property_av_speed_correction(void *ctx, struct m_property *prop,
                                           int action, void *arg)
{
    MPContext *mpctx = ctx;
    char *type = prop->priv;
    double val = 0;
    switch (type[0]) {
    case 'a': val = mpctx->speed_factor_a; break;
    case 'v': val = mpctx->speed_factor_v; break;
    default: abort();
    }

    if (action == M_PROPERTY_PRINT) {
        *(char **)arg = talloc_asprintf(NULL, "%+.05f%%", (val - 1) * 100);
        return M_PROPERTY_OK;
    }

    return m_property_double_ro(action, arg, val);
}

static int mp_property_display_sync_active(void *ctx, struct m_property *prop,
                                           int action, void *arg)
{
    MPContext *mpctx = ctx;
    return m_property_flag_ro(action, arg, mpctx->display_sync_active);
}

/// filename with path (RO)
static int mp_property_path(void *ctx, struct m_property *prop,
                            int action, void *arg)
{
    MPContext *mpctx = ctx;
    if (!mpctx->filename)
        return M_PROPERTY_UNAVAILABLE;
    return m_property_strdup_ro(action, arg, mpctx->filename);
}

static int mp_property_filename(void *ctx, struct m_property *prop,
                                int action, void *arg)
{
    MPContext *mpctx = ctx;
    if (!mpctx->filename)
        return M_PROPERTY_UNAVAILABLE;
    char *filename = talloc_strdup(NULL, mpctx->filename);
    if (mp_is_url(bstr0(filename)))
        mp_url_unescape_inplace(filename);
    char *f = (char *)mp_basename(filename);
    if (!f[0])
        f = filename;
    if (action == M_PROPERTY_KEY_ACTION) {
        struct m_property_action_arg *ka = arg;
        if (strcmp(ka->key, "no-ext") == 0) {
            action = ka->action;
            arg = ka->arg;
            bstr root;
            if (mp_splitext(f, &root))
                f = bstrto0(filename, root);
        }
    }
    int r = m_property_strdup_ro(action, arg, f);
    talloc_free(filename);
    return r;
}

static int mp_property_stream_open_filename(void *ctx, struct m_property *prop,
                                            int action, void *arg)
{
    MPContext *mpctx = ctx;
    if (!mpctx->stream_open_filename || !mpctx->playing)
        return M_PROPERTY_UNAVAILABLE;
    switch (action) {
    case M_PROPERTY_SET: {
        if (mpctx->demuxer)
            return M_PROPERTY_ERROR;
        mpctx->stream_open_filename =
            talloc_strdup(mpctx->stream_open_filename, *(char **)arg);
        return M_PROPERTY_OK;
    }
    case M_PROPERTY_GET_TYPE:
    case M_PROPERTY_GET:
        return m_property_strdup_ro(action, arg, mpctx->stream_open_filename);
    }
    return M_PROPERTY_NOT_IMPLEMENTED;
}

static int mp_property_file_size(void *ctx, struct m_property *prop,
                                 int action, void *arg)
{
    MPContext *mpctx = ctx;
    if (!mpctx->demuxer)
        return M_PROPERTY_UNAVAILABLE;

    int64_t size;
    if (demux_stream_control(mpctx->demuxer, STREAM_CTRL_GET_SIZE, &size) < 1)
        return M_PROPERTY_UNAVAILABLE;

    if (action == M_PROPERTY_PRINT) {
        *(char **)arg = format_file_size(size);
        return M_PROPERTY_OK;
    }
    return m_property_int64_ro(action, arg, size);
}

static int mp_property_media_title(void *ctx, struct m_property *prop,
                                   int action, void *arg)
{
    MPContext *mpctx = ctx;
    char *name = NULL;
    if (mpctx->opts->media_title)
        name = mpctx->opts->media_title;
    if (name && name[0])
        return m_property_strdup_ro(action, arg, name);
    if (mpctx->demuxer) {
        name = mp_tags_get_str(mpctx->demuxer->metadata, "service_name");
        if (name && name[0])
            return m_property_strdup_ro(action, arg, name);
        name = mp_tags_get_str(mpctx->demuxer->metadata, "title");
        if (name && name[0])
            return m_property_strdup_ro(action, arg, name);
        name = mp_tags_get_str(mpctx->demuxer->metadata, "icy-title");
        if (name && name[0])
            return m_property_strdup_ro(action, arg, name);
    }
    if (mpctx->playing && mpctx->playing->title)
        return m_property_strdup_ro(action, arg, mpctx->playing->title);
    return mp_property_filename(ctx, prop, action, arg);
}

static int mp_property_stream_path(void *ctx, struct m_property *prop,
                                   int action, void *arg)
{
    MPContext *mpctx = ctx;
    if (!mpctx->demuxer || !mpctx->demuxer->filename)
        return M_PROPERTY_UNAVAILABLE;
    return m_property_strdup_ro(action, arg, mpctx->demuxer->filename);
}

/// Demuxer name (RO)
static int mp_property_demuxer(void *ctx, struct m_property *prop,
                               int action, void *arg)
{
    MPContext *mpctx = ctx;
    struct demuxer *demuxer = mpctx->demuxer;
    if (!demuxer)
        return M_PROPERTY_UNAVAILABLE;
    return m_property_strdup_ro(action, arg, demuxer->desc->name);
}

static int mp_property_file_format(void *ctx, struct m_property *prop,
                                   int action, void *arg)
{
    MPContext *mpctx = ctx;
    struct demuxer *demuxer = mpctx->demuxer;
    if (!demuxer)
        return M_PROPERTY_UNAVAILABLE;
    const char *name = demuxer->filetype ? demuxer->filetype : demuxer->desc->name;
    return m_property_strdup_ro(action, arg, name);
}

static int mp_property_stream_pos(void *ctx, struct m_property *prop,
                                  int action, void *arg)
{
    MPContext *mpctx = ctx;
    struct demuxer *demuxer = mpctx->demuxer;
    if (!demuxer || demuxer->filepos < 0)
        return M_PROPERTY_UNAVAILABLE;
    return m_property_int64_ro(action, arg, demuxer->filepos);
}

/// Stream end offset (RO)
static int mp_property_stream_end(void *ctx, struct m_property *prop,
                                  int action, void *arg)
{
    return mp_property_file_size(ctx, prop, action, arg);
}

// Does some magic to handle "<name>/full" as time formatted with milliseconds.
// Assumes prop is the type of the actual property.
static int property_time(int action, void *arg, double time)
{
    if (time == MP_NOPTS_VALUE)
        return M_PROPERTY_UNAVAILABLE;

    const struct m_option time_type = {.type = CONF_TYPE_TIME};
    switch (action) {
    case M_PROPERTY_GET:
        *(double *)arg = time;
        return M_PROPERTY_OK;
    case M_PROPERTY_GET_TYPE:
        *(struct m_option *)arg = time_type;
        return M_PROPERTY_OK;
    case M_PROPERTY_KEY_ACTION: {
        struct m_property_action_arg *ka = arg;

        if (strcmp(ka->key, "full") != 0)
            return M_PROPERTY_UNKNOWN;

        switch (ka->action) {
        case M_PROPERTY_GET:
            *(double *)ka->arg = time;
            return M_PROPERTY_OK;
        case M_PROPERTY_PRINT:
            *(char **)ka->arg = mp_format_time(time, true);
            return M_PROPERTY_OK;
        case M_PROPERTY_GET_TYPE:
            *(struct m_option *)ka->arg = time_type;
            return M_PROPERTY_OK;
        }
    }
    }
    return M_PROPERTY_NOT_IMPLEMENTED;
}

static int mp_property_duration(void *ctx, struct m_property *prop,
                                int action, void *arg)
{
    MPContext *mpctx = ctx;
    double len = get_time_length(mpctx);

    if (len < 0)
        return M_PROPERTY_UNAVAILABLE;

    return property_time(action, arg, len);
}

static int mp_property_avsync(void *ctx, struct m_property *prop,
                              int action, void *arg)
{
    MPContext *mpctx = ctx;
    if (!mpctx->ao_chain || !mpctx->vo_chain)
        return M_PROPERTY_UNAVAILABLE;
    if (action == M_PROPERTY_PRINT) {
        *(char **)arg = talloc_asprintf(NULL, "%7.3f", mpctx->last_av_difference);
        return M_PROPERTY_OK;
    }
    return m_property_double_ro(action, arg, mpctx->last_av_difference);
}

static int mp_property_total_avsync_change(void *ctx, struct m_property *prop,
                                           int action, void *arg)
{
    MPContext *mpctx = ctx;
    if (!mpctx->ao_chain || !mpctx->vo_chain)
        return M_PROPERTY_UNAVAILABLE;
    if (mpctx->total_avsync_change == MP_NOPTS_VALUE)
        return M_PROPERTY_UNAVAILABLE;
    return m_property_double_ro(action, arg, mpctx->total_avsync_change);
}

static int mp_property_frame_drop_dec(void *ctx, struct m_property *prop,
                                      int action, void *arg)
{
    MPContext *mpctx = ctx;
    if (!mpctx->vo_chain)
        return M_PROPERTY_UNAVAILABLE;

    return m_property_int_ro(action, arg, mpctx->vo_chain->video_src->dropped_frames);
}

static int mp_property_mistimed_frame_count(void *ctx, struct m_property *prop,
                                            int action, void *arg)
{
    MPContext *mpctx = ctx;
    if (!mpctx->vo_chain || !mpctx->display_sync_active)
        return M_PROPERTY_UNAVAILABLE;

    return m_property_int_ro(action, arg, mpctx->mistimed_frames_total);
}

static int mp_property_vsync_ratio(void *ctx, struct m_property *prop,
                                   int action, void *arg)
{
    MPContext *mpctx = ctx;
    if (!mpctx->vo_chain || !mpctx->display_sync_active)
        return M_PROPERTY_UNAVAILABLE;

    int vsyncs = 0, frames = 0;
    for (int n = 0; n < mpctx->num_past_frames; n++) {
        int vsync = mpctx->past_frames[n].num_vsyncs;
        if (vsync < 0)
            break;
        vsyncs += vsync;
        frames += 1;
    }

    if (!frames)
        return M_PROPERTY_UNAVAILABLE;

    return m_property_double_ro(action, arg, vsyncs / (double)frames);
}

static int mp_property_frame_drop_vo(void *ctx, struct m_property *prop,
                                     int action, void *arg)
{
    MPContext *mpctx = ctx;
    if (!mpctx->vo_chain)
        return M_PROPERTY_UNAVAILABLE;

    return m_property_int_ro(action, arg, vo_get_drop_count(mpctx->video_out));
}

static int mp_property_vo_delayed_frame_count(void *ctx, struct m_property *prop,
                                              int action, void *arg)
{
    MPContext *mpctx = ctx;
    if (!mpctx->vo_chain)
        return M_PROPERTY_UNAVAILABLE;

    return m_property_int_ro(action, arg, vo_get_delayed_count(mpctx->video_out));
}

/// Current position in percent (RW)
static int mp_property_percent_pos(void *ctx, struct m_property *prop,
                                   int action, void *arg)
{
    MPContext *mpctx = ctx;
    if (!mpctx->playback_initialized)
        return M_PROPERTY_UNAVAILABLE;

    switch (action) {
    case M_PROPERTY_SET: {
        double pos = *(double *)arg;
        queue_seek(mpctx, MPSEEK_FACTOR, pos / 100.0, MPSEEK_DEFAULT, 0);
        return M_PROPERTY_OK;
    }
    case M_PROPERTY_GET: {
        double pos = get_current_pos_ratio(mpctx, false) * 100.0;
        if (pos < 0)
            return M_PROPERTY_UNAVAILABLE;
        *(double *)arg = pos;
        return M_PROPERTY_OK;
    }
    case M_PROPERTY_GET_TYPE:
        *(struct m_option *)arg = (struct m_option){
            .type = CONF_TYPE_DOUBLE,
            .flags = M_OPT_RANGE,
            .min = 0,
            .max = 100,
        };
        return M_PROPERTY_OK;
    case M_PROPERTY_PRINT: {
        int pos = get_percent_pos(mpctx);
        if (pos < 0)
            return M_PROPERTY_UNAVAILABLE;
        *(char **)arg = talloc_asprintf(NULL, "%d", pos);
        return M_PROPERTY_OK;
    }
    }
    return M_PROPERTY_NOT_IMPLEMENTED;
}

static int mp_property_time_start(void *ctx, struct m_property *prop,
                                  int action, void *arg)
{
    // minor backwards-compat.
    return property_time(action, arg, 0);
}

/// Current position in seconds (RW)
static int mp_property_time_pos(void *ctx, struct m_property *prop,
                                int action, void *arg)
{
    MPContext *mpctx = ctx;
    if (!mpctx->playback_initialized)
        return M_PROPERTY_UNAVAILABLE;

    if (action == M_PROPERTY_SET) {
        queue_seek(mpctx, MPSEEK_ABSOLUTE, *(double *)arg, MPSEEK_DEFAULT, 0);
        return M_PROPERTY_OK;
    }
    return property_time(action, arg, get_current_time(mpctx));
}

/// Current audio pts in seconds (R)
static int mp_property_audio_pts(void *ctx, struct m_property *prop,
                                int action, void *arg)
{
    MPContext *mpctx = ctx;
    if (!mpctx->playback_initialized || mpctx->audio_status < STATUS_PLAYING ||
        mpctx->audio_status >= STATUS_EOF)
        return M_PROPERTY_UNAVAILABLE;

    return property_time(action, arg, playing_audio_pts(mpctx));
}

static bool time_remaining(MPContext *mpctx, double *remaining)
{
    double len = get_time_length(mpctx);
    double playback = get_playback_time(mpctx);

    if (playback == MP_NOPTS_VALUE || len <= 0)
        return false;

    *remaining = len - playback;

    return len >= 0;
}

static int mp_property_remaining(void *ctx, struct m_property *prop,
                                 int action, void *arg)
{
    double remaining;
    if (!time_remaining(ctx, &remaining))
        return M_PROPERTY_UNAVAILABLE;

    return property_time(action, arg, remaining);
}

static int mp_property_playtime_remaining(void *ctx, struct m_property *prop,
                                          int action, void *arg)
{
    MPContext *mpctx = ctx;
    double remaining;
    if (!time_remaining(mpctx, &remaining))
        return M_PROPERTY_UNAVAILABLE;

    double speed = mpctx->video_speed;
    return property_time(action, arg, remaining / speed);
}

static int mp_property_playback_time(void *ctx, struct m_property *prop,
                                     int action, void *arg)
{
    MPContext *mpctx = ctx;
    if (!mpctx->playback_initialized)
        return M_PROPERTY_UNAVAILABLE;

    if (action == M_PROPERTY_SET) {
        queue_seek(mpctx, MPSEEK_ABSOLUTE, *(double *)arg, MPSEEK_DEFAULT, 0);
        return M_PROPERTY_OK;
    }
    return property_time(action, arg, get_playback_time(mpctx));
}

/// Current BD/DVD title (RW)
static int mp_property_disc_title(void *ctx, struct m_property *prop,
                                  int action, void *arg)
{
    MPContext *mpctx = ctx;
    struct demuxer *d = mpctx->demuxer;
    if (!d)
        return M_PROPERTY_UNAVAILABLE;
    unsigned int title = -1;
    switch (action) {
    case M_PROPERTY_GET:
        if (demux_stream_control(d, STREAM_CTRL_GET_CURRENT_TITLE, &title) < 0)
            return M_PROPERTY_UNAVAILABLE;
        *(int*)arg = title;
        return M_PROPERTY_OK;
    case M_PROPERTY_GET_TYPE:
        *(struct m_option *)arg = (struct m_option){
            .type = CONF_TYPE_INT,
            .flags = M_OPT_MIN,
            .min = -1,
        };
        return M_PROPERTY_OK;
    case M_PROPERTY_SET:
        title = *(int*)arg;
        if (demux_stream_control(d, STREAM_CTRL_SET_CURRENT_TITLE, &title) < 0)
            return M_PROPERTY_NOT_IMPLEMENTED;
        if (!mpctx->stop_play)
            mpctx->stop_play = PT_RELOAD_FILE;
        return M_PROPERTY_OK;
    }
    return M_PROPERTY_NOT_IMPLEMENTED;
}

/// Current chapter (RW)
static int mp_property_chapter(void *ctx, struct m_property *prop,
                               int action, void *arg)
{
    MPContext *mpctx = ctx;
    if (!mpctx->playback_initialized)
        return M_PROPERTY_UNAVAILABLE;

    int chapter = get_current_chapter(mpctx);
    int num = get_chapter_count(mpctx);
    if (chapter < -1)
        return M_PROPERTY_UNAVAILABLE;

    switch (action) {
    case M_PROPERTY_GET:
        *(int *) arg = chapter;
        return M_PROPERTY_OK;
    case M_PROPERTY_GET_TYPE:
        *(struct m_option *)arg = (struct m_option){
            .type = CONF_TYPE_INT,
            .flags = M_OPT_MIN | M_OPT_MAX,
            .min = -1,
            .max = num - 1,
        };
        return M_PROPERTY_OK;
    case M_PROPERTY_PRINT: {
        *(char **) arg = chapter_display_name(mpctx, chapter);
        return M_PROPERTY_OK;
    }
    case M_PROPERTY_SWITCH:
    case M_PROPERTY_SET: ;
        mark_seek(mpctx);
        int step_all;
        if (action == M_PROPERTY_SWITCH) {
            struct m_property_switch_arg *sarg = arg;
            step_all = lrint(sarg->inc);
            // Check threshold for relative backward seeks
            if (mpctx->opts->chapter_seek_threshold >= 0 && step_all < 0) {
                double current_chapter_start =
                    chapter_start_time(mpctx, chapter);
                // If we are far enough into a chapter, seek back to the
                // beginning of current chapter instead of previous one
                if (current_chapter_start != MP_NOPTS_VALUE &&
                    get_current_time(mpctx) - current_chapter_start >
                    mpctx->opts->chapter_seek_threshold)
                {
                    step_all++;
                }
            }
        } else // Absolute set
            step_all = *(int *)arg - chapter;
        chapter += step_all;
        if (chapter < -1)
            chapter = -1;
        if (chapter >= num && step_all > 0) {
            if (mpctx->opts->keep_open) {
                seek_to_last_frame(mpctx);
            } else {
                // semi-broken file; ignore for user convenience
                if (action == M_PROPERTY_SWITCH && num < 2)
                    return M_PROPERTY_UNAVAILABLE;
                if (!mpctx->stop_play)
                    mpctx->stop_play = PT_NEXT_ENTRY;
                mp_wakeup_core(mpctx);
            }
        } else {
            double pts = chapter_start_time(mpctx, chapter);
            if (pts != MP_NOPTS_VALUE) {
                queue_seek(mpctx, MPSEEK_ABSOLUTE, pts, MPSEEK_DEFAULT, 0);
                mpctx->last_chapter_seek = chapter;
                mpctx->last_chapter_pts = pts;
            }
        }
        return M_PROPERTY_OK;
    }
    return M_PROPERTY_NOT_IMPLEMENTED;
}

static int get_chapter_entry(int item, int action, void *arg, void *ctx)
{
    struct MPContext *mpctx = ctx;
    char *name = chapter_name(mpctx, item);
    double time = chapter_start_time(mpctx, item);
    struct m_sub_property props[] = {
        {"title",       SUB_PROP_STR(name)},
        {"time",        {.type = CONF_TYPE_TIME}, {.time = time}},
        {0}
    };

    int r = m_property_read_sub(props, action, arg);
    return r;
}

static int parse_node_chapters(struct MPContext *mpctx,
                               struct mpv_node *given_chapters)
{
    if (!mpctx->demuxer)
        return M_PROPERTY_UNAVAILABLE;

    if (given_chapters->format != MPV_FORMAT_NODE_ARRAY)
        return M_PROPERTY_ERROR;

    double len = get_time_length(mpctx);

    talloc_free(mpctx->chapters);
    mpctx->num_chapters = 0;
    mpctx->chapters = talloc_array(NULL, struct demux_chapter, 0);

    for (int n = 0; n < given_chapters->u.list->num; n++) {
        struct mpv_node *chapter_data = &given_chapters->u.list->values[n];

        if (chapter_data->format != MPV_FORMAT_NODE_MAP)
            continue;

        mpv_node_list *chapter_data_elements = chapter_data->u.list;

        double time = -1;
        char *title = 0;

        for (int e = 0; e < chapter_data_elements->num; e++) {
            struct mpv_node *chapter_data_element =
                &chapter_data_elements->values[e];
            char *key = chapter_data_elements->keys[e];
            switch (chapter_data_element->format) {
            case MPV_FORMAT_INT64:
                if (strcmp(key, "time") == 0)
                    time = (double)chapter_data_element->u.int64;
                break;
            case MPV_FORMAT_DOUBLE:
                if (strcmp(key, "time") == 0)
                    time = chapter_data_element->u.double_;
                break;
            case MPV_FORMAT_STRING:
                if (strcmp(key, "title") == 0)
                    title = chapter_data_element->u.string;
                break;
            }
        }

        if (time >= 0 && time < len) {
            struct demux_chapter new = {
                .pts = time,
                .metadata = talloc_zero(mpctx->chapters, struct mp_tags),
            };
            if (title)
                mp_tags_set_str(new.metadata, "title", title);
            MP_TARRAY_APPEND(NULL, mpctx->chapters, mpctx->num_chapters, new);
        }
    }

    mp_notify(mpctx, MPV_EVENT_CHAPTER_CHANGE, NULL);

    return M_PROPERTY_OK;
}

static int mp_property_list_chapters(void *ctx, struct m_property *prop,
                                     int action, void *arg)
{
    MPContext *mpctx = ctx;
    int count = get_chapter_count(mpctx);
    switch (action) {
    case M_PROPERTY_PRINT: {
        int cur = mpctx->playback_initialized ? get_current_chapter(mpctx) : -1;
        char *res = NULL;
        int n;

        if (count < 1) {
            res = talloc_asprintf_append(res, "No chapters.");
        }

        for (n = 0; n < count; n++) {
            char *name = chapter_display_name(mpctx, n);
            double t = chapter_start_time(mpctx, n);
            char* time = mp_format_time(t, false);
            res = talloc_asprintf_append(res, "%s", time);
            talloc_free(time);
            const char *m = n == cur ? list_current : list_normal;
            res = talloc_asprintf_append(res, "   %s%s\n", m, name);
            talloc_free(name);
        }

        *(char **)arg = res;
        return M_PROPERTY_OK;
    }
    case M_PROPERTY_SET: {
        struct mpv_node *given_chapters = arg;
        return parse_node_chapters(mpctx, given_chapters);
    }
    }
    return m_property_read_list(action, arg, count, get_chapter_entry, mpctx);
}

static int mp_property_edition(void *ctx, struct m_property *prop,
                               int action, void *arg)
{
    MPContext *mpctx = ctx;
    struct demuxer *demuxer = mpctx->demuxer;
    if (!mpctx->playback_initialized || !demuxer || demuxer->num_editions <= 0)
        return mp_property_generic_option(mpctx, prop, action, arg);

    int edition = demuxer->edition;

    switch (action) {
    case M_PROPERTY_GET:
        *(int *)arg = edition;
        return M_PROPERTY_OK;
    case M_PROPERTY_SET: {
        edition = *(int *)arg;
        if (edition != demuxer->edition) {
            mpctx->opts->edition_id = edition;
            if (!mpctx->stop_play)
                mpctx->stop_play = PT_RELOAD_FILE;
            mp_wakeup_core(mpctx);
            break; // make it accessible to the demuxer via option change notify
        }
        return M_PROPERTY_OK;
    }
    case M_PROPERTY_GET_CONSTRICTED_TYPE: {
        int r = mp_property_generic_option(mpctx, prop, M_PROPERTY_GET_TYPE, arg);
        ((struct m_option *)arg)->max = demuxer->num_editions - 1;
        return r;
    }
    }
    return mp_property_generic_option(mpctx, prop, action, arg);
}

static int get_edition_entry(int item, int action, void *arg, void *ctx)
{
    struct MPContext *mpctx = ctx;

    struct demuxer *demuxer = mpctx->demuxer;
    struct demux_edition *ed = &demuxer->editions[item];

    char *title = mp_tags_get_str(ed->metadata, "title");

    struct m_sub_property props[] = {
        {"id",          SUB_PROP_INT(item)},
        {"title",       SUB_PROP_STR(title),
                        .unavailable = !title},
        {"default",     SUB_PROP_FLAG(ed->default_edition)},
        {0}
    };

    return m_property_read_sub(props, action, arg);
}

static int property_list_editions(void *ctx, struct m_property *prop,
                                  int action, void *arg)
{
    MPContext *mpctx = ctx;
    struct demuxer *demuxer = mpctx->demuxer;
    if (!demuxer)
        return M_PROPERTY_UNAVAILABLE;

    if (action == M_PROPERTY_PRINT) {
        char *res = NULL;

        struct demux_edition *editions = demuxer->editions;
        int num_editions = demuxer->num_editions;
        int current = demuxer->edition;

        if (!num_editions)
            res = talloc_asprintf_append(res, "No editions.");

        for (int n = 0; n < num_editions; n++) {
            struct demux_edition *ed = &editions[n];

            res = talloc_strdup_append(res, n == current ? list_current
                                                         : list_normal);
            res = talloc_asprintf_append(res, "%d: ", n);
            char *title = mp_tags_get_str(ed->metadata, "title");
            if (!title)
                title = "unnamed";
            res = talloc_asprintf_append(res, "'%s'\n", title);
        }

        *(char **)arg = res;
        return M_PROPERTY_OK;
    }
    return m_property_read_list(action, arg, demuxer->num_editions,
                                get_edition_entry, mpctx);
}

/// Number of titles in BD/DVD
static int mp_property_disc_titles(void *ctx, struct m_property *prop,
                                   int action, void *arg)
{
    MPContext *mpctx = ctx;
    struct demuxer *demuxer = mpctx->demuxer;
    unsigned int num_titles;
    if (!demuxer || demux_stream_control(demuxer, STREAM_CTRL_GET_NUM_TITLES,
                                         &num_titles) < 1)
        return M_PROPERTY_UNAVAILABLE;
    return m_property_int_ro(action, arg, num_titles);
}

static int get_disc_title_entry(int item, int action, void *arg, void *ctx)
{
    struct MPContext *mpctx = ctx;
    struct demuxer *demuxer = mpctx->demuxer;

    double len = item;
    if (demux_stream_control(demuxer, STREAM_CTRL_GET_TITLE_LENGTH, &len) < 1)
        len = -1;

    struct m_sub_property props[] = {
        {"id",          SUB_PROP_INT(item)},
        {"length",      {.type = CONF_TYPE_TIME}, {.time = len},
                        .unavailable = len < 0},
        {0}
    };

    return m_property_read_sub(props, action, arg);
}

static int mp_property_list_disc_titles(void *ctx, struct m_property *prop,
                                        int action, void *arg)
{
    MPContext *mpctx = ctx;
    struct demuxer *demuxer = mpctx->demuxer;
    unsigned int num_titles;
    if (!demuxer || demux_stream_control(demuxer, STREAM_CTRL_GET_NUM_TITLES,
                                         &num_titles) < 1)
        return M_PROPERTY_UNAVAILABLE;
    return m_property_read_list(action, arg, num_titles,
                                get_disc_title_entry, mpctx);
}

/// Number of chapters in file
static int mp_property_chapters(void *ctx, struct m_property *prop,
                                int action, void *arg)
{
    MPContext *mpctx = ctx;
    if (!mpctx->playback_initialized)
        return M_PROPERTY_UNAVAILABLE;
    int count = get_chapter_count(mpctx);
    return m_property_int_ro(action, arg, count);
}

static int mp_property_editions(void *ctx, struct m_property *prop,
                                int action, void *arg)
{
    MPContext *mpctx = ctx;
    struct demuxer *demuxer = mpctx->demuxer;
    if (!demuxer)
        return M_PROPERTY_UNAVAILABLE;
    if (demuxer->num_editions <= 0)
        return M_PROPERTY_UNAVAILABLE;
    return m_property_int_ro(action, arg, demuxer->num_editions);
}

/// Current dvd angle (RW)
static int mp_property_angle(void *ctx, struct m_property *prop,
                             int action, void *arg)
{
    MPContext *mpctx = ctx;
    struct demuxer *demuxer = mpctx->demuxer;
    if (!demuxer)
        return M_PROPERTY_UNAVAILABLE;

    int ris, angles = -1, angle = 1;

    ris = demux_stream_control(demuxer, STREAM_CTRL_GET_NUM_ANGLES, &angles);
    if (ris == STREAM_UNSUPPORTED)
        return M_PROPERTY_UNAVAILABLE;

    ris = demux_stream_control(demuxer, STREAM_CTRL_GET_ANGLE, &angle);
    if (ris == STREAM_UNSUPPORTED)
        return -1;

    if (angle < 0 || angles <= 1)
        return M_PROPERTY_UNAVAILABLE;

    switch (action) {
    case M_PROPERTY_GET:
        *(int *) arg = angle;
        return M_PROPERTY_OK;
    case M_PROPERTY_PRINT: {
        *(char **) arg = talloc_asprintf(NULL, "%d/%d", angle, angles);
        return M_PROPERTY_OK;
    }
    case M_PROPERTY_SET:
        angle = *(int *)arg;
        if (angle < 0 || angle > angles)
            return M_PROPERTY_ERROR;

        demux_flush(demuxer);
        ris = demux_stream_control(demuxer, STREAM_CTRL_SET_ANGLE, &angle);
        if (ris == STREAM_OK) {
            demux_control(demuxer, DEMUXER_CTRL_RESYNC, NULL);
            demux_flush(demuxer);
        }

        reset_audio_state(mpctx);
        reset_video_state(mpctx);
        mp_wakeup_core(mpctx);

        return ris == STREAM_OK ? M_PROPERTY_OK : M_PROPERTY_ERROR;
    case M_PROPERTY_GET_TYPE: {
        struct m_option opt = {
            .type = CONF_TYPE_INT,
            .flags = CONF_RANGE,
            .min = 1,
            .max = angles,
        };
        *(struct m_option *)arg = opt;
        return M_PROPERTY_OK;
    }
    }
    return M_PROPERTY_NOT_IMPLEMENTED;
}

static int get_tag_entry(int item, int action, void *arg, void *ctx)
{
    struct mp_tags *tags = ctx;

    struct m_sub_property props[] = {
        {"key",     SUB_PROP_STR(tags->keys[item])},
        {"value",   SUB_PROP_STR(tags->values[item])},
        {0}
    };

    return m_property_read_sub(props, action, arg);
}

static int tag_property(int action, void *arg, struct mp_tags *tags)
{
    switch (action) {
    case M_PROPERTY_GET: {
        mpv_node_list *list = talloc_zero(NULL, mpv_node_list);
        mpv_node node = {
            .format = MPV_FORMAT_NODE_MAP,
            .u.list = list,
        };
        list->num = tags->num_keys;
        list->values = talloc_array(list, mpv_node, list->num);
        list->keys = talloc_array(list, char*, list->num);
        for (int n = 0; n < tags->num_keys; n++) {
            list->keys[n] = talloc_strdup(list, tags->keys[n]);
            list->values[n] = (struct mpv_node){
                .format = MPV_FORMAT_STRING,
                .u.string = talloc_strdup(list, tags->values[n]),
            };
        }
        *(mpv_node*)arg = node;
        return M_PROPERTY_OK;
    }
    case M_PROPERTY_GET_TYPE: {
        *(struct m_option *)arg = (struct m_option){.type = CONF_TYPE_NODE};
        return M_PROPERTY_OK;
    }
    case M_PROPERTY_PRINT: {
        char *res = NULL;
        for (int n = 0; n < tags->num_keys; n++) {
            res = talloc_asprintf_append_buffer(res, "%s: %s\n",
                                                tags->keys[n], tags->values[n]);
        }
        if (!res)
            res = talloc_strdup(NULL, "(empty)");
        *(char **)arg = res;
        return M_PROPERTY_OK;
    }
    case M_PROPERTY_KEY_ACTION: {
        struct m_property_action_arg *ka = arg;
        bstr key;
        char *rem;
        m_property_split_path(ka->key, &key, &rem);
        if (bstr_equals0(key, "list")) {
            struct m_property_action_arg nka = *ka;
            nka.key = rem;
            return m_property_read_list(action, &nka, tags->num_keys,
                                        get_tag_entry, tags);
        }
        // Direct access without this prefix is allowed for compatibility.
        bstr k = bstr0(ka->key);
        bstr_eatstart0(&k, "by-key/");
        char *meta = mp_tags_get_bstr(tags, k);
        if (!meta)
            return M_PROPERTY_UNKNOWN;
        switch (ka->action) {
        case M_PROPERTY_GET:
            *(char **)ka->arg = talloc_strdup(NULL, meta);
            return M_PROPERTY_OK;
        case M_PROPERTY_GET_TYPE:
            *(struct m_option *)ka->arg = (struct m_option){
                .type = CONF_TYPE_STRING,
            };
            return M_PROPERTY_OK;
        }
    }
    }
    return M_PROPERTY_NOT_IMPLEMENTED;
}

/// Demuxer meta data
static int mp_property_metadata(void *ctx, struct m_property *prop,
                                int action, void *arg)
{
    MPContext *mpctx = ctx;
    struct demuxer *demuxer = mpctx->demuxer;
    if (!demuxer)
        return M_PROPERTY_UNAVAILABLE;

    return tag_property(action, arg, demuxer->metadata);
}

static int mp_property_filtered_metadata(void *ctx, struct m_property *prop,
                                         int action, void *arg)
{
    MPContext *mpctx = ctx;
    if (!mpctx->filtered_tags)
        return M_PROPERTY_UNAVAILABLE;

    return tag_property(action, arg, mpctx->filtered_tags);
}

static int mp_property_chapter_metadata(void *ctx, struct m_property *prop,
                                        int action, void *arg)
{
    MPContext *mpctx = ctx;
    int chapter = get_current_chapter(mpctx);
    if (chapter < 0)
        return M_PROPERTY_UNAVAILABLE;
    return tag_property(action, arg, mpctx->chapters[chapter].metadata);
}

static int mp_property_filter_metadata(void *ctx, struct m_property *prop,
                                       int action, void *arg)
{
    MPContext *mpctx = ctx;
    const char *type = prop->priv;

    if (action == M_PROPERTY_KEY_ACTION) {
        struct m_property_action_arg *ka = arg;
        bstr key;
        char *rem;
        m_property_split_path(ka->key, &key, &rem);
        struct mp_tags metadata = {0};
        int res = CONTROL_UNKNOWN;
        if (strcmp(type, "vf") == 0) {
            if (!mpctx->vo_chain)
                return M_PROPERTY_UNAVAILABLE;
            struct vf_chain *vf = mpctx->vo_chain->vf;
            res = vf_control_by_label(vf, VFCTRL_GET_METADATA, &metadata, key);
        } else if (strcmp(type, "af") == 0) {
#if HAVE_LIBAF
            if (!(mpctx->ao_chain && mpctx->ao_chain->af))
                return M_PROPERTY_UNAVAILABLE;
            struct af_stream *af = mpctx->ao_chain->af;
            res = af_control_by_label(af, AF_CONTROL_GET_METADATA, &metadata, key);
#endif
        }
        switch (res) {
        case CONTROL_UNKNOWN:
            return M_PROPERTY_UNKNOWN;
        case CONTROL_NA: // empty
        case CONTROL_OK:
            if (strlen(rem)) {
                struct m_property_action_arg next_ka = *ka;
                next_ka.key = rem;
                return tag_property(M_PROPERTY_KEY_ACTION, &next_ka, &metadata);
            } else {
                return tag_property(ka->action, ka->arg, &metadata);
            }
            return M_PROPERTY_OK;
        default:
            return M_PROPERTY_ERROR;
        }
    }
    return M_PROPERTY_NOT_IMPLEMENTED;
}

static int mp_property_pause(void *ctx, struct m_property *prop,
                             int action, void *arg)
{
    MPContext *mpctx = ctx;

    if (mpctx->playback_initialized && action == M_PROPERTY_SET) {
        set_pause_state(mpctx, *(int *)arg);
        return M_PROPERTY_OK;
    }
    return mp_property_generic_option(mpctx, prop, action, arg);
}

static int mp_property_core_idle(void *ctx, struct m_property *prop,
                                 int action, void *arg)
{
    MPContext *mpctx = ctx;
    return m_property_flag_ro(action, arg, !mpctx->playback_active);
}

static int mp_property_idle(void *ctx, struct m_property *prop,
                            int action, void *arg)
{
    MPContext *mpctx = ctx;
    struct command_ctx *cmd = mpctx->command_ctx;
    return m_property_flag_ro(action, arg, cmd->is_idle);
}

static int mp_property_eof_reached(void *ctx, struct m_property *prop,
                                   int action, void *arg)
{
    MPContext *mpctx = ctx;
    if (!mpctx->playback_initialized)
        return M_PROPERTY_UNAVAILABLE;
    bool eof = mpctx->video_status == STATUS_EOF &&
               mpctx->audio_status == STATUS_EOF;
    return m_property_flag_ro(action, arg, eof);
}

static int mp_property_seeking(void *ctx, struct m_property *prop,
                               int action, void *arg)
{
    MPContext *mpctx = ctx;
    if (!mpctx->playback_initialized)
        return M_PROPERTY_UNAVAILABLE;
    return m_property_flag_ro(action, arg, !mpctx->restart_complete);
}

static int mp_property_playback_abort(void *ctx, struct m_property *prop,
                                      int action, void *arg)
{
    MPContext *mpctx = ctx;
    return m_property_flag_ro(action, arg, !mpctx->playing || mpctx->stop_play);
}

static int mp_property_cache(void *ctx, struct m_property *prop,
                             int action, void *arg)
{
    MPContext *mpctx = ctx;
    float cache = mp_get_cache_percent(mpctx);
    if (cache < 0)
        return M_PROPERTY_UNAVAILABLE;

    if (action == M_PROPERTY_PRINT) {
        *(char **)arg = talloc_asprintf(NULL, "%d", (int)cache);
        return M_PROPERTY_OK;
    }

    return m_property_float_ro(action, arg, cache);
}

static int property_int_kb_size(int kb_size, int action, void *arg)
{
    switch (action) {
    case M_PROPERTY_GET:
        *(int *)arg = kb_size;
        return M_PROPERTY_OK;
    case M_PROPERTY_PRINT:
        *(char **)arg = format_file_size(kb_size * 1024LL);
        return M_PROPERTY_OK;
    case M_PROPERTY_GET_TYPE:
        *(struct m_option *)arg = (struct m_option){.type = CONF_TYPE_INT};
        return M_PROPERTY_OK;
    }
    return M_PROPERTY_NOT_IMPLEMENTED;
}

static int mp_property_cache_size(void *ctx, struct m_property *prop,
                                  int action, void *arg)
{
    MPContext *mpctx = ctx;
    struct demuxer *demuxer = mpctx->demuxer;
    if (!demuxer)
        return M_PROPERTY_UNAVAILABLE;
    switch (action) {
    case M_PROPERTY_GET:
    case M_PROPERTY_PRINT: {
        struct stream_cache_info info = {0};
        demux_stream_control(demuxer, STREAM_CTRL_GET_CACHE_INFO, &info);
        if (info.size <= 0)
            break;
        return property_int_kb_size(info.size / 1024, action, arg);
    }
    case M_PROPERTY_GET_TYPE:
        *(struct m_option *)arg = (struct m_option){
            .type = CONF_TYPE_INT,
            .flags = M_OPT_MIN,
            .min = 0,
        };
        return M_PROPERTY_OK;
    case M_PROPERTY_SET: {
        int64_t size = *(int *)arg * 1024LL;
        int r = demux_stream_control(demuxer, STREAM_CTRL_SET_CACHE_SIZE, &size);
        if (r == STREAM_UNSUPPORTED)
            break;
        if (r == STREAM_OK)
            return M_PROPERTY_OK;
        return M_PROPERTY_ERROR;
    }
    }
    return M_PROPERTY_NOT_IMPLEMENTED;
}

static int mp_property_cache_used(void *ctx, struct m_property *prop,
                                  int action, void *arg)
{
    MPContext *mpctx = ctx;
    if (!mpctx->demuxer)
        return M_PROPERTY_UNAVAILABLE;

    struct stream_cache_info info = {0};
    demux_stream_control(mpctx->demuxer, STREAM_CTRL_GET_CACHE_INFO, &info);
    if (info.size <= 0)
        return M_PROPERTY_UNAVAILABLE;
    return property_int_kb_size(info.fill / 1024, action, arg);
}

static int mp_property_cache_free(void *ctx, struct m_property *prop,
                                  int action, void *arg)
{
    MPContext *mpctx = ctx;
    if (!mpctx->demuxer)
        return M_PROPERTY_UNAVAILABLE;

    struct stream_cache_info info = {0};
    demux_stream_control(mpctx->demuxer, STREAM_CTRL_GET_CACHE_INFO, &info);
    if (info.size <= 0)
        return M_PROPERTY_UNAVAILABLE;

    return property_int_kb_size((info.size - info.fill) / 1024, action, arg);
}

static int mp_property_cache_speed(void *ctx, struct m_property *prop,
                                   int action, void *arg)
{
    MPContext *mpctx = ctx;
    if (!mpctx->demuxer)
        return M_PROPERTY_UNAVAILABLE;

    struct stream_cache_info info = {0};
    demux_stream_control(mpctx->demuxer, STREAM_CTRL_GET_CACHE_INFO, &info);
    if (info.size <= 0)
        return M_PROPERTY_UNAVAILABLE;

    if (action == M_PROPERTY_PRINT) {
        *(char **)arg = talloc_strdup_append(format_file_size(info.speed), "/s");
        return M_PROPERTY_OK;
    }
    return m_property_int64_ro(action, arg, info.speed);
}

static int mp_property_cache_idle(void *ctx, struct m_property *prop,
                                  int action, void *arg)
{
    MPContext *mpctx = ctx;
    struct stream_cache_info info = {0};
    if (mpctx->demuxer)
        demux_stream_control(mpctx->demuxer, STREAM_CTRL_GET_CACHE_INFO, &info);
    if (info.size <= 0)
        return M_PROPERTY_UNAVAILABLE;
    return m_property_flag_ro(action, arg, info.idle);
}

static int mp_property_demuxer_cache_duration(void *ctx, struct m_property *prop,
                                              int action, void *arg)
{
    MPContext *mpctx = ctx;
    if (!mpctx->demuxer)
        return M_PROPERTY_UNAVAILABLE;

    struct demux_ctrl_reader_state s;
    if (demux_control(mpctx->demuxer, DEMUXER_CTRL_GET_READER_STATE, &s) < 1)
        return M_PROPERTY_UNAVAILABLE;

    if (s.ts_duration < 0)
        return M_PROPERTY_UNAVAILABLE;

    return m_property_double_ro(action, arg, s.ts_duration);
}

static int mp_property_demuxer_cache_time(void *ctx, struct m_property *prop,
                                          int action, void *arg)
{
    MPContext *mpctx = ctx;
    if (!mpctx->demuxer)
        return M_PROPERTY_UNAVAILABLE;

    struct demux_ctrl_reader_state s;
    if (demux_control(mpctx->demuxer, DEMUXER_CTRL_GET_READER_STATE, &s) < 1)
        return M_PROPERTY_UNAVAILABLE;

    if (s.ts_end == MP_NOPTS_VALUE)
        return M_PROPERTY_UNAVAILABLE;

    return m_property_double_ro(action, arg, s.ts_end);
}

static int mp_property_demuxer_cache_idle(void *ctx, struct m_property *prop,
                                          int action, void *arg)
{
    MPContext *mpctx = ctx;
    if (!mpctx->demuxer)
        return M_PROPERTY_UNAVAILABLE;

    struct demux_ctrl_reader_state s;
    if (demux_control(mpctx->demuxer, DEMUXER_CTRL_GET_READER_STATE, &s) < 1)
        return M_PROPERTY_UNAVAILABLE;

    return m_property_flag_ro(action, arg, s.idle);
}

static int mp_property_demuxer_cache_state(void *ctx, struct m_property *prop,
                                           int action, void *arg)
{
    MPContext *mpctx = ctx;
    if (!mpctx->demuxer)
        return M_PROPERTY_UNAVAILABLE;

    if (action == M_PROPERTY_GET_TYPE) {
        *(struct m_option *)arg = (struct m_option){.type = CONF_TYPE_NODE};
        return M_PROPERTY_OK;
    }
    if (action != M_PROPERTY_GET)
        return M_PROPERTY_NOT_IMPLEMENTED;

    struct demux_ctrl_reader_state s;
    if (demux_control(mpctx->demuxer, DEMUXER_CTRL_GET_READER_STATE, &s) < 1)
        return M_PROPERTY_UNAVAILABLE;

    struct mpv_node *r = (struct mpv_node *)arg;
    node_init(r, MPV_FORMAT_NODE_MAP, NULL);

    struct mpv_node *ranges =
        node_map_add(r, "seekable-ranges", MPV_FORMAT_NODE_ARRAY);
    for (int n = 0; n < s.num_seek_ranges; n++) {
        struct demux_seek_range *range = &s.seek_ranges[n];
        struct mpv_node *sub = node_array_add(ranges, MPV_FORMAT_NODE_MAP);
        node_map_add_double(sub, "start", range->start);
        node_map_add_double(sub, "end", range->end);
    }

    if (s.ts_end != MP_NOPTS_VALUE)
        node_map_add_double(r, "cache-end", s.ts_end);

    if (s.ts_reader != MP_NOPTS_VALUE)
        node_map_add_double(r, "reader-pts", s.ts_reader);

    node_map_add_flag(r, "eof", s.eof);
    node_map_add_flag(r, "underrun", s.underrun);
    node_map_add_flag(r, "idle", s.idle);
    node_map_add_int64(r, "total-bytes", s.total_bytes);
    node_map_add_int64(r, "fw-bytes", s.fw_bytes);

    return M_PROPERTY_OK;
}

static int mp_property_demuxer_start_time(void *ctx, struct m_property *prop,
                                          int action, void *arg)
{
    MPContext *mpctx = ctx;
    if (!mpctx->demuxer)
        return M_PROPERTY_UNAVAILABLE;

    return m_property_double_ro(action, arg, mpctx->demuxer->start_time);
}

static int mp_property_paused_for_cache(void *ctx, struct m_property *prop,
                                        int action, void *arg)
{
    MPContext *mpctx = ctx;
    if (!mpctx->playback_initialized)
        return M_PROPERTY_UNAVAILABLE;
    return m_property_flag_ro(action, arg, mpctx->paused_for_cache);
}

static int mp_property_cache_buffering(void *ctx, struct m_property *prop,
                                       int action, void *arg)
{
    MPContext *mpctx = ctx;
    int state = get_cache_buffering_percentage(mpctx);
    if (state < 0)
        return M_PROPERTY_UNAVAILABLE;
    return m_property_int_ro(action, arg, state);
}

static int mp_property_demuxer_is_network(void *ctx, struct m_property *prop,
                                          int action, void *arg)
{
    MPContext *mpctx = ctx;
    if (!mpctx->demuxer)
        return M_PROPERTY_UNAVAILABLE;

    return m_property_flag_ro(action, arg, mpctx->demuxer->is_network);
}


static int mp_property_clock(void *ctx, struct m_property *prop,
                             int action, void *arg)
{
    char outstr[6];
    time_t t = time(NULL);
    struct tm *tmp = localtime(&t);

    if ((tmp != NULL) && (strftime(outstr, sizeof(outstr), "%H:%M", tmp) == 5))
        return m_property_strdup_ro(action, arg, outstr);
    return M_PROPERTY_UNAVAILABLE;
}

static int mp_property_seekable(void *ctx, struct m_property *prop,
                                int action, void *arg)
{
    MPContext *mpctx = ctx;
    if (!mpctx->demuxer)
        return M_PROPERTY_UNAVAILABLE;
    return m_property_flag_ro(action, arg, mpctx->demuxer->seekable);
}

static int mp_property_partially_seekable(void *ctx, struct m_property *prop,
                                          int action, void *arg)
{
    MPContext *mpctx = ctx;
    if (!mpctx->demuxer)
        return M_PROPERTY_UNAVAILABLE;
    return m_property_flag_ro(action, arg, mpctx->demuxer->partially_seekable);
}

static int mp_property_mixer_active(void *ctx, struct m_property *prop,
                                    int action, void *arg)
{
    MPContext *mpctx = ctx;
    return m_property_flag_ro(action, arg, !!mpctx->ao);
}

/// Volume (RW)
static int mp_property_volume(void *ctx, struct m_property *prop,
                              int action, void *arg)
{
    MPContext *mpctx = ctx;
    struct MPOpts *opts = mpctx->opts;

    switch (action) {
    case M_PROPERTY_GET_CONSTRICTED_TYPE:
        *(struct m_option *)arg = (struct m_option){
            .type = CONF_TYPE_FLOAT,
            .flags = M_OPT_RANGE,
            .min = 0,
            .max = opts->softvol_max,
        };
        return M_PROPERTY_OK;
    case M_PROPERTY_PRINT:
        *(char **)arg = talloc_asprintf(NULL, "%i", (int)opts->softvol_volume);
        return M_PROPERTY_OK;
    }

    return mp_property_generic_option(mpctx, prop, action, arg);
}

/// Mute (RW)
static int mp_property_mute(void *ctx, struct m_property *prop,
                            int action, void *arg)
{
    MPContext *mpctx = ctx;

    if (action == M_PROPERTY_GET_CONSTRICTED_TYPE) {
        *(struct m_option *)arg = (struct m_option){.type = CONF_TYPE_FLAG};
        return M_PROPERTY_OK;
    }

    int r = mp_property_generic_option(mpctx, prop, action, arg);
    if (action == M_PROPERTY_SET)
        audio_update_volume(mpctx);
    return r;
}

static int mp_property_ao_volume(void *ctx, struct m_property *prop,
                                 int action, void *arg)
{
    MPContext *mpctx = ctx;
    struct ao *ao = mpctx->ao;
    if (!ao)
        return M_PROPERTY_NOT_IMPLEMENTED;

    switch (action) {
    case M_PROPERTY_SET: {
        float value = *(float *)arg;
        ao_control_vol_t vol = {value, value};
        if (ao_control(ao, AOCONTROL_SET_VOLUME, &vol) != CONTROL_OK)
            return M_PROPERTY_UNAVAILABLE;
        return M_PROPERTY_OK;
    }
    case M_PROPERTY_GET: {
        ao_control_vol_t vol = {0};
        if (ao_control(ao, AOCONTROL_GET_VOLUME, &vol) != CONTROL_OK)
            return M_PROPERTY_UNAVAILABLE;
        *(float *)arg = (vol.left + vol.right) / 2.0f;
        return M_PROPERTY_OK;
    }
    case M_PROPERTY_GET_TYPE:
        *(struct m_option *)arg = (struct m_option){
            .type = CONF_TYPE_FLOAT,
            .flags = M_OPT_RANGE,
            .min = 0,
            .max = 100,
        };
        return M_PROPERTY_OK;
    case M_PROPERTY_PRINT: {
        ao_control_vol_t vol = {0};
        if (ao_control(ao, AOCONTROL_GET_VOLUME, &vol) != CONTROL_OK)
            return M_PROPERTY_UNAVAILABLE;
        *(char **)arg = talloc_asprintf(NULL, "%.f", (vol.left + vol.right) / 2.0f);
        return M_PROPERTY_OK;
    }
    }
    return M_PROPERTY_NOT_IMPLEMENTED;
}


static int mp_property_ao_mute(void *ctx, struct m_property *prop,
                               int action, void *arg)
{
    MPContext *mpctx = ctx;
    struct ao *ao = mpctx->ao;
    if (!ao)
        return M_PROPERTY_NOT_IMPLEMENTED;

    switch (action) {
    case M_PROPERTY_SET: {
        bool value = *(int *)arg;
        if (ao_control(ao, AOCONTROL_SET_MUTE, &value) != CONTROL_OK)
            return M_PROPERTY_UNAVAILABLE;
        return M_PROPERTY_OK;
    }
    case M_PROPERTY_GET: {
        bool value = false;
        if (ao_control(ao, AOCONTROL_GET_MUTE, &value) != CONTROL_OK)
            return M_PROPERTY_UNAVAILABLE;
        *(int *)arg = value;
        return M_PROPERTY_OK;
    }
    case M_PROPERTY_GET_TYPE:
        *(struct m_option *)arg = (struct m_option){.type = CONF_TYPE_FLAG};
        return M_PROPERTY_OK;
    }
    return M_PROPERTY_NOT_IMPLEMENTED;
}

static int get_device_entry(int item, int action, void *arg, void *ctx)
{
    struct ao_device_list *list = ctx;
    struct ao_device_desc *entry = &list->devices[item];

    struct m_sub_property props[] = {
        {"name",        SUB_PROP_STR(entry->name)},
        {"description", SUB_PROP_STR(entry->desc)},
        {0}
    };

    return m_property_read_sub(props, action, arg);
}

static void create_hotplug(struct MPContext *mpctx)
{
    struct command_ctx *cmd = mpctx->command_ctx;

    if (!cmd->hotplug) {
        cmd->hotplug = ao_hotplug_create(mpctx->global, mp_wakeup_core_cb,
                                         mpctx);
    }
}

static int mp_property_audio_device(void *ctx, struct m_property *prop,
                                    int action, void *arg)
{
    struct MPContext *mpctx = ctx;
    struct command_ctx *cmd = mpctx->command_ctx;
    if (action == M_PROPERTY_PRINT) {
        create_hotplug(mpctx);

        struct ao_device_list *list = ao_hotplug_get_device_list(cmd->hotplug);
        for (int n = 0; n < list->num_devices; n++) {
            struct ao_device_desc *dev = &list->devices[n];
            if (dev->name && strcmp(dev->name, mpctx->opts->audio_device) == 0) {
                *(char **)arg = talloc_strdup(NULL, dev->desc ? dev->desc : "?");
                return M_PROPERTY_OK;
            }
        }
    }
    return mp_property_generic_option(mpctx, prop, action, arg);
}

static int mp_property_audio_devices(void *ctx, struct m_property *prop,
                                     int action, void *arg)
{
    struct MPContext *mpctx = ctx;
    struct command_ctx *cmd = mpctx->command_ctx;
    create_hotplug(mpctx);

    struct ao_device_list *list = ao_hotplug_get_device_list(cmd->hotplug);
    return m_property_read_list(action, arg, list->num_devices,
                                get_device_entry, list);
}

static int mp_property_ao(void *ctx, struct m_property *p, int action, void *arg)
{
    MPContext *mpctx = ctx;
    return m_property_strdup_ro(action, arg,
                                    mpctx->ao ? ao_get_name(mpctx->ao) : NULL);
}

/// Audio delay (RW)
static int mp_property_audio_delay(void *ctx, struct m_property *prop,
                                   int action, void *arg)
{
    MPContext *mpctx = ctx;
    float delay = mpctx->opts->audio_delay;
    switch (action) {
    case M_PROPERTY_PRINT:
        *(char **)arg = format_delay(delay);
        return M_PROPERTY_OK;
    case M_PROPERTY_SET:
        mpctx->opts->audio_delay = *(float *)arg;
        if (mpctx->ao_chain && mpctx->vo_chain)
            mpctx->delay += mpctx->opts->audio_delay - delay;
        mp_wakeup_core(mpctx);
        return M_PROPERTY_OK;
    }
    return mp_property_generic_option(mpctx, prop, action, arg);
}

/// Audio codec tag (RO)
static int mp_property_audio_codec_name(void *ctx, struct m_property *prop,
                                        int action, void *arg)
{
    MPContext *mpctx = ctx;
    struct track *track = mpctx->current_track[0][STREAM_AUDIO];
    const char *c = track && track->stream ? track->stream->codec->codec : NULL;
    return m_property_strdup_ro(action, arg, c);
}

/// Audio codec name (RO)
static int mp_property_audio_codec(void *ctx, struct m_property *prop,
                                   int action, void *arg)
{
    MPContext *mpctx = ctx;
    struct track *track = mpctx->current_track[0][STREAM_AUDIO];
    const char *c = track && track->d_audio ? track->d_audio->decoder_desc : NULL;
    return m_property_strdup_ro(action, arg, c);
}

static int property_audiofmt(struct mp_aframe *fmt, int action, void *arg)
{
    if (!fmt || !mp_aframe_config_is_valid(fmt))
        return M_PROPERTY_UNAVAILABLE;

    struct mp_chmap chmap = {0};
    mp_aframe_get_chmap(fmt, &chmap);

    struct m_sub_property props[] = {
        {"samplerate",      SUB_PROP_INT(mp_aframe_get_rate(fmt))},
        {"channel-count",   SUB_PROP_INT(chmap.num)},
        {"channels",        SUB_PROP_STR(mp_chmap_to_str(&chmap))},
        {"hr-channels",     SUB_PROP_STR(mp_chmap_to_str_hr(&chmap))},
        {"format",          SUB_PROP_STR(af_fmt_to_str(mp_aframe_get_format(fmt)))},
        {0}
    };

    return m_property_read_sub(props, action, arg);
}

static int mp_property_audio_params(void *ctx, struct m_property *prop,
                                    int action, void *arg)
{
    MPContext *mpctx = ctx;
    return property_audiofmt(mpctx->ao_chain ? mpctx->ao_chain->input_format : NULL,
                             action, arg);
}

static int mp_property_audio_out_params(void *ctx, struct m_property *prop,
                                        int action, void *arg)
{
    MPContext *mpctx = ctx;
    struct mp_aframe *frame = NULL;
    if (mpctx->ao) {
        frame = mp_aframe_create();
        int samplerate;
        int format;
        struct mp_chmap channels;
        ao_get_format(mpctx->ao, &samplerate, &format, &channels);
        mp_aframe_set_rate(frame, samplerate);
        mp_aframe_set_format(frame, format);
        mp_aframe_set_chmap(frame, &channels);
    }
    int r = property_audiofmt(frame, action, arg);
    talloc_free(frame);
    return r;
}

static struct track* track_next(struct MPContext *mpctx, enum stream_type type,
                                int direction, struct track *track)
{
    assert(direction == -1 || direction == +1);
    struct track *prev = NULL, *next = NULL;
    bool seen = track == NULL;
    for (int n = 0; n < mpctx->num_tracks; n++) {
        struct track *cur = mpctx->tracks[n];
        if (cur->type == type) {
            if (cur == track) {
                seen = true;
            } else if (!cur->selected) {
                if (seen && !next) {
                    next = cur;
                }
                if (!seen || !track) {
                    prev = cur;
                }
            }
        }
    }
    return direction > 0 ? next : prev;
}

static int property_switch_track(struct m_property *prop, int action, void *arg,
                                 MPContext *mpctx, int order,
                                 enum stream_type type)
{
    struct track *track = mpctx->current_track[order][type];

    switch (action) {
    case M_PROPERTY_GET:
        if (mpctx->playback_initialized) {
            *(int *)arg = track ? track->user_tid : -2;
        } else {
            *(int *)arg = mpctx->opts->stream_id[order][type];
        }
        return M_PROPERTY_OK;
    case M_PROPERTY_PRINT:
        if (!track)
            *(char **) arg = talloc_strdup(NULL, "no");
        else {
            char *lang = track->lang;
            if (!lang)
                lang = "unknown";

            if (track->title)
                *(char **)arg = talloc_asprintf(NULL, "(%d) %s (\"%s\")",
                                           track->user_tid, lang, track->title);
            else
                *(char **)arg = talloc_asprintf(NULL, "(%d) %s",
                                                track->user_tid, lang);
        }
        return M_PROPERTY_OK;

    case M_PROPERTY_SWITCH: {
        if (!mpctx->playback_initialized)
            return M_PROPERTY_ERROR;
        struct m_property_switch_arg *sarg = arg;
        do {
            track = track_next(mpctx, type, sarg->inc >= 0 ? +1 : -1, track);
            mp_switch_track_n(mpctx, order, type, track, FLAG_MARK_SELECTION);
        } while (mpctx->current_track[order][type] != track);
        print_track_list(mpctx, "Track switched:");
        return M_PROPERTY_OK;
    }
    case M_PROPERTY_SET:
        if (mpctx->playback_initialized) {
            track = mp_track_by_tid(mpctx, type, *(int *)arg);
            mp_switch_track_n(mpctx, order, type, track, FLAG_MARK_SELECTION);
            print_track_list(mpctx, "Track switched:");
            mp_wakeup_core(mpctx);
        } else {
            mpctx->opts->stream_id[order][type] = *(int *)arg;
        }
        return M_PROPERTY_OK;
    }
    return mp_property_generic_option(mpctx, prop, action, arg);
}

static int track_channels(struct track *track)
{
    return track->stream ? track->stream->codec->channels.num : 0;
}

static int get_track_entry(int item, int action, void *arg, void *ctx)
{
    struct MPContext *mpctx = ctx;
    struct track *track = mpctx->tracks[item];

    struct mp_codec_params p =
        track->stream ? *track->stream->codec : (struct mp_codec_params){0};

    const char *decoder_desc = NULL;
    if (track->d_video)
        decoder_desc = track->d_video->decoder_desc;
    if (track->d_audio)
        decoder_desc = track->d_audio->decoder_desc;

    bool has_rg = track->stream && track->stream->codec->replaygain_data;
    struct replaygain_data rg = has_rg ? *track->stream->codec->replaygain_data
                                       : (struct replaygain_data){0};

    struct m_sub_property props[] = {
        {"id",          SUB_PROP_INT(track->user_tid)},
        {"type",        SUB_PROP_STR(stream_type_name(track->type)),
                        .unavailable = !stream_type_name(track->type)},
        {"src-id",      SUB_PROP_INT(track->demuxer_id),
                        .unavailable = track->demuxer_id == -1},
        {"title",       SUB_PROP_STR(track->title),
                        .unavailable = !track->title},
        {"lang",        SUB_PROP_STR(track->lang),
                        .unavailable = !track->lang},
        {"audio-channels", SUB_PROP_INT(track_channels(track)),
                        .unavailable = track_channels(track) <= 0},
        {"albumart",    SUB_PROP_FLAG(track->attached_picture)},
        {"default",     SUB_PROP_FLAG(track->default_track)},
        {"forced",      SUB_PROP_FLAG(track->forced_track)},
        {"external",    SUB_PROP_FLAG(track->is_external)},
        {"selected",    SUB_PROP_FLAG(track->selected)},
        {"external-filename", SUB_PROP_STR(track->external_filename),
                        .unavailable = !track->external_filename},
        {"ff-index",    SUB_PROP_INT(track->ff_index)},
        {"decoder-desc", SUB_PROP_STR(decoder_desc),
                        .unavailable = !decoder_desc},
        {"codec",       SUB_PROP_STR(p.codec),
                        .unavailable = !p.codec},
        {"demux-w",     SUB_PROP_INT(p.disp_w), .unavailable = !p.disp_w},
        {"demux-h",     SUB_PROP_INT(p.disp_h), .unavailable = !p.disp_h},
        {"demux-channel-count", SUB_PROP_INT(p.channels.num),
                        .unavailable = !p.channels.num},
        {"demux-channels", SUB_PROP_STR(mp_chmap_to_str(&p.channels)),
                        .unavailable = !p.channels.num},
        {"demux-samplerate", SUB_PROP_INT(p.samplerate),
                        .unavailable = !p.samplerate},
        {"demux-fps",   SUB_PROP_DOUBLE(p.fps), .unavailable = p.fps <= 0},
        {"replaygain-track-peak", SUB_PROP_FLOAT(rg.track_peak),
                        .unavailable = !has_rg},
        {"replaygain-track-gain", SUB_PROP_FLOAT(rg.track_gain),
                        .unavailable = !has_rg},
        {"replaygain-album-peak", SUB_PROP_FLOAT(rg.album_peak),
                        .unavailable = !has_rg},
        {"replaygain-album-gain", SUB_PROP_FLOAT(rg.album_gain),
                        .unavailable = !has_rg},
        {0}
    };

    return m_property_read_sub(props, action, arg);
}

static const char *track_type_name(enum stream_type t)
{
    switch (t) {
    case STREAM_VIDEO: return "Video";
    case STREAM_AUDIO: return "Audio";
    case STREAM_SUB: return "Sub";
    }
    return NULL;
}

static int property_list_tracks(void *ctx, struct m_property *prop,
                                int action, void *arg)
{
    MPContext *mpctx = ctx;
    if (action == M_PROPERTY_PRINT) {
        char *res = NULL;

        for (int type = 0; type < STREAM_TYPE_COUNT; type++) {
            for (int n = 0; n < mpctx->num_tracks; n++) {
                struct track *track = mpctx->tracks[n];
                if (track->type != type)
                    continue;

                res = talloc_asprintf_append(res, "%s: ",
                                             track_type_name(track->type));
                res = talloc_strdup_append(res,
                                track->selected ? list_current : list_normal);
                res = talloc_asprintf_append(res, "(%d) ", track->user_tid);
                if (track->title)
                    res = talloc_asprintf_append(res, "'%s' ", track->title);
                if (track->lang)
                    res = talloc_asprintf_append(res, "(%s) ", track->lang);
                if (track->is_external)
                    res = talloc_asprintf_append(res, "(external) ");
                res = talloc_asprintf_append(res, "\n");
            }

            res = talloc_asprintf_append(res, "\n");
        }

        struct demuxer *demuxer = mpctx->demuxer;
        if (demuxer && demuxer->num_editions > 1)
            res = talloc_asprintf_append(res, "\nEdition: %d of %d\n",
                                        demuxer->edition + 1,
                                        demuxer->num_editions);

        *(char **)arg = res;
        return M_PROPERTY_OK;
    }
    return m_property_read_list(action, arg, mpctx->num_tracks,
                                get_track_entry, mpctx);
}

/// Selected audio id (RW)
static int mp_property_audio(void *ctx, struct m_property *prop,
                             int action, void *arg)
{
    return property_switch_track(prop, action, arg, ctx, 0, STREAM_AUDIO);
}

/// Selected video id (RW)
static int mp_property_video(void *ctx, struct m_property *prop,
                             int action, void *arg)
{
    return property_switch_track(prop, action, arg, ctx, 0, STREAM_VIDEO);
}

static struct track *find_track_by_demuxer_id(MPContext *mpctx,
                                              enum stream_type type,
                                              int demuxer_id)
{
    for (int n = 0; n < mpctx->num_tracks; n++) {
        struct track *track = mpctx->tracks[n];
        if (track->type == type && track->demuxer_id == demuxer_id)
            return track;
    }
    return NULL;
}

static int mp_property_program(void *ctx, struct m_property *prop,
                               int action, void *arg)
{
    MPContext *mpctx = ctx;
    demux_program_t prog = {0};

    struct demuxer *demuxer = mpctx->demuxer;
    if (!demuxer || !mpctx->playback_initialized)
        return M_PROPERTY_UNAVAILABLE;

    switch (action) {
    case M_PROPERTY_SWITCH:
    case M_PROPERTY_SET:
        if (action == M_PROPERTY_SET && arg)
            prog.progid = *((int *) arg);
        else
            prog.progid = -1;
        if (demux_control(demuxer, DEMUXER_CTRL_IDENTIFY_PROGRAM, &prog) ==
            CONTROL_UNKNOWN)
            return M_PROPERTY_ERROR;

        if (prog.aid < 0 && prog.vid < 0) {
            MP_ERR(mpctx, "Selected program contains no audio or video streams!\n");
            return M_PROPERTY_ERROR;
        }
        mp_switch_track(mpctx, STREAM_VIDEO,
                find_track_by_demuxer_id(mpctx, STREAM_VIDEO, prog.vid), 0);
        mp_switch_track(mpctx, STREAM_AUDIO,
                find_track_by_demuxer_id(mpctx, STREAM_AUDIO, prog.aid), 0);
        mp_switch_track(mpctx, STREAM_SUB,
                find_track_by_demuxer_id(mpctx, STREAM_VIDEO, prog.sid), 0);
        print_track_list(mpctx, "Program switched:");
        return M_PROPERTY_OK;
    case M_PROPERTY_GET_TYPE:
        *(struct m_option *)arg = (struct m_option){
            .type = CONF_TYPE_INT,
            .flags = CONF_RANGE,
            .min = -1,
            .max = (1 << 16) - 1,
        };
        return M_PROPERTY_OK;
    }
    return M_PROPERTY_NOT_IMPLEMENTED;
}

static int mp_property_hwdec(void *ctx, struct m_property *prop,
                             int action, void *arg)
{
    MPContext *mpctx = ctx;
    struct track *track = mpctx->current_track[0][STREAM_VIDEO];
    struct dec_video *vd = track ? track->d_video : NULL;
    struct MPOpts *opts = mpctx->opts;

    if (action == M_PROPERTY_SET) {
        char *new = *(char **)arg;

        if (strcmp(opts->hwdec_api, new) == 0)
            return M_PROPERTY_OK;

        talloc_free(opts->hwdec_api);
        opts->hwdec_api = talloc_strdup(NULL, new);

        if (!vd)
            return M_PROPERTY_OK;

        video_vd_control(vd, VDCTRL_REINIT, NULL);
        double last_pts = mpctx->last_vo_pts;
        if (last_pts != MP_NOPTS_VALUE)
            queue_seek(mpctx, MPSEEK_ABSOLUTE, last_pts, MPSEEK_EXACT, 0);

        return M_PROPERTY_OK;
    }
    return mp_property_generic_option(mpctx, prop, action, arg);
}

static int mp_property_hwdec_current(void *ctx, struct m_property *prop,
                                     int action, void *arg)
{
    MPContext *mpctx = ctx;
    struct track *track = mpctx->current_track[0][STREAM_VIDEO];
    struct dec_video *vd = track ? track->d_video : NULL;

    if (!vd)
        return M_PROPERTY_UNAVAILABLE;

    char *current = NULL;
    video_vd_control(vd, VDCTRL_GET_HWDEC, &current);
    if (!current)
        current = "no";
    return m_property_strdup_ro(action, arg, current);
}

static int mp_property_hwdec_interop(void *ctx, struct m_property *prop,
                                     int action, void *arg)
{
    MPContext *mpctx = ctx;
    if (!mpctx->video_out || !mpctx->video_out->hwdec_devs)
        return M_PROPERTY_UNAVAILABLE;

    char *names = hwdec_devices_get_names(mpctx->video_out->hwdec_devs);
    int res = m_property_strdup_ro(action, arg, names);
    talloc_free(names);
    return res;
}

/// Helper to set vo flags.
/** \ingroup PropertyImplHelper
 */
static int mp_property_vo_flag(struct m_property *prop, int action, void *arg,
                               int vo_ctrl, int *vo_var, MPContext *mpctx)
{
    int old = *vo_var;
    int res = mp_property_generic_option(mpctx, prop, action, arg);
    if (action == M_PROPERTY_SET && old != *vo_var) {
        if (mpctx->video_out)
            vo_control(mpctx->video_out, vo_ctrl, 0);
    }
    return res;
}

/// Fullscreen state (RW)
static int mp_property_fullscreen(void *ctx, struct m_property *prop,
                                  int action, void *arg)
{
    MPContext *mpctx = ctx;
    int oldval = mpctx->opts->vo->fullscreen;
    int r = mp_property_vo_flag(prop, action, arg, VOCTRL_FULLSCREEN,
                                &mpctx->opts->vo->fullscreen, mpctx);
    if (oldval && oldval != mpctx->opts->vo->fullscreen)
        mpctx->mouse_event_ts--; // Show mouse cursor
    return r;
}

/// Show playback progress in Windows 7+ taskbar (RW)
static int mp_property_taskbar_progress(void *ctx, struct m_property *prop,
                             int action, void *arg)
{
    MPContext *mpctx = ctx;
    if (action == M_PROPERTY_SET) {
        int desired = !!*(int *) arg;
        if (mpctx->opts->vo->taskbar_progress == desired)
            return M_PROPERTY_OK;
        mpctx->opts->vo->taskbar_progress = desired;
        if (mpctx->video_out)
            update_vo_playback_state(mpctx);
        return M_PROPERTY_OK;
    }
    return mp_property_generic_option(mpctx, prop, action, arg);
}

/// Window always on top (RW)
static int mp_property_ontop(void *ctx, struct m_property *prop,
                             int action, void *arg)
{
    MPContext *mpctx = ctx;
    return mp_property_vo_flag(prop, action, arg, VOCTRL_ONTOP,
                               &mpctx->opts->vo->ontop, mpctx);
}

/// Show window borders (RW)
static int mp_property_border(void *ctx, struct m_property *prop,
                              int action, void *arg)
{
    MPContext *mpctx = ctx;
    return mp_property_vo_flag(prop, action, arg, VOCTRL_BORDER,
                               &mpctx->opts->vo->border, mpctx);
}

static int mp_property_all_workspaces(void *ctx, struct m_property *prop,
                                      int action, void *arg)
{
    MPContext *mpctx = ctx;
    return mp_property_vo_flag(prop, action, arg, VOCTRL_ALL_WORKSPACES,
                               &mpctx->opts->vo->all_workspaces, mpctx);
}

static int get_frame_count(struct MPContext *mpctx)
{
    struct demuxer *demuxer = mpctx->demuxer;
    if (!demuxer)
        return -1;
    if (!mpctx->vo_chain)
        return -1;
    double len = get_time_length(mpctx);
    double fps = mpctx->vo_chain->container_fps;
    if (len < 0 || fps <= 0)
        return 0;

    return len * fps;
}

static int mp_property_frame_number(void *ctx, struct m_property *prop,
                                    int action, void *arg)
{
    MPContext *mpctx = ctx;
    int frames = get_frame_count(mpctx);
    if (frames < 0)
        return M_PROPERTY_UNAVAILABLE;

    return m_property_int_ro(action, arg,
        lrint(get_current_pos_ratio(mpctx, false) * frames));
}

static int mp_property_frame_count(void *ctx, struct m_property *prop,
                                   int action, void *arg)
{
    MPContext *mpctx = ctx;
    int frames = get_frame_count(mpctx);
    if (frames < 0)
        return M_PROPERTY_UNAVAILABLE;

    return m_property_int_ro(action, arg, frames);
}

/// Video codec tag (RO)
static int mp_property_video_format(void *ctx, struct m_property *prop,
                                    int action, void *arg)
{
    MPContext *mpctx = ctx;
    struct track *track = mpctx->current_track[0][STREAM_VIDEO];
    const char *c = track && track->stream ? track->stream->codec->codec : NULL;
    return m_property_strdup_ro(action, arg, c);
}

/// Video codec name (RO)
static int mp_property_video_codec(void *ctx, struct m_property *prop,
                                   int action, void *arg)
{
    MPContext *mpctx = ctx;
    struct track *track = mpctx->current_track[0][STREAM_VIDEO];
    const char *c = track && track->d_video ? track->d_video->decoder_desc : NULL;
    return m_property_strdup_ro(action, arg, c);
}

static int property_imgparams(struct mp_image_params p, int action, void *arg)
{
    if (!p.imgfmt)
        return M_PROPERTY_UNAVAILABLE;

    int d_w, d_h;
    mp_image_params_get_dsize(&p, &d_w, &d_h);

    struct mp_imgfmt_desc desc = mp_imgfmt_get_desc(p.imgfmt);
    int bpp = 0;
    for (int i = 0; i < desc.num_planes; i++)
        bpp += desc.bpp[i] >> (desc.xs[i] + desc.ys[i]);

    struct m_sub_property props[] = {
        {"pixelformat",     SUB_PROP_STR(mp_imgfmt_to_name(p.imgfmt))},
        {"average-bpp",     SUB_PROP_INT(bpp),
                            .unavailable = !bpp},
        {"plane-depth",     SUB_PROP_INT(desc.plane_bits),
                            .unavailable = !(desc.flags & MP_IMGFLAG_PLANAR)},
        {"w",               SUB_PROP_INT(p.w)},
        {"h",               SUB_PROP_INT(p.h)},
        {"dw",              SUB_PROP_INT(d_w)},
        {"dh",              SUB_PROP_INT(d_h)},
        {"aspect",          SUB_PROP_FLOAT(d_w / (double)d_h)},
        {"par",             SUB_PROP_FLOAT(p.p_w / (double)p.p_h)},
        {"colormatrix",
            SUB_PROP_STR(m_opt_choice_str(mp_csp_names, p.color.space))},
        {"colorlevels",
            SUB_PROP_STR(m_opt_choice_str(mp_csp_levels_names, p.color.levels))},
        {"primaries",
            SUB_PROP_STR(m_opt_choice_str(mp_csp_prim_names, p.color.primaries))},
        {"gamma",
            SUB_PROP_STR(m_opt_choice_str(mp_csp_trc_names, p.color.gamma))},
        {"sig-peak", SUB_PROP_FLOAT(p.color.sig_peak)},
        {"light",
            SUB_PROP_STR(m_opt_choice_str(mp_csp_light_names, p.color.light))},
        {"chroma-location",
            SUB_PROP_STR(m_opt_choice_str(mp_chroma_names, p.chroma_location))},
        {"stereo-in",
            SUB_PROP_STR(m_opt_choice_str(mp_stereo3d_names, p.stereo_in))},
        {"stereo-out",
            SUB_PROP_STR(m_opt_choice_str(mp_stereo3d_names, p.stereo_out))},
        {"rotate",          SUB_PROP_INT(p.rotate)},
        {0}
    };

    return m_property_read_sub(props, action, arg);
}

static struct mp_image_params get_video_out_params(struct MPContext *mpctx)
{
    if (!mpctx->vo_chain || mpctx->vo_chain->vf->initialized < 1)
        return (struct mp_image_params){0};

    return mpctx->vo_chain->vf->output_params;
}

static int mp_property_vo_imgparams(void *ctx, struct m_property *prop,
                                    int action, void *arg)
{
    return property_imgparams(get_video_out_params(ctx), action, arg);
}

static int mp_property_dec_imgparams(void *ctx, struct m_property *prop,
                                    int action, void *arg)
{
    MPContext *mpctx = ctx;
    struct mp_image_params p = {0};
    struct vo_chain *vo_c = mpctx->vo_chain;
    if (vo_c && vo_c->video_src)
        video_get_dec_params(vo_c->video_src, &p);
    if (!p.imgfmt)
        return M_PROPERTY_UNAVAILABLE;
    return property_imgparams(p, action, arg);
}

static int mp_property_vd_imgparams(void *ctx, struct m_property *prop,
                                    int action, void *arg)
{
    MPContext *mpctx = ctx;
    struct vo_chain *vo_c = mpctx->vo_chain;
    if (!vo_c)
        return M_PROPERTY_UNAVAILABLE;
    struct track *track = mpctx->current_track[0][STREAM_VIDEO];
    struct mp_codec_params *c =
        track && track->stream ? track->stream->codec : NULL;
    if (vo_c->vf->input_params.imgfmt) {
        return property_imgparams(vo_c->vf->input_params, action, arg);
    } else if (c && c->disp_w && c->disp_h) {
        // Simplistic fallback for stupid scripts querying "width"/"height"
        // before the first frame is decoded.
        struct m_sub_property props[] = {
            {"w", SUB_PROP_INT(c->disp_w)},
            {"h", SUB_PROP_INT(c->disp_h)},
            {0}
        };
        return m_property_read_sub(props, action, arg);
    }
    return M_PROPERTY_UNAVAILABLE;
}

static int mp_property_video_frame_info(void *ctx, struct m_property *prop,
                                        int action, void *arg)
{
    MPContext *mpctx = ctx;
    struct mp_image *f =
        mpctx->video_out ? vo_get_current_frame(mpctx->video_out) : NULL;
    if (!f)
        return M_PROPERTY_UNAVAILABLE;

    const char *pict_types[] = {0, "I", "P", "B"};
    const char *pict_type = f->pict_type >= 1 && f->pict_type <= 3
                          ? pict_types[f->pict_type] : NULL;

    struct m_sub_property props[] = {
        {"picture-type",    SUB_PROP_STR(pict_type), .unavailable = !pict_type},
        {"interlaced",      SUB_PROP_FLAG(!!(f->fields & MP_IMGFIELD_INTERLACED))},
        {"tff",             SUB_PROP_FLAG(!!(f->fields & MP_IMGFIELD_TOP_FIRST))},
        {"repeat",          SUB_PROP_FLAG(!!(f->fields & MP_IMGFIELD_REPEAT_FIRST))},
        {0}
    };

    talloc_free(f);
    return m_property_read_sub(props, action, arg);
}

static int mp_property_window_scale(void *ctx, struct m_property *prop,
                                    int action, void *arg)
{
    MPContext *mpctx = ctx;
    struct vo *vo = mpctx->video_out;
    if (!vo)
        goto generic;

    struct mp_image_params params = get_video_out_params(mpctx);
    int vid_w, vid_h;
    mp_image_params_get_dsize(&params, &vid_w, &vid_h);
    if (vid_w < 1 || vid_h < 1)
        goto generic;

    switch (action) {
    case M_PROPERTY_SET: {
        double scale = *(double *)arg;
        int s[2] = {vid_w * scale, vid_h * scale};
        if (s[0] > 0 && s[1] > 0)
            vo_control(vo, VOCTRL_SET_UNFS_WINDOW_SIZE, s);
        goto generic;
    }
    case M_PROPERTY_GET: {
        int s[2];
        if (vo_control(vo, VOCTRL_GET_UNFS_WINDOW_SIZE, s) <= 0 ||
            s[0] < 1 || s[1] < 1)
            goto generic;
        double xs = (double)s[0] / vid_w;
        double ys = (double)s[1] / vid_h;
        *(double *)arg = (xs + ys) / 2;
        return M_PROPERTY_OK;
    }
    }
generic:
    return mp_property_generic_option(mpctx, prop, action, arg);
}

static int mp_property_win_minimized(void *ctx, struct m_property *prop,
                                     int action, void *arg)
{
    MPContext *mpctx = ctx;
    struct vo *vo = mpctx->video_out;
    if (!vo)
        return M_PROPERTY_UNAVAILABLE;

    int state = 0;
    if (vo_control(vo, VOCTRL_GET_WIN_STATE, &state) < 1)
        return M_PROPERTY_UNAVAILABLE;

    return m_property_flag_ro(action, arg, state & VO_WIN_STATE_MINIMIZED);
}

static int mp_property_display_fps(void *ctx, struct m_property *prop,
                                   int action, void *arg)
{
    MPContext *mpctx = ctx;
    double fps = mpctx->opts->frame_drop_fps;
    struct vo *vo = mpctx->video_out;
    if (vo)
        fps = vo_get_display_fps(vo);
    if (action == M_PROPERTY_SET) {
        int ret = mp_property_generic_option(mpctx, prop, action, arg);
        if (vo)
            vo_event(vo, VO_EVENT_WIN_STATE);
        return ret;
    }
    return m_property_double_ro(action, arg, fps);
}

static int mp_property_framedrop(void *ctx, struct m_property *prop,
                                 int action, void *arg)
{
    MPContext *mpctx = ctx;
    int ret = mp_property_generic_option(mpctx, prop, action, arg);
    if (action == M_PROPERTY_SET && ret == M_PROPERTY_OK && mpctx->video_out)
        vo_event(mpctx->video_out, VO_EVENT_WIN_STATE);
    return ret;
}

static int mp_property_estimated_display_fps(void *ctx, struct m_property *prop,
                                             int action, void *arg)
{
    MPContext *mpctx = ctx;
    struct vo *vo = mpctx->video_out;
    if (!vo)
        return M_PROPERTY_UNAVAILABLE;
    double interval = vo_get_estimated_vsync_interval(vo);
    if (interval <= 0)
        return M_PROPERTY_UNAVAILABLE;
    return m_property_double_ro(action, arg, 1.0 / interval);
}

static int mp_property_vsync_jitter(void *ctx, struct m_property *prop,
                                    int action, void *arg)
{
    MPContext *mpctx = ctx;
    struct vo *vo = mpctx->video_out;
    if (!vo)
        return M_PROPERTY_UNAVAILABLE;
    double stddev = vo_get_estimated_vsync_jitter(vo);
    if (stddev < 0)
        return M_PROPERTY_UNAVAILABLE;
    return m_property_double_ro(action, arg, stddev);
}

static int mp_property_display_names(void *ctx, struct m_property *prop,
                                     int action, void *arg)
{
    MPContext *mpctx = ctx;
    struct vo *vo = mpctx->video_out;
    if (!vo)
        return M_PROPERTY_UNAVAILABLE;

    switch (action) {
    case M_PROPERTY_GET_TYPE:
        *(struct m_option *)arg = (struct m_option){.type = CONF_TYPE_STRING_LIST};
        return M_PROPERTY_OK;
    case M_PROPERTY_GET: {
        char** display_names;
        if (vo_control(vo, VOCTRL_GET_DISPLAY_NAMES, &display_names) < 1)
            return M_PROPERTY_UNAVAILABLE;

        *(char ***)arg = display_names;
        return M_PROPERTY_OK;
    }
    }
    return M_PROPERTY_NOT_IMPLEMENTED;
}

static int mp_property_vo_configured(void *ctx, struct m_property *prop,
                                     int action, void *arg)
{
    MPContext *mpctx = ctx;
    return m_property_flag_ro(action, arg,
                        mpctx->video_out && mpctx->video_out->config_ok);
}

static void get_frame_perf(struct mpv_node *node, struct mp_frame_perf *perf)
{
    for (int i = 0; i < perf->count; i++) {
        struct mp_pass_perf *data = &perf->perf[i];
        struct mpv_node *pass = node_array_add(node, MPV_FORMAT_NODE_MAP);

        node_map_add_string(pass, "desc", perf->desc[i]);
        node_map_add(pass, "last", MPV_FORMAT_INT64)->u.int64 = data->last;
        node_map_add(pass, "avg", MPV_FORMAT_INT64)->u.int64 = data->avg;
        node_map_add(pass, "peak", MPV_FORMAT_INT64)->u.int64 = data->peak;
        node_map_add(pass, "count", MPV_FORMAT_INT64)->u.int64 = data->count;
        struct mpv_node *samples = node_map_add(pass, "samples", MPV_FORMAT_NODE_ARRAY);
        for (int n = 0; n < data->count; n++)
            node_array_add(samples, MPV_FORMAT_INT64)->u.int64 = data->samples[n];
    }
}

static char *asprint_perf(char *res, struct mp_frame_perf *perf)
{
    for (int i = 0; i < perf->count; i++) {
        struct mp_pass_perf *pass = &perf->perf[i];
        res = talloc_asprintf_append(res,
                  "- %s: last %dus avg %dus peak %dus\n", perf->desc[i],
                  (int)pass->last/1000, (int)pass->avg/1000, (int)pass->peak/1000);
    }

    return res;
}

static int mp_property_vo_passes(void *ctx, struct m_property *prop,
                                 int action, void *arg)
{
    MPContext *mpctx = ctx;
    if (!mpctx->video_out)
        return M_PROPERTY_UNAVAILABLE;

    // Return the type right away if requested, to avoid having to
    // go through a completely unnecessary VOCTRL
    if (action == M_PROPERTY_GET_TYPE) {
        *(struct m_option *)arg = (struct m_option){.type = CONF_TYPE_NODE};
        return M_PROPERTY_OK;
    }

    int ret = M_PROPERTY_UNAVAILABLE;
    struct voctrl_performance_data *data = talloc_ptrtype(NULL, data);
    if (vo_control(mpctx->video_out, VOCTRL_PERFORMANCE_DATA, data) <= 0)
        goto out;

    switch (action) {
    case M_PROPERTY_PRINT: {
        char *res = NULL;
        res = talloc_asprintf_append(res, "fresh:\n");
        res = asprint_perf(res, &data->fresh);
        res = talloc_asprintf_append(res, "\nredraw:\n");
        res = asprint_perf(res, &data->redraw);
        *(char **)arg = res;
        ret = M_PROPERTY_OK;
        goto out;
    }

    case M_PROPERTY_GET: {
        struct mpv_node node;
        node_init(&node, MPV_FORMAT_NODE_MAP, NULL);
        struct mpv_node *fresh = node_map_add(&node, "fresh", MPV_FORMAT_NODE_ARRAY);
        struct mpv_node *redraw = node_map_add(&node, "redraw", MPV_FORMAT_NODE_ARRAY);
        get_frame_perf(fresh, &data->fresh);
        get_frame_perf(redraw, &data->redraw);
        *(struct mpv_node *)arg = node;
        ret = M_PROPERTY_OK;
        goto out;
    }
    }

    ret = M_PROPERTY_NOT_IMPLEMENTED;

out:
    talloc_free(data);
    return ret;
}

static int mp_property_vo(void *ctx, struct m_property *p, int action, void *arg)
{
    MPContext *mpctx = ctx;
    return m_property_strdup_ro(action, arg,
                    mpctx->video_out ? mpctx->video_out->driver->name : NULL);
}

static int mp_property_osd_w(void *ctx, struct m_property *prop,
                             int action, void *arg)
{
    MPContext *mpctx = ctx;
    struct mp_osd_res vo_res = osd_get_vo_res(mpctx->osd);
    return m_property_int_ro(action, arg, vo_res.w);
}

static int mp_property_osd_h(void *ctx, struct m_property *prop,
                             int action, void *arg)
{
    MPContext *mpctx = ctx;
    struct mp_osd_res vo_res = osd_get_vo_res(mpctx->osd);
    return m_property_int_ro(action, arg, vo_res.h);
}

static int mp_property_osd_par(void *ctx, struct m_property *prop,
                               int action, void *arg)
{
    MPContext *mpctx = ctx;
    struct mp_osd_res vo_res = osd_get_vo_res(mpctx->osd);
    return m_property_double_ro(action, arg, vo_res.display_par);
}

static int mp_property_osd_sym(void *ctx, struct m_property *prop,
                               int action, void *arg)
{
    MPContext *mpctx = ctx;
    char temp[20];
    get_current_osd_sym(mpctx, temp, sizeof(temp));
    return m_property_strdup_ro(action, arg, temp);
}

static int mp_property_osd_ass(void *ctx, struct m_property *prop,
                               int action, void *arg)
{
    struct m_sub_property props[] = {
        {"0",   SUB_PROP_STR(OSD_ASS_0)},
        {"1",   SUB_PROP_STR(OSD_ASS_1)},
        {0}
    };
    return m_property_read_sub(props, action, arg);
}

/// Video fps (RO)
static int mp_property_fps(void *ctx, struct m_property *prop,
                           int action, void *arg)
{
    MPContext *mpctx = ctx;
    float fps = mpctx->vo_chain ? mpctx->vo_chain->container_fps : 0;
    if (fps < 0.1 || !isfinite(fps))
        return M_PROPERTY_UNAVAILABLE;;
    return m_property_float_ro(action, arg, fps);
}

static int mp_property_vf_fps(void *ctx, struct m_property *prop,
                              int action, void *arg)
{
    MPContext *mpctx = ctx;
    if (!mpctx->vo_chain)
        return M_PROPERTY_UNAVAILABLE;
    double avg = calc_average_frame_duration(mpctx);
    if (avg <= 0)
        return M_PROPERTY_UNAVAILABLE;
    return m_property_double_ro(action, arg, 1.0 / avg);
}

/// Video aspect (RO)
static int mp_property_aspect(void *ctx, struct m_property *prop,
                              int action, void *arg)
{
    MPContext *mpctx = ctx;

    float aspect = mpctx->opts->movie_aspect;
    if (mpctx->vo_chain && aspect <= 0) {
        struct mp_image_params *params = &mpctx->vo_chain->vf->input_params;
        if (params && params->p_w > 0 && params->p_h > 0) {
            int d_w, d_h;
            mp_image_params_get_dsize(params, &d_w, &d_h);
            aspect = (float)d_w / d_h;
        }
    }
    struct track *track = mpctx->current_track[0][STREAM_VIDEO];
    if (track && track->d_video && aspect <= 0) {
        struct dec_video *d_video = track->d_video;
        struct mp_codec_params *c = d_video->header->codec;
        if (c->disp_w && c->disp_h)
            aspect = (float)c->disp_w / c->disp_h;
    }

    switch (action) {
    case M_PROPERTY_PRINT: {
        if (mpctx->opts->movie_aspect < 0) {
            *(char **)arg = talloc_asprintf(NULL, "%.3f (original)", aspect);
            return M_PROPERTY_OK;
        }
        break;
    }
    case M_PROPERTY_GET: {
        *(float *)arg = aspect;
        return M_PROPERTY_OK;
    }
    }
    return mp_property_generic_option(mpctx, prop, action, arg);
}

/// Selected subtitles (RW)
static int mp_property_sub(void *ctx, struct m_property *prop,
                           int action, void *arg)
{
    return property_switch_track(prop, action, arg, ctx, 0, STREAM_SUB);
}

static int mp_property_sub2(void *ctx, struct m_property *prop,
                            int action, void *arg)
{
    return property_switch_track(prop, action, arg, ctx, 1, STREAM_SUB);
}

/// Subtitle delay (RW)
static int mp_property_sub_delay(void *ctx, struct m_property *prop,
                                 int action, void *arg)
{
    MPContext *mpctx = ctx;
    struct MPOpts *opts = mpctx->opts;
    switch (action) {
    case M_PROPERTY_PRINT:
        *(char **)arg = format_delay(opts->sub_delay);
        return M_PROPERTY_OK;
    }
    return mp_property_generic_option(mpctx, prop, action, arg);
}

/// Subtitle speed (RW)
static int mp_property_sub_speed(void *ctx, struct m_property *prop,
                                 int action, void *arg)
{
    MPContext *mpctx = ctx;
    struct MPOpts *opts = mpctx->opts;
    if (action == M_PROPERTY_PRINT) {
        *(char **)arg = talloc_asprintf(NULL, "%4.1f%%", 100 * opts->sub_speed);
        return M_PROPERTY_OK;
    }
    return mp_property_generic_option(mpctx, prop, action, arg);
}

static int mp_property_sub_pos(void *ctx, struct m_property *prop,
                               int action, void *arg)
{
    MPContext *mpctx = ctx;
    struct MPOpts *opts = mpctx->opts;
    if (action == M_PROPERTY_PRINT) {
        *(char **)arg = talloc_asprintf(NULL, "%d/100", opts->sub_pos);
        return M_PROPERTY_OK;
    }
    return mp_property_generic_option(mpctx, prop, action, arg);
}

static int mp_property_sub_text(void *ctx, struct m_property *prop,
                                int action, void *arg)
{
    MPContext *mpctx = ctx;
    struct track *track = mpctx->current_track[0][STREAM_SUB];
    struct dec_sub *sub = track ? track->d_sub : NULL;
    double pts = mpctx->playback_pts;
    if (!sub || pts == MP_NOPTS_VALUE)
        return M_PROPERTY_UNAVAILABLE;

    pts -= mpctx->opts->sub_delay;

    char *text = sub_get_text(sub, pts);
    if (!text)
        text = "";

    return m_property_strdup_ro(action, arg, text);
}

static int mp_property_cursor_autohide(void *ctx, struct m_property *prop,
                                       int action, void *arg)
{
    MPContext *mpctx = ctx;
    struct MPOpts *opts = mpctx->opts;
    int old_value = opts->cursor_autohide_delay;
    int r = mp_property_generic_option(mpctx, prop, action, arg);
    if (opts->cursor_autohide_delay != old_value)
        mpctx->mouse_timer = 0;
    return r;
}

static int prop_stream_ctrl(struct MPContext *mpctx, int ctrl, void *arg)
{
    if (!mpctx->demuxer)
        return M_PROPERTY_UNAVAILABLE;
    int r = demux_stream_control(mpctx->demuxer, ctrl, arg);
    switch (r) {
    case STREAM_OK: return M_PROPERTY_OK;
    case STREAM_UNSUPPORTED: return M_PROPERTY_UNAVAILABLE;
    default: return M_PROPERTY_ERROR;
    }
}

static int mp_property_tv_norm(void *ctx, struct m_property *prop,
                               int action, void *arg)
{
    switch (action) {
    case M_PROPERTY_SET:
        return prop_stream_ctrl(ctx, STREAM_CTRL_TV_SET_NORM, *(char **)arg);
    case M_PROPERTY_SWITCH:
        return prop_stream_ctrl(ctx, STREAM_CTRL_TV_STEP_NORM, NULL);
    case M_PROPERTY_GET_TYPE:
        *(struct m_option *)arg = (struct m_option){.type = CONF_TYPE_STRING};
        return M_PROPERTY_OK;
    }
    return M_PROPERTY_NOT_IMPLEMENTED;
}

static int mp_property_tv_scan(void *ctx, struct m_property *prop,
                               int action, void *arg)
{
    switch (action) {
    case M_PROPERTY_SET:
        return prop_stream_ctrl(ctx, STREAM_CTRL_TV_SET_SCAN, arg);
    case M_PROPERTY_GET_TYPE:
        *(struct m_option *)arg = (struct m_option){.type = CONF_TYPE_FLAG};
        return M_PROPERTY_OK;
    }
    return M_PROPERTY_NOT_IMPLEMENTED;
}

/// TV color settings (RW)
static int mp_property_tv_color(void *ctx, struct m_property *prop,
                                int action, void *arg)
{
    int req[2] = {(intptr_t)prop->priv};
    switch (action) {
    case M_PROPERTY_SET:
        req[1] = *(int *)arg;
        return prop_stream_ctrl(ctx, STREAM_CTRL_SET_TV_COLORS, req);
    case M_PROPERTY_GET: {
        int r = prop_stream_ctrl(ctx, STREAM_CTRL_GET_TV_COLORS, req);
        if (r == M_PROPERTY_OK)
            *(int *)arg = req[1];
        return r;
    }
    case M_PROPERTY_GET_TYPE:
        *(struct m_option *)arg = (struct m_option){
            .type = CONF_TYPE_INT,
            .flags = M_OPT_RANGE,
            .min = -100,
            .max = 100,
        };
        return M_PROPERTY_OK;
    }
    return M_PROPERTY_NOT_IMPLEMENTED;
}

static int mp_property_tv_freq(void *ctx, struct m_property *prop,
                               int action, void *arg)
{
    switch (action) {
    case M_PROPERTY_SET:
        return prop_stream_ctrl(ctx, STREAM_CTRL_SET_TV_FREQ, arg);
    case M_PROPERTY_GET:
        return prop_stream_ctrl(ctx, STREAM_CTRL_GET_TV_FREQ, arg);
    case M_PROPERTY_GET_TYPE:
        *(struct m_option *)arg = (struct m_option){.type = CONF_TYPE_FLOAT};
        return M_PROPERTY_OK;
    }
    return M_PROPERTY_NOT_IMPLEMENTED;
}

static int mp_property_tv_channel(void *ctx, struct m_property *prop,
                                  int action, void *arg)
{
    switch (action) {
    case M_PROPERTY_GET_TYPE:
        *(struct m_option *)arg = (struct m_option){.type = CONF_TYPE_STRING};
        return M_PROPERTY_OK;
    case M_PROPERTY_SET:
        return prop_stream_ctrl(ctx, STREAM_CTRL_TV_SET_CHAN, *(char **)arg);
    case M_PROPERTY_GET:
        return prop_stream_ctrl(ctx, STREAM_CTRL_TV_GET_CHAN, arg);
    case M_PROPERTY_SWITCH: {
        struct m_property_switch_arg *sa = arg;
        int dir = sa->inc >= 0 ? 1 : -1;
        return prop_stream_ctrl(ctx, STREAM_CTRL_TV_STEP_CHAN, &dir);
    }
    }
    return M_PROPERTY_NOT_IMPLEMENTED;
}

static int mp_property_dvb_channel(void *ctx, struct m_property *prop,
                                   int action, void *arg)
{
    MPContext *mpctx = ctx;
    int r;
    switch (action) {
    case M_PROPERTY_SET:
        r = prop_stream_ctrl(mpctx, STREAM_CTRL_DVB_SET_CHANNEL, arg);
        if (r == M_PROPERTY_OK && !mpctx->stop_play)
            mpctx->stop_play = PT_CURRENT_ENTRY;
        return r;
    case M_PROPERTY_SWITCH: {
        struct m_property_switch_arg *sa = arg;
        int dir = sa->inc >= 0 ? 1 : -1;
        r = prop_stream_ctrl(mpctx, STREAM_CTRL_DVB_STEP_CHANNEL, &dir);
        if (r == M_PROPERTY_OK && !mpctx->stop_play)
            mpctx->stop_play = PT_CURRENT_ENTRY;
        return r;
    }
    case M_PROPERTY_GET_TYPE:
        *(struct m_option *)arg = (struct m_option){.type = &m_option_type_intpair};
        return M_PROPERTY_OK;
    }
    return M_PROPERTY_NOT_IMPLEMENTED;
}

static int mp_property_dvb_channel_name(void *ctx, struct m_property *prop,
                                        int action, void *arg)
{
    MPContext *mpctx = ctx;
    int r;
    switch (action) {
    case M_PROPERTY_SET:
        r = prop_stream_ctrl(mpctx, STREAM_CTRL_DVB_SET_CHANNEL_NAME, arg);
        if (r == M_PROPERTY_OK && !mpctx->stop_play)
            mpctx->stop_play = PT_CURRENT_ENTRY;
        return r;
    case M_PROPERTY_SWITCH: {
        struct m_property_switch_arg *sa = arg;
        int dir = sa->inc >= 0 ? 1 : -1;
        r = prop_stream_ctrl(mpctx, STREAM_CTRL_DVB_STEP_CHANNEL, &dir);
        if (r == M_PROPERTY_OK && !mpctx->stop_play)
            mpctx->stop_play = PT_CURRENT_ENTRY;
        return r;
    }
    case M_PROPERTY_GET: {
        return prop_stream_ctrl(mpctx, STREAM_CTRL_DVB_GET_CHANNEL_NAME, arg);
    }
    case M_PROPERTY_GET_TYPE:
        *(struct m_option *)arg = (struct m_option){.type = CONF_TYPE_STRING};
        return M_PROPERTY_OK;
    }
    return M_PROPERTY_NOT_IMPLEMENTED;
}

static int mp_property_playlist_pos_x(void *ctx, struct m_property *prop,
                                      int action, void *arg, int base)
{
    MPContext *mpctx = ctx;
    struct playlist *pl = mpctx->playlist;
    if (!pl->first)
        return M_PROPERTY_UNAVAILABLE;

    switch (action) {
    case M_PROPERTY_GET: {
        int pos = playlist_entry_to_index(pl, pl->current);
        if (pos < 0)
            return M_PROPERTY_UNAVAILABLE;
        *(int *)arg = pos + base;
        return M_PROPERTY_OK;
    }
    case M_PROPERTY_SET: {
        int pos = *(int *)arg - base;
        struct playlist_entry *e = playlist_entry_from_index(pl, pos);
        if (!e)
            return M_PROPERTY_ERROR;
        mp_set_playlist_entry(mpctx, e);
        return M_PROPERTY_OK;
    }
    case M_PROPERTY_GET_TYPE: {
        struct m_option opt = {
            .type = CONF_TYPE_INT,
            .flags = CONF_RANGE,
            .min = base,
            .max = playlist_entry_count(pl) - 1 + base,
        };
        *(struct m_option *)arg = opt;
        return M_PROPERTY_OK;
    }
    }
    return M_PROPERTY_NOT_IMPLEMENTED;
}

static int mp_property_playlist_pos(void *ctx, struct m_property *prop,
                                    int action, void *arg)
{
    return mp_property_playlist_pos_x(ctx, prop, action, arg, 0);
}

static int mp_property_playlist_pos_1(void *ctx, struct m_property *prop,
                                      int action, void *arg)
{
    return mp_property_playlist_pos_x(ctx, prop, action, arg, 1);
}

struct get_playlist_ctx {
    struct MPContext *mpctx;
    int last_index;
    struct playlist_entry *last_entry;
};

static int get_playlist_entry(int item, int action, void *arg, void *ctx)
{
    struct get_playlist_ctx *p = ctx;
    struct MPContext *mpctx = p->mpctx;

    struct playlist_entry *e;
    // This is an optimization that prevents O(n^2) behaviour when the entire
    // playlist is requested. If a request is made for the last requested entry
    // or the entry immediately following it, it can be found without a full
    // traversal of the linked list.
    if (p->last_entry && item == p->last_index)
        e = p->last_entry;
    else if (p->last_entry && item == p->last_index + 1)
        e = p->last_entry->next;
    else
        e = playlist_entry_from_index(mpctx->playlist, item);
    p->last_index = item;
    p->last_entry = e;
    if (!e)
        return M_PROPERTY_ERROR;

    bool current = mpctx->playlist->current == e;
    bool playing = mpctx->playing == e;
    struct m_sub_property props[] = {
        {"filename",    SUB_PROP_STR(e->filename)},
        {"current",     SUB_PROP_FLAG(1), .unavailable = !current},
        {"playing",     SUB_PROP_FLAG(1), .unavailable = !playing},
        {"title",       SUB_PROP_STR(e->title), .unavailable = !e->title},
        {0}
    };

    return m_property_read_sub(props, action, arg);
}

static int mp_property_playlist(void *ctx, struct m_property *prop,
                                int action, void *arg)
{
    MPContext *mpctx = ctx;
    if (action == M_PROPERTY_PRINT) {
        struct playlist *pl = mpctx->playlist;
        char *res = talloc_strdup(NULL, "");

        for (struct playlist_entry *e = pl->first; e; e = e->next)
        {
            char *p = e->filename;
            if (!mp_is_url(bstr0(p))) {
                char *s = mp_basename(e->filename);
                if (s[0])
                    p = s;
            }
            const char *m = pl->current == e ? list_current : list_normal;
            res = talloc_asprintf_append(res, "%s%s\n", m, p);
        }

        *(char **)arg =
            cut_osd_list(mpctx, res, playlist_entry_to_index(pl, pl->current));
        return M_PROPERTY_OK;
    }

    struct get_playlist_ctx p = { .mpctx = mpctx };
    return m_property_read_list(action, arg, playlist_entry_count(mpctx->playlist),
                                get_playlist_entry, &p);
}

static char *print_obj_osd_list(struct m_obj_settings *list)
{
    char *res = NULL;
    for (int n = 0; list && list[n].name; n++) {
        res = talloc_asprintf_append(res, "%s [", list[n].name);
        for (int i = 0; list[n].attribs && list[n].attribs[i]; i += 2) {
            res = talloc_asprintf_append(res, "%s%s=%s", i > 0 ? " " : "",
                                         list[n].attribs[i],
                                         list[n].attribs[i + 1]);
        }
        res = talloc_asprintf_append(res, "]");
        if (!list[n].enabled)
            res = talloc_strdup_append(res, " (disabled)");
        res = talloc_strdup_append(res, "\n");
    }
    if (!res)
        res = talloc_strdup(NULL, "(empty)");
    return res;
}

static int property_filter(struct m_property *prop, int action, void *arg,
                           MPContext *mpctx, enum stream_type mt)
{
    switch (action) {
    case M_PROPERTY_PRINT: {
        struct m_config_option *opt = m_config_get_co(mpctx->mconfig,
                                                      bstr0(prop->name));
        *(char **)arg = print_obj_osd_list(*(struct m_obj_settings **)opt->data);
        return M_PROPERTY_OK;
    }
    case M_PROPERTY_SET:
        return set_filters(mpctx, mt, *(struct m_obj_settings **)arg) >= 0
            ? M_PROPERTY_OK : M_PROPERTY_ERROR;
    }
    return mp_property_generic_option(mpctx, prop, action, arg);
}

static int mp_property_vf(void *ctx, struct m_property *prop,
                          int action, void *arg)
{
    return property_filter(prop, action, arg, ctx, STREAM_VIDEO);
}

static int mp_property_af(void *ctx, struct m_property *prop,
                          int action, void *arg)
{
    return property_filter(prop, action, arg, ctx, STREAM_AUDIO);
}

static int mp_property_ab_loop(void *ctx, struct m_property *prop,
                               int action, void *arg)
{
    struct MPContext *mpctx = ctx;
    struct MPOpts *opts = mpctx->opts;
    if (action == M_PROPERTY_KEY_ACTION) {
        double val;
        if (mp_property_generic_option(mpctx, prop, M_PROPERTY_GET, &val) < 1)
            return M_PROPERTY_ERROR;

        return property_time(action, arg, val);
    }
    int r = mp_property_generic_option(mpctx, prop, action, arg);
    if (r > 0 && action == M_PROPERTY_SET) {
        mpctx->ab_loop_clip = mpctx->playback_pts < opts->ab_loop[1];
        if (strcmp(prop->name, "ab-loop-b") == 0) {
            if (opts->ab_loop[1] != MP_NOPTS_VALUE &&
                mpctx->playback_pts <= opts->ab_loop[1])
                mpctx->ab_loop_clip = true;
        }
        // Update if visible
        set_osd_bar_chapters(mpctx, OSD_BAR_SEEK);
        mp_wakeup_core(mpctx);
    }
    return r;
}

static int mp_property_packet_bitrate(void *ctx, struct m_property *prop,
                                      int action, void *arg)
{
    MPContext *mpctx = ctx;
    int type = (uintptr_t)prop->priv & ~0x100;
    bool old = (uintptr_t)prop->priv & 0x100;

    struct demuxer *demuxer = NULL;
    if (mpctx->current_track[0][type])
        demuxer = mpctx->current_track[0][type]->demuxer;
    if (!demuxer)
        demuxer = mpctx->demuxer;
    if (!demuxer)
        return M_PROPERTY_UNAVAILABLE;

    double r[STREAM_TYPE_COUNT];
    if (demux_control(demuxer, DEMUXER_CTRL_GET_BITRATE_STATS, &r) < 1)
        return M_PROPERTY_UNAVAILABLE;
    if (r[type] < 0)
        return M_PROPERTY_UNAVAILABLE;

    // r[type] is in bytes/second -> bits
    double rate = r[type] * 8;

    // Same story, but used kilobits for some reason.
    if (old)
        return m_property_int64_ro(action, arg, rate / 1000.0 + 0.5);

    if (action == M_PROPERTY_PRINT) {
        rate /= 1000;
        if (rate < 1000) {
            *(char **)arg = talloc_asprintf(NULL, "%d kbps", (int)rate);
        } else {
            *(char **)arg = talloc_asprintf(NULL, "%.3f mbps", rate / 1000.0);
        }
        return M_PROPERTY_OK;
    }
    return m_property_int64_ro(action, arg, rate);
}

static int mp_property_cwd(void *ctx, struct m_property *prop,
                           int action, void *arg)
{
    switch (action) {
    case M_PROPERTY_GET: {
        char *cwd = mp_getcwd(NULL);
        if (!cwd)
            return M_PROPERTY_ERROR;
        *(char **)arg = cwd;
        return M_PROPERTY_OK;
    }
    case M_PROPERTY_GET_TYPE:
        *(struct m_option *)arg = (struct m_option){.type = CONF_TYPE_STRING};
        return M_PROPERTY_OK;
    }
    return M_PROPERTY_NOT_IMPLEMENTED;
}

static int mp_property_record_file(void *ctx, struct m_property *prop,
                                   int action, void *arg)
{
    struct MPContext *mpctx = ctx;
    struct MPOpts *opts = mpctx->opts;
    if (action == M_PROPERTY_SET) {
        char *new = *(char **)arg;
        if (!bstr_equals(bstr0(new), bstr0(opts->record_file))) {
            talloc_free(opts->record_file);
            opts->record_file = talloc_strdup(NULL, new);
            open_recorder(mpctx, false);
            // open_recorder() unsets it on failure.
            if (new && !opts->record_file)
                return M_PROPERTY_ERROR;
        }
        return M_PROPERTY_OK;
    }
    return mp_property_generic_option(mpctx, prop, action, arg);
}

static int mp_property_protocols(void *ctx, struct m_property *prop,
                                 int action, void *arg)
{
    switch (action) {
    case M_PROPERTY_GET:
        *(char ***)arg = stream_get_proto_list();
        return M_PROPERTY_OK;
    case M_PROPERTY_GET_TYPE:
        *(struct m_option *)arg = (struct m_option){.type = CONF_TYPE_STRING_LIST};
        return M_PROPERTY_OK;
    }
    return M_PROPERTY_NOT_IMPLEMENTED;
}

static int get_decoder_entry(int item, int action, void *arg, void *ctx)
{
    struct mp_decoder_list *codecs = ctx;
    struct mp_decoder_entry *c = &codecs->entries[item];

    struct m_sub_property props[] = {
        {"family",      SUB_PROP_STR(c->family)},
        {"codec",       SUB_PROP_STR(c->codec)},
        {"driver" ,     SUB_PROP_STR(c->decoder)},
        {"description", SUB_PROP_STR(c->desc)},
        {0}
    };

    return m_property_read_sub(props, action, arg);
}

static int mp_property_decoders(void *ctx, struct m_property *prop,
                                int action, void *arg)
{
    struct mp_decoder_list *codecs = talloc_zero(NULL, struct mp_decoder_list);
    struct mp_decoder_list *v = talloc_steal(codecs, video_decoder_list());
    struct mp_decoder_list *a = talloc_steal(codecs, audio_decoder_list());
    mp_append_decoders(codecs, v);
    mp_append_decoders(codecs, a);
    int r = m_property_read_list(action, arg, codecs->num_entries,
                                 get_decoder_entry, codecs);
    talloc_free(codecs);
    return r;
}

static int mp_property_encoders(void *ctx, struct m_property *prop,
                                int action, void *arg)
{
    struct mp_decoder_list *codecs = talloc_zero(NULL, struct mp_decoder_list);
    mp_add_lavc_encoders(codecs);
    int r = m_property_read_list(action, arg, codecs->num_entries,
                                 get_decoder_entry, codecs);
    talloc_free(codecs);
    return r;
}

static int mp_property_lavf_demuxers(void *ctx, struct m_property *prop,
                                 int action, void *arg)
{
    switch (action) {
    case M_PROPERTY_GET:
        *(char ***)arg = mp_get_lavf_demuxers();
        return M_PROPERTY_OK;
    case M_PROPERTY_GET_TYPE:
        *(struct m_option *)arg = (struct m_option){.type = CONF_TYPE_STRING_LIST};
        return M_PROPERTY_OK;
    }
    return M_PROPERTY_NOT_IMPLEMENTED;
}

static int mp_property_version(void *ctx, struct m_property *prop,
                               int action, void *arg)
{
    return m_property_strdup_ro(action, arg, mpv_version);
}

static int mp_property_configuration(void *ctx, struct m_property *prop,
                                     int action, void *arg)
{
    return m_property_strdup_ro(action, arg, CONFIGURATION);
}

static int mp_property_ffmpeg(void *ctx, struct m_property *prop,
                               int action, void *arg)
{
    return m_property_strdup_ro(action, arg, av_version_info());
}

static int mp_property_alias(void *ctx, struct m_property *prop,
                             int action, void *arg)
{
    const char *real_property = prop->priv;
    return mp_property_do(real_property, action, arg, ctx);
}

static int mp_property_deprecated_alias(void *ctx, struct m_property *prop,
                                        int action, void *arg)
{
    MPContext *mpctx = ctx;
    struct command_ctx *cmd = mpctx->command_ctx;
    const char *real_property = prop->priv;
    for (int n = 0; n < cmd->num_warned_deprecated; n++) {
        if (strcmp(cmd->warned_deprecated[n], prop->name) == 0)
            goto done;
    }
    MP_WARN(mpctx, "Warning: property '%s' was replaced with '%s' and "
            "might be removed in the future.\n", prop->name, real_property);
    MP_TARRAY_APPEND(cmd, cmd->warned_deprecated, cmd->num_warned_deprecated,
                     (char *)prop->name);

done:
    return mp_property_do(real_property, action, arg, ctx);
}

static int mp_property_shitfuck(void *ctx, struct m_property *prop,
                                int action, void *arg)
{
    MPContext *mpctx = ctx;
    int flags = M_SETOPT_RUNTIME;
    const char *rname = prop->priv;

    MP_WARN(mpctx, "Do not use %s, use %s, bug reports will be ignored.\n",
            prop->name, rname);

    struct m_config_option *co = m_config_get_co_raw(mpctx->mconfig, bstr0(rname));
    if (!co)
        return M_PROPERTY_UNKNOWN;

    switch (action) {
    case M_PROPERTY_GET_TYPE:
        *(struct m_option *)arg = *(co->opt);
        return M_PROPERTY_OK;
    case M_PROPERTY_GET:
        if (!co->data)
            return M_PROPERTY_NOT_IMPLEMENTED;
        m_option_copy(co->opt, arg, co->data);
        return M_PROPERTY_OK;
    case M_PROPERTY_SET:
        if (m_config_set_option_raw_direct(mpctx->mconfig, co, arg, flags) < 0)
            return M_PROPERTY_ERROR;
        return M_PROPERTY_OK;
    }
    return M_PROPERTY_NOT_IMPLEMENTED;
}

static int access_options(struct m_property_action_arg *ka, bool local,
                          MPContext *mpctx)
{
    struct m_config_option *opt = m_config_get_co(mpctx->mconfig,
                                                  bstr0(ka->key));
    if (!opt)
        return M_PROPERTY_UNKNOWN;
    if (!opt->data)
        return M_PROPERTY_UNAVAILABLE;

    switch (ka->action) {
    case M_PROPERTY_GET:
        m_option_copy(opt->opt, ka->arg, opt->data);
        return M_PROPERTY_OK;
    case M_PROPERTY_SET: {
        if (local && !mpctx->playing)
            return M_PROPERTY_ERROR;
        int flags = M_SETOPT_RUNTIME | (local ? M_SETOPT_BACKUP : 0);
        int r = m_config_set_option_raw(mpctx->mconfig, opt, ka->arg, flags);
        mp_wakeup_core(mpctx);
        return r < 0 ? M_PROPERTY_ERROR : M_PROPERTY_OK;
    }
    case M_PROPERTY_GET_TYPE:
        *(struct m_option *)ka->arg = *opt->opt;
        return M_PROPERTY_OK;
    }
    return M_PROPERTY_NOT_IMPLEMENTED;
}

static int access_option_list(int action, void *arg, bool local, MPContext *mpctx)
{
    switch (action) {
    case M_PROPERTY_GET_TYPE:
        *(struct m_option *)arg = (struct m_option){.type = CONF_TYPE_STRING_LIST};
        return M_PROPERTY_OK;
    case M_PROPERTY_GET:
        *(char ***)arg = m_config_list_options(NULL, mpctx->mconfig);
        return M_PROPERTY_OK;
    case M_PROPERTY_KEY_ACTION:
        return access_options(arg, local, mpctx);
    }
    return M_PROPERTY_NOT_IMPLEMENTED;
}


static int mp_property_options(void *ctx, struct m_property *prop,
                               int action, void *arg)
{
    MPContext *mpctx = ctx;
    return access_option_list(action, arg, false, mpctx);
}

static int mp_property_local_options(void *ctx, struct m_property *prop,
                                     int action, void *arg)
{
    MPContext *mpctx = ctx;
    return access_option_list(action, arg, true, mpctx);
}

static int mp_property_option_info(void *ctx, struct m_property *prop,
                                   int action, void *arg)
{
    MPContext *mpctx = ctx;
    switch (action) {
    case M_PROPERTY_KEY_ACTION: {
        struct m_property_action_arg *ka = arg;
        bstr key;
        char *rem;
        m_property_split_path(ka->key, &key, &rem);
        struct m_config_option *co = m_config_get_co(mpctx->mconfig, key);
        if (!co)
            return M_PROPERTY_UNKNOWN;

        union m_option_value def = {0};
        if (co->default_data)
            memcpy(&def, co->default_data, co->opt->type->size);

        const struct m_option *opt = co->opt;
        bool has_minmax =
            opt->type == &m_option_type_int ||
            opt->type == &m_option_type_int64 ||
            opt->type == &m_option_type_float ||
            opt->type == &m_option_type_double;
        char **choices = NULL;

        if (opt->type == &m_option_type_choice) {
            has_minmax = true;
            struct m_opt_choice_alternatives *alt = opt->priv;
            int num = 0;
            for ( ; alt->name; alt++)
                MP_TARRAY_APPEND(NULL, choices, num, alt->name);
            MP_TARRAY_APPEND(NULL, choices, num, NULL);
        }
        if (opt->type == &m_option_type_obj_settings_list) {
            struct m_obj_list *objs = opt->priv;
            int num = 0;
            for (int n = 0; ; n++) {
                struct m_obj_desc desc = {0};
                if (!objs->get_desc(&desc, n))
                    break;
                MP_TARRAY_APPEND(NULL, choices, num, (char *)desc.name);
            }
            MP_TARRAY_APPEND(NULL, choices, num, NULL);
        }

        struct m_sub_property props[] = {
            {"name",                    SUB_PROP_STR(co->name)},
            {"type",                    SUB_PROP_STR(opt->type->name)},
            {"set-from-commandline",    SUB_PROP_FLAG(co->is_set_from_cmdline)},
            {"set-locally",             SUB_PROP_FLAG(co->is_set_locally)},
            {"default-value",           *opt, def},
            {"min",                     SUB_PROP_DOUBLE(opt->min),
             .unavailable = !(has_minmax && (opt->flags & M_OPT_MIN))},
            {"max",                     SUB_PROP_DOUBLE(opt->max),
             .unavailable = !(has_minmax && (opt->flags & M_OPT_MAX))},
            {"choices", .type = {.type = CONF_TYPE_STRING_LIST},
             .value = {.string_list = choices}, .unavailable = !choices},
            {0}
        };

        struct m_property_action_arg next_ka = *ka;
        next_ka.key = rem;
        int r = m_property_read_sub(props, M_PROPERTY_KEY_ACTION, &next_ka);
        talloc_free(choices);
        return r;
    }
    }
    return M_PROPERTY_NOT_IMPLEMENTED;
}

static int mp_property_list(void *ctx, struct m_property *prop,
                            int action, void *arg)
{
    struct MPContext *mpctx = ctx;
    struct command_ctx *cmd = mpctx->command_ctx;

    switch (action) {
    case M_PROPERTY_GET_TYPE:
        *(struct m_option *)arg = (struct m_option){.type = CONF_TYPE_STRING_LIST};
        return M_PROPERTY_OK;
    case M_PROPERTY_GET: {
        char **list = NULL;
        int num = 0;
        for (int n = 0; cmd->properties[n].name; n++) {
            MP_TARRAY_APPEND(NULL, list, num,
                                talloc_strdup(NULL, cmd->properties[n].name));
        }
        MP_TARRAY_APPEND(NULL, list, num, NULL);
        *(char ***)arg = list;
        return M_PROPERTY_OK;
    }
    }
    return M_PROPERTY_NOT_IMPLEMENTED;
}

static int mp_profile_list(void *ctx, struct m_property *prop,
                           int action, void *arg)
{
    MPContext *mpctx = ctx;
    switch (action) {
    case M_PROPERTY_GET_TYPE:
        *(struct m_option *)arg = (struct m_option){.type = CONF_TYPE_NODE};
        return M_PROPERTY_OK;
    case M_PROPERTY_GET: {
        *(struct mpv_node *)arg = m_config_get_profiles(mpctx->mconfig);
        return M_PROPERTY_OK;
    }
    }
    return M_PROPERTY_NOT_IMPLEMENTED;
}

// Redirect a property name to another
#define M_PROPERTY_ALIAS(name, real_property) \
    {(name), mp_property_alias, .priv = (real_property)}

#define M_PROPERTY_DEPRECATED_ALIAS(name, real_property) \
    {(name), mp_property_deprecated_alias, .priv = (real_property)}

// Base list of properties. This does not include option-mapped properties.
static const struct m_property mp_properties_base[] = {
    // General
    {"speed", mp_property_playback_speed},
    {"audio-speed-correction", mp_property_av_speed_correction, .priv = "a"},
    {"video-speed-correction", mp_property_av_speed_correction, .priv = "v"},
    {"display-sync-active", mp_property_display_sync_active},
    {"filename", mp_property_filename},
    {"stream-open-filename", mp_property_stream_open_filename},
    {"file-size", mp_property_file_size},
    {"path", mp_property_path},
    {"media-title", mp_property_media_title},
    {"stream-path", mp_property_stream_path},
    {"current-demuxer", mp_property_demuxer},
    {"file-format", mp_property_file_format},
    {"stream-pos", mp_property_stream_pos},
    {"stream-end", mp_property_stream_end},
    {"duration", mp_property_duration},
    {"avsync", mp_property_avsync},
    {"total-avsync-change", mp_property_total_avsync_change},
    {"mistimed-frame-count", mp_property_mistimed_frame_count},
    {"vsync-ratio", mp_property_vsync_ratio},
    {"decoder-frame-drop-count", mp_property_frame_drop_dec},
    {"frame-drop-count", mp_property_frame_drop_vo},
    {"vo-delayed-frame-count", mp_property_vo_delayed_frame_count},
    {"percent-pos", mp_property_percent_pos},
    {"time-start", mp_property_time_start},
    {"time-pos", mp_property_time_pos},
    {"time-remaining", mp_property_remaining},
    {"audio-pts", mp_property_audio_pts},
    {"playtime-remaining", mp_property_playtime_remaining},
    {"playback-time", mp_property_playback_time},
    {"disc-title", mp_property_disc_title},
    {"chapter", mp_property_chapter},
    {"edition", mp_property_edition},
    {"disc-titles", mp_property_disc_titles},
    {"chapters", mp_property_chapters},
    {"editions", mp_property_editions},
    {"angle", mp_property_angle},
    {"metadata", mp_property_metadata},
    {"filtered-metadata", mp_property_filtered_metadata},
    {"chapter-metadata", mp_property_chapter_metadata},
    {"vf-metadata", mp_property_filter_metadata, .priv = "vf"},
    {"af-metadata", mp_property_filter_metadata, .priv = "af"},
    {"pause", mp_property_pause},
    {"core-idle", mp_property_core_idle},
    {"eof-reached", mp_property_eof_reached},
    {"seeking", mp_property_seeking},
    {"playback-abort", mp_property_playback_abort},
    {"cache-percent", mp_property_cache},
    {"cache-free", mp_property_cache_free},
    {"cache-used", mp_property_cache_used},
    {"cache-size", mp_property_cache_size},
    {"cache-idle", mp_property_cache_idle},
    {"cache-speed", mp_property_cache_speed},
    {"demuxer-cache-duration", mp_property_demuxer_cache_duration},
    {"demuxer-cache-time", mp_property_demuxer_cache_time},
    {"demuxer-cache-idle", mp_property_demuxer_cache_idle},
    {"demuxer-start-time", mp_property_demuxer_start_time},
    {"demuxer-cache-state", mp_property_demuxer_cache_state},
    {"cache-buffering-state", mp_property_cache_buffering},
    {"paused-for-cache", mp_property_paused_for_cache},
    {"demuxer-via-network", mp_property_demuxer_is_network},
    {"clock", mp_property_clock},
    {"seekable", mp_property_seekable},
    {"partially-seekable", mp_property_partially_seekable},
    {"idle-active", mp_property_idle},

    {"chapter-list", mp_property_list_chapters},
    {"track-list", property_list_tracks},
    {"edition-list", property_list_editions},
    {"disc-title-list", mp_property_list_disc_titles},

    {"playlist", mp_property_playlist},
    {"playlist-pos", mp_property_playlist_pos},
    {"playlist-pos-1", mp_property_playlist_pos_1},
    M_PROPERTY_ALIAS("playlist-count", "playlist/count"),

    // Audio
    {"mixer-active", mp_property_mixer_active},
    {"volume", mp_property_volume},
    {"mute", mp_property_mute},
    {"ao-volume", mp_property_ao_volume},
    {"ao-mute", mp_property_ao_mute},
    {"audio-delay", mp_property_audio_delay},
    {"audio-codec-name", mp_property_audio_codec_name},
    {"audio-codec", mp_property_audio_codec},
    {"audio-params", mp_property_audio_params},
    {"audio-out-params", mp_property_audio_out_params},
    {"aid", mp_property_audio},
    {"audio-device", mp_property_audio_device},
    {"audio-device-list", mp_property_audio_devices},
    {"current-ao", mp_property_ao},

    // Video
    {"fullscreen", mp_property_fullscreen},
    {"taskbar-progress", mp_property_taskbar_progress},
    {"ontop", mp_property_ontop},
    {"border", mp_property_border},
    {"on-all-workspaces", mp_property_all_workspaces},
    {"video-out-params", mp_property_vo_imgparams},
    {"video-dec-params", mp_property_dec_imgparams},
    {"video-params", mp_property_vd_imgparams},
    {"video-format", mp_property_video_format},
    {"video-frame-info", mp_property_video_frame_info},
    {"video-codec", mp_property_video_codec},
    M_PROPERTY_ALIAS("dwidth", "video-out-params/dw"),
    M_PROPERTY_ALIAS("dheight", "video-out-params/dh"),
    M_PROPERTY_ALIAS("width", "video-params/w"),
    M_PROPERTY_ALIAS("height", "video-params/h"),
    {"window-scale", mp_property_window_scale},
    {"vo-configured", mp_property_vo_configured},
    {"vo-passes", mp_property_vo_passes},
    {"current-vo", mp_property_vo},
    {"container-fps", mp_property_fps},
    {"estimated-vf-fps", mp_property_vf_fps},
    {"video-aspect", mp_property_aspect},
    {"vid", mp_property_video},
    {"program", mp_property_program},
    {"hwdec", mp_property_hwdec},
    {"hwdec-current", mp_property_hwdec_current},
    {"hwdec-interop", mp_property_hwdec_interop},

    {"estimated-frame-count", mp_property_frame_count},
    {"estimated-frame-number", mp_property_frame_number},

    {"osd-width", mp_property_osd_w},
    {"osd-height", mp_property_osd_h},
    {"osd-par", mp_property_osd_par},

    {"osd-sym-cc", mp_property_osd_sym},
    {"osd-ass-cc", mp_property_osd_ass},

    // Subs
    {"sid", mp_property_sub},
    {"secondary-sid", mp_property_sub2},
    {"sub-delay", mp_property_sub_delay},
    {"sub-speed", mp_property_sub_speed},
    {"sub-pos", mp_property_sub_pos},
    {"sub-text", mp_property_sub_text},

    {"vf", mp_property_vf},
    {"af", mp_property_af},

    {"ab-loop-a", mp_property_ab_loop},
    {"ab-loop-b", mp_property_ab_loop},

#define PROPERTY_BITRATE(name, old, type) \
    {name, mp_property_packet_bitrate, (void *)(uintptr_t)((type)|(old?0x100:0))}
    PROPERTY_BITRATE("packet-video-bitrate", true, STREAM_VIDEO),
    PROPERTY_BITRATE("packet-audio-bitrate", true, STREAM_AUDIO),
    PROPERTY_BITRATE("packet-sub-bitrate", true, STREAM_SUB),

    PROPERTY_BITRATE("video-bitrate", false, STREAM_VIDEO),
    PROPERTY_BITRATE("audio-bitrate", false, STREAM_AUDIO),
    PROPERTY_BITRATE("sub-bitrate", false, STREAM_SUB),

#define PROPERTY_TV_COLOR(name, type) \
    {name, mp_property_tv_color, (void *)(intptr_t)type}
    PROPERTY_TV_COLOR("tv-brightness", TV_COLOR_BRIGHTNESS),
    PROPERTY_TV_COLOR("tv-contrast", TV_COLOR_CONTRAST),
    PROPERTY_TV_COLOR("tv-saturation", TV_COLOR_SATURATION),
    PROPERTY_TV_COLOR("tv-hue", TV_COLOR_HUE),
    {"tv-freq", mp_property_tv_freq},
    {"tv-norm", mp_property_tv_norm},
    {"tv-scan", mp_property_tv_scan},
    {"tv-channel", mp_property_tv_channel},
    {"dvb-channel", mp_property_dvb_channel},
    {"dvb-channel-name", mp_property_dvb_channel_name},

    {"cursor-autohide", mp_property_cursor_autohide},

    {"window-minimized", mp_property_win_minimized},
    {"display-names", mp_property_display_names},
    {"display-fps", mp_property_display_fps},
    {"estimated-display-fps", mp_property_estimated_display_fps},
    {"vsync-jitter", mp_property_vsync_jitter},
    {"framedrop", mp_property_framedrop},

    {"working-directory", mp_property_cwd},

    {"record-file", mp_property_record_file},

    {"protocol-list", mp_property_protocols},
    {"decoder-list", mp_property_decoders},
    {"encoder-list", mp_property_encoders},
    {"demuxer-lavf-list", mp_property_lavf_demuxers},

    {"mpv-version", mp_property_version},
    {"mpv-configuration", mp_property_configuration},
    {"ffmpeg-version", mp_property_ffmpeg},

    {"options", mp_property_options},
    {"file-local-options", mp_property_local_options},
    {"option-info", mp_property_option_info},
    {"property-list", mp_property_list},
    {"profile-list", mp_profile_list},

    M_PROPERTY_ALIAS("video", "vid"),
    M_PROPERTY_ALIAS("audio", "aid"),
    M_PROPERTY_ALIAS("sub", "sid"),

    // compatibility
    M_PROPERTY_ALIAS("colormatrix", "video-params/colormatrix"),
    M_PROPERTY_ALIAS("colormatrix-input-range", "video-params/colorlevels"),
    M_PROPERTY_ALIAS("colormatrix-primaries", "video-params/primaries"),
    M_PROPERTY_ALIAS("colormatrix-gamma", "video-params/gamma"),

    M_PROPERTY_DEPRECATED_ALIAS("drop-frame-count", "decoder-frame-drop-count"),
    M_PROPERTY_DEPRECATED_ALIAS("vo-drop-frame-count", "frame-drop-count"),
};

// Each entry describes which properties an event (possibly) changes.
#define E(x, ...) [x] = (const char*const[]){__VA_ARGS__, NULL}
static const char *const *const mp_event_property_change[] = {
    E(MPV_EVENT_START_FILE, "*"),
    E(MPV_EVENT_END_FILE, "*"),
    E(MPV_EVENT_FILE_LOADED, "*"),
    E(MP_EVENT_CHANGE_ALL, "*"),
    E(MPV_EVENT_TRACKS_CHANGED, "track-list"),
    E(MPV_EVENT_TRACK_SWITCHED, "vid", "video", "aid", "audio", "sid", "sub",
      "secondary-sid"),
    E(MPV_EVENT_IDLE, "*"),
    E(MPV_EVENT_PAUSE,   "pause"),
    E(MPV_EVENT_UNPAUSE, "pause"),
    E(MPV_EVENT_TICK, "time-pos", "audio-pts", "stream-pos", "avsync",
      "percent-pos", "time-remaining", "playtime-remaining", "playback-time",
      "estimated-vf-fps", "drop-frame-count", "vo-drop-frame-count",
      "total-avsync-change", "audio-speed-correction", "video-speed-correction",
      "vo-delayed-frame-count", "mistimed-frame-count", "vsync-ratio",
      "estimated-display-fps", "vsync-jitter", "sub-text", "audio-bitrate",
      "video-bitrate", "sub-bitrate", "decoder-frame-drop-count",
      "frame-drop-count", "video-frame-info"),
    E(MP_EVENT_DURATION_UPDATE, "duration"),
    E(MPV_EVENT_VIDEO_RECONFIG, "video-out-params", "video-params",
      "video-format", "video-codec", "video-bitrate", "dwidth", "dheight",
      "width", "height", "fps", "aspect", "vo-configured", "current-vo",
      "colormatrix", "colormatrix-input-range", "colormatrix-output-range",
      "colormatrix-primaries", "video-aspect", "video-dec-params",
      "hwdec", "hwdec-current", "hwdec-interop"),
    E(MPV_EVENT_AUDIO_RECONFIG, "audio-format", "audio-codec", "audio-bitrate",
      "samplerate", "channels", "audio", "volume", "mute",
      "current-ao", "audio-codec-name", "audio-params",
      "audio-out-params", "volume-max", "mixer-active"),
    E(MPV_EVENT_SEEK, "seeking", "core-idle", "eof-reached"),
    E(MPV_EVENT_PLAYBACK_RESTART, "seeking", "core-idle", "eof-reached"),
    E(MPV_EVENT_METADATA_UPDATE, "metadata", "filtered-metadata", "media-title"),
    E(MPV_EVENT_CHAPTER_CHANGE, "chapter", "chapter-metadata"),
    E(MP_EVENT_CACHE_UPDATE, "cache", "cache-free", "cache-used", "cache-idle",
      "demuxer-cache-duration", "demuxer-cache-idle", "paused-for-cache",
      "demuxer-cache-time", "cache-buffering-state", "cache-speed",
      "cache-percent"),
    E(MP_EVENT_WIN_RESIZE, "window-scale", "osd-width", "osd-height", "osd-par"),
    E(MP_EVENT_WIN_STATE, "window-minimized", "display-names", "display-fps",
      "fullscreen"),
    E(MP_EVENT_CHANGE_PLAYLIST, "playlist", "playlist-pos", "playlist-pos-1",
      "playlist-count", "playlist/count"),
    E(MP_EVENT_CORE_IDLE, "core-idle", "eof-reached"),
};
#undef E

// If there is no prefix, return length+1 (avoids matching full name as prefix).
static int prefix_len(const char *p)
{
    const char *end = strchr(p, '/');
    return end ? end - p : strlen(p) + 1;
}

static bool match_property(const char *a, const char *b)
{
    if (strcmp(a, "*") == 0)
        return true;
    // Give options and properties the same ID each, so change notifications
    // work both way.
    if (strncmp(a, "options/", 8) == 0)
        a += 8;
    if (strncmp(b, "options/", 8) == 0)
        b += 8;
    int len_a = prefix_len(a);
    int len_b = prefix_len(b);
    return strncmp(a, b, MPMIN(len_a, len_b)) == 0;
}

// Return a bitset of events which change the property.
uint64_t mp_get_property_event_mask(const char *name)
{
    uint64_t mask = 0;
    for (int n = 0; n < MP_ARRAY_SIZE(mp_event_property_change); n++) {
        const char *const *const list = mp_event_property_change[n];
        for (int i = 0; list && list[i]; i++) {
            if (match_property(list[i], name))
                mask |= 1ULL << n;
        }
    }
    return mask;
}

// Return an ID for the property. It might not be unique, but is good enough
// for property change handling. Return -1 if property unknown.
int mp_get_property_id(struct MPContext *mpctx, const char *name)
{
    struct command_ctx *ctx = mpctx->command_ctx;
    for (int n = 0; ctx->properties[n].name; n++) {
        if (match_property(ctx->properties[n].name, name))
            return n;
    }
    return -1;
}

static bool is_property_set(int action, void *val)
{
    switch (action) {
    case M_PROPERTY_SET:
    case M_PROPERTY_SWITCH:
    case M_PROPERTY_SET_STRING:
    case M_PROPERTY_SET_NODE:
        return true;
    case M_PROPERTY_KEY_ACTION: {
        struct m_property_action_arg *key = val;
        return is_property_set(key->action, key->arg);
    }
    default:
        return false;
    }
}

static int mp_property_do_silent(const char *name, int action, void *val,
                                 struct MPContext *ctx)
{
    struct command_ctx *cmd = ctx->command_ctx;
    cmd->silence_option_deprecations += 1;
    int r = m_property_do(ctx->log, cmd->properties, name, action, val, ctx);
    cmd->silence_option_deprecations -= 1;
    if (r == M_PROPERTY_OK && is_property_set(action, val))
        mp_notify_property(ctx, (char *)name);
    return r;
}

int mp_property_do(const char *name, int action, void *val,
                   struct MPContext *ctx)
{
    int r = mp_property_do_silent(name, action, val, ctx);
    if (mp_msg_test(ctx->log, MSGL_V) && is_property_set(action, val)) {
        struct m_option ot = {0};
        void *data = val;
        switch (action) {
        case M_PROPERTY_SET_NODE:
            ot.type = &m_option_type_node;
            break;
        case M_PROPERTY_SET_STRING:
            ot.type = &m_option_type_string;
            data = &val;
            break;
        }
        char *t = ot.type ? m_option_print(&ot, data) : NULL;
        MP_VERBOSE(ctx, "Set property: %s%s%s -> %d\n",
                   name, t ? "=" : "", t ? t : "", r);
        talloc_free(t);
    }
    return r;
}

char *mp_property_expand_string(struct MPContext *mpctx, const char *str)
{
    struct command_ctx *ctx = mpctx->command_ctx;
    return m_properties_expand_string(ctx->properties, str, mpctx);
}

// Before expanding properties, parse C-style escapes like "\n"
char *mp_property_expand_escaped_string(struct MPContext *mpctx, const char *str)
{
    void *tmp = talloc_new(NULL);
    bstr strb = bstr0(str);
    bstr dst = {0};
    while (strb.len) {
        if (!mp_append_escaped_string(tmp, &dst, &strb)) {
            talloc_free(tmp);
            return talloc_strdup(NULL, "(broken escape sequences)");
        }
        // pass " through literally
        if (!bstr_eatstart0(&strb, "\""))
            break;
        bstr_xappend(tmp, &dst, bstr0("\""));
    }
    char *r = mp_property_expand_string(mpctx, dst.start);
    talloc_free(tmp);
    return r;
}

void property_print_help(struct MPContext *mpctx)
{
    struct command_ctx *ctx = mpctx->command_ctx;
    m_properties_print_help_list(mpctx->log, ctx->properties);
}

/* List of default ways to show a property on OSD.
 *
 * If osd_progbar is set, a bar showing the current position between min/max
 * values of the property is shown. In this case osd_msg is only used for
 * terminal output if there is no video; it'll be a label shown together with
 * percentage.
 */
static const struct property_osd_display {
    // property name
    const char *name;
    // name used on OSD
    const char *osd_name;
    // progressbar type
    int osd_progbar;
    // Needs special ways to display the new value (seeks are delayed)
    int seek_msg, seek_bar;
    // Show a marker thing on OSD bar. Ignored if osd_progbar==0.
    float marker;
    // Free-form message (if NULL, osd_name or the property name is used)
    const char *msg;
} property_osd_display[] = {
    // general
    { "loop-playlist", "Loop" },
    { "loop-file", "Loop current file" },
    { "chapter", .seek_msg = OSD_SEEK_INFO_CHAPTER_TEXT,
                 .seek_bar = OSD_SEEK_INFO_BAR },
    { "hr-seek", "hr-seek" },
    { "speed", "Speed" },
    { "clock", "Clock" },
    // audio
    { "volume", "Volume",
      .msg = "Volume: ${?volume:${volume}% ${?mute==yes:(Muted)}}${!volume:${volume}}",
      .osd_progbar = OSD_VOLUME, .marker = 100 },
    { "ao-volume", "AO Volume",
      .msg = "AO Volume: ${?ao-volume:${ao-volume}% ${?ao-mute==yes:(Muted)}}${!ao-volume:${ao-volume}}",
      .osd_progbar = OSD_VOLUME, .marker = 100 },
    { "mute", "Mute" },
    { "ao-mute", "AO Mute" },
    { "audio-delay", "A-V delay" },
    { "audio", "Audio" },
    // video
    { "panscan", "Panscan", .osd_progbar = OSD_PANSCAN },
    { "taskbar-progress", "Progress in taskbar" },
    { "snap-window", "Snap to screen edges" },
    { "ontop", "Stay on top" },
    { "border", "Border" },
    { "framedrop", "Framedrop" },
    { "deinterlace", "Deinterlace" },
    { "colormatrix",
       .msg = "YUV colormatrix:\n${colormatrix}" },
    { "colormatrix-input-range",
       .msg = "YUV input range:\n${colormatrix-input-range}" },
    { "colormatrix-output-range",
       .msg = "RGB output range:\n${colormatrix-output-range}" },
    { "colormatrix-primaries",
       .msg = "Colorspace primaries:\n${colormatrix-primaries}", },
    { "colormatrix-gamma",
       .msg = "Colorspace gamma:\n${colormatrix-gamma}", },
    { "gamma", "Gamma", .osd_progbar = OSD_BRIGHTNESS },
    { "brightness", "Brightness", .osd_progbar = OSD_BRIGHTNESS },
    { "contrast", "Contrast", .osd_progbar = OSD_CONTRAST },
    { "saturation", "Saturation", .osd_progbar = OSD_SATURATION },
    { "hue", "Hue", .osd_progbar = OSD_HUE },
    { "angle", "Angle" },
    // subs
    { "sub", "Subtitles" },
    { "secondary-sid", "Secondary subtitles" },
    { "sub-pos", "Sub position" },
    { "sub-delay", "Sub delay" },
    { "sub-speed", "Sub speed" },
    { "sub-visibility", .msg = "Subtitles ${!sub-visibility==yes:hidden}"
        "${?sub-visibility==yes:visible${?sub==no: (but no subtitles selected)}}" },
    { "sub-forced-only", "Forced sub only" },
    { "sub-scale", "Sub Scale"},
    { "sub-ass-vsfilter-aspect-compat", "Subtitle VSFilter aspect compat"},
    { "sub-ass-override", "ASS subtitle style override"},
    { "vf", "Video filters", .msg = "Video filters:\n${vf}"},
    { "af", "Audio filters", .msg = "Audio filters:\n${af}"},
    { "tv-brightness", "Brightness", .osd_progbar = OSD_BRIGHTNESS },
    { "tv-hue", "Hue", .osd_progbar = OSD_HUE},
    { "tv-saturation", "Saturation", .osd_progbar = OSD_SATURATION },
    { "tv-contrast", "Contrast", .osd_progbar = OSD_CONTRAST },
    { "ab-loop-a", "A-B loop start"},
    { "ab-loop-b", .msg = "A-B loop: ${ab-loop-a} - ${ab-loop-b}"},
    { "audio-device", "Audio device"},
    // By default, don't display the following properties on OSD
    { "pause", NULL },
    { "fullscreen", NULL },
    {0}
};

static void show_property_osd(MPContext *mpctx, const char *name, int osd_mode)
{
    struct MPOpts *opts = mpctx->opts;
    struct property_osd_display disp = { .name = name, .osd_name = name };

    if (!osd_mode)
        return;

    // look for the command
    for (const struct property_osd_display *p = property_osd_display; p->name; p++)
    {
        if (!strcmp(p->name, name)) {
            disp = *p;
            break;
        }
    }

    if (osd_mode == MP_ON_OSD_AUTO) {
        osd_mode =
            ((disp.msg || disp.osd_name || disp.seek_msg) ? MP_ON_OSD_MSG : 0) |
            ((disp.osd_progbar || disp.seek_bar) ? MP_ON_OSD_BAR : 0);
    }

    if (!disp.osd_progbar)
        disp.osd_progbar = ' ';

    if (!disp.osd_name)
        disp.osd_name = name;

    if (disp.seek_msg || disp.seek_bar) {
        mpctx->add_osd_seek_info |=
            (osd_mode & MP_ON_OSD_MSG ? disp.seek_msg : 0) |
            (osd_mode & MP_ON_OSD_BAR ? disp.seek_bar : 0);
        return;
    }

    struct m_option prop = {0};
    mp_property_do(name, M_PROPERTY_GET_CONSTRICTED_TYPE, &prop, mpctx);
    if ((osd_mode & MP_ON_OSD_BAR) && (prop.flags & CONF_RANGE) == CONF_RANGE) {
        if (prop.type == CONF_TYPE_INT) {
            int n = prop.min;
            if (disp.osd_progbar)
                n = disp.marker;
            int i;
            if (mp_property_do(name, M_PROPERTY_GET, &i, mpctx) > 0)
                set_osd_bar(mpctx, disp.osd_progbar, prop.min, prop.max, n, i);
        } else if (prop.type == CONF_TYPE_FLOAT) {
            float n = prop.min;
            if (disp.osd_progbar)
                n = disp.marker;
            float f;
            if (mp_property_do(name, M_PROPERTY_GET, &f, mpctx) > 0)
                set_osd_bar(mpctx, disp.osd_progbar, prop.min, prop.max, n, f);
        }
    }

    if (osd_mode & MP_ON_OSD_MSG) {
        void *tmp = talloc_new(NULL);

        const char *msg = disp.msg;
        if (!msg)
            msg = talloc_asprintf(tmp, "%s: ${%s}", disp.osd_name, name);

        char *osd_msg = talloc_steal(tmp, mp_property_expand_string(mpctx, msg));

        if (osd_msg && osd_msg[0])
            set_osd_msg(mpctx, 1, opts->osd_duration, "%s", osd_msg);

        talloc_free(tmp);
    }
}

static bool reinit_filters(MPContext *mpctx, enum stream_type mediatype)
{
    switch (mediatype) {
    case STREAM_VIDEO:
        return reinit_video_filters(mpctx) >= 0;
    case STREAM_AUDIO:
        return reinit_audio_filters(mpctx) >= 0;
    }
    return false;
}

static const char *const filter_opt[STREAM_TYPE_COUNT] = {
    [STREAM_VIDEO] = "vf",
    [STREAM_AUDIO] = "af",
};

static int set_filters(struct MPContext *mpctx, enum stream_type mediatype,
                       struct m_obj_settings *new_chain)
{
    bstr option = bstr0(filter_opt[mediatype]);
    struct m_config_option *co = m_config_get_co(mpctx->mconfig, option);
    if (!co)
        return -1;

    struct m_obj_settings **list = co->data;
    struct m_obj_settings *old_settings = *list;
    *list = NULL;
    m_option_copy(co->opt, list, &new_chain);

    bool success = reinit_filters(mpctx, mediatype);

    if (success) {
        m_option_free(co->opt, &old_settings);
        mp_notify_property(mpctx, filter_opt[mediatype]);
    } else {
        m_option_free(co->opt, list);
        *list = old_settings;
        reinit_filters(mpctx, mediatype);
    }

    return success ? 0 : -1;
}

static int edit_filters(struct MPContext *mpctx, struct mp_log *log,
                        enum stream_type mediatype,
                        const char *cmd, const char *arg)
{
    bstr option = bstr0(filter_opt[mediatype]);
    struct m_config_option *co = m_config_get_co(mpctx->mconfig, option);
    if (!co)
        return -1;

    // The option parser is used to modify the filter list itself.
    char optname[20];
    snprintf(optname, sizeof(optname), "%.*s-%s", BSTR_P(option), cmd);

    struct m_obj_settings *new_chain = NULL;
    m_option_copy(co->opt, &new_chain, co->data);

    int r = m_option_parse(log, co->opt, bstr0(optname), bstr0(arg), &new_chain);
    if (r >= 0)
        r = set_filters(mpctx, mediatype, new_chain);

    m_option_free(co->opt, &new_chain);

    return r >= 0 ? 0 : -1;
}

static int edit_filters_osd(struct MPContext *mpctx, enum stream_type mediatype,
                            const char *cmd, const char *arg, bool on_osd)
{
    int r = edit_filters(mpctx, mpctx->log, mediatype, cmd, arg);
    if (on_osd) {
        if (r >= 0) {
            const char *prop = filter_opt[mediatype];
            show_property_osd(mpctx, prop, MP_ON_OSD_MSG);
        } else {
            set_osd_msg(mpctx, 1, mpctx->opts->osd_duration,
                         "Changing filters failed!");
        }
    }
    return r;
}

static void recreate_overlays(struct MPContext *mpctx)
{
    struct command_ctx *cmd = mpctx->command_ctx;
    int overlay_next = !cmd->overlay_osd_current;
    struct sub_bitmaps *new = &cmd->overlay_osd[overlay_next];
    new->format = SUBBITMAP_RGBA;
    new->change_id = 1;

    bool valid = false;

    new->num_parts = 0;
    for (int n = 0; n < cmd->num_overlays; n++) {
        struct overlay *o = &cmd->overlays[n];
        if (o->source) {
            struct mp_image *s = o->source;
            struct sub_bitmap b = {
                .bitmap = s->planes[0],
                .stride = s->stride[0],
                .w = s->w, .dw = s->w,
                .h = s->h, .dh = s->h,
                .x = o->x,
                .y = o->y,
            };
            MP_TARRAY_APPEND(cmd, new->parts, new->num_parts, b);
        }
    }

    if (!cmd->overlay_packer)
        cmd->overlay_packer = talloc_zero(cmd, struct bitmap_packer);

    cmd->overlay_packer->padding = 1; // assume bilinear scaling
    packer_set_size(cmd->overlay_packer, new->num_parts);

    for (int n = 0; n < new->num_parts; n++)
        cmd->overlay_packer->in[n] = (struct pos){new->parts[n].w, new->parts[n].h};

    if (packer_pack(cmd->overlay_packer) < 0 || new->num_parts == 0)
        goto done;

    struct pos bb[2];
    packer_get_bb(cmd->overlay_packer, bb);

    new->packed_w = bb[1].x;
    new->packed_h = bb[1].y;

    if (!new->packed || new->packed->w < new->packed_w ||
                        new->packed->h < new->packed_h)
    {
        talloc_free(new->packed);
        new->packed = mp_image_alloc(IMGFMT_BGRA, cmd->overlay_packer->w,
                                                  cmd->overlay_packer->h);
        if (!new->packed)
            goto done;
    }

    // clear padding
    mp_image_clear(new->packed, 0, 0, new->packed->w, new->packed->h);

    for (int n = 0; n < new->num_parts; n++) {
        struct sub_bitmap *b = &new->parts[n];
        struct pos pos = cmd->overlay_packer->result[n];

        int stride = new->packed->stride[0];
        void *pdata = (uint8_t *)new->packed->planes[0] + pos.y * stride + pos.x * 4;
        memcpy_pic(pdata, b->bitmap, b->w * 4, b->h, stride, b->stride);

        b->bitmap = pdata;
        b->stride = stride;

        b->src_x = pos.x;
        b->src_y = pos.y;
    }

    valid = true;
done:
    if (!valid) {
        new->format = SUBBITMAP_EMPTY;
        new->num_parts = 0;
    }

    osd_set_external2(mpctx->osd, new);
    cmd->overlay_osd_current = overlay_next;
}

// Set overlay with the given ID to the contents as described by "new".
static void replace_overlay(struct MPContext *mpctx, int id, struct overlay *new)
{
    struct command_ctx *cmd = mpctx->command_ctx;
    assert(id >= 0);
    if (id >= cmd->num_overlays) {
        MP_TARRAY_GROW(cmd, cmd->overlays, id);
        while (cmd->num_overlays <= id)
            cmd->overlays[cmd->num_overlays++] = (struct overlay){0};
    }

    struct overlay *ptr = &cmd->overlays[id];

    talloc_free(ptr->source);
    *ptr = *new;

    recreate_overlays(mpctx);
}

static int overlay_add(struct MPContext *mpctx, int id, int x, int y,
                       char *file, int offset, char *fmt, int w, int h,
                       int stride)
{
    int r = -1;
    if (strcmp(fmt, "bgra") != 0) {
        MP_ERR(mpctx, "overlay-add: unsupported OSD format '%s'\n", fmt);
        goto error;
    }
    if (id < 0 || id >= 64) { // arbitrary upper limit
        MP_ERR(mpctx, "overlay-add: invalid id %d\n", id);
        goto error;
    }
    if (w <= 0 || h <= 0 || stride < w * 4 || (stride % 4)) {
        MP_ERR(mpctx, "overlay-add: inconsistent parameters\n");
        goto error;
    }
    struct overlay overlay = {
        .source = mp_image_alloc(IMGFMT_BGRA, w, h),
        .x = x,
        .y = y,
    };
    if (!overlay.source)
        goto error;
    int fd = -1;
    bool close_fd = true;
    void *p = NULL;
    if (file[0] == '@') {
        char *end;
        fd = strtol(&file[1], &end, 10);
        if (!file[1] || end[0])
            fd = -1;
        close_fd = false;
    } else if (file[0] == '&') {
        char *end;
        unsigned long long addr = strtoull(&file[1], &end, 0);
        if (!file[1] || end[0])
            addr = 0;
        p = (void *)(uintptr_t)addr;
    } else {
        fd = open(file, O_RDONLY | O_BINARY | O_CLOEXEC);
    }
    int map_size = 0;
    if (fd >= 0) {
        map_size = offset + h * stride;
        void *m = mmap(NULL, map_size, PROT_READ, MAP_SHARED, fd, 0);
        if (close_fd)
            close(fd);
        if (m && m != MAP_FAILED)
            p = m;
    }
    if (!p) {
        MP_ERR(mpctx, "overlay-add: could not open or map '%s'\n", file);
        talloc_free(overlay.source);
        goto error;
    }
    memcpy_pic(overlay.source->planes[0], (char *)p + offset, w * 4, h,
               overlay.source->stride[0], stride);
    if (map_size)
        munmap(p, map_size);

    replace_overlay(mpctx, id, &overlay);
    r = 0;
error:
    return r;
}

static void overlay_remove(struct MPContext *mpctx, int id)
{
    struct command_ctx *cmd = mpctx->command_ctx;
    if (id >= 0 && id < cmd->num_overlays)
        replace_overlay(mpctx, id, &(struct overlay){0});
}

static void overlay_uninit(struct MPContext *mpctx)
{
    struct command_ctx *cmd = mpctx->command_ctx;
    if (!mpctx->osd)
        return;
    for (int id = 0; id < cmd->num_overlays; id++)
        overlay_remove(mpctx, id);
    osd_set_external2(mpctx->osd, NULL);
    for (int n = 0; n < 2; n++)
        mp_image_unrefp(&cmd->overlay_osd[n].packed);
}

struct cycle_counter {
    char **args;
    int counter;
};

static bool stringlist_equals(char **l1, char **l2)
{
    assert(l1 && l2);
    for (int i = 0; ; i++) {
        if (!l1[i] && !l2[i])
            return true;
        if (!l1[i] || !l2[i])
            return false;
        if (strcmp(l1[i], l2[i]) != 0)
            return false;
    }
}

static char **stringlist_dup(void *talloc_ctx, char **list)
{
    int num = 0;
    char **res = NULL;
    for (int i = 0; list && list[i]; i++)
        MP_TARRAY_APPEND(talloc_ctx, res, num, talloc_strdup(talloc_ctx, list[i]));
    MP_TARRAY_APPEND(talloc_ctx, res, num, NULL);
    return res;
}

static int *get_cmd_cycle_counter(struct MPContext *mpctx, char **args)
{
    struct command_ctx *cmd = mpctx->command_ctx;
    for (int n = 0; n < cmd->num_cycle_counters; n++) {
        struct cycle_counter *ctr = &cmd->cycle_counters[n];
        if (stringlist_equals(ctr->args, args))
            return &ctr->counter;
    }
    struct cycle_counter ctr = {stringlist_dup(cmd, args), -1};
    MP_TARRAY_APPEND(cmd, cmd->cycle_counters, cmd->num_cycle_counters, ctr);
    return &cmd->cycle_counters[cmd->num_cycle_counters - 1].counter;
}

static int mp_property_multiply(char *property, double f, struct MPContext *mpctx)
{
    union m_option_value val = {0};
    struct m_option opt = {0};
    int r;

    r = mp_property_do(property, M_PROPERTY_GET_CONSTRICTED_TYPE, &opt, mpctx);
    if (r != M_PROPERTY_OK)
        return r;
    assert(opt.type);

    if (!opt.type->multiply)
        return M_PROPERTY_NOT_IMPLEMENTED;

    r = mp_property_do(property, M_PROPERTY_GET, &val, mpctx);
    if (r != M_PROPERTY_OK)
        return r;
    opt.type->multiply(&opt, &val, f);
    r = mp_property_do(property, M_PROPERTY_SET, &val, mpctx);
    m_option_free(&opt, &val);
    return r;
}

static struct track *find_track_with_url(struct MPContext *mpctx, int type,
                                         const char *url)
{
    for (int n = 0; n < mpctx->num_tracks; n++) {
        struct track *track = mpctx->tracks[n];
        if (track && track->type == type && track->is_external &&
            strcmp(track->external_filename, url) == 0)
            return track;
    }
    return NULL;
}

// Whether this property should react to key events generated by auto-repeat.
static bool check_property_autorepeat(char *property,  struct MPContext *mpctx)
{
    struct m_option prop = {0};
    if (mp_property_do(property, M_PROPERTY_GET_TYPE, &prop, mpctx) <= 0)
        return true;

    // This is a heuristic at best.
    if (prop.type == &m_option_type_flag || prop.type == &m_option_type_choice)
        return false;

    return true;
}

// Whether changes to this property (add/cycle cmds) benefit from cmd->scale
static bool check_property_scalable(char *property, struct MPContext *mpctx)
{
    struct m_option prop = {0};
    if (mp_property_do(property, M_PROPERTY_GET_TYPE, &prop, mpctx) <= 0)
        return true;

    // These properties are backed by a floating-point number
    return prop.type == &m_option_type_float ||
           prop.type == &m_option_type_double ||
           prop.type == &m_option_type_time ||
           prop.type == &m_option_type_aspect;
}

static struct mpv_node *add_map_entry(struct mpv_node *dst, const char *key)
{
    struct mpv_node_list *list = dst->u.list;
    assert(dst->format == MPV_FORMAT_NODE_MAP && dst->u.list);
    MP_TARRAY_GROW(list, list->values, list->num);
    MP_TARRAY_GROW(list, list->keys, list->num);
    list->keys[list->num] = talloc_strdup(list, key);
    return &list->values[list->num++];
}

#define ADD_MAP_INT(dst, name, i) (*add_map_entry(dst, name) = \
    (struct mpv_node){ .format = MPV_FORMAT_INT64, .u.int64 = (i) });

#define ADD_MAP_CSTR(dst, name, s) (*add_map_entry(dst, name) = \
    (struct mpv_node){ .format = MPV_FORMAT_STRING, .u.string = (s) });

int run_command(struct MPContext *mpctx, struct mp_cmd *cmd, struct mpv_node *res)
{
    struct command_ctx *cmdctx = mpctx->command_ctx;
    struct MPOpts *opts = mpctx->opts;
    int osd_duration = opts->osd_duration;
    int on_osd = cmd->flags & MP_ON_OSD_FLAGS;
    bool auto_osd = on_osd == MP_ON_OSD_AUTO;
    bool msg_osd = auto_osd || (on_osd & MP_ON_OSD_MSG);
    bool bar_osd = auto_osd || (on_osd & MP_ON_OSD_BAR);
    bool msg_or_nobar_osd = msg_osd && !(auto_osd && opts->osd_bar_visible);
    int osdl = msg_osd ? 1 : OSD_LEVEL_INVISIBLE;
    bool async = cmd->flags & MP_ASYNC_CMD;

    mp_cmd_dump(mpctx->log, cmd->id == MP_CMD_IGNORE ? MSGL_TRACE : MSGL_DEBUG,
                "Run command:", cmd);

    if (cmd->flags & MP_EXPAND_PROPERTIES) {
        for (int n = 0; n < cmd->nargs; n++) {
            if (cmd->args[n].type->type == CONF_TYPE_STRING) {
                char *s = mp_property_expand_string(mpctx, cmd->args[n].v.s);
                if (!s)
                    return -1;
                talloc_free(cmd->args[n].v.s);
                cmd->args[n].v.s = s;
            }
        }
    }

    switch (cmd->id) {
    case MP_CMD_SEEK: {
        double v = cmd->args[0].v.d * cmd->scale;
        int abs = cmd->args[1].v.i & 3;
        enum seek_precision precision = MPSEEK_DEFAULT;
        switch (((cmd->args[2].v.i | cmd->args[1].v.i) >> 3) & 3) {
        case 1: precision = MPSEEK_KEYFRAME; break;
        case 2: precision = MPSEEK_EXACT; break;
        }
        if (!mpctx->playback_initialized)
            return -1;
        mark_seek(mpctx);
        switch (abs) {
        case 0: { // Relative seek
            queue_seek(mpctx, MPSEEK_RELATIVE, v, precision, MPSEEK_FLAG_DELAY);
            set_osd_function(mpctx, (v > 0) ? OSD_FFW : OSD_REW);
            break;
        }
        case 1: { // Absolute seek by percentage
            double ratio = v / 100.0;
            double cur_pos = get_current_pos_ratio(mpctx, false);
            queue_seek(mpctx, MPSEEK_FACTOR, ratio, precision, MPSEEK_FLAG_DELAY);
            set_osd_function(mpctx, cur_pos < ratio ? OSD_FFW : OSD_REW);
            break;
        }
        case 2: { // Absolute seek to a timestamp in seconds
            if (v < 0) {
                // Seek from end
                double len = get_time_length(mpctx);
                if (len < 0)
                    return -1;
                v = MPMAX(0, len + v);
            }
            queue_seek(mpctx, MPSEEK_ABSOLUTE, v, precision, MPSEEK_FLAG_DELAY);
            set_osd_function(mpctx,
                             v > get_current_time(mpctx) ? OSD_FFW : OSD_REW);
            break;
        }
        case 3: { // Relative seek by percentage
            queue_seek(mpctx, MPSEEK_FACTOR,
                              get_current_pos_ratio(mpctx, false) + v / 100.0,
                              precision, MPSEEK_FLAG_DELAY);
            set_osd_function(mpctx, v > 0 ? OSD_FFW : OSD_REW);
            break;
        }}
        if (bar_osd)
            mpctx->add_osd_seek_info |= OSD_SEEK_INFO_BAR;
        if (msg_or_nobar_osd)
            mpctx->add_osd_seek_info |= OSD_SEEK_INFO_TEXT;
        break;
    }

    case MP_CMD_REVERT_SEEK: {
        if (!mpctx->playback_initialized)
            return -1;
        double oldpts = cmdctx->last_seek_pts;
        if (cmdctx->marked_pts != MP_NOPTS_VALUE)
            oldpts = cmdctx->marked_pts;
        if (cmd->args[0].v.i == 1) {
            cmdctx->marked_pts = get_current_time(mpctx);
        } else if (oldpts != MP_NOPTS_VALUE) {
            cmdctx->last_seek_pts = get_current_time(mpctx);
            cmdctx->marked_pts = MP_NOPTS_VALUE;
            queue_seek(mpctx, MPSEEK_ABSOLUTE, oldpts, MPSEEK_EXACT,
                       MPSEEK_FLAG_DELAY);
            set_osd_function(mpctx, OSD_REW);
            if (bar_osd)
                mpctx->add_osd_seek_info |= OSD_SEEK_INFO_BAR;
            if (msg_or_nobar_osd)
                mpctx->add_osd_seek_info |= OSD_SEEK_INFO_TEXT;
        } else {
            return -1;
        }
        break;
    }

    case MP_CMD_SET: {
        int r = mp_property_do(cmd->args[0].v.s, M_PROPERTY_SET_STRING,
                               cmd->args[1].v.s, mpctx);
        if (r == M_PROPERTY_OK || r == M_PROPERTY_UNAVAILABLE) {
            show_property_osd(mpctx, cmd->args[0].v.s, on_osd);
        } else if (r == M_PROPERTY_UNKNOWN) {
            set_osd_msg(mpctx, osdl, osd_duration,
                        "Unknown property: '%s'", cmd->args[0].v.s);
            return -1;
        } else if (r <= 0) {
            set_osd_msg(mpctx, osdl, osd_duration,
                        "Failed to set property '%s' to '%s'",
                        cmd->args[0].v.s, cmd->args[1].v.s);
            return -1;
        }
        break;
    }

    case MP_CMD_ADD:
    case MP_CMD_CYCLE:
    {
        char *property = cmd->args[0].v.s;
        if (cmd->repeated && !check_property_autorepeat(property, mpctx)) {
            MP_VERBOSE(mpctx, "Dropping command '%.*s' from auto-repeated key.\n",
                       BSTR_P(cmd->original));
            break;
        }
        double scale = 1;
        int scale_units = cmd->scale_units;
        if (check_property_scalable(property, mpctx)) {
            scale = cmd->scale;
            scale_units = 1;
        }
        for (int i = 0; i < scale_units; i++) {
            struct m_property_switch_arg s = {
                .inc = cmd->args[1].v.d * scale,
                .wrap = cmd->id == MP_CMD_CYCLE,
            };
            int r = mp_property_do(property, M_PROPERTY_SWITCH, &s, mpctx);
            if (r == M_PROPERTY_OK || r == M_PROPERTY_UNAVAILABLE) {
                show_property_osd(mpctx, property, on_osd);
            } else if (r == M_PROPERTY_UNKNOWN) {
                set_osd_msg(mpctx, osdl, osd_duration,
                            "Unknown property: '%s'", property);
                return -1;
            } else if (r <= 0) {
                set_osd_msg(mpctx, osdl, osd_duration,
                        "Failed to change property '%s'", property);
                return -1;
            }
        }
        break;
    }

    case MP_CMD_MULTIPLY: {
        char *property = cmd->args[0].v.s;
        double f = cmd->args[1].v.d;
        int r = mp_property_multiply(property, f, mpctx);

        if (r == M_PROPERTY_OK || r == M_PROPERTY_UNAVAILABLE) {
            show_property_osd(mpctx, property, on_osd);
        } else if (r == M_PROPERTY_UNKNOWN) {
            set_osd_msg(mpctx, osdl, osd_duration,
                        "Unknown property: '%s'", property);
            return -1;
        } else if (r <= 0) {
            set_osd_msg(mpctx, osdl, osd_duration,
                        "Failed to multiply property '%s' by %g", property, f);
            return -1;
        }
        break;
    }

    case MP_CMD_CYCLE_VALUES: {
        char *args[MP_CMD_MAX_ARGS + 1] = {0};
        for (int n = 0; n < cmd->nargs; n++)
            args[n] = cmd->args[n].v.s;
        int first = 1, dir = 1;
        if (strcmp(args[0], "!reverse") == 0) {
            first += 1;
            dir = -1;
        }
        int *ptr = get_cmd_cycle_counter(mpctx, &args[first - 1]);
        int count = cmd->nargs - first;
        if (ptr && count > 0) {
            *ptr = *ptr < 0 ? (dir > 0 ? 0 : -1) : *ptr + dir;
            if (*ptr >= count)
                *ptr = 0;
            if (*ptr < 0)
                *ptr = count - 1;
            char *property = args[first - 1];
            char *value = args[first + *ptr];
            int r = mp_property_do(property, M_PROPERTY_SET_STRING, value, mpctx);
            if (r == M_PROPERTY_OK || r == M_PROPERTY_UNAVAILABLE) {
                show_property_osd(mpctx, property, on_osd);
            } else if (r == M_PROPERTY_UNKNOWN) {
                set_osd_msg(mpctx, osdl, osd_duration,
                            "Unknown property: '%s'", property);
                return -1;
            } else if (r <= 0) {
                set_osd_msg(mpctx, osdl, osd_duration,
                            "Failed to set property '%s' to '%s'",
                            property, value);
                return -1;
            }
        }
        break;
    }

    case MP_CMD_FRAME_STEP:
        if (!mpctx->playback_initialized)
            return -1;
        if (cmd->is_up_down) {
            if (cmd->is_up) {
                if (mpctx->step_frames < 1)
                    set_pause_state(mpctx, true);
            } else {
                if (cmd->repeated) {
                    set_pause_state(mpctx, false);
                } else {
                    add_step_frame(mpctx, 1);
                }
            }
        } else {
            add_step_frame(mpctx, 1);
        }
        break;

    case MP_CMD_FRAME_BACK_STEP:
        if (!mpctx->playback_initialized)
            return -1;
        add_step_frame(mpctx, -1);
        break;

    case MP_CMD_QUIT:
    case MP_CMD_QUIT_WATCH_LATER:
        if (cmd->id == MP_CMD_QUIT_WATCH_LATER || opts->position_save_on_quit)
            mp_write_watch_later_conf(mpctx);
        mpctx->stop_play = PT_QUIT;
        mpctx->quit_custom_rc = cmd->args[0].v.i;
        mpctx->has_quit_custom_rc = true;
        mp_wakeup_core(mpctx);
        break;

    case MP_CMD_PLAYLIST_NEXT:
    case MP_CMD_PLAYLIST_PREV:
    {
        int dir = cmd->id == MP_CMD_PLAYLIST_PREV ? -1 : +1;
        int force = cmd->args[0].v.i;

        struct playlist_entry *e = mp_next_file(mpctx, dir, force, true);
        if (!e && !force)
            return -1;
        mp_set_playlist_entry(mpctx, e);
        if (on_osd & MP_ON_OSD_MSG)
            mpctx->add_osd_seek_info |= OSD_SEEK_INFO_CURRENT_FILE;
        break;
    }

    case MP_CMD_SUB_STEP:
    case MP_CMD_SUB_SEEK: {
        if (!mpctx->playback_initialized)
            return -1;
        struct track *track = mpctx->current_track[0][STREAM_SUB];
        struct dec_sub *sub = track ? track->d_sub : NULL;
        double refpts = get_current_time(mpctx);
        if (sub && refpts != MP_NOPTS_VALUE) {
            double a[2];
            a[0] = refpts - opts->sub_delay;
            a[1] = cmd->args[0].v.i;
            if (sub_control(sub, SD_CTRL_SUB_STEP, a) > 0) {
                if (cmd->id == MP_CMD_SUB_STEP) {
                    opts->sub_delay -= a[0];
                    osd_changed(mpctx->osd);
                    show_property_osd(mpctx, "sub-delay", on_osd);
                } else {
                    // We can easily get stuck by failing to seek to the video
                    // frame which actually shows the sub first (because video
                    // frame PTS and sub PTS rarely match exactly). Add some
                    // rounding for the mess of it.
                    a[0] += 0.01 * (a[1] >= 0 ? 1 : -1);
                    mark_seek(mpctx);
                    queue_seek(mpctx, MPSEEK_RELATIVE, a[0], MPSEEK_EXACT,
                               MPSEEK_FLAG_DELAY);
                    set_osd_function(mpctx, (a[0] > 0) ? OSD_FFW : OSD_REW);
                    if (bar_osd)
                        mpctx->add_osd_seek_info |= OSD_SEEK_INFO_BAR;
                    if (msg_or_nobar_osd)
                        mpctx->add_osd_seek_info |= OSD_SEEK_INFO_TEXT;
                }
            }
        }
        break;
    }

    case MP_CMD_PRINT_TEXT: {
        MP_INFO(mpctx, "%s\n", cmd->args[0].v.s);
        break;
    }

    case MP_CMD_SHOW_TEXT: {
        // if no argument supplied use default osd_duration, else <arg> ms.
        set_osd_msg(mpctx, cmd->args[2].v.i,
                    (cmd->args[1].v.i < 0 ? osd_duration : cmd->args[1].v.i),
                    "%s", cmd->args[0].v.s);
        break;
    }

    case MP_CMD_EXPAND_TEXT: {
        if (!res)
            return -1;
        *res = (mpv_node){
            .format = MPV_FORMAT_STRING,
            .u.string = mp_property_expand_string(mpctx, cmd->args[0].v.s)
        };
        break;
    }

    case MP_CMD_LOADFILE: {
        char *filename = cmd->args[0].v.s;
        int append = cmd->args[1].v.i;

        if (!append)
            playlist_clear(mpctx->playlist);

        struct playlist_entry *entry = playlist_entry_new(filename);
        if (cmd->args[2].v.str_list) {
            char **pairs = cmd->args[2].v.str_list;
            for (int i = 0; pairs[i] && pairs[i + 1]; i += 2) {
                playlist_entry_add_param(entry, bstr0(pairs[i]),
                                         bstr0(pairs[i + 1]));
            }
        }
        playlist_add(mpctx->playlist, entry);

        if (!append || (append == 2 && !mpctx->playlist->current)) {
            if (opts->position_save_on_quit) // requested in issue #1148
                mp_write_watch_later_conf(mpctx);
            mp_set_playlist_entry(mpctx, entry);
        }
        mp_notify(mpctx, MP_EVENT_CHANGE_PLAYLIST, NULL);
        mp_wakeup_core(mpctx);
        break;
    }

    case MP_CMD_LOADLIST: {
        char *filename = cmd->args[0].v.s;
        bool append = cmd->args[1].v.i;
        struct playlist *pl = playlist_parse_file(filename, mpctx->global);
        if (pl) {
            prepare_playlist(mpctx, pl);
            struct playlist_entry *new = pl->current;
            if (!append)
                playlist_clear(mpctx->playlist);
            playlist_append_entries(mpctx->playlist, pl);
            talloc_free(pl);

            if (!append && mpctx->playlist->first)
                mp_set_playlist_entry(mpctx, new ? new : mpctx->playlist->first);

            mp_notify(mpctx, MP_EVENT_CHANGE_PLAYLIST, NULL);
            mp_wakeup_core(mpctx);
        } else {
            MP_ERR(mpctx, "Unable to load playlist %s.\n", filename);
            return -1;
        }
        break;
    }

    case MP_CMD_PLAYLIST_CLEAR: {
        // Supposed to clear the playlist, except the currently played item.
        if (mpctx->playlist->current_was_replaced)
            mpctx->playlist->current = NULL;
        while (mpctx->playlist->first) {
            struct playlist_entry *e = mpctx->playlist->first;
            if (e == mpctx->playlist->current) {
                e = e->next;
                if (!e)
                    break;
            }
            playlist_remove(mpctx->playlist, e);
        }
        mp_notify(mpctx, MP_EVENT_CHANGE_PLAYLIST, NULL);
        mp_wakeup_core(mpctx);
        break;
    }

    case MP_CMD_PLAYLIST_REMOVE: {
        struct playlist_entry *e = playlist_entry_from_index(mpctx->playlist,
                                                             cmd->args[0].v.i);
        if (cmd->args[0].v.i < 0)
            e = mpctx->playlist->current;
        if (!e)
            return -1;
        // Can't play a removed entry
        if (mpctx->playlist->current == e && !mpctx->stop_play)
            mpctx->stop_play = PT_NEXT_ENTRY;
        playlist_remove(mpctx->playlist, e);
        mp_notify(mpctx, MP_EVENT_CHANGE_PLAYLIST, NULL);
        mp_wakeup_core(mpctx);
        break;
    }

    case MP_CMD_PLAYLIST_MOVE: {
        struct playlist_entry *e1 = playlist_entry_from_index(mpctx->playlist,
                                                              cmd->args[0].v.i);
        struct playlist_entry *e2 = playlist_entry_from_index(mpctx->playlist,
                                                              cmd->args[1].v.i);
        if (!e1)
            return -1;
        playlist_move(mpctx->playlist, e1, e2);
        mp_notify(mpctx, MP_EVENT_CHANGE_PLAYLIST, NULL);
        break;
    }

    case MP_CMD_PLAYLIST_SHUFFLE: {
        playlist_shuffle(mpctx->playlist);
        mp_notify(mpctx, MP_EVENT_CHANGE_PLAYLIST, NULL);
        break;
    }

    case MP_CMD_STOP:
        playlist_clear(mpctx->playlist);
        if (mpctx->stop_play != PT_QUIT)
            mpctx->stop_play = PT_STOP;
        mp_wakeup_core(mpctx);
        break;

    case MP_CMD_SHOW_PROGRESS:
        mpctx->add_osd_seek_info |=
                (msg_osd ? OSD_SEEK_INFO_TEXT : 0) |
                (bar_osd ? OSD_SEEK_INFO_BAR : 0);
        mpctx->osd_force_update = true;
        mp_wakeup_core(mpctx);
        break;

    case MP_CMD_TV_LAST_CHANNEL: {
        if (!mpctx->demuxer)
            return -1;
        demux_stream_control(mpctx->demuxer, STREAM_CTRL_TV_LAST_CHAN, NULL);
        break;
    }

    case MP_CMD_SUB_ADD:
    case MP_CMD_AUDIO_ADD: {
        if (!mpctx->playing)
            return -1;
        int type = cmd->id == MP_CMD_SUB_ADD ? STREAM_SUB : STREAM_AUDIO;
        if (cmd->args[1].v.i == 2) {
            struct track *t = find_track_with_url(mpctx, type, cmd->args[0].v.s);
            if (t) {
                if (mpctx->playback_initialized) {
                    mp_switch_track(mpctx, t->type, t, FLAG_MARK_SELECTION);
                    print_track_list(mpctx, "Track switched:");
                } else {
                    opts->stream_id[0][t->type] = t->user_tid;
                }
                return 0;
            }
        }
        struct track *t = mp_add_external_file(mpctx, cmd->args[0].v.s, type);
        if (!t)
            return -1;
        if (cmd->args[1].v.i == 1) {
            t->no_default = true;
        } else {
            if (mpctx->playback_initialized) {
                mp_switch_track(mpctx, t->type, t, FLAG_MARK_SELECTION);
            } else {
                opts->stream_id[0][t->type] = t->user_tid;
            }
        }
        char *title = cmd->args[2].v.s;
        if (title && title[0])
            t->title = talloc_strdup(t, title);
        char *lang = cmd->args[3].v.s;
        if (lang && lang[0])
            t->lang = talloc_strdup(t, lang);
        if (mpctx->playback_initialized)
            print_track_list(mpctx, "Track added:");
        break;
    }

    case MP_CMD_SUB_REMOVE:
    case MP_CMD_AUDIO_REMOVE: {
        int type = cmd->id == MP_CMD_SUB_REMOVE ? STREAM_SUB : STREAM_AUDIO;
        struct track *t = mp_track_by_tid(mpctx, type, cmd->args[0].v.i);
        if (!t)
            return -1;
        mp_remove_track(mpctx, t);
        if (mpctx->playback_initialized)
            print_track_list(mpctx, "Track removed:");
        break;
    }

    case MP_CMD_SUB_RELOAD:
    case MP_CMD_AUDIO_RELOAD: {
        if (!mpctx->playback_initialized) {
            MP_ERR(mpctx, "Cannot reload while not initialized.\n");
            return -1;
        }
        int type = cmd->id == MP_CMD_SUB_RELOAD ? STREAM_SUB : STREAM_AUDIO;
        struct track *t = mp_track_by_tid(mpctx, type, cmd->args[0].v.i);
        struct track *nt = NULL;
        if (t && t->is_external && t->external_filename) {
            char *filename = talloc_strdup(NULL, t->external_filename);
            mp_remove_track(mpctx, t);
            nt = mp_add_external_file(mpctx, filename, type);
            talloc_free(filename);
        }
        if (nt) {
            mp_switch_track(mpctx, nt->type, nt, 0);
            print_track_list(mpctx, "Reloaded:");
            return 0;
        }
        return -1;
    }

    case MP_CMD_RESCAN_EXTERNAL_FILES: {
        if (!mpctx->playing)
            return -1;
        autoload_external_files(mpctx);
        if (cmd->args[0].v.i && mpctx->playback_initialized) {
            // somewhat fuzzy and not ideal
            struct track *a = select_default_track(mpctx, 0, STREAM_AUDIO);
            if (a && a->is_external)
                mp_switch_track(mpctx, STREAM_AUDIO, a, 0);
            struct track *s = select_default_track(mpctx, 0, STREAM_SUB);
            if (s && s->is_external)
                mp_switch_track(mpctx, STREAM_SUB, s, 0);

            print_track_list(mpctx, "Track list:\n");
        }
        break;
    }

    case MP_CMD_SCREENSHOT: {
        int mode = cmd->args[0].v.i & 3;
        int freq = (cmd->args[0].v.i | cmd->args[1].v.i) >> 3;
        screenshot_request(mpctx, mode, freq, msg_osd, async);
        break;
    }

    case MP_CMD_SCREENSHOT_TO_FILE:
        screenshot_to_file(mpctx, cmd->args[0].v.s, cmd->args[1].v.i, msg_osd,
                           async);
        break;

    case MP_CMD_SCREENSHOT_RAW: {
        if (!res)
            return -1;
        struct mp_image *img = screenshot_get_rgb(mpctx, cmd->args[0].v.i);
        if (!img)
            return -1;
        struct mpv_node_list *info = talloc_zero(NULL, struct mpv_node_list);
        talloc_steal(info, img);
        *res = (mpv_node){ .format = MPV_FORMAT_NODE_MAP, .u.list = info };
        ADD_MAP_INT(res, "w", img->w);
        ADD_MAP_INT(res, "h", img->h);
        ADD_MAP_INT(res, "stride", img->stride[0]);
        ADD_MAP_CSTR(res, "format", "bgr0");
        struct mpv_byte_array *ba = talloc_ptrtype(info, ba);
        *ba = (struct mpv_byte_array){
            .data = img->planes[0],
            .size = img->stride[0] * img->h,
        };
        *add_map_entry(res, "data") =
            (struct mpv_node){.format = MPV_FORMAT_BYTE_ARRAY, .u.ba = ba,};
        break;
    }

    case MP_CMD_RUN: {
        char *args[MP_CMD_MAX_ARGS + 1] = {0};
        for (int n = 0; n < cmd->nargs; n++)
            args[n] = cmd->args[n].v.s;
        mp_subprocess_detached(mpctx->log, args);
        break;
    }

    case MP_CMD_ENABLE_INPUT_SECTION:
        mp_input_enable_section(mpctx->input, cmd->args[0].v.s, cmd->args[1].v.i);
        break;

    case MP_CMD_DISABLE_INPUT_SECTION:
        mp_input_disable_section(mpctx->input, cmd->args[0].v.s);
        break;

    case MP_CMD_DEFINE_INPUT_SECTION:
        mp_input_define_section(mpctx->input, cmd->args[0].v.s, "<api>",
                                cmd->args[1].v.s, !!cmd->args[2].v.i,
                                cmd->sender);
        break;

    case MP_CMD_AB_LOOP: {
        double now = get_current_time(mpctx);
        if (opts->ab_loop[0] == MP_NOPTS_VALUE) {
            mp_property_do("ab-loop-a", M_PROPERTY_SET, &now, mpctx);
            show_property_osd(mpctx, "ab-loop-a", on_osd);
        } else if (opts->ab_loop[1] == MP_NOPTS_VALUE) {
            mp_property_do("ab-loop-b", M_PROPERTY_SET, &now, mpctx);
            show_property_osd(mpctx, "ab-loop-b", on_osd);
        } else {
            now = MP_NOPTS_VALUE;
            mp_property_do("ab-loop-a", M_PROPERTY_SET, &now, mpctx);
            mp_property_do("ab-loop-b", M_PROPERTY_SET, &now, mpctx);
            set_osd_msg(mpctx, osdl, osd_duration, "Clear A-B loop");
        }
        break;
    }

    case MP_CMD_DROP_BUFFERS: {
        reset_audio_state(mpctx);
        reset_video_state(mpctx);

        if (mpctx->demuxer)
            demux_flush(mpctx->demuxer);

        break;
    }

    case MP_CMD_AO_RELOAD:
        reload_audio_output(mpctx);
        break;

    case MP_CMD_VO_RESIZE: {
        if (!mpctx->video_out)
            return -1;
        vo_control(mpctx->video_out, VOCTRL_EXTERNAL_RESIZE, NULL);
        break;
    }

    case MP_CMD_AF:
        return edit_filters_osd(mpctx, STREAM_AUDIO, cmd->args[0].v.s,
                                cmd->args[1].v.s, msg_osd);

    case MP_CMD_VF:
        return edit_filters_osd(mpctx, STREAM_VIDEO, cmd->args[0].v.s,
                                cmd->args[1].v.s, msg_osd);

    case MP_CMD_VF_COMMAND:
        if (!mpctx->vo_chain)
            return -1;
        return vf_send_command(mpctx->vo_chain->vf, cmd->args[0].v.s,
                               cmd->args[1].v.s, cmd->args[2].v.s);

#if HAVE_LIBAF
    case MP_CMD_AF_COMMAND:
        if (!mpctx->ao_chain)
            return -1;
        return af_send_command(mpctx->ao_chain->af, cmd->args[0].v.s,
                               cmd->args[1].v.s, cmd->args[2].v.s);
#endif

    case MP_CMD_SCRIPT_BINDING: {
        mpv_event_client_message event = {0};
        char *name = cmd->args[0].v.s;
        if (!name || !name[0])
            return -1;
        char *sep = strchr(name, '/');
        char *target = NULL;
        char space[MAX_CLIENT_NAME];
        if (sep) {
            snprintf(space, sizeof(space), "%.*s", (int)(sep - name), name);
            target = space;
            name = sep + 1;
        }
        char state[3] = {'p', cmd->is_mouse_button ? 'm' : '-'};
        if (cmd->is_up_down)
            state[0] = cmd->repeated ? 'r' : (cmd->is_up ? 'u' : 'd');
        event.num_args = 4;
        event.args = (const char*[4]){"key-binding", name, state,
                                      cmd->key_name ? cmd->key_name : ""};
        if (mp_client_send_event_dup(mpctx, target,
                                     MPV_EVENT_CLIENT_MESSAGE, &event) < 0)
        {
            MP_VERBOSE(mpctx, "Can't find script '%s' when handling input.\n",
                       target ? target : "-");
            return -1;
        }
        break;
    }

    case MP_CMD_SCRIPT_MESSAGE_TO: {
        mpv_event_client_message *event = talloc_ptrtype(NULL, event);
        *event = (mpv_event_client_message){0};
        for (int n = 1; n < cmd->nargs; n++) {
            MP_TARRAY_APPEND(event, event->args, event->num_args,
                             talloc_strdup(event, cmd->args[n].v.s));
        }
        if (mp_client_send_event(mpctx, cmd->args[0].v.s,
                                 MPV_EVENT_CLIENT_MESSAGE, event) < 0)
        {
            MP_VERBOSE(mpctx, "Can't find script '%s' for %s.\n",
                       cmd->args[0].v.s, cmd->name);
            return -1;
        }
        break;
    }
    case MP_CMD_SCRIPT_MESSAGE: {
        const char *args[MP_CMD_MAX_ARGS];
        mpv_event_client_message event = {.args = args};
        for (int n = 0; n < cmd->nargs; n++)
            event.args[event.num_args++] = cmd->args[n].v.s;
        mp_client_broadcast_event(mpctx, MPV_EVENT_CLIENT_MESSAGE, &event);
        break;
    }

    case MP_CMD_OVERLAY_ADD:
        overlay_add(mpctx,
                    cmd->args[0].v.i, cmd->args[1].v.i, cmd->args[2].v.i,
                    cmd->args[3].v.s, cmd->args[4].v.i, cmd->args[5].v.s,
                    cmd->args[6].v.i, cmd->args[7].v.i, cmd->args[8].v.i);
        break;

    case MP_CMD_OVERLAY_REMOVE:
        overlay_remove(mpctx, cmd->args[0].v.i);
        break;

    case MP_CMD_COMMAND_LIST: {
        for (struct mp_cmd *sub = cmd->args[0].v.p; sub; sub = sub->queue_next)
            run_command(mpctx, sub, NULL);
        break;
    }

    case MP_CMD_IGNORE:
        break;

    case MP_CMD_WRITE_WATCH_LATER_CONFIG: {
        mp_write_watch_later_conf(mpctx);
        break;
    }

    case MP_CMD_HOOK_ADD:
        if (!cmd->sender) {
            MP_ERR(mpctx, "Can be used from client API only.\n");
            return -1;
        }
        mp_hook_add(mpctx, cmd->sender, cmd->args[0].v.s, cmd->args[1].v.i,
                    cmd->args[2].v.i);
        break;
    case MP_CMD_HOOK_ACK:
        if (!cmd->sender) {
            MP_ERR(mpctx, "Can be used from client API only.\n");
            return -1;
        }
        mp_hook_run(mpctx, cmd->sender, cmd->args[0].v.s);
        break;

    case MP_CMD_MOUSE: {
        const int x = cmd->args[0].v.i, y = cmd->args[1].v.i;
        int button = cmd->args[2].v.i;
        if (button == -1) {// no button
            mp_input_set_mouse_pos_artificial(mpctx->input, x, y);
            break;
        }
        if (button < 0 || button >= 20) {// invalid button
            MP_ERR(mpctx, "%d is not a valid mouse button number.\n", button);
            return -1;
        }
        const bool dbc = cmd->args[3].v.i;
        if (dbc && button > (MP_MBTN_RIGHT - MP_MBTN_BASE)) {
            MP_ERR(mpctx, "%d is not a valid mouse button for double-clicks.\n",
                   button);
            return -1;
        }
        button += dbc ? MP_MBTN_DBL_BASE : MP_MBTN_BASE;
        mp_input_set_mouse_pos_artificial(mpctx->input, x, y);
        mp_input_put_key_artificial(mpctx->input, button);
        break;
    }

    case MP_CMD_KEYPRESS:
    case MP_CMD_KEYDOWN: {
        const char *key_name = cmd->args[0].v.s;
        int code = mp_input_get_key_from_name(key_name);
        if (code < 0) {
            MP_ERR(mpctx, "%s is not a valid input name.\n", key_name);
            return -1;
        }
        if (cmd->id == MP_CMD_KEYDOWN)
            code |= MP_KEY_STATE_DOWN;

        mp_input_put_key_artificial(mpctx->input, code);
        break;
    }

    case MP_CMD_KEYUP: {
        const char *key_name = cmd->args[0].v.s;
        if (key_name[0] == '\0') {
            mp_input_put_key_artificial(mpctx->input, MP_INPUT_RELEASE_ALL);
        } else {
            int code = mp_input_get_key_from_name(key_name);
            if (code < 0) {
                MP_ERR(mpctx, "%s is not a valid input name.\n", key_name);
                return -1;
            }
            mp_input_put_key_artificial(mpctx->input, code | MP_KEY_STATE_UP);
        }
        break;
    }

    case MP_CMD_APPLY_PROFILE: {
        char *profile = cmd->args[0].v.s;
        if (m_config_set_profile(mpctx->mconfig, profile, M_SETOPT_RUNTIME) < 0)
            return -1;
        break;
    }

    case MP_CMD_LOAD_SCRIPT: {
        char *script = cmd->args[0].v.s;
        if (mp_load_user_script(mpctx, script) < 0)
            return -1;
        break;
    }

    default:
        MP_VERBOSE(mpctx, "Received unknown cmd %s\n", cmd->name);
        return -1;
    }
    return 0;
}

void command_uninit(struct MPContext *mpctx)
{
    overlay_uninit(mpctx);
    ao_hotplug_destroy(mpctx->command_ctx->hotplug);
    talloc_free(mpctx->command_ctx);
    mpctx->command_ctx = NULL;
}

void command_init(struct MPContext *mpctx)
{
    struct command_ctx *ctx = talloc(NULL, struct command_ctx);
    *ctx = (struct command_ctx){
        .last_seek_pts = MP_NOPTS_VALUE,
    };
    mpctx->command_ctx = ctx;

    int num_base = MP_ARRAY_SIZE(mp_properties_base);
    int num_opts = m_config_get_co_count(mpctx->mconfig);
    ctx->properties =
        talloc_zero_array(ctx, struct m_property, num_base + num_opts + 1);
    memcpy(ctx->properties, mp_properties_base, sizeof(mp_properties_base));

    int count = num_base;
    for (int n = 0; n < num_opts; n++) {
        struct m_config_option *co = m_config_get_co_index(mpctx->mconfig, n);
        assert(co->name[0]);
        if ((co->opt->flags & M_OPT_NOPROP) &&
            co->opt->type != &m_option_type_cli_alias)
            continue;

        struct m_property prop = {0};

        if (co->opt->type == &m_option_type_alias) {
            const char *alias = (const char *)co->opt->priv;

            prop = (struct m_property){
                .name = co->name,
                .call = co->opt->deprecation_message ?
                            mp_property_deprecated_alias : mp_property_alias,
                .priv = (void *)alias,
                .is_option = true,
            };
        } else if (co->opt->type == &m_option_type_cli_alias) {
            bstr rname = bstr0(co->opt->priv);
            for (int i = rname.len - 1; i >= 0; i--) {
                if (rname.start[i] == '-') {
                    rname.len = i;
                    break;
                }
            }

            prop = (struct m_property){
                .name = co->name,
                .call = mp_property_shitfuck,
                .priv = bstrto0(ctx, rname),
                .is_option = true,
            };
        } else {
            prop = (struct m_property){
                .name = co->name,
                .call = mp_property_generic_option,
                .is_option = true,
            };
        }

        if (prop.name) {
            // The option might be covered by a manual property already.
            if (m_property_list_find(ctx->properties, prop.name))
                continue;

            ctx->properties[count++] = prop;
        }
    }
}

static void command_event(struct MPContext *mpctx, int event, void *arg)
{
    struct command_ctx *ctx = mpctx->command_ctx;

    if (event == MPV_EVENT_START_FILE) {
        ctx->last_seek_pts = MP_NOPTS_VALUE;
        ctx->marked_pts = MP_NOPTS_VALUE;
    }

    if (event == MPV_EVENT_IDLE)
        ctx->is_idle = true;
    if (event == MPV_EVENT_START_FILE)
        ctx->is_idle = false;
    if (event == MPV_EVENT_END_FILE || event == MPV_EVENT_FILE_LOADED) {
        // Update chapters - does nothing if something else is visible.
        set_osd_bar_chapters(mpctx, OSD_BAR_SEEK);
    }
}

void handle_command_updates(struct MPContext *mpctx)
{
    struct command_ctx *ctx = mpctx->command_ctx;

    // This is a bit messy: ao_hotplug wakes up the player, and then we have
    // to recheck the state. Then the client(s) will read the property.
    if (ctx->hotplug && ao_hotplug_check_update(ctx->hotplug))
        mp_notify_property(mpctx, "audio-device-list");
}

void mp_notify(struct MPContext *mpctx, int event, void *arg)
{
    // The OSD can implicitly reference some properties.
    mpctx->osd_idle_update = true;

    command_event(mpctx, event, arg);

    mp_client_broadcast_event(mpctx, event, arg);
}

static void update_priority(struct MPContext *mpctx)
{
#if HAVE_WIN32_DESKTOP
    struct MPOpts *opts = mpctx->opts;
    if (opts->w32_priority > 0)
        SetPriorityClass(GetCurrentProcess(), opts->w32_priority);
#endif
}

void mp_option_change_callback(void *ctx, struct m_config_option *co, int flags)
{
    struct MPContext *mpctx = ctx;
    struct command_ctx *cmd = mpctx->command_ctx;

    if (flags & UPDATE_TERM)
        mp_update_logging(mpctx, false);

    if (flags & UPDATE_DEINT)
        recreate_auto_filters(mpctx);

    if (flags & UPDATE_OSD) {
        osd_changed(mpctx->osd);
        for (int n = 0; n < NUM_PTRACKS; n++) {
            struct track *track = mpctx->current_track[n][STREAM_SUB];
            struct dec_sub *sub = track ? track->d_sub : NULL;
            if (sub)
                sub_control(track->d_sub, SD_CTRL_UPDATE_SPEED, NULL);
        }
        mp_wakeup_core(mpctx);
    }

    if (flags & UPDATE_BUILTIN_SCRIPTS)
        mp_load_builtin_scripts(mpctx);

    if (flags & UPDATE_IMGPAR) {
        struct track *track = mpctx->current_track[0][STREAM_VIDEO];
        if (track && track->d_video) {
            video_reset_params(track->d_video);
            mp_force_video_refresh(mpctx);
        }
    }

    if (flags & UPDATE_INPUT) {
        mp_input_update_opts(mpctx->input);

        // Rather coarse change-detection, but sufficient effort.
        struct MPOpts *opts = mpctx->opts;
        if (!bstr_equals(bstr0(cmd->cur_ipc), bstr0(opts->ipc_path)) ||
            !bstr_equals(bstr0(cmd->cur_ipc_input), bstr0(opts->input_file)))
        {
            talloc_free(cmd->cur_ipc);
            talloc_free(cmd->cur_ipc_input);
            cmd->cur_ipc = talloc_strdup(cmd, opts->ipc_path);
            cmd->cur_ipc_input = talloc_strdup(cmd, opts->input_file);
            mp_uninit_ipc(mpctx->ipc_ctx);
            mpctx->ipc_ctx = mp_init_ipc(mpctx->clients, mpctx->global);
        }
    }

    if (flags & UPDATE_AUDIO)
        reload_audio_output(mpctx);

    if (flags & UPDATE_PRIORITY)
        update_priority(mpctx);

    if (flags & UPDATE_SCREENSAVER)
        update_screensaver_state(mpctx);

    if (flags & UPDATE_VOL)
        audio_update_volume(mpctx);

    if (flags & UPDATE_LAVFI_COMPLEX)
        update_lavfi_complex(mpctx);
}

void mp_notify_property(struct MPContext *mpctx, const char *property)
{
    mp_client_property_change(mpctx, property);
}
