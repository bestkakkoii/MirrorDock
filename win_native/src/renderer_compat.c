/*
 * UxPlay's renderer API (renderers/video_renderer.h, audio_renderer.h,
 * mux_renderer.h) implemented on the Windows-native media engine, so
 * uxplay.cpp keeps its control flow unchanged.
 *
 * Mirror video/audio go to native_media_*.  Features that only exist on top
 * of GStreamer playbin/muxers (HLS playback, cover-art rendering, MP4
 * recording, RTP re-streaming) are not part of this build: uxplay.cpp
 * rejects their options at startup, and these entry points are inert.
 */
#include <glib.h>

#include "../../renderers/video_renderer.h"
#include "../../renderers/audio_renderer.h"
#include "../../renderers/mux_renderer.h"
#include "../include/native_media.h"

static const bool *compat_audio_sync = NULL;
static const bool *compat_video_sync = NULL;

bool gstreamer_init(void)
{
    return true;
}

bool gstreamer_decoder_check(const char *decoder)
{
    (void) decoder;
    return false;
}

void video_renderer_set_window_handle(uintptr_t handle)
{
    (void) handle;
}

void video_renderer_init(logger_t *logger, const char *server_name, videoflip_t videoflip[2], const char *parser,
                         const char *rtp_pipeline, const char *decoder, const char *converter, const char *videosink,
                         const char *videosink_options, bool initial_fullscreen, bool video_sync, bool h265_support,
                         bool coverart_support, guint playbin_version, const char *uri)
{
    (void) logger;
    (void) server_name;
    (void) videoflip;
    (void) parser;
    (void) rtp_pipeline;
    (void) decoder;
    (void) converter;
    (void) videosink;
    (void) videosink_options;
    (void) initial_fullscreen;
    (void) video_sync;
    (void) h265_support;
    (void) coverart_support;
    (void) playbin_version;
    (void) uri;
}

void video_renderer_start(void)
{
}

void video_renderer_stop(void)
{
    native_media_airplay_video_stop();
}

void video_renderer_destroy(void)
{
    native_media_airplay_video_stop();
}

void video_renderer_set_device_model(const char *model, const char *name)
{
    (void) model;
    (void) name;
}

void video_renderer_set_track_metadata(const char *title, const char *artist, const char *album)
{
    (void) title;
    (void) artist;
    (void) album;
}

void video_renderer_pause(void)
{
    native_media_airplay_video_pause();
}

void video_renderer_resume(void)
{
    native_media_airplay_video_resume();
}

void video_renderer_hls_ready(void)
{
}

void video_renderer_seek(float position)
{
    (void) position;
}

void video_renderer_set_start(float position)
{
    (void) position;
}

int video_renderer_cycle(void)
{
    return -1;
}

bool video_renderer_is_paused(void)
{
    return false;
}

bool video_renderer_eos_watch(void)
{
    return false;
}

uint64_t video_renderer_render_buffer(unsigned char *data, int *data_len, int *nal_count, uint64_t *ntp_time)
{
    (void) nal_count;
    native_media_airplay_video_frame(data, *data_len, *ntp_time, 0);
    return 0;
}

void video_renderer_display_jpeg(const void *data, int *data_len)
{
    (void) data;
    (void) data_len;
}

void video_renderer_flush(void)
{
}

unsigned int video_renderer_listen(void *loop, int id)
{
    (void) loop;
    (void) id;
    return 0; /* window events reach the main loop through native_media events */
}

bool waiting_for_x11_window(void)
{
    return false;
}

bool video_get_playback_info(double *duration, double *position, double *seek_start, double *seek_duration,
                             float *rate, bool *buffer_empty, bool *buffer_full)
{
    *duration = 0.0;
    *position = -1.0;
    *seek_start = 0.0;
    *seek_duration = 0.0;
    *rate = 0.0f;
    *buffer_empty = true;
    *buffer_full = false;
    return true;
}

int video_renderer_choose_codec_and_size(bool video_is_jpeg, bool video_is_h265, video_geometry_t *video_geometry)
{
    if (video_is_jpeg)
    {
        return -1;
    }
    native_media_airplay_video_configure(video_is_h265 ? 1 : 0, video_geometry->width, video_geometry->height);
    return 0;
}

void video_renderer_hls_set_volume(double volume)
{
    (void) volume;
}

void audio_renderer_init(logger_t *logger, const char *audiosink, const bool *audio_sync, const bool *video_sync,
                         const char *artp_pipeline)
{
    (void) logger;
    (void) audiosink;
    (void) artp_pipeline;
    compat_audio_sync = audio_sync;
    compat_video_sync = video_sync;
}

void audio_renderer_start(unsigned char *compression_type)
{
    bool const alac = *compression_type == 2;
    bool const synchronized = alac ? (compat_audio_sync && *compat_audio_sync) : (compat_video_sync && *compat_video_sync);
    native_media_airplay_audio_configure(*compression_type, synchronized, 0.0);
}

void audio_renderer_stop(void)
{
    native_media_airplay_audio_stop();
}

void audio_renderer_render_buffer(unsigned char *data, int *data_len, unsigned short *seqnum, uint64_t *ntp_time)
{
    (void) seqnum;
    native_media_airplay_audio_packet(data, *data_len, *ntp_time, 0);
}

void audio_renderer_set_volume(double volume)
{
    native_media_set_volume(volume);
}

void audio_renderer_flush(void)
{
}

void audio_renderer_destroy(void)
{
    native_media_airplay_audio_stop();
}

unsigned int audio_renderer_listen(void *loop, int id)
{
    (void) loop;
    (void) id;
    return 0;
}

void mux_renderer_init(logger_t *logger, const char *filename, bool use_audio, bool use_video)
{
    (void) logger;
    (void) filename;
    (void) use_audio;
    (void) use_video;
}

void mux_renderer_choose_audio_codec(unsigned char audio_ct)
{
    (void) audio_ct;
}

void mux_renderer_choose_video_codec(bool is_h265)
{
    (void) is_h265;
}

void mux_renderer_push_video(unsigned char *data, int data_len, uint64_t ntp_time)
{
    (void) data;
    (void) data_len;
    (void) ntp_time;
}

void mux_renderer_push_audio(unsigned char *data, int data_len, uint64_t ntp_time)
{
    (void) data;
    (void) data_len;
    (void) ntp_time;
}

void mux_renderer_stop(void)
{
}

void mux_renderer_destroy(void)
{
}
