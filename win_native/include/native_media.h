/*
 * C interface between uxplay.cpp (and the C AirPlay core) and the
 * Windows-native media engine implemented as C++26 modules in win_native/src.
 *
 * Threading: every function may be called from any thread.  The engine owns
 * its own UI, render, decode and audio threads; calls only enqueue work or
 * update state and never block on rendering.
 *
 * Time base: all *_time_ns arguments are on the server "local time" timeline
 * (raop_ntp_get_local_time(), QueryPerformanceCounter in nanoseconds).
 */
#ifndef UXPLAY_NATIVE_MEDIA_H
#define UXPLAY_NATIVE_MEDIA_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* numeric values match lib/logger.h (LOGGER_ERR, LOGGER_WARNING, ...) */
typedef enum native_media_log_level {
    NATIVE_MEDIA_LOG_ERROR = 3,
    NATIVE_MEDIA_LOG_WARNING = 4,
    NATIVE_MEDIA_LOG_INFO = 6,
    NATIVE_MEDIA_LOG_DEBUG = 7
} native_media_log_level;

typedef enum native_media_event {
    NATIVE_MEDIA_EVENT_WINDOW_CLOSED = 1, /* user closed the video window: drop the AirPlay client */
    NATIVE_MEDIA_EVENT_USB_STARTED = 2,   /* USB (QuickTime) mirroring became the active source */
    NATIVE_MEDIA_EVENT_USB_STOPPED = 3,   /* USB mirroring ended (unplugged, stopped, or failed) */
    NATIVE_MEDIA_EVENT_EXIT_REQUESTED = 4 /* "Exit" chosen in the notification-area menu */
} native_media_event;

typedef void (*native_media_log_callback)(int level, const char *message);
typedef void (*native_media_event_callback)(native_media_event event, void *user_data);

typedef struct native_media_settings {
    const char *window_title;          /* UTF-8 */
    bool start_fullscreen;
    bool timestamp_sync;               /* false: present AirPlay mirror media on arrival ("-vsync no") */
    double audio_offset_ms;            /* "-vsync x": positive delays audio relative to video */
    bool usb_mirroring;                /* legacy QuickTime USB capture (-usb; iOS 15 and earlier) */
    bool keep_window_on_stop;          /* "-nc": keep the last frame visible after the client stops */
    int log_level;                     /* messages above this level are discarded */
    int gpu_preference;                /* 0 = display-owning GPU (default), 1 = integrated, 2 = discrete */
    bool tray_icon;                    /* notification-area icon with status and menu */
    const char *log_path;              /* UTF-8 path opened by "Open log" (may be NULL) */
    native_media_log_callback log_callback;
    native_media_event_callback event_callback;
    void *event_user_data;
} native_media_settings;

typedef struct native_media_display_info {
    int width;
    int height;
    int refresh_hz;
    int hevc_hardware; /* 1 if the selected GPU decodes HEVC Main in hardware */
} native_media_display_info;

/* returns 0 on success; on failure the reason has been logged */
int native_media_start(const native_media_settings *settings);
void native_media_stop(void);

/* geometry and refresh rate of the monitor that will show the video */
bool native_media_query_display(native_media_display_info *info);

/* AirPlay mirror video.  codec: 0 = H.264, 1 = H.265.  Frames are Annex-B
 * access units (start-code delimited, parameter sets prepended on change). */
void native_media_airplay_video_configure(int codec, int width, int height);
void native_media_airplay_video_frame(const unsigned char *data, int length,
                                      uint64_t local_time_ns, uint64_t remote_time_ns);
void native_media_airplay_video_pause(void);
void native_media_airplay_video_resume(void);
void native_media_airplay_video_stop(void);

/* AirPlay audio.  compression_type (AirPlay "ct"): 1 = PCM, 2 = ALAC,
 * 4 = AAC-LC, 8 = AAC-ELD.  timestamp_sync selects timestamp-scheduled
 * playback (mirror "-vsync", audio-only "-async") versus play-on-arrival. */
void native_media_airplay_audio_configure(unsigned char compression_type, bool timestamp_sync,
                                          double audio_offset_ms);
void native_media_airplay_audio_packet(const unsigned char *data, int length,
                                       uint64_t local_time_ns, uint64_t remote_time_ns);
void native_media_airplay_audio_stop(void);

/* linear gain in [0, 10]; applies to the AirPlay source */
void native_media_set_volume(double linear_gain);

/* Server-side local address the current AirPlay client connected to: the
 * window shows "USB" when it is the iPhone USB link, "Wi-Fi" otherwise. */
void native_media_set_airplay_local_address(const char *local_address);

/* balloon notification from the notification-area icon (e.g. AirPlay PIN) */
void native_media_notify(const char *title, const char *text);

#ifdef __cplusplus
}
#endif

#endif /* UXPLAY_NATIVE_MEDIA_H */
