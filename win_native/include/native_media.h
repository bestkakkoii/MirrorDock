// SPDX-License-Identifier: MIT
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
    NATIVE_MEDIA_EVENT_EXIT_REQUESTED = 4, /* "Exit" chosen in the notification-area menu */
    NATIVE_MEDIA_EVENT_LINK_PREFERENCE_CHANGED = 5, /* "USB first" / "Wi-Fi only" chosen: see
                                                       native_media_prefer_usb_link() */
    NATIVE_MEDIA_EVENT_PERFORMANCE_MODE_CHANGED = 6, /* see native_media_performance_mode() */
    NATIVE_MEDIA_EVENT_DISPLAY_CHANGED = 7, /* monitor attached/detached or its mode changed: query again */
    NATIVE_MEDIA_EVENT_START_FAILED = 8,    /* the engine could not start: native_media_failure_text() */
    NATIVE_MEDIA_EVENT_DEVICE_LOST = 9,     /* the Direct3D device was lost: restart the program */
    NATIVE_MEDIA_EVENT_SESSION_ENDING = 10  /* Windows shuts down / sign-out: stop the network services,
                                               then call native_media_network_stopped() */
} native_media_event;

/* what the receiver optimizes for (uxplay_media_types: performance_mode) */
typedef enum native_media_mode {
    NATIVE_MEDIA_MODE_QUALITY = 0,
    NATIVE_MEDIA_MODE_BALANCED = 1,
    NATIVE_MEDIA_MODE_PERFORMANCE = 2
} native_media_mode;

typedef void (*native_media_log_callback)(int level, const char *message);
typedef void (*native_media_event_callback)(native_media_event event, void *user_data);

typedef struct native_media_settings {
    const char *window_title;          /* UTF-8 */
    bool start_fullscreen;
    bool timestamp_sync;               /* false: present AirPlay mirror media on arrival ("-vsync no") */
    double audio_offset_ms;            /* "-vsync x": positive delays audio relative to video */
    bool keep_window_on_stop;          /* "-nc": keep the last frame visible after the client stops */
    int log_level;                     /* messages above this level are discarded */
    int gpu_preference;                /* 0 = display-owning GPU (default), 1 = integrated, 2 = discrete */
    int ui_language;                   /* interface text: 0 = Windows display language (default),
                                          1 = Traditional Chinese, 2 = Simplified Chinese, 3 = English,
                                          4 = Japanese, 5 = Korean, 6 = Spanish, 7 = German, 8 = French,
                                          9 = Vietnamese, 10 = Thai, 11 = Russian */
    int performance_mode;              /* native_media_mode, or -1 for the choice saved by the user
                                          (balanced when none is saved) */
    int crop_mode;                     /* 0 = show the whole picture, 1 = cut black bars off
                                          automatically, -1 = the choice saved by the user (0 when
                                          none is saved) */
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
    int h264_hardware; /* 1 if it decodes H.264 in hardware */
} native_media_display_info;

/* Starts the engine on a background thread and returns 0 at once; a failure
 * is reported later through NATIVE_MEDIA_EVENT_START_FAILED (the reason is
 * logged and available from native_media_failure_text()). */
int native_media_start(const native_media_settings *settings);
void native_media_stop(void);
/* UTF-8 reason of a start failure ("" when there is none); valid until
 * native_media_stop() */
const char *native_media_failure_text(void);

/* The mode in force (native_media_mode): the saved or requested one, then
 * whatever the user picks in the menu (NATIVE_MEDIA_EVENT_PERFORMANCE_MODE_CHANGED). */
int native_media_performance_mode(void);
/* the host recomputed the stream size for the new mode; applies_next_connection:
 * a client is connected and keeps its current stream until it reconnects */
void native_media_performance_mode_applied(bool applies_next_connection);

/* Single instance: asks an already running instance with the same receiver
 * name to show itself.  Returns the number of instances that were notified
 * (0: none is running, or it has no window yet). */
int native_media_activate_running_instance(const char *receiver_name);

/* The host stopped its network services after NATIVE_MEDIA_EVENT_SESSION_ENDING. */
void native_media_network_stopped(void);

/* Modal error box for a failure the user would otherwise never see (the
 * program has no console): title from the interface language, text UTF-8. */
void native_media_show_start_failure(const char *reason, const char *log_path);

/* Crash record: an unhandled exception or std::terminate writes the exception
 * code, the faulting address as module+offset and a minidump next to the log
 * (directory UTF-8) before the process ends.  Call once, first thing in main. */
void native_media_install_crash_report(const char *directory);

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
 * window shows "USB" when it is the iPhone USB link, "Wi-Fi" otherwise.
 * Returns 1 when the client is on the USB link. */
int native_media_set_airplay_local_address(const char *local_address);

/* Link preference, persisted per user (prefer_usb_link in the settings key):
 * true = AirPlay over the USB cable whenever the iPhone link is up (default),
 * false = Wi-Fi only (the cable can stay plugged in to charge).  Valid from
 * native_media_start() on. */
bool native_media_prefer_usb_link(void);

/* whether the iPhone USB network link is up right now */
bool native_media_usb_link_up(void);
/* True when local_address (text form, IPv4 or IPv6) belongs to the PC side of
 * the iPhone USB link.  No side effects; callable from any thread. */
bool native_media_is_usb_link_address(const char *local_address);

/* the host applied a link-preference change; session_ended: it ended the
 * current AirPlay session so the iPhone reconnects on the chosen link */
void native_media_link_preference_applied(bool session_ended);

/* balloon notification from the notification-area icon */
void native_media_notify(const char *title, const char *text);
/* shows the pairing PIN the user has to type on the iPhone */
void native_media_notify_pin(const char *pin);

#ifdef __cplusplus
}
#endif

#endif /* UXPLAY_NATIVE_MEDIA_H */
