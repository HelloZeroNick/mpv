/*
 * OHAudio audio output driver.
 *
 * Copyright (C) 2025 Bao Han <erbws@foxmail.com>
 *
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

#include "ao.h"
#include "internal.h"
#include "common/msg.h"
#include "audio/format.h"
#include "options/m_option.h"
#include "osdep/threads.h"
#include "osdep/timer.h"

#include <stdatomic.h>
#include <ohaudio/native_audiorenderer.h>
#include <ohaudio/native_audiostreambuilder.h>

// When the audio service stops pulling data (app background freeze, focus
// takeover, ...) while mpv still considers the stream playing, the write
// callback stops firing and nothing else signals the outage: interrupt hints
// may never reach us (delivered while the process was frozen) and no error
// callback fires because nothing failed. The watchdog closes that gap.
#define WATCHDOG_TICK_NS MP_TIME_MS_TO_NS(1000)
#define WATCHDOG_STALL_START MP_TIME_S_TO_NS(3)
#define WATCHDOG_RETRY_INTERVAL MP_TIME_S_TO_NS(3)
#define WATCHDOG_STALL_RELOAD MP_TIME_S_TO_NS(20)

struct priv {
    OH_AudioStreamBuilder* builder;
    OH_AudioRenderer *renderer;
    int64_t last_query;
    int64_t frame_position;
    int64_t frame_timestamp;
    int64_t written_frames;
    int64_t queued_frames;
    bool timestamp_valid;

    int encoding_type;

    // Set by set_pause()/reset()/start(); the interrupt callback must not
    // resume the stream when the player itself wants silence.
    atomic_bool user_paused;
    // Set while the system has taken the focus away (call, other app, ...).
    // Cleared again by the next AUDIOSTREAM_INTERRUPT_HINT_RESUME.
    atomic_bool interrupted;
    // Timestamp of the last write-callback invocation, as mp_time_ns().
    atomic_ullong last_write_ns;
    // Whether the driver has been asked to play (start/set_pause(false)).
    atomic_bool started;
    // Watchdog bookkeeping, all owned by the watchdog thread.
    atomic_bool watchdog_exit;
    atomic_ullong last_start_retry_ns;
    mp_thread watchdog_thread;
    bool watchdog_valid;
};

static void reset_timing(struct priv *p)
{
    p->last_query = 0;
    p->frame_position = 0;
    p->frame_timestamp = 0;
    p->written_frames = 0;
    p->queued_frames = 0;
    p->timestamp_valid = false;
}

// Best-effort state-aware restart of the renderer. Safe to call from any
// thread; tolerates a NULL renderer and already-running streams.
static bool try_restart(struct ao* ao, const char *reason)
{
    struct priv* p = ao->priv;

    if (!p->renderer || atomic_load(&p->user_paused))
        return false;

    OH_AudioStream_State state = AUDIOSTREAM_STATE_INVALID;
    if (OH_AudioRenderer_GetCurrentState(p->renderer, &state) ==
            AUDIOSTREAM_SUCCESS &&
        state == AUDIOSTREAM_STATE_RUNNING)
        return true;

    OH_AudioStream_Result res = OH_AudioRenderer_Start(p->renderer);
    if (res == AUDIOSTREAM_SUCCESS) {
        MP_WARN(ao, "Restarted audio renderer (%s).\n", reason);
        p->last_query = 0;
        return true;
    }
    MP_ERR(ao, "Fail to restart audio renderer (%s): error code %d.\n",
           reason, res);
    return false;
}

static MP_THREAD_VOID watchdog_thread(void *arg)
{
    struct ao* ao = arg;
    struct priv* p = ao->priv;

    while (!atomic_load(&p->watchdog_exit)) {
        mp_sleep_ns(WATCHDOG_TICK_NS);
        if (atomic_load(&p->watchdog_exit))
            break;

        if (!atomic_load(&p->started) || atomic_load(&p->user_paused))
            continue;

        // At EOF the core keeps the stream RUNNING and the write callback is
        // still invoked with silence-padded buffers, so a silent callback
        // uniquely means the audio service stopped pulling data while mpv
        // believes we are playing.
        unsigned long long last = atomic_load(&p->last_write_ns);
        if (!last)
            continue;
        int64_t stall = mp_time_ns() - (int64_t)last;
        if (stall < WATCHDOG_STALL_START)
            continue;

        OH_AudioStream_State state = AUDIOSTREAM_STATE_INVALID;
        bool state_ok = p->renderer &&
            OH_AudioRenderer_GetCurrentState(p->renderer, &state) ==
                AUDIOSTREAM_SUCCESS;

        // A running service with a silent callback is odd, but restarting a
        // running stream is wrong; only give up after a very long outage.
        if (state_ok && state == AUDIOSTREAM_STATE_RUNNING &&
            stall < WATCHDOG_STALL_RELOAD)
            continue;

        // A very long outage means every in-place recovery is exhausted (or
        // impossible): ask the core to rebuild the whole audio output chain.
        // Reload re-creates the renderer, which also re-requests audio focus,
        // so it recovers even when the stream died server-side.
        if (stall >= WATCHDOG_STALL_RELOAD) {
            MP_ERR(ao, "Audio output stalled for %.1fs, requesting AO reload.\n",
                   stall / 1e9);
            ao_request_reload(ao);
            // The core will uninit this AO soon; stop watchdog actions until
            // then by pretending we are paused.
            atomic_store(&p->user_paused, true);
            continue;
        }

        // Otherwise try a throttled in-place restart first: the audio service
        // refuses focus requests from a frozen/backgrounded app, so keep
        // retrying while the app stays in the background; it succeeds right
        // after returning to the foreground.
        unsigned long long last_retry = atomic_load(&p->last_start_retry_ns);
        if (mp_time_ns() - (int64_t)last_retry >= WATCHDOG_RETRY_INTERVAL) {
            atomic_store(&p->last_start_retry_ns, (unsigned long long)mp_time_ns());
            if (try_restart(ao, "watchdog")) {
                // The restart succeeded: the write callback resumes and the
                // stall clears on its own; nothing more to do this tick.
                continue;
            }
        }
    }
    MP_THREAD_RETURN();
}

static void uninit(struct ao* ao)
{
    struct priv* p = ao->priv;

    if (p->watchdog_valid) {
        atomic_store(&p->watchdog_exit, true);
        mp_thread_join(p->watchdog_thread);
        p->watchdog_valid = false;
    }

    if (p->renderer) {
        uint32_t underflows;
        if (OH_AudioRenderer_GetUnderflowCount(p->renderer, &underflows) ==
            AUDIOSTREAM_SUCCESS)
            MP_TRACE(ao, "OHAudio underflows: %u\n", underflows);
        OH_AudioRenderer_Release(p->renderer);
        p->renderer = NULL;
    }

    if (p->builder) {
        OH_AudioStreamBuilder_Destroy(p->builder);
        p->builder = NULL;
    }

    reset_timing(p);
}

static OH_AudioData_Callback_Result audio_on_write_callback(
    OH_AudioRenderer* renderer,
    void* userData,
    void* audioData,
    int32_t audioDataSize)
{
    struct ao* ao = userData;
    struct priv* p = ao->priv;

    atomic_store(&p->last_write_ns, (unsigned long long)mp_time_ns());

    int samples = audioDataSize / ao->sstride;
    int64_t now = mp_time_ns();
    int64_t written = p->written_frames;

    // The timestamp query may involve audio-service IPC. Keep it outside the
    // per-buffer hot path except at the documented >= 200 ms interval.
    if (!p->last_query || now - p->last_query >= MP_TIME_MS_TO_NS(250)) {
        int64_t position, timestamp;
        OH_AudioStream_Result res = OH_AudioRenderer_GetAudioTimestampInfo(
            renderer, &position, &timestamp);
        p->last_query = now;
        if (res == AUDIOSTREAM_SUCCESS && position >= 0 && timestamp > 0 &&
            timestamp <= now && now - timestamp < MP_TIME_S_TO_NS(2))
        {
            // A route change resets position while written frames remain
            // cumulative. Start a new local epoch when that happens.
            if (p->timestamp_valid && position < p->frame_position) {
                p->written_frames = position;
                written = position;
            }
            p->frame_position = position;
            p->frame_timestamp = timestamp;
            p->timestamp_valid = true;
        }
    }

    if (p->timestamp_valid) {
        int64_t elapsed = now - p->frame_timestamp;
        int64_t played = p->frame_position +
            elapsed * ao->samplerate / MP_TIME_S_TO_NS(1);
        played = MPCLAMP(played, 0, written);
        p->queued_frames = written - played;
    } else {
        // Before the hardware clock starts, all previously returned callback
        // buffers are still the best estimate of the queued audio.
        p->queued_frames = MPMIN(written, ao->samplerate / 4);
    }

    int64_t end_time = now +
        MP_TIME_S_TO_NS(samples + p->queued_frames) / ao->samplerate;
    ao_read_data(ao, &audioData, samples, end_time, NULL, true, true);
    p->written_frames += samples;

    return AUDIO_DATA_CALLBACK_RESULT_VALID;
}

// Audio focus was taken away (incoming call, alarm, other app, ...). With
// AUDIOSTREAM_INTERRUPT_FORCE the system has already paused the stream, and
// playback does NOT resume on its own once the focus returns: the resume
// hint has to be handled here, or audio stays silent forever while the video
// keeps playing. Runs on an OHAudio thread.
static void audio_on_interrupt_callback(
    OH_AudioRenderer* renderer,
    void* userData,
    OH_AudioInterrupt_ForceType type,
    OH_AudioInterrupt_Hint hint)
{
    struct ao* ao = userData;
    struct priv* p = ao->priv;

    switch (hint) {
    case AUDIOSTREAM_INTERRUPT_HINT_PAUSE:
    case AUDIOSTREAM_INTERRUPT_HINT_MUTE:
        atomic_store(&p->interrupted, true);
        reset_timing(p);
        MP_WARN(ao, "Audio interrupted by the system (hint %d).\n", (int)hint);
        break;
    case AUDIOSTREAM_INTERRUPT_HINT_STOP:
        // A stop hint is terminal: no resume hint will ever follow. Try to
        // take the focus back immediately (works once the competing stream
        // releases it); if that fails the watchdog keeps retrying.
        atomic_store(&p->interrupted, true);
        reset_timing(p);
        MP_WARN(ao, "Audio stopped by the system (hint %d).\n", (int)hint);
        if (!atomic_load(&p->user_paused))
            try_restart(ao, "interrupt stop");
        break;
    case AUDIOSTREAM_INTERRUPT_HINT_RESUME:
    case AUDIOSTREAM_INTERRUPT_HINT_UNMUTE:
        atomic_store(&p->interrupted, false);
        // Only restart when the player itself is not paused; otherwise the
        // next user-initiated unpause calls set_pause(false) -> Start().
        if (!atomic_load(&p->user_paused)) {
            OH_AudioStream_Result res = OH_AudioRenderer_Start(renderer);
            if (res == AUDIOSTREAM_SUCCESS) {
                MP_VERBOSE(ao, "Resumed after audio interruption.\n");
            } else {
                // Leave the recovery to the watchdog: hint races with the
                // service resuming on its own are common, and a start that
                // fails now may succeed once the service settles.
                MP_ERR(ao, "Fail to resume after interruption: error code %d.\n",
                       res);
            }
        }
        p->last_query = 0;
        break;
    case AUDIOSTREAM_INTERRUPT_HINT_DUCK:
    case AUDIOSTREAM_INTERRUPT_HINT_UNDUCK:
    default:
        // Volume ducking is applied by the system on FORCE events; nothing
        // to do for a movie player.
        break;
    }
}

// Fatal stream errors (audio service crash, ...). Ask the player core to
// tear down and reinitialize the whole AO, mirroring ao_aaudio.c.
static void audio_on_error_callback(
    OH_AudioRenderer* renderer,
    void* userData,
    OH_AudioStream_Result error)
{
    struct ao* ao = userData;

    MP_ERR(ao, "OHAudio stream error: %d, requesting AO reload.\n", (int)error);
    ao_request_reload(ao);
}

// Output device switched (BT, headphones, ...). The frame position restarts
// on the new device, so drop the timing epoch; the write callback rebuilds
// it. When the old device disappeared, some systems leave the stream paused
// - restart it unless the player itself is paused.
static void audio_on_device_change_callback(
    OH_AudioRenderer* renderer,
    void* userData,
    OH_AudioStream_DeviceChangeReason reason)
{
    struct ao* ao = userData;
    struct priv* p = ao->priv;

    MP_VERBOSE(ao, "Output device changed, reason %d.\n", (int)reason);
    reset_timing(p);

    if (reason == REASON_OLD_DEVICE_UNAVAILABLE)
        try_restart(ao, "device change");
}

static int init(struct ao* ao)
{
    struct priv* p = ao->priv;
    OH_AudioStream_Result res = 0;

    res = OH_AudioStreamBuilder_Create(&p->builder, AUDIOSTREAM_TYPE_RENDERER);

    if (res != AUDIOSTREAM_SUCCESS) {
        MP_ERR(ao, "Fail to create audio stream: error code %d.\n", res);
        goto error;
    }

    ao->channels.num = MPCLAMP(ao->channels.num, 1, 16);
    ao->samplerate = MPCLAMP(ao->samplerate, 8000, 192000);

    OH_AudioStreamBuilder_SetChannelCount(p->builder, ao->channels.num);
    OH_AudioStreamBuilder_SetSamplingRate(p->builder, ao->samplerate);
    OH_AudioStreamBuilder_SetEncodingType(p->builder, p->encoding_type);
    OH_AudioStreamBuilder_SetRendererInfo(p->builder, AUDIOSTREAM_USAGE_MOVIE);

    if (af_fmt_is_int(ao->format)) {
        if (af_fmt_to_bytes(ao->format) > 2)
            ao->format = AF_FORMAT_S32;
        else
            ao->format = af_fmt_from_planar(ao->format);
    } else {
        ao->format = AF_FORMAT_FLOAT;
    }

    switch (ao->format) {
    case AF_FORMAT_U8:
        OH_AudioStreamBuilder_SetSampleFormat(p->builder, AUDIOSTREAM_SAMPLE_U8);
        break;
    case AF_FORMAT_S16:
        OH_AudioStreamBuilder_SetSampleFormat(p->builder, AUDIOSTREAM_SAMPLE_S16LE);
        break;
    case AF_FORMAT_S32:
        OH_AudioStreamBuilder_SetSampleFormat(p->builder, AUDIOSTREAM_SAMPLE_S32LE);
        break;
    case AF_FORMAT_FLOAT:
        OH_AudioStreamBuilder_SetSampleFormat(p->builder, AUDIOSTREAM_SAMPLE_F32LE);
        break;
    default:
        OH_AudioStreamBuilder_SetSampleFormat(p->builder, AUDIOSTREAM_SAMPLE_S16LE);
        break;
    }

    OH_AudioRenderer_OnWriteDataCallback callback = audio_on_write_callback;
    OH_AudioStreamBuilder_SetRendererWriteDataCallback(p->builder, callback, ao);
    OH_AudioStreamBuilder_SetRendererInterruptCallback(p->builder,
        audio_on_interrupt_callback, ao);
    OH_AudioStreamBuilder_SetRendererErrorCallback(p->builder,
        audio_on_error_callback, ao);
    OH_AudioStreamBuilder_SetRendererOutputDeviceChangeCallback(p->builder,
        audio_on_device_change_callback, ao);

    res = OH_AudioStreamBuilder_GenerateRenderer(p->builder, &p->renderer);

    if (res != AUDIOSTREAM_SUCCESS) {
        MP_ERR(ao, "Fail to generate audio renderer: error code %d.\n", res);
        goto error;
    }

    atomic_init(&p->user_paused, false);
    atomic_init(&p->interrupted, false);
    atomic_init(&p->last_write_ns, 0);
    atomic_init(&p->started, false);
    atomic_init(&p->watchdog_exit, false);
    atomic_init(&p->last_start_retry_ns, 0);
    reset_timing(p);

    if (mp_thread_create(&p->watchdog_thread, watchdog_thread, ao) != 0) {
        MP_ERR(ao, "Fail to create audio watchdog thread.\n");
        goto error;
    }
    p->watchdog_valid = true;

    return 0;
error:
    uninit(ao);
    return -1;
}

static int control(struct ao *ao, enum aocontrol cmd, void *arg)
{
    struct priv *p = ao->priv;
    float volume;
    float *vol = arg;
    OH_AudioStream_Result res = 0;

    if (!p->renderer)
        return CONTROL_ERROR;

    switch (cmd) {
    case AOCONTROL_GET_VOLUME:
        res = OH_AudioRenderer_GetVolume(p->renderer, &volume);
        if (res != AUDIOSTREAM_SUCCESS) {
            MP_ERR(ao, "Fail to get volume, error code %d.\n", res);
            return CONTROL_ERROR;
        }
        *vol = volume * 100;
        break;
    case AOCONTROL_SET_VOLUME:
        res = OH_AudioRenderer_SetVolume(p->renderer, *vol / 100);
        if (res != AUDIOSTREAM_SUCCESS) {
            MP_ERR(ao, "Fail to set volume, error code %d.\n", res);
            return CONTROL_ERROR;
        }
        break;
    default:
        return CONTROL_UNKNOWN;
    }
    return CONTROL_OK;
}

static void reset(struct ao* ao)
{
    struct priv* p = ao->priv;

    atomic_store(&p->user_paused, true);
    atomic_store(&p->started, false);
    if (p->renderer) {
        OH_AudioRenderer_Pause(p->renderer);
        OH_AudioRenderer_Flush(p->renderer);
    }
    reset_timing(p);
}

static bool set_pause(struct ao *ao, bool paused)
{
    struct priv* p = ao->priv;

    atomic_store(&p->user_paused, paused);
    if (paused) {
        atomic_store(&p->started, false);
        if (p->renderer)
            OH_AudioRenderer_Pause(p->renderer);
    } else {
        atomic_store(&p->started, true);
        atomic_store(&p->last_write_ns, 0);
        p->last_query = 0;
        if (p->renderer) {
            OH_AudioStream_Result res = OH_AudioRenderer_Start(p->renderer);
            if (res != AUDIOSTREAM_SUCCESS)
                MP_ERR(ao, "Fail to start audio renderer: error code %d.\n",
                       res);
        }
    }
    return true;
}

static void start(struct ao* ao)
{
    struct priv* p = ao->priv;

    atomic_store(&p->user_paused, false);
    atomic_store(&p->started, true);
    atomic_store(&p->last_write_ns, 0);
    p->last_query = 0;
    if (p->renderer) {
        OH_AudioStream_Result res = OH_AudioRenderer_Start(p->renderer);
        if (res != AUDIOSTREAM_SUCCESS)
            MP_ERR(ao, "Fail to start audio renderer: error code %d.\n", res);
    }
}

#define OPT_BASE_STRUCT struct priv

const struct ao_driver audio_out_ohaudio = {
    .description = "OHAudio audio output",
    .name      = "ohaudio",
    .init      = init,
    .control   = control,
    .uninit    = uninit,
    .reset     = reset,
    .set_pause = set_pause,
    .start     = start,

    .priv_size = sizeof(struct priv),
    .priv_defaults = &(const struct priv) {
        .encoding_type = AUDIOSTREAM_ENCODING_TYPE_RAW,
    },
    .options = (const struct m_option[]) {
        {"encoding_type", OPT_INT(encoding_type),
            M_RANGE(0, 2)},
        {0}
    },
    .options_prefix = "ohaudio",
};
