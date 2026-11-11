/**
 * RPiPlay - An open-source AirPlay mirroring server for Raspberry Pi
 * Copyright (C) 2019 Florian Draschbacher
 * Modified extensively to become 
 * UxPlay - An open-souce AirPlay mirroring server.
 * Modifications Copyright (C) 2021-23 F. Duncanh
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301  USA
 */

#include <stddef.h>
#include <cstring>
#include <unistd.h>
#include <ctype.h>
#include <string>
#include <atomic>
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <vector>
#include <fstream>
#include <sstream>
#include <sys/stat.h>
#include <cstdio>
#include <stdarg.h>
#include <math.h>
#include <inttypes.h>

#include <winsock2.h>
#include <iphlpapi.h>
#include <sddl.h>
#include <io.h>
#include <fcntl.h>

#include "lib/raop.h"
#include "lib/stream.h"
#include "lib/logger.h"
#include "lib/crypto.h"
/* Windows-native media engine (Direct3D 11 + FFmpeg D3D11VA video, WASAPI
 * audio, window, notification area, USB link); see README_AI.md */
#include "win_native/include/native_media.h"


#include "win_native/include/uxplay_version.h"

/* version of the UxPlay protocol core this program derives from */
#define VERSION UXPLAY_CORE_VERSION

#define SECOND_IN_USECS 1000000
#define DEFAULT_NAME UXPLAY_PRODUCT_NAME
#define DEFAULT_DEBUG_LOG false
#define LOWEST_ALLOWED_PORT 1024
#define HIGHEST_PORT 65535
#define MISSED_FEEDBACK_LIMIT 15
#define MIN_PASSWORD_LENGTH 4

static const char *appname = DEFAULT_NAME;
static std::string server_name = appname;
static dnssd_t *dnssd = NULL;
static raop_t *raop = NULL;
static bool audio_sync = false;
static bool video_sync = true;
static int64_t audio_delay_alac = 0;
static int64_t audio_delay_aac = 0;
/* session-cycle flags: written by RAOP threads and by main-loop tasks, read
   by main() between cycles */
static std::atomic<bool> relaunch_video{false};
/* changed by RAOP connection threads, read by the main loop */
static std::atomic<unsigned int> open_connections{0};
/* Windows: the current AirPlay client is on the iPhone USB link (written by
   the RAOP connection thread, read by the main loop) */
static std::atomic<bool> current_connection_over_usb{false};
static bool use_video = true;
static int  audiodelay = -1;
static bool use_audio = true;
static bool new_window_closing_behavior = true;
static std::atomic<bool> close_window{false};
static std::atomic<bool> full_video_reset{true};
/* a new client connection replaced the old one inside the AirPlay core
   ("nohold", or the hand-over of a client that connects again): the relaunch
   that follows must keep that new connection */
static std::atomic<bool> connection_handed_over{false};
static bool show_client_FPS_data = false;
static FILE *video_dumpfile = NULL;
static std::string video_dumpfile_name = "videodump";
static int video_dump_limit = 0;
static int video_dumpfile_count = 0;
static int video_dump_count = 0;
static bool dump_video = false;
static unsigned char mark[] = { 0x00, 0x00, 0x00, 0x01 };
static FILE *audio_dumpfile = NULL;
static std::string audio_dumpfile_name = "audiodump";
static int audio_dump_limit = 0;
static int audio_dumpfile_count = 0;
static int audio_dump_count = 0;
static bool dump_audio = false;
static unsigned char audio_type = 0x00;
static unsigned char previous_audio_type = 0x00;
static bool fullscreen = false;
static std::string coverart_filename = "";
static std::string metadata_filename = "";
static bool do_append_hostname = true;
static bool use_random_hw_addr = false;
static unsigned short display[5] = {0}, tcp[3] = {0}, udp[3] = {0};
static bool debug_log = DEFAULT_DEBUG_LOG;
static bool suppress_packet_debug_data = false;
static int log_level = LOGGER_INFO;
static int nohold = 0;
static bool nofreeze = false;
static unsigned short raop_port;
static unsigned short airplay_port;
static std::vector<std::string> allowed_clients;
static std::vector<std::string> blocked_clients;
static bool restrict_clients;
static bool setup_legacy_pairing = false;
static unsigned char pin_pw = 0;  /* 0: no client access control; 1: onscreen pin ; 2: require password (same password for all clients)  3: random pw*/
static std::string password = "";
static unsigned int min_password_length = MIN_PASSWORD_LENGTH;
static unsigned short pin = 0;
static std::string keyfile = "";
static std::string mac_address = "";
static std::string dacpfile = "";
static bool registration_list = false;
static std::string pairing_register = "";
static std::vector <std::string> registered_keys;
static double db_low = -30.0;
static double db_high = 0.0;
static bool taper_volume = false;
static double initial_volume = 0.0;
static double current_volume = 0.0;
static bool h265_support = false;
static bool h265_user_requested = false;  /* explicit "-h265": offer H.265 even if the GPU lacks HEVC hardware decode */
static unsigned int missed_feedback_limit = MISSED_FEEDBACK_LIMIT;
/* written by the RAOP feedback handler, read by the main loop's 1 s timer */
static std::atomic<unsigned int> missed_feedback{0};
static std::atomic<bool> reset_httpd{false};
/* exit was requested (menu, Ctrl+C, shutdown): never cleared, so a reset
   that arrives at the same time cannot turn the exit into a relaunch */
static std::atomic<bool> exit_requested{false};
/* the Direct3D device was lost: start the program again after the shutdown */
static std::atomic<bool> restart_after_exit{false};
static bool force_h264_only = false;    /* "-h264": do not offer H.265 to the client */
static int gpu_preference = 0;          /* "-gpu auto|intel|nvidia" */
static int ui_language_choice = 0;      /* "-ui": 0 auto, 1 zh-TW, 2 zh-CN, 3 en, 4 ja, 5 ko, 6 es, 7 de, 8 fr,
                                           9 vi, 10 th (native_media_settings) */
static int mode_choice = -1;            /* "-mode": native_media_mode, -1 = the choice saved by the menu */
static int crop_choice = -1;            /* "-crop": 0 off, 1 auto, -1 = the choice saved by the menu */
static bool user_set_display = false;   /* "-s": the stream size is the user's, not the mode's */
static bool user_set_fps = false;       /* "-fps" */
static HANDLE instance_mutex = NULL;    /* one receiver per name */
static bool instance_claimed = false;   /* this process is the instance of its name: uxplay.log is its own */
static HANDLE shutdown_complete_event = NULL;


/* logging */

/* The native build is a GUI-subsystem program (a console window costs ~1.2 s
 * to appear on Windows 11).  Started from a terminal, or with its output
 * redirected, the log goes there.  Otherwise it goes to
 * %LOCALAPPDATA%\MirrorDock\mirror_dock.log ("Open log file" in the menu):
 * the log of the previous run is kept as mirror_dock.previous.log, and the
 * file starts over when it reaches LOG_FILE_LIMIT.  Until open_log_file() is called (options
 * parsed, single-instance check passed) the lines are held in memory, so a
 * second start never touches the log of the instance that is running.
 * Every line carries the local time; all sources (this file, lib/, the media
 * engine) pass through log() and its one lock. */
#define LOG_FILE_LIMIT (8u * 1024u * 1024u)
#define PENDING_LOG_LIMIT (1024u * 1024u)

static std::string native_log_directory;  /* %LOCALAPPDATA%\MirrorDock ("" when unavailable) */
static std::string native_log_path;       /* the log file, once it is open */
static std::mutex log_mutex;
static bool log_to_file = false;          /* no terminal, no redirection: the log file is used */
static bool log_file_open = false;
static std::string pending_log_lines;     /* written when the log file opens */
static size_t log_file_bytes = 0;
static std::string last_error_line;       /* last ERROR-level message (start-up failure dialog) */
/* The log is <directory>\<log_base_name>.log.  When that file is being
 * written by another running instance, this one changes to
 * log_alternate_name (set by main() from the receiver name). */
static std::string log_base_name = UXPLAY_PRODUCT_FILE_NAME;
static std::string log_alternate_name;

/* opens (or starts over) the log file; log_mutex held */
static bool reopen_log_file_locked(void) {
    if (native_log_directory.empty()) {
        return false;
    }
    if (log_file_open) {
        /* release the file so it can be renamed */
        FILE *stream = NULL;
        freopen_s(&stream, "NUL", "w", stdout);
        freopen_s(&stream, "NUL", "w", stderr);
        log_file_open = false;
    }
    std::string in_use;
    for (int choice = 0; choice < 2; choice++) {
        std::string path = native_log_directory + "\\" + log_base_name + ".log";
        std::string previous = native_log_directory + "\\" + log_base_name + ".previous.log";
        /* The file may still be open in an instance that was ended a moment
           ago (this start took its place): wait up to a second for it rather
           than truncate a log that could not be kept. */
        DWORD keep_error = ERROR_SUCCESS;
        for (int attempt = 0; attempt < 10; attempt++) {
            keep_error = MoveFileExA(path.c_str(), previous.c_str(), MOVEFILE_REPLACE_EXISTING) ? ERROR_SUCCESS
                                                                                                : GetLastError();
            if (keep_error != ERROR_SHARING_VIOLATION) {
                break;
            }
            Sleep(100);
        }
        if (keep_error == ERROR_SHARING_VIOLATION && choice == 0 && !log_alternate_name.empty() &&
            log_alternate_name != log_base_name) {
            /* Another instance is writing this file - a receiver with another
               name, or an older version that takes no part in the
               single-instance check.  Its log is left alone: this instance
               uses a file of its own, also for later rotations. */
            in_use = path;
            log_base_name = log_alternate_name;
            continue;
        }
        /* plain freopen opens the file shared (freopen_s denies other readers),
         * so "Open log file" can show it while the program runs */
        if (freopen(path.c_str(), "w", stdout) == NULL) {
            return false;
        }
        setvbuf(stdout, NULL, _IONBF, 0);
        _dup2(_fileno(stdout), _fileno(stderr));
        native_log_path = path;
        log_file_open = true;
        log_file_bytes = 0;
        if (!in_use.empty()) {
            fprintf(stdout, "*** WARNING: %s is in use by another running instance; this instance logs here\n",
                    in_use.c_str());
        }
        if (keep_error != ERROR_SUCCESS && keep_error != ERROR_FILE_NOT_FOUND) {
            fprintf(stdout, "*** WARNING: the previous log could not be kept as %s (Windows error %lu)\n",
                    previous.c_str(), (unsigned long) keep_error);
        }
        return true;
    }
    return false;
}

/* writes one finished line; log_mutex held */
static void write_log_line_locked(const std::string &line) {
    if (log_to_file && !log_file_open) {
        if (pending_log_lines.size() + line.size() <= PENDING_LOG_LIMIT) {
            pending_log_lines += line;
        }
        return;
    }
    fputs(line.c_str(), stdout);
    if (log_file_open) {
        log_file_bytes += line.size();
        if (log_file_bytes > LOG_FILE_LIMIT && reopen_log_file_locked()) {
            fputs("the log reached its size limit and starts over; the earlier part is the .previous.log file "
                  "next to it\n",
                  stdout);
        }
    }
}

static void log(int level, const char* format, ...) {
    if (level > log_level) return;
    char text[4352];
    va_list vargs;
    va_start(vargs, format);
    int length = vsnprintf(text, sizeof(text), format, vargs);
    va_end(vargs);
    if (length < 0) {
        snprintf(text, sizeof(text), "(a log message could not be formatted: \"%.200s\")", format);
    }
    SYSTEMTIME now;
    GetLocalTime(&now);
    char prefix[64];
    snprintf(prefix, sizeof(prefix), "%02u-%02u %02u:%02u:%02u.%03u %s", now.wMonth, now.wDay, now.wHour,
             now.wMinute, now.wSecond, now.wMilliseconds,
             level <= LOGGER_ERR ? "*** ERROR: " : (level == LOGGER_WARNING ? "*** WARNING: " : ""));
    std::string line(prefix);
    line += text;
    line += '\n';
    std::lock_guard<std::mutex> lock(log_mutex);
    if (level <= LOGGER_ERR) {
        last_error_line = text;
    }
    write_log_line_locked(line);
}

/* Decides where the log goes (see above).  The log directory is prepared in
 * every case: the crash record is written there too. */
static void native_setup_output(void) {
    const char *local_app_data = getenv("LOCALAPPDATA");
    if (local_app_data) {
        native_log_directory = std::string(local_app_data) + "\\" UXPLAY_PRODUCT_NAME;
        /* The folder the program used under its earlier name is taken over
           once.  While an earlier version still runs, its open log keeps the
           folder where it is: this version then starts a folder of its own. */
        std::string const legacy_directory = std::string(local_app_data) + "\\" UXPLAY_LEGACY_PRODUCT_NAME;
        if (GetFileAttributesA(native_log_directory.c_str()) == INVALID_FILE_ATTRIBUTES &&
            GetFileAttributesA(legacy_directory.c_str()) != INVALID_FILE_ATTRIBUTES) {
            MoveFileExA(legacy_directory.c_str(), native_log_directory.c_str(), 0);
        }
        if (!CreateDirectoryA(native_log_directory.c_str(), NULL) && GetLastError() != ERROR_ALREADY_EXISTS) {
            native_log_directory.erase();
        }
    }
    /* output redirected by the parent (mirror_dock.exe > file, pipes): keep it */
    HANDLE inherited = GetStdHandle(STD_OUTPUT_HANDLE);
    if (inherited != NULL && inherited != INVALID_HANDLE_VALUE && _fileno(stdout) >= 0) {
        DWORD type = GetFileType(inherited);
        if (type == FILE_TYPE_DISK || type == FILE_TYPE_PIPE) {
            setvbuf(stdout, NULL, _IONBF, 0);
            setvbuf(stderr, NULL, _IONBF, 0);
            return;
        }
    }
    if (AttachConsole(ATTACH_PARENT_PROCESS)) {
        FILE *stream = NULL;
        freopen_s(&stream, "CONOUT$", "w", stdout);
        freopen_s(&stream, "CONOUT$", "w", stderr);
        setvbuf(stdout, NULL, _IONBF, 0);
        setvbuf(stderr, NULL, _IONBF, 0);
        return;
    }
    log_to_file = true;
}

/* The lines of a start that failed before it became the instance of its
 * name, in a file of their own; returns its path ("" when it cannot be
 * written). */
static std::string write_start_failure_log(void) {
    std::lock_guard<std::mutex> lock(log_mutex);
    if (native_log_directory.empty()) {
        return "";
    }
    std::string path = native_log_directory + "\\" UXPLAY_PRODUCT_FILE_NAME ".start-failure.log";
    FILE *file = fopen(path.c_str(), "w");
    if (file == NULL) {
        return "";
    }
    fputs(pending_log_lines.c_str(), file);
    fclose(file);
    return path;
}

/* Lets go of the log file at exit, before the single-instance mutex is
 * released: the next start can then keep it as uxplay.previous.log instead of
 * truncating a file that is still open. */
static void close_log_file(void) {
    std::lock_guard<std::mutex> lock(log_mutex);
    if (!log_file_open) {
        return;
    }
    FILE *stream = NULL;
    freopen_s(&stream, "NUL", "w", stdout);
    freopen_s(&stream, "NUL", "w", stderr);
    log_file_open = false;
}

/* Starts writing the log file (no-op with a terminal or redirected output). */
static void open_log_file(void) {
    std::lock_guard<std::mutex> lock(log_mutex);
    if (!log_to_file || log_file_open) {
        return;
    }
    if (!reopen_log_file_locked()) {
        /* nowhere to write: drop what was held instead of growing */
        pending_log_lines.clear();
        return;
    }
    fputs(pending_log_lines.c_str(), stdout);
    log_file_bytes += pending_log_lines.size();
    pending_log_lines.clear();
}

#define LOGD(...) log(LOGGER_DEBUG, __VA_ARGS__)
#define LOGI(...) log(LOGGER_INFO, __VA_ARGS__)
#define LOGW(...) log(LOGGER_WARNING, __VA_ARGS__)
#define LOGE(...) log(LOGGER_ERR, __VA_ARGS__)

/* A failure that ends the start: without a terminal nobody would see it (the
 * program has no console and, at this point, no window), so the reason is
 * also shown in a dialog that names the log file. */
static std::string startup_error_text;

static void startup_message(const char *format, ...) {
    char text[2048];
    va_list vargs;
    va_start(vargs, format);
    int length = vsnprintf(text, sizeof(text), format, vargs);
    va_end(vargs);
    if (length < 0) {
        snprintf(text, sizeof(text), "start-up error (message format \"%.200s\")", format);
    }
    size_t used = strlen(text);
    while (used > 0 && (text[used - 1] == '\n' || text[used - 1] == '\r')) {
        text[--used] = '\0';
    }
    startup_error_text = text;
    LOGE("%s", text);
}

static void report_startup_failure(void) {
    std::string reason;
    {
        std::lock_guard<std::mutex> lock(log_mutex);
        reason = startup_error_text.empty() ? last_error_line : startup_error_text;
    }
    if (!log_to_file) {
        return; /* the terminal (or the redirected log) already shows it */
    }
    std::string shown_log;
    if (instance_claimed) {
        open_log_file();
        shown_log = native_log_path;
    } else {
        /* uxplay.log may belong to an instance that is running: this start
           (it failed before it became the instance) writes its own file */
        shown_log = write_start_failure_log();
    }
    native_media_show_start_failure(reason.c_str(), shown_log.c_str());
}

[[noreturn]] static void startup_exit(int exit_code) {
    if (exit_code != 0) {
        report_startup_failure();
    }
    exit(exit_code);
}



/* helper function for writing user-only accesible files (unix permisions 600) */

FILE* fopen600(const char* filename, const char* mode) {
    if (!filename || !mode) return NULL;

    DWORD desiredAccess = 0;
    DWORD creationDisposition = 0;
    int openFlags = 0;

    if (mode[0] == 'r') {
        desiredAccess = GENERIC_READ;
        creationDisposition = OPEN_EXISTING;
        openFlags = _O_RDONLY;
    } else if (mode[0] == 'w') {
        desiredAccess = GENERIC_WRITE;
        creationDisposition = CREATE_ALWAYS;
        openFlags = _O_WRONLY;
    } else if (mode[0] == 'a') {
        desiredAccess = FILE_APPEND_DATA;
        creationDisposition = OPEN_ALWAYS;
        openFlags = _O_WRONLY | _O_APPEND;
    } else {
        return NULL;
    }

    // "D:P" breaks inheritance from parent folder.
    // "(A;;GA;;;OW)" grants GENERIC_ALL (GA) to the Owner (OW).
    // This gives the owner full control (Read, Write, and DELETE) while blocking others.
    SECURITY_ATTRIBUTES sa = { sizeof(sa), NULL, FALSE };
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorA(
            "D:P(A;;GA;;;OW)", 
            SDDL_REVISION_1, 
            &sa.lpSecurityDescriptor, 
            NULL)) {
        return NULL;
    }

    HANDLE hFile = CreateFileA(
        filename, 
        desiredAccess, 
        0, // Exclusive lock while file handle is open
        &sa, 
        creationDisposition, 
        FILE_ATTRIBUTE_NORMAL, 
        NULL
    );

    LocalFree(sa.lpSecurityDescriptor);

    if (hFile == INVALID_HANDLE_VALUE) return NULL;

    int fd = _open_osfhandle(reinterpret_cast<intptr_t>(hFile), openFlags);
    if (fd == -1) {
        CloseHandle(hFile);
        return NULL;
    }

    FILE* f = _fdopen(fd, mode);
    if (!f) _close(fd);
    return f;

}


static bool file_has_write_access (const char * filename) {
    bool exists = false;
    bool write = false;
    if ((exists = _access(filename, 0) != -1)) {
        write = (_access(filename, 2) != -1);
    }
    if (!exists) {
        FILE *fp = fopen(filename, "w");
        if (fp) {
            write = true;
	    fclose(fp);
	    remove(filename);
        }
    }
    return write;
}

/* 95 byte png file with a 1x1 white square (single pixel): placeholder for coverart*/
static const unsigned char empty_image[] = {
    0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a,  0x00, 0x00, 0x00, 0x0d, 0x49, 0x48, 0x44, 0x52,
    0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01,  0x01, 0x03, 0x00, 0x00, 0x00, 0x25, 0xdb, 0x56,
    0xca, 0x00, 0x00, 0x00, 0x03, 0x50, 0x4c, 0x54,  0x45, 0x00, 0x00, 0x00, 0xa7, 0x7a, 0x3d, 0xda,
    0x00, 0x00, 0x00, 0x01, 0x74, 0x52, 0x4e, 0x53,  0x00, 0x40, 0xe6, 0xd8, 0x66, 0x00, 0x00, 0x00,
    0x0a, 0x49, 0x44, 0x41, 0x54, 0x08, 0xd7, 0x63,  0x60, 0x00, 0x00, 0x00, 0x02, 0x00, 0x01, 0xe2,
    0x21, 0xbc, 0x33, 0x00, 0x00, 0x00, 0x00, 0x49,  0x45, 0x4e, 0x44, 0xae, 0x42, 0x60, 0x82 };

static size_t write_coverart(const char *filename, const void *image, size_t len) {
    FILE *fp = fopen(filename, "wb");
    if (!fp) {
        LOGE("Failed to open file %s", filename);
        return 0;
    }
    size_t count = fwrite(image, 1, len, fp);
    fclose(fp);
    return count;
}

static size_t write_metadata(const char *filename, const char *text) {
    FILE *fp = fopen(filename, "wb");
    if (!fp) {
        LOGE("Failed to open file %s", filename);
        return 0;
    }
    size_t count = fwrite(text, sizeof(char), strlen(text) + 1, fp);
    fclose(fp);
    return count;
}

static char *create_pin_display(char *pin_str, int margin, int gap) {
    char *ptr;
    char num[2] = { 0 };
    int w = 10;
    int h = 8;
    static const char digits[10][8][11] = {
        { "0821111380", "2114005113", "1110000111", "1110000111", "1110000111", "1110000111", "5113002114", "0751111470" },
        { "0002111000", "0021111000", "0000111000", "0000111000", "0000111000", "0000111000", "0000111000", "0011111110" },
        { "0811112800", "2114005113", "0000000111", "0000082114", "0862111470", "2114700000", "1117000000", "1111111111" },
        { "0821111380", "2114005113", "0000082114", "0000111170", "0000075130", "1110000111", "5113002114", "0751111470" },
        { "0000211110", "0001401110", "0021401110", "0214001110", "2110001110", "1111111111", "0000001110", "0000001110" },
        { "1111111110", "1110000000", "1110000000", "1112111380", "0000075113", "0000000111", "5113002114", "0711114700" },
        { "0821111380", "2114005113", "1110000000", "1112111380", "1114075113", "1110000111", "5113002114", "0751111470" },
        { "1111111111", "0000002114", "0000021140", "0000211400", "0002114000", "0021140000", "0211400000", "2114000000" },
        { "0831111280", "2114002114", "5113802114", "0751111170", "8214775138", "1110000111", "5113002114", "0751111470" },
        { "0821111380", "2114005113", "1110000111", "5113802111", "0751114111", "0000000111", "5113002114", "0751111470" }
    };

    char pixels[9] = { ' ', '8', 'd', 'b', 'P', 'Y', 'o', '"', '.' };
    /* Ascii art used here is derived from the FIGlet font "collosal" */

    int pin_val = (int) strtoul(pin_str, &ptr, 10);
    if (*ptr) {
        return NULL;
    }
    int len = strlen(pin_str);
    int *pin = (int *) calloc( len, sizeof(int));
    if(!pin) {
        return NULL;
    }

    for (int i = 0; i < len; i++) {
        pin[len - 1 - i] = pin_val % 10;
        pin_val = pin_val / 10;
    }
  
    int size = 4 + h*(margin + len*(w + gap + 1));
    char *pin_image = (char *) calloc(size, sizeof(char));
    if (!pin_image) {
        return NULL;
    }
    char *pos = pin_image;
    snprintf(pos, 2, "\n"); 
    pos++;

    for (int i = 0; i < h; i++) {
        for (int j = 0; j < margin; j++) {
            snprintf(pos, 2,  " ");
            pos++;
        }

        for (int j = 0; j < len; j++) {
            int l = pin[j];
            const char *p = digits[l][i];
            for (int k = 0; k < w; k++) {
                char *ptr;
                strncpy(num, p++, 1);
                int r = (int) strtoul(num, &ptr, 10);
                snprintf(pos, 2, "%c", pixels[r]);
                pos++;
            }
            for (int n=0; n < gap ; n++) {
                snprintf(pos, 2, " ");
                pos++;
            }
        }
        snprintf(pos, 2, "\n");
        pos++;
    }
    free(pin);
    snprintf(pos, 2, "\n");
    return pin_image;
}

static void dump_audio_to_file(unsigned char *data, int datalen, unsigned char type) {
    if (!audio_dumpfile && audio_type != previous_audio_type) {
        char suffix[20];
        std::string fn = audio_dumpfile_name;
        previous_audio_type = audio_type;
        audio_dumpfile_count++;
        audio_dump_count = 0;
        /* type 0x20 is lossless ALAC, type 0x80 is compressed AAC-ELD, type 0x10 is "other" */
        if (audio_type == 0x20) {
            snprintf(suffix, sizeof(suffix), ".%d.alac", audio_dumpfile_count);
        } else if (audio_type == 0x80) {
            snprintf(suffix, sizeof(suffix), ".%d.aac", audio_dumpfile_count);
        } else {
            snprintf(suffix, sizeof(suffix), ".%d.aud", audio_dumpfile_count);
        }
        fn.append(suffix);
        audio_dumpfile = fopen(fn.c_str(),"w");
        if (audio_dumpfile == NULL) {
            LOGE("could not open file %s for dumping audio frames",fn.c_str());
        }
    }

    if (audio_dumpfile) {
        fwrite(data, 1, datalen, audio_dumpfile);
        if (audio_dump_limit) {
            audio_dump_count++;
            if (audio_dump_count == audio_dump_limit) {
                fclose(audio_dumpfile);
                audio_dumpfile = NULL;
            }          
        }
    }
}

static void dump_video_to_file(unsigned char *data, int datalen) {
    /*  SPS NAL has (data[4] & 0x1f) = 0x07  */
    if ((data[4] & 0x1f) == 0x07  && video_dumpfile && video_dump_limit) {
        fwrite(mark, 1, sizeof(mark), video_dumpfile);
        fclose(video_dumpfile);
        video_dumpfile = NULL;
        video_dump_count = 0;                     
    }

    if (!video_dumpfile) {
        std::string fn = video_dumpfile_name;
        if (video_dump_limit) {
            char suffix[20];
            video_dumpfile_count++;
            snprintf(suffix, sizeof(suffix), ".%d", video_dumpfile_count);
            fn.append(suffix);
        }
        fn.append(".h264");
        video_dumpfile = fopen (fn.c_str(),"w");
        if (video_dumpfile == NULL) {
            LOGE("could not open file %s for dumping h264 frames",fn.c_str());
        }
    }

    if (video_dumpfile) {
        if (video_dump_limit == 0) {
            fwrite(data, 1, datalen, video_dumpfile);
        } else if (video_dump_count < video_dump_limit) {
            video_dump_count++;
            fwrite(data, 1, datalen, video_dumpfile);
        }
    }
}

/* Main-thread event loop (GLib's GMainLoop is not used): one run per AirPlay
 * session cycle.  Other threads post tasks (engine events, Ctrl+C) or request
 * a reset; client feedback is checked every second. */
class main_event_loop {
public:
    /* from any thread; a task posted between runs runs in the next run */
    void post(std::function<void()> task) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            tasks_.push_back(std::move(task));
        }
        wake_.notify_one();
    }

    /* main thread, inside run(): end the run after the current task */
    void quit() {
        std::lock_guard<std::mutex> lock(mutex_);
        quit_ = true;
    }

    /* from a RAOP thread: end the run after reset_grace, which lets the
       requesting handler finish before main() tears connections down.  A
       request made while no run is active is kept and ends the next run:
       main() acknowledges the requests it is about to handle
       (acknowledge_resets), so one that arrives later is never lost */
    void request_reset() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (reset_pending_) {
                return;
            }
            reset_pending_ = true;
            reset_at_ = std::chrono::steady_clock::now() + reset_grace;
        }
        wake_.notify_one();
    }

    /* main thread, between two runs: the caller handles every reset that was
       requested so far */
    void acknowledge_resets() {
        std::lock_guard<std::mutex> lock(mutex_);
        reset_pending_ = false;
    }

    bool running() {
        std::lock_guard<std::mutex> lock(mutex_);
        return running_;
    }

    void run(void (*every_second)()) {
        using clock = std::chrono::steady_clock;
        std::unique_lock<std::mutex> lock(mutex_);
        running_ = true;
        quit_ = false;
        clock::time_point next_tick = clock::now() + std::chrono::seconds(1);
        while (!quit_) {
            /* a reset requested during the wait must shorten it: the wait is
               left when the request arrives and re-armed with its deadline */
            bool const reset_armed = reset_pending_;
            clock::time_point wake_at = reset_armed ? std::min(next_tick, reset_at_) : next_tick;
            wake_.wait_until(lock, wake_at, [this, reset_armed] {
                return quit_ || !tasks_.empty() || reset_pending_ != reset_armed;
            });
            std::deque<std::function<void()>> ready;
            ready.swap(tasks_);
            lock.unlock();
            for (std::function<void()> &task : ready) {
                task();
            }
            clock::time_point const now = clock::now();
            if (now >= next_tick) {
                next_tick = now + std::chrono::seconds(1);
                every_second();
            }
            lock.lock();
            if (reset_pending_ && clock::now() >= reset_at_) {
                quit_ = true;
            }
        }
        running_ = false;
    }

private:
    static constexpr std::chrono::milliseconds reset_grace{100};

    std::mutex mutex_;
    std::condition_variable wake_;
    std::deque<std::function<void()>> tasks_;
    bool running_ = false;
    bool quit_ = false;
    bool reset_pending_ = false;
    std::chrono::steady_clock::time_point reset_at_;
};

static main_event_loop event_loop;

static void exit_after_failure(void);

#define SERVER_RESTART_LIMIT 5

/* main loop thread: false once the RTSP server was started again
   SERVER_RESTART_LIMIT times within a minute (it will not stay up) */
static bool note_server_restart(void) {
    static std::deque<std::chrono::steady_clock::time_point> restarts;
    std::chrono::steady_clock::time_point const now = std::chrono::steady_clock::now();
    while (!restarts.empty() && now - restarts.front() > std::chrono::minutes(1)) {
        restarts.pop_front();
    }
    restarts.push_back(now);
    return restarts.size() <= SERVER_RESTART_LIMIT;
}

static void check_client_feedback() {
    /* The RTSP server thread ended by itself (repeated socket failures) and
       took its connections with it: without it the receiver is advertised but
       unreachable.  The session cycle ends here, so the relaunch block stops
       audio and video (no frozen last picture) and starts the server again. */
    if (raop && !raop_is_running(raop)) {
        if (!note_server_restart()) {
            startup_message("the AirPlay (RTSP) server stopped by itself %d times within a minute; giving up",
                            SERVER_RESTART_LIMIT + 1);
            exit_after_failure();
            return;
        }
        LOGW("the RTSP server stopped by itself: starting it again");
        reset_httpd = true;
        relaunch_video = true;
        full_video_reset = true;
        event_loop.quit();
        return;
    }
    if (open_connections) {
        if (missed_feedback_limit && missed_feedback > missed_feedback_limit) {
            LOGI("***ERROR lost connection with client (network problem?)");
            LOGI("   Interval since last client feedback request exceeds limit of %u seconds", missed_feedback_limit);
            LOGI("   Sometimes the network connection may recover after a longer delay:\n"
                 "   the default limit n = %d seconds, can be changed with the \"-reset n\" option", MISSED_FEEDBACK_LIMIT);
            if (!nofreeze) {
                close_window = false; /* leave "frozen" window open if reset_video is false */
            }
            reset_httpd = true;
            relaunch_video = true;
            full_video_reset = true;
            event_loop.quit();
            return;
        } else if (missed_feedback > 2) {
            LOGE("%3u seconds since last client feedback request (expected every two seconds); client may be offline",
                 missed_feedback.load());
        }
        missed_feedback++;
    } else {
        missed_feedback = 0;
    }
}

/* signals handlers (ctrl-c, etc )*/

static void cleanup(int exit_code);

/* Ctrl+C, console close, "Exit" in the notification-area menu */
static void request_exit() {
    exit_requested = true;
    event_loop.post([] {
        event_loop.quit();
    });
}

static BOOL WINAPI CtrlHandler(DWORD signal) {
    switch (signal) {
    case CTRL_C_EVENT:
    case CTRL_CLOSE_EVENT:
    case CTRL_SHUTDOWN_EVENT:
        /* The shutdown always runs on the main thread: the exit task is
         * kept until its loop runs (start-up, or between two session cycles).
         * Windows terminates the process as soon as this handler returns for
         * CTRL_CLOSE_EVENT/CTRL_SHUTDOWN_EVENT: wait (bounded) for the orderly
         * shutdown so clients and mDNS are told. */
        request_exit();
        if (signal != CTRL_C_EVENT && shutdown_complete_event) {
            WaitForSingleObject(shutdown_complete_event, 4000);
        }
        return TRUE;
    default:
        return FALSE;
    }
}

/* one session cycle: returns when the session must be relaunched or the
   program exits (relaunch_video == false) */
static void main_loop()  {
    /* the session flags (relaunch_video, reset_httpd, close_window,
       connection_handed_over) are taken by main() after each run, so a
       request that arrives between two runs is still there for the next */
    missed_feedback = 0;
    event_loop.run(check_client_feedback);
}

static int parse_hw_addr (std::string str, std::vector<char> &hw_addr) {
    for (int i = 0; i < (int) str.length(); i += 3) {
        hw_addr.push_back((char) stol(str.substr(i), NULL, 16));
    }
    return 0;
}

static const char *get_homedir() {
    const char *homedir = getenv("XDG_CONFIG_HOMEDIR");
    if (homedir == NULL) {
        homedir = getenv("HOME");
    }
    if (homedir == NULL) {
        homedir = getenv("USERPROFILE");  /* Windows: HOME is usually not set */
    }
    return homedir;
}

static std::string find_uxplay_config_file() {
    std::string no_config_file = "";
    const char *homedir = NULL;
    const char *uxplayrc = NULL;
    std::string config0, config1, config2;
    struct stat sb;
    uxplayrc = getenv("UXPLAYRC");   /* first look for $UXPLAYRC */
    if (uxplayrc) {
        config0 = uxplayrc;
        if (stat(config0.c_str(), &sb) == 0) return config0;
    }
    homedir = get_homedir();
    if (homedir) {
      config1 = homedir;
      config1.append("/.uxplayrc");
      if (stat(config1.c_str(), &sb) == 0) return config1;  /* look for ~/.uxplayrc */
      config2 = homedir;
      config2.append("/.config/uxplayrc"); /* look for ~/.config/uxplayrc */
      if (stat(config2.c_str(), &sb) == 0) return config2;
    }
    return no_config_file;
}

static std::string find_mac () {
/*  finds the MAC address of an Ethernet or Wi-Fi interface that is up */
    std::string mac = "";
    char str[3];
    ULONG buflen = sizeof(IP_ADAPTER_ADDRESSES);
    PIP_ADAPTER_ADDRESSES addresses = (IP_ADAPTER_ADDRESSES*) malloc(buflen);
    if (addresses == NULL) { 					
        return mac;
    }
    if (GetAdaptersAddresses(AF_UNSPEC, 0, NULL, addresses, &buflen) == ERROR_BUFFER_OVERFLOW) {
        free(addresses);
        addresses = (IP_ADAPTER_ADDRESSES*) malloc(buflen);
        if (addresses == NULL) {
            return mac;
        }
    }
    if (GetAdaptersAddresses(AF_UNSPEC, 0, NULL, addresses, &buflen) == NO_ERROR) {
        for (PIP_ADAPTER_ADDRESSES address = addresses; address != NULL; address = address->Next) {
            if (address->PhysicalAddressLength != 6                 /* MAC has 6 octets */
                || (address->IfType != 6 && address->IfType != 71)  /* Ethernet or Wireless interface */
                || address->OperStatus != 1) {                      /* interface is up */
                continue;
            }
            mac.erase();
            for (int i = 0; i < 6; i++) {
                snprintf(str, sizeof(str), "%02x", int(address->PhysicalAddress[i]));
                mac = mac + str;
                if (i < 5) mac = mac + ":";
            }
	    break;
        }
    }
    free(addresses);
    return mac;
}

static bool validate_mac(char * mac_address) {
    char c;
    if (strlen(mac_address) != 17)  return false;
    for (int i = 0; i < 17; i++) {
        c = *(mac_address + i);
        if (i % 3 == 2) {
            if (c != ':')  return false;
        } else {
            if (c < '0') return false;
            if (c > '9' && c < 'A') return false;
            if (c > 'F' && c < 'a') return false;
            if (c > 'f') return false;
        }
    }
    return true;
}

static std::string random_mac () {
    char str[4];
    unsigned char random[6];
    get_random_bytes(random, sizeof(random));
    /* mark MAC address as locally administered, i.e. random */
    random[0] = random[0] & ~0x01;
    random[0] = random[0] | 0x02;
    snprintf(str,3,"%2.2x", random[0]);
    std::string mac_address(str);
    for (int i = 1; i < 6; i++) {
        snprintf(str,4,":%2.2x", random[i]);
        mac_address = mac_address + str;
    }
    return mac_address;
}

static void print_info (char *name) {
    printf(UXPLAY_PRODUCT_NAME " %s: an open-source AirPlay mirroring and audio receiver for Windows.\n",
           UXPLAY_VERSION_TEXT);
    printf(UXPLAY_PROJECT_URL "\n");
    printf("(AirPlay protocol core: UxPlay %s, https://github.com/FDH2/UxPlay)\n", VERSION);
    printf("Usage: %s [-n name] [-s wxh] [-p [n]] [(other options)]\n", name);
    printf("Options:\n");
    printf("-n name   Specify network name of the AirPlay server (UTF-8/ascii)\n");
    printf("-nh       Do not add \"@hostname\" at the end of AirPlay server name\n");
    printf("-h265     Offer H.265 even if the GPU has no HEVC hardware decoder\n");
    printf("-h264     Offer only H.264 (default: H.265 too if the GPU decodes it)\n");
    printf("-gpu g    GPU for decode/present: auto (display owner), integrated|intel,\n");
    printf("          discrete|nvidia (any vendor, AMD included), software|warp\n");
    printf("-mode m   quality (sharpest picture, a little more buffering), balanced\n");
    printf("          (adapts to monitor, GPU and network; the default) or\n");
    printf("          performance (lighter stream, lowest delay); also in the menu\n");
    printf("-crop c   auto: cut the black bars around the content off, so the window\n");
    printf("          hugs it (key C); off: show the whole picture; also in the menu,\n");
    printf("          where a part can be framed by hand too (key X)\n");
    printf("-ui l     Interface language: auto (the Windows display language, the\n");
    printf("          default), en, zh-TW (Traditional Chinese), zh-CN (Simplified\n");
    printf("          Chinese), ja, ko, es, de, fr, vi, th\n");
    printf("-pin[xxxx]Use a 4-digit pin code to control client access (default: no)\n");
    printf("          default pin is random: optionally use fixed pin xxxx\n");
    printf("-reg [fn] Keep a register in ~/.uxplay.register to verify returning\n");
    printf("          client pin-registration; (option: use file \"fn\" for this)\n");
    printf("-pw [pwd] Require use of password to control client access;\n");
    printf("          (with no pwd, pin entry is required at *each* connection.)\n");
    printf("          (option \"-pw\" after \"-pin\" overrides it, and vice versa)\n");
    printf("-vsync [x]Mirror mode: sync audio to video using timestamps (default)\n");
    printf("          x is optional audio delay: millisecs, decimal, can be neg.\n");
    printf("-vsync no Switch off audio/(server)video timestamp synchronization \n");
    printf("-async [x]Audio-Only mode: sync audio to client video (default: no)\n");
    printf("-async no Switch off audio/(client)video timestamp synchronization\n");
    printf("-db l[:h] Set minimum volume attenuation to l dB (decibels, negative);\n");
    printf("          optional: set maximum to h dB (+ or -) default: -30.0:0.0 dB\n");
    printf("-taper    Use a \"tapered\" AirPlay volume-control profile\n");
    printf("-vol <v>  Set initial audio-streaming volume: range [mute=0.0:1.0=full]\n");
    printf("-s wxh[@r]Request to client for video display resolution [refresh_rate]\n");
    printf("          default: the monitor's resolution and refresh rate\n");
    printf("-o        Set display \"overscanned\" mode on (not usually needed)\n");
    printf("-fs       Start in full screen (toggle: double-click, F, F11, Alt+Enter)\n");
    printf("-p        Use legacy ports UDP 6000:6001:7011 TCP 7000:7001:7100\n");
    printf("-p n      Use TCP and UDP ports n,n+1,n+2. range %d-%d\n", LOWEST_ALLOWED_PORT, HIGHEST_PORT);
    printf("          use \"-p n1,n2,n3\" to set each port, \"n1,n2\" for n3 = n2+1\n");
    printf("          \"-p tcp n\" or \"-p udp n\" sets TCP or UDP ports separately\n");
    printf("-vs 0     Streamed audio only, with no video display window\n");
    printf("-as 0     (or -a)  Turn audio off, streamed video only\n");
    printf("-al x     Audio latency in seconds (default 0.25) reported to client.\n");
    printf("-ca <fn>  In Audio (ALAC) mode, write cover art to file <fn>\n");
    printf("-md <fn>  In Airplay Audio (ALAC) mode, write metadata text to file <fn>\n");
    printf("-reset n  Reset after n seconds of client silence (default n=%d, 0=never)\n", MISSED_FEEDBACK_LIMIT);
    printf("-nofreeze Do NOT leave frozen screen in place after reset\n");
    printf("-nc       Do NOT  Close video window when client stops mirroring\n");
    printf("-nc no    Cancel the -nc option (DO close video window) \n");
    printf("-nohold   Drop current connection when new client connects.\n");
    printf("-restrict Restrict clients to those specified by \"-allow <deviceID>\"\n");
    printf("          the deviceID is displayed when a client attempts to connect\n");
    printf("          Use \"-restrict no\" for no client restrictions (default)\n");
    printf("-allow <i>Permit deviceID = <i> to connect if restrictions are imposed\n");
    printf("-block <i>Always block connections from deviceID = <i>\n");
    printf("-FPSdata  Show video-streaming performance reports sent by client.\n");
    printf("-fps n    Set maximum allowed streaming framerate (default: refresh rate,\n");
    printf("          at most 60)\n");
    printf("-m [mac]  Set MAC address (also Device ID);use for concurrent instances\n");
    printf("          if mac xx:xx:xx:xx:xx:xx is not given, a random MAC is used\n");
    printf("-key [fn] Store private key in ~/.uxplay.pem (or in file \"fn\")\n");
    printf("-dacp [fn]Export client DACP information to file ~/.uxplay.dacp\n");
    printf("          (option to use file \"fn\" instead); used for client remote\n");
    printf("-d [n]    Enable debug logging; optional: n=1 to skip normal packet data\n");
    printf("-vdmp [n] Dump h264 video output to \"fn.h264\"; fn=\"videodump\",change\n");
    printf("          with \"-vdmp [n] filename\". If [n] is given, file fn.x.h264\n");
    printf("          x=1,2,.. opens whenever a new SPS/PPS NAL arrives, and <=n\n");
    printf("          NAL units are dumped.\n");
    printf("-admp [n] Dump audio output to \"fn.x.fmt\", fmt ={aac, alac, aud}, x\n");
    printf("          =1,2,..; fn=\"audiodump\"; change with \"-admp [n] filename\".\n");
    printf("          x increases when audio format changes. If n is given, <= n\n");
    printf("          audio packets are dumped. \"aud\"= unknown format.\n");
    printf("-v        Displays version information\n");
    printf("-h        Displays this help\n");
    printf("-rc fn    Read startup options from file \"fn\" instead of ~/.uxplayrc, etc\n");
    printf("Startup options in $UXPLAYRC, ~/.uxplayrc, or ~/.config/uxplayrc are\n");
    printf("applied first (command-line options may modify them): format is one \n");
    printf("option per line, no initial \"-\"; lines starting with \"#\" are ignored.\n");
    printf("(~ is %%HOME%%, or %%USERPROFILE%% when HOME is not set.)\n");
    printf("Options of other UxPlay builds (GStreamer pipelines, HLS, MP4 recording,\n");
    printf("Bluetooth LE, Raspberry Pi) are accepted and ignored with a warning.\n");
}

static bool option_has_value(const int i, const int argc, std::string option, const char *next_arg) {
    if (i >= argc - 1 || next_arg[0] == '-') {
        LOGE("invalid: \"%s\" had no argument", option.c_str());
        return false;
     }
    return true;
}

static bool get_display_settings (std::string value, unsigned short *w, unsigned short *h, unsigned short *r) {
    // assume str  = wxh@r is valid if w and h are positive decimal integers
    // with no more than 4 digits, r < 256 (stored in one byte).
    char *end;
    std::size_t pos = value.find_first_of("x");
    if (pos == std::string::npos) return false;
    std::string str1 = value.substr(pos+1);
    value.erase(pos);
    if (value.length() == 0 || value.length() > 4 || value[0] == '-') return false;
    *w = (unsigned short) strtoul(value.c_str(), &end, 10);
    if (*end || *w == 0)  return false;
    pos = str1.find_first_of("@");
    if(pos != std::string::npos) {
        std::string str2 = str1.substr(pos+1);
        if (str2.length() == 0 || str2.length() > 3 || str2[0] == '-') return false;
        *r = (unsigned short) strtoul(str2.c_str(), &end, 10);
        if (*end || *r == 0 || *r > 255) return false;
        str1.erase(pos);
    }
    if (str1.length() == 0 || str1.length() > 4 || str1[0] == '-') return false;
    *h = (unsigned short) strtoul(str1.c_str(), &end, 10);
    if (*end || *h == 0) return false;
    return true;
}

static bool get_value (const char *str, unsigned int *n) {
    // if n > 0 str must be a positive decimal <= input value *n  
    // if n = 0, str must be a non-negative decimal
    if (strlen(str) == 0 || strlen(str) > 10 || str[0] == '-') return false;
    char *end;
    unsigned long l = strtoul(str, &end, 10);
    if (*end) return false;
    if (*n && (l == 0 || l > *n)) return false;
    *n = (unsigned int) l;
    return true;
}

static bool get_ports (int nports, std::string option, const char * value, unsigned short * const port) {
    /*valid entries are comma-separated values port_1,port_2,...,port_r, 0 < r <= nports */
    /*where ports are distinct, and are in the allowed range.                            */
    /*missed values are consecutive to last given value (at least one value needed).    */
    char *end;
    unsigned long l;
    std::size_t pos;
    std::string val(value), str;
    for (int i = 0; i <= nports ; i++)  {
        if(i == nports) break;
        pos = val.find_first_of(',');
        str = val.substr(0,pos);
        if(str.length() == 0 || str.length() > 5 || str[0] == '-') break;
        l = strtoul(str.c_str(), &end, 10);
        if (*end || l < LOWEST_ALLOWED_PORT || l > HIGHEST_PORT) break;
         *(port + i) = (unsigned short) l;
        for  (int j = 0; j < i ; j++) {
            if( *(port + j) == *(port + i)) break;
        }
        if(pos == std::string::npos) {
            if (nports + *(port + i) > i + 1 + HIGHEST_PORT) break;
            for (int j = i + 1; j < nports; j++) {
                *(port + j) = *(port + j - 1) + 1;
            }
            return true;
        }
        val.erase(0, pos+1);
    }
    LOGE("invalid \"%s %s\", all %d ports must be in range [%d,%d]",
         option.c_str(), value, nports, LOWEST_ALLOWED_PORT, HIGHEST_PORT);
    return false;
}


static void append_hostname(std::string &server_name) {
    std::string hostname;
    char buffer[256] = "";
    unsigned long size = sizeof(buffer);
    if (GetComputerNameA(buffer, &size)) {
        hostname.append(buffer);
    }
    /* remove any ".local" suffix (causes namespace problem on macOS when using a custom mDNSResponder)*/
    std::string dot_local = ".local";
    size_t pos = hostname.find(dot_local);
    if (pos != std::string::npos) {
        hostname.erase(pos);
    }
    server_name.append("@");
    server_name.append(hostname);
}

bool is_utf8(const char *string, bool *is_printable_ascii) {
    /* test if C-string is printable ascii or valid UTF-8 (max 4 bytes) */
    if (is_printable_ascii)  {
        *is_printable_ascii = true;
    }
    int len = (int) strlen(string);
    for (int i = 0; i < len; i++) {
        unsigned char c = (unsigned char) string[i];
        int n = 0;
        if (0x20 <= c && c <= 0x7e) {
            continue;   //printable ascii, no control characters.
        } else if (is_printable_ascii) {
            *is_printable_ascii = false;
        }
        if (0x00 <= c && c <= 0x7f) {
            continue; //one byte code, 0bbbbbbb
        } else if (c == 0xc0 || c == 0xc1) {
             return false;  //two byte code, invalid start byte
        } else if ((c & 0xe0) == 0xc0) {
            n = 1; //two byte code, 110bbbbb
        } else if (c == 0xe0 && i < len - 1 && (unsigned char) string[i + 1] < 0xa0) {
            return false;  //three byte code, overlong encoding
        } else if (c == 0xed && i < len - 1 && (unsigned char) string[i + 1] > 0x9f) {
            return false;  //three byte code, exclude U+dc00 to U+dfff
        } else if ((c & 0xf0) == 0xe0) {
            n = 2; //three byte code 1110bbbb
        } else if (c >= 0xf5) {
            return false;  //four byte code, invalid start byte
        } else if (c == 0xf0 && i < len - 1 && (unsigned char) string[i + 1] < 0x90) {
            return false;  //four byte code, overlong encoding
        } else if (c == 0xf4 && i < len - 1 && (unsigned char) string[i + 1] > 0x8f) {
            return false;  //four byte code, out of range character (> U+10ffff)
        } else if ((c & 0xf8) == 0xf0) {
            n = 3; //four byte code, 11110bbb
        } else {
            return false;  //more than 4 bytes
        }
        for (int j = 0; j < n && i < len ; j++) { // n bytes matching 10bbbbbb must follow ?
            if ((++i == len) || (((unsigned char) string[i] & 0xc0) != 0x80)) {
                return false;
            }
        }
	
    }
    return true;
}

/* Options of other UxPlay builds (GStreamer pipelines and sinks, HLS, MP4
 * recording, video flip/rotation, Bluetooth LE beacon, Raspberry Pi, D-Bus
 * screensaver, macOS peer-to-peer): accepted, so a startup file shared with
 * those builds keeps working, and ignored with a warning.  Returns false for
 * an option that is not in these lists. */
static bool skip_unsupported_option(const std::string &arg, int &i, int argc, char *argv[]) {
    static const char *const takes_value[] = {
        "-vp", "-vd", "-vc", "-vrtp", "-artp", "-f", "-r", "-custom", "-rpi", "-scrsv",
        "-hls", "-lang", "-slang", "-mp4", "-ble", "-srgb" };
    static const char *const no_value[] = { "-avdec", "-v4l2", "-bt709", "-p2p", "-t" };
    bool known = false;
    bool with_value = false;
    for (const char *option : takes_value) {
        if (arg == option) {
            known = true;
            with_value = true;
        }
    }
    for (const char *option : no_value) {
        if (arg == option) {
            known = true;
        }
    }
    if (!known) {
        return false;
    }
    std::string value;
    if (with_value && i < argc - 1 && argv[i + 1] && argv[i + 1][0] != '-') {
        value = argv[++i];
    }
    LOGW("option \"%s%s%s\" is not supported by the Windows-native build: ignored", arg.c_str(),
         value.empty() ? "" : " ", value.c_str());
    return true;
}

static void parse_arguments (int argc, char *argv[]) {
    // Parse arguments
    for (int i = 1; i < argc; i++) {
        if (!is_utf8(argv[i], NULL)) {
            startup_message("Error: detected a non-ascii or non-UTF-8 string \"%s\""
                    "while parsing input arguments", argv[i]);
            startup_exit(1);
        }
    }
    for (int i = 1; i < argc; i++) {
        std::string arg(argv[i]);
        if (arg == "-rc") {
            i++;  //specifies startup file: has already been processed
        } else if (arg == "-allow") {
            if (!option_has_value(i, argc, arg, argv[i+1])) startup_exit(1);
            i++;
            allowed_clients.push_back(argv[i]);
        } else if (arg == "-block") {
            if (!option_has_value(i, argc, arg, argv[i+1])) startup_exit(1);
            i++;
            blocked_clients.push_back(argv[i]);    
        } else if (arg == "-restrict") {
            if (i <  argc - 1) {
                if (strlen(argv[i+1]) == 2 && strncmp(argv[i+1], "no", 2) == 0) {
                    restrict_clients = false;
                    i++;
                    continue;
                }
	    } 
            restrict_clients = true;
        } else if (arg == "-n") {
            if (!option_has_value(i, argc, arg, argv[i+1])) startup_exit(1);
            bool ascii;
            server_name.erase();
            bool utf8 = is_utf8(argv[++i], &ascii);
            if (!utf8) {
                startup_message("invalid (non-UTF-8/ascii) server name in \"-n %s\"", argv[i]);
                startup_exit(1);
            }
            server_name = std::string(argv[i]);
            if (!ascii) {
                LOGW("a non-ascii (UTF-8) server-name \"%s\" was specified:"
                     " ensure correct locale settings to display it", server_name.c_str());
            }
        } else if (arg == "-nh") {
            do_append_hostname = false;
        } else if (arg == "-async") {
            audio_sync = true;
	    if (i <  argc - 1) {
                if (strlen(argv[i+1]) == 2 && strncmp(argv[i+1], "no", 2) == 0) {
                    audio_sync = false;
                    i++;
                    continue;
		}
                char *end;
                int n = (int) (strtof(argv[i + 1], &end) * 1000);
                if (*end == '\0') {
                    i++;
                    if (n > -SECOND_IN_USECS && n < SECOND_IN_USECS) {
                        audio_delay_alac = n * 1000; /* units are nsecs */
                    } else {
                        startup_message("invalid -async %s: requested delays must be smaller than +/- 1000 millisecs\n", argv[i] );
                        startup_exit(1);
                    }
                }
            }
        } else if (arg == "-vsync") {
            video_sync = true;
	    if (i <  argc - 1) {
                if (strlen(argv[i+1]) == 2 && strncmp(argv[i+1], "no", 2) == 0) {
                    video_sync = false;
                    i++;
                    continue;
                }
                char *end;
                int n = (int) (strtof(argv[i + 1], &end) * 1000);
                if (*end == '\0') {
                    i++;
                    if (n > -SECOND_IN_USECS && n < SECOND_IN_USECS) {
                        audio_delay_aac = n * 1000;     /* units are nsecs */
                    } else {
                        startup_message("invalid -vsync %s: requested delays must be smaller than +/- 1000 millisecs\n", argv[i]);
                        startup_exit(1);
                    }
                }
            }
        } else if (arg == "-s") {
            if (!option_has_value(i, argc, argv[i], argv[i+1])) startup_exit(1);
            std::string value(argv[++i]);
            user_set_display = true;
            if (!get_display_settings(value, &display[0], &display[1], &display[2])) {
                startup_message("invalid \"-s %s\"; -s wxh : max w,h=9999; -s wxh@r : max r=255\n",
                        argv[i]);
                startup_exit(1);
            }
        } else if (arg == "-fps") {
            if (!option_has_value(i, argc, arg, argv[i+1])) startup_exit(1);
            unsigned int n = 255;
            if (!get_value(argv[++i], &n)) {
                startup_message("invalid \"-fps %s\"; -fps n : max n=255, default n=30\n", argv[i]);
                startup_exit(1);
            }
            display[3] = (unsigned short) n;
            user_set_fps = true;
        } else if (arg == "-o") {
            display[4] = 1;
        } else if (arg == "-p") {
            if (i == argc - 1 || argv[i + 1][0] == '-') {
                tcp[0] = 7100; tcp[1] = 7000; tcp[2] = 7001;
                udp[0] = 7011; udp[1] = 6001; udp[2] = 6000;
                continue;
            }
            std::string value(argv[++i]);
            if (value == "tcp" || value == "udp") {
                if (i == argc - 1 || argv[i + 1][0] == '-') {
                    startup_message("invalid \"-p %s\": missing second argument (ports must be specified after \"%s\") \n",
                            argv[i], argv[i]);
                    startup_exit(1);
                }
                arg.append(" " + value);
                unsigned short *ports = (value == "udp") ? udp : tcp;
                if (!get_ports(3, arg, argv[++i], ports)) {
                    startup_exit(1);
                }
            } else {
                if (!get_ports(3, arg, argv[i], tcp)) {
                    startup_exit(1);
                }
                for (int j = 0; j < 3; j++) {
                    udp[j] = tcp[j];
                }
            }
        } else if (arg == "-m") {
            if (i < argc - 1 && *argv[i+1] != '-') {
                if (validate_mac(argv[++i])) {
                    mac_address.erase();
                    mac_address = argv[i];
                    use_random_hw_addr = false;
                } else {
                    startup_message("invalid mac address \"%s\": address must have form"
                            " \"xx:xx:xx:xx:xx:xx\", x = 0-9, A-F or a-f\n", argv[i]);
                    startup_exit(1);
                }
            } else {
                use_random_hw_addr  = true;
            }
        } else if (arg == "-a") {
            use_audio = false;
        } else if (arg == "-d") {
            if (i < argc - 1 && *argv[i+1] != '-') {
                unsigned int n = 1;
                if (!get_value(argv[++i], &n)) {
                    startup_message("invalid \"-d %s\"; -d n : max n=1 (suppress packet data in debug output)\n", argv[i]);
                    startup_exit(1);
                }
                debug_log = true;
                suppress_packet_debug_data = true;
            } else {
                debug_log = !debug_log;
                suppress_packet_debug_data = false;
            }
        } else if (arg == "-h"  || arg == "--help" || arg == "-?" || arg == "-help") {
            print_info(argv[0]);
            exit(0);
        } else if (arg == "-v") {
            printf(UXPLAY_PRODUCT_NAME " version %s (UxPlay protocol core %s); for help, use option \"-h\"\n",
                   UXPLAY_VERSION_TEXT, VERSION);
            exit(0);
        } else if (arg == "-vs" || arg == "-as") {
            /* GStreamer sink choices are ignored; "0" still means "no video
               window" (-vs 0) or "no audio" (-as 0) */
            if (!option_has_value(i, argc, arg, argv[i+1])) startup_exit(1);
            std::string sink(argv[++i]);
            if (sink == "0") {
                if (arg == "-vs") {
                    use_video = false;
                } else {
                    use_audio = false;
                }
            } else {
                LOGW("option \"%s %s\" ignored: %s", arg.c_str(), sink.c_str(),
                     arg == "-vs" ? "video is presented by the Direct3D 11 window"
                                  : "audio plays on the default Windows output (WASAPI)");
            }
        } else if (arg == "-nc") {
            new_window_closing_behavior = false;
            if (i <  argc - 1) {
                if (strlen(argv[i+1]) == 2 && strncmp(argv[i+1], "no", 2) == 0) {
                    new_window_closing_behavior = true;
                    i++;
                    continue;
                }
            }
        } else if (arg == "-fs" ) {
            fullscreen = true;
        } else if (arg == "-FPSdata") {
            show_client_FPS_data = true;
        } else if (arg == "-reset") {
            if (!option_has_value(i, argc, arg, argv[i+1])) startup_exit(1);
            /* now using feedback  (every 1 sec ) instead of ntp timeouts (every 3 secs) to detect offline client and reset connections */
            /* a note, not a failure: it must not become the reason shown for a later one */
            LOGI("*** NOTE CHANGE: -reset n now means reset n seconds (not 3n seconds) after client goes offline");
            missed_feedback_limit = 0;
            if (!get_value(argv[++i], &missed_feedback_limit)) {
                startup_message("invalid \"-reset %s\"; -reset n must have n >= 0,  default n = %d seconds\n", argv[i], MISSED_FEEDBACK_LIMIT);
                startup_exit(1);
            }
        } else if (arg == "-vdmp") {
            dump_video = true;
            if (i < argc - 1 && *argv[i+1] != '-') {
                unsigned int n = 0;
                if (get_value (argv[++i], &n)) {
                    if (n == 0) {
                        startup_message("invalid \"-vdmp 0 %s\"; -vdmp n  needs a non-zero value of n\n", argv[i]);
                        startup_exit(1);
                    }
                    video_dump_limit = n;
                    if (option_has_value(i, argc, arg, argv[i+1])) {
                        video_dumpfile_name.erase();
                        video_dumpfile_name.append(argv[++i]);
                    }
                } else {
                    video_dumpfile_name.erase();
                    video_dumpfile_name.append(argv[i]);
                }
                const char *fn = video_dumpfile_name.c_str();
                if (!file_has_write_access(fn)) {
                    startup_message("%s cannot be written to:\noption \"-vdmp <fn>\" must be to a file with write access\n", fn);
                    startup_exit(1);
                }   		
            }
        } else if (arg == "-admp") {
            dump_audio = true;
            if (i < argc - 1 && *argv[i+1] != '-') {
                unsigned int n = 0;
                if (get_value (argv[++i], &n)) {
                    if (n == 0) {
                        startup_message("invalid \"-admp 0 %s\"; -admp n  needs a non-zero value of n\n", argv[i]);
                        startup_exit(1);
                    }
                    audio_dump_limit = n;
                    if (option_has_value(i, argc, arg, argv[i+1])) {
                        audio_dumpfile_name.erase();
                        audio_dumpfile_name.append(argv[++i]);
                    }
                } else {
                    audio_dumpfile_name.erase();
                    audio_dumpfile_name.append(argv[i]);
                }
                const char *fn = audio_dumpfile_name.c_str();
                if (!file_has_write_access(fn)) {
                    startup_message("%s cannot be written to:\noption \"-admp <fn>\" must be to a file with write access\n", fn);
                    startup_exit(1);
                }
            }
        } else if (arg  == "-ca" ) {
            if (i < argc - 1 && *argv[i+1] != '-') {
                coverart_filename.erase();
                coverart_filename.append(argv[++i]);
                const char *fn = coverart_filename.c_str();
                if (!file_has_write_access(fn)) {
                    startup_message("%s cannot be written to:\noption \"-ca <fn>\" must be to a file with write access\n", fn);
                    startup_exit(1);
                }
            } else {
                LOGW("option \"-ca\" without a file name (on-screen cover art) is not supported: ignored");
            }
        } else if (arg  == "-md" ) {
            if (option_has_value(i, argc, arg, argv[i+1])) {
                metadata_filename.erase();
                metadata_filename.append(argv[++i]);
                const char *fn = metadata_filename.c_str();
                if (!file_has_write_access(fn)) {
                    startup_message("%s cannot be written to:\noption \"-md <fn>\" must be to a file with write access\n", fn);
                    startup_exit(1);
                }   
            } else {
                startup_message("option -md must be followed by a filename for metadata text output\n");
                startup_exit(1);
            }
        } else if (arg == "-nohold") {
            nohold = 1;
        } else if (arg == "-al") {
	    int n;
            char *end;
            if (i < argc - 1 && *argv[i+1] != '-') {
                n = (int) (strtof(argv[++i], &end) * SECOND_IN_USECS);
                if (*end == '\0' && n >=0 && n <= 10 * SECOND_IN_USECS) {
                    audiodelay = n;
                    continue;
                }
            }
            startup_message("invalid -al %s: value must be a decimal time offset in seconds, range [0,10]\n"
                    "(like 5 or 4.8, which will be converted to a whole number of microseconds)\n", argv[i]);
            startup_exit(1);
        } else if (arg == "-pin") {
            setup_legacy_pairing = true;
            pin_pw = 1;
            if (i < argc - 1 && *argv[i+1] != '-') {
                unsigned int n = 9999;
                if (!get_value(argv[++i], &n)) {
                    startup_message("invalid \"-pin %s\"; -pin nnnn : nnnn range [0001:9999], (4 digits)\n", argv[i]);
                    startup_exit(1);
                }
                pin = n + 10000;
            }
        } else if (arg == "-reg") {
            registration_list = true;
            pairing_register.erase();
            if (i < argc - 1 && *argv[i+1] != '-') {
                pairing_register.append(argv[++i]);
                const char * fn = pairing_register.c_str();
                if (!file_has_write_access(fn)) {
                    startup_message("%s cannot be written to:\noption \"-reg <fn>\" must be to a file with write access\n", fn);
                    startup_exit(1);
      }   
            }
        } else if (arg == "-key") {
            keyfile.erase();
            if (i < argc - 1 && *argv[i+1] != '-') {
                keyfile.append(argv[++i]);
                const char * fn = keyfile.c_str();
                if (!file_has_write_access(fn)) {
                    startup_message("%s cannot be written to:\noption \"-key <fn>\" must be to a file with write access\n", fn);
                    startup_exit(1);
                }   
            } else {
	        //                startup_message("option \"-key <fn>\" requires a path <fn> to a file for persistent key storage\n");
	        // startup_exit(1);
                keyfile.erase();
                keyfile.append("0");
            }
        } else if (arg == "-pw") {
            setup_legacy_pairing = false;
            if (i < argc - 1 && *argv[i+1] != '-') {
                password.erase();
                password.append(argv[++i]);
                pin_pw = 2;
                if (password.size() < min_password_length) {
                    startup_message("invalid client-access password \"%s\": length must be at least %u characters\n", password.c_str(), min_password_length);
                    startup_exit(1);
                }
            } else {
                pin_pw = 3;  //a random password (pin) will be displayed at each connection
            }
        } else if (arg == "-dacp") {
            dacpfile.erase();
            if (i < argc - 1 && *argv[i+1] != '-') {
                dacpfile.append(argv[++i]);
                const char *fn = dacpfile.c_str();
                if (!file_has_write_access(fn)) {
                    startup_message("%s cannot be written to:\noption \"-dacp <fn>\" must be to a file with write access\n", fn);
                    startup_exit(1);
                }   
            } else {
                const char *homedir = get_homedir();
                if (!homedir) {
                    startup_message("option \"-dacp\": no home directory (HOME, USERPROFILE) for ~/.uxplay.dacp;"
                            " use \"-dacp <fn>\"\n");
                    startup_exit(1);
                }
                dacpfile.append(homedir);
                dacpfile.append("/.uxplay.dacp");
            }
        } else if (arg == "-taper") {
            taper_volume = true;
        } else if (arg == "-db") {
            bool db_bad = true;
            double db1, db2;
            const char *text = "db value must be \"low\" or \"low:high\", low < 0 and high > low are decibel gains";
            if (i == argc - 1) {
                startup_message("invalid \"-db\": this option requires a value: %s\n", text) ;
                startup_exit(1);
            }
            char *end1, *end2;
            db1 = strtod(argv[i+1], &end1);
            if (end1 == argv[i+1] && argv[i+1][0] == '-') {
                startup_message("invalid \"-db\": this option requires a value: %s\n", text) ;
            startup_exit(1);
            }
            if (db1 < 0.0 && *end1 == ':') {
                db2 = strtod(++end1, &end2);
                if ( *end2 == '\0' && end2 > end1 && db1 < db2) {
                    db_bad = false;
                }
            } else  if (db1 < 0.0 && *end1 =='\0') {
                db_bad = false;
                db2 = 0.0;
            }
            if (db_bad) {
                startup_message("invalid \"-db %s\": %s\n", argv[i+1], text);
                startup_exit(1);
            }
            i++;
            db_low = db1;
            db_high = db2;
            LOGI("db range %f:%f", db_low, db_high);
        } else if (arg ==  "-vol") {
            if (!option_has_value(i, argc, arg, argv[i+1])) {
                startup_exit(1);
            }
            bool vol_bad = true;
            if (i < argc - 1) {
                char *end;
                double frac = strtod(argv[i+1], &end);
                /* end != argv[i+1] guards against -vol ""  */
                if (end != argv[i+1] && *end == '\0' && frac >= 0.0 && frac <= 1.0) {
                    if (frac == 0.0) {
                        initial_volume = -144.0;
                    } else if (frac == 1.0) {
                        initial_volume = 0.0;
                    } else {
                        double db_flat = -30.0  + 30.0*frac;
                        //double db = 10.0 * (log10(frac) / log10(2.0));  //tapered 
                        //printf("db %f db_flat %f \n", db, db_flat);
                        //db = (db > db_flat) ? db : db_flat;
                        initial_volume = db_flat;
                    }
                    LOGI("initial_volume attenuation %f db", initial_volume);
                    vol_bad = false;
                }
            }
            if (vol_bad) {
                startup_message("invalid \"-vol %s\", value must be between 0.0 (mute) and 1.0 (full volume)\n", argv[i+1]);  
                startup_exit(1);
            }
            i++;
        } else if (arg == "-h265") {
            h265_support = true;
            h265_user_requested = true;
            force_h264_only = false;
        } else if (arg == "-h264") {
            force_h264_only = true;
            h265_support = false;
        } else if (arg == "-gpu") {
            if (!option_has_value(i, argc, arg, argv[i+1])) startup_exit(1);
            std::string choice(argv[++i]);
            if (choice == "auto") {
                gpu_preference = 0;
            } else if (choice == "intel" || choice == "integrated") {
                gpu_preference = 1;
            } else if (choice == "nvidia" || choice == "discrete") {
                gpu_preference = 2;
            } else if (choice == "software" || choice == "warp") {
                gpu_preference = 3;
            } else {
                startup_message("invalid \"-gpu %s\": choices are auto, integrated (intel), discrete (nvidia), "
                        "software (warp)\n", choice.c_str());
                startup_exit(1);
            }
        } else if (arg == "-ui") {
            if (!option_has_value(i, argc, arg, argv[i+1])) startup_exit(1);
            std::string choice(argv[++i]);
            std::transform(choice.begin(), choice.end(), choice.begin(),
                           [](unsigned char c) { return (char) std::tolower(c); });
            /* the numbering of native_media_settings::ui_language */
            static const char *const language_codes[] = {"auto", "zh-tw", "zh-cn", "en", "ja", "ko",
                                                         "es",   "de",    "fr",    "vi", "th"};
            int chosen = -1;
            for (int code = 0; code < (int) (sizeof(language_codes) / sizeof(language_codes[0])); code++) {
                if (choice == language_codes[code]) {
                    chosen = code;
                }
            }
            if (choice == "zh-hant") {
                chosen = 1;
            } else if (choice == "zh-hans") {
                chosen = 2;
            }
            if (chosen >= 0) {
                ui_language_choice = chosen;
            } else {
                startup_message("invalid \"-ui %s\": choices are auto, en, zh-TW, zh-CN, ja, ko, es, de, fr, vi, th\n",
                                argv[i]);
                startup_exit(1);
            }
        } else if (arg == "-mode") {
            if (!option_has_value(i, argc, arg, argv[i+1])) startup_exit(1);
            std::string choice(argv[++i]);
            std::transform(choice.begin(), choice.end(), choice.begin(),
                           [](unsigned char c) { return (char) std::tolower(c); });
            if (choice == "quality") {
                mode_choice = NATIVE_MEDIA_MODE_QUALITY;
            } else if (choice == "balanced") {
                mode_choice = NATIVE_MEDIA_MODE_BALANCED;
            } else if (choice == "performance") {
                mode_choice = NATIVE_MEDIA_MODE_PERFORMANCE;
            } else {
                startup_message("invalid \"-mode %s\": choices are quality, balanced, performance\n", argv[i]);
                startup_exit(1);
            }
        } else if (arg == "-crop") {
            if (!option_has_value(i, argc, arg, argv[i+1])) startup_exit(1);
            std::string choice(argv[++i]);
            std::transform(choice.begin(), choice.end(), choice.begin(),
                           [](unsigned char c) { return (char) std::tolower(c); });
            if (choice == "auto") {
                crop_choice = 1;
            } else if (choice == "off") {
                crop_choice = 0;
            } else {
                startup_message("invalid \"-crop %s\": choices are auto, off\n", argv[i]);
                startup_exit(1);
            }
        } else if (arg == "-nofreeze") {
            nofreeze = true;
        } else if (skip_unsupported_option(arg, i, argc, argv)) {
            continue;
        } else {
            startup_message("unknown option %s, stopping (for help use option \"-h\")\n",argv[i]);
            startup_exit(1);
        }
    }
}

static void process_metadata(int count, const char *dmap_tag, const unsigned char* metadata, int datalen, std::string *metadata_text) {
    int dmap_type = 0;
    /* DMAP metadata items can be strings (dmap_type = 9); other types are byte, short, int, long, date, and list.  *
     * The DMAP item begins with a 4-character (4-letter) "dmap_tag" string that identifies the type.               */

    LOGD("%d: dmap_tag [%s], %d", count, dmap_tag, datalen);

    /* UTF-8 String-type DMAP tags seen in Apple Music Radio are processed here.   *
     * (DMAP tags "asal", "asar", "ascp", "asgn", "minm" ). TODO expand this */  
    
    if (datalen == 0) {
        return;
    }

    if (dmap_tag[0] == 'a' && dmap_tag[1] == 's') {
        dmap_type = 9;
        switch (dmap_tag[2]) {
        case 'a':
            switch (dmap_tag[3]) {
            case 'a':
                metadata_text->append("Album artist: ");  /*asaa*/
                break;
            case 'l':
                metadata_text->append("Album: ");  /*asal*/
                break;
            case 'r':
                metadata_text->append("Artist: ");  /*asar*/
                break;
            default:
                dmap_type = 0;
                break;
            }
            break;    
        case 'c':
            switch (dmap_tag[3]) {
            case 'm':
                metadata_text->append("Comment: ");  /*ascm*/
                break;
            case 'n':
                metadata_text->append("Content description: ");  /*ascn*/
                break;
            case 'p':
                metadata_text->append("Composer: ");  /*ascp*/
                break;
            case 't':
                metadata_text->append("Category: ");  /*asct*/
                break;
            default:
                dmap_type = 0;
                break;
            }
            break;
        case 's':
            switch (dmap_tag[3]) {
            case 'a':
                metadata_text->append("Sort Artist: "); /*assa*/
                break;
            case 'c':
                metadata_text->append("Sort Composer: ");  /*assc*/
                break;
            case 'l':
                metadata_text->append("Sort Album artist: ");  /*assl*/
                break;
            case 'n':
                metadata_text->append("Sort Name: ");  /*assn*/
                break;
            case 's':
                metadata_text->append("Sort Series: ");  /*asss*/
                break;
            case 'u':
                metadata_text->append("Sort Album: ");  /*assu*/
                break;
            default:
                dmap_type = 0;
                break;
            }
            break;
        default:
	    if (strcmp(dmap_tag, "asdt") == 0) {
                metadata_text->append("Description: ");
            } else if (strcmp (dmap_tag, "asfm") == 0) {
                metadata_text->append("Format: ");
            } else if (strcmp (dmap_tag, "asgn") == 0) {
                metadata_text->append("Genre: ");
            } else if (strcmp (dmap_tag, "asky") == 0) {
                metadata_text->append("Keywords: ");
            } else if (strcmp (dmap_tag, "aslc") == 0) {
                metadata_text->append("Long Content Description: ");
            } else {
                dmap_type = 0;
            }
            break;
        }
    } else if (strcmp (dmap_tag, "minm") == 0) {
        dmap_type = 9;
        metadata_text->append("Title: ");
    }

    if (dmap_type == 9) {
        char *str = (char *) calloc(datalen + 1, sizeof(char));
        if (!str) {
            /* a client's metadata must never end the program */
            LOGE("metadata item [%s]: no memory for %d bytes; item ignored", dmap_tag, datalen);
            return;
        }
        memcpy(str, metadata, datalen);
        metadata_text->append(str);
        metadata_text->append("\n");
        free(str);
    } else if (debug_log) {
        std::string md = "";
        char hex[4];
        for (int i = 0; i < datalen; i++) {
            if (i > 0 && i % 16 == 0) {
                md.append("\n");
            }
            snprintf(hex, 4, "%2.2x ", (int) metadata[i]);
            md.append(hex);
        }
        LOGI("%s", md.c_str());
    }
}

static int parse_dmap_header(const unsigned char *metadata, char *tag, int *len) {
    const unsigned char *header = metadata;

    bool istag = true;
    for (int i = 0; i < 4; i++) {
        tag[i] =  (char) *header;
        if (!isalpha(tag[i])) {
            istag = false;
        }
        header++;
    }

    *len = 0;
    for (int i = 0; i < 4; i++) {
        *len <<= 8;
        *len += (int) *header;
        header++;
    }
    if (!istag || *len < 0) {
        return 1;
    }
    return 0;
}

static int register_dnssd() {
    int dnssd_error;

    dnssd_error = dnssd_register_raop(dnssd, raop_port);
    if (dnssd_error) {
        LOGE("dnssd_register_raop failed with error code %d", dnssd_error);
        dnssd_error_text(&dnssd_error, appname);
        return -3;
    }

    dnssd_error = dnssd_register_airplay(dnssd, airplay_port);
    if (dnssd_error) {
        LOGE("dnssd_register_airplay failed with error code %d", dnssd_error);
        dnssd_error_text(&dnssd_error, appname);
        return -4;
    }

    LOGD("register_dnssd: advertised AirPlay service with \"Features\" code = 0x%llX",
         dnssd_get_airplay_features(dnssd));
    return 0;
}

static void unregister_dnssd() {
    if (dnssd) {
        dnssd_unregister_raop(dnssd);
        dnssd_unregister_airplay(dnssd);
    }
    return;
}

static void stop_dnssd() {
    if (dnssd) {
        unregister_dnssd();
        dnssd_destroy(dnssd);
        dnssd = NULL;
	return;
    }	
}

static int start_dnssd(std::vector<char> hw_addr, std::string name) {
    int dnssd_error;
    if (dnssd) {
        LOGE("start_dnssd error: dnssd != NULL");
        return 2;
    }
    /* pin_pw controls client access
       pin_pw = 1: client must enter pin displayed onscreen (first access only, remembered by client, coupled to UxPlay's deviceID)
              = 2: client must enter password (same password for all clients)
              = 3: client must enter randomly selected 4-digit password displayed like an on-screen pin (every access)
              = 0: no access control
    */
    dnssd = dnssd_init(name.c_str(), strlen(name.c_str()), hw_addr.data(), hw_addr.size(), pin_pw, &dnssd_error);
    if (dnssd_error) {
        LOGE("Could not initialize dnssd library!: error %d", dnssd_error);
        return 1;
    }

    /* the user's saved "USB first" / "Wi-Fi only" choice applies from the
       first advertisement on */
    {
        bool prefer_usb = native_media_prefer_usb_link();
        dnssd_set_usb_link_allowed(dnssd, prefer_usb ? 1 : 0);
        LOGI("AirPlay link preference: %s", prefer_usb ? "USB first (Wi-Fi when no USB link)"
                                                       : "Wi-Fi only (the USB cable is not used for AirPlay)");
    }

    /* after dnssd starts, reset the default feature set here 
     * (overwrites features set in dnssdint.h)
     * default: FEATURES_1 = 0x5A7FFEE6, FEATURES_2 = 0 */

    dnssd_set_airplay_features(dnssd,  0, 0); // AirPlay video supported 
    dnssd_set_airplay_features(dnssd,  1, 1); // photo supported 
    dnssd_set_airplay_features(dnssd,  2, 1); // video protected with FairPlay DRM 
    dnssd_set_airplay_features(dnssd,  3, 0); // volume control supported for videos

    dnssd_set_airplay_features(dnssd,  4, 0); // http live streaming (HLS) supported
    dnssd_set_airplay_features(dnssd,  5, 1); // slideshow supported 
    dnssd_set_airplay_features(dnssd,  6, 1); // 
    dnssd_set_airplay_features(dnssd,  7, 1); // mirroring supported

    dnssd_set_airplay_features(dnssd,  8, 0); // screen rotation  supported 
    dnssd_set_airplay_features(dnssd,  9, 1); // audio supported 
    dnssd_set_airplay_features(dnssd, 10, 1); //  
    dnssd_set_airplay_features(dnssd, 11, 1); // audio packet redundancy supported

    dnssd_set_airplay_features(dnssd, 12, 1); // FaiPlay secure auth supported 
    dnssd_set_airplay_features(dnssd, 13, 1); // photo preloading  supported 
    dnssd_set_airplay_features(dnssd, 14, 1); // Authentication bit 4:  FairPlay authentication
    dnssd_set_airplay_features(dnssd, 15, 1); // Metadata bit 1 support:   Artwork 

    dnssd_set_airplay_features(dnssd, 16, 1); // Metadata bit 2 support:  Soundtrack  Progress 
    dnssd_set_airplay_features(dnssd, 17, 1); // Metadata bit 0 support:  Text (DAACP) "Now Playing" info.
    dnssd_set_airplay_features(dnssd, 18, 1); // Audio format 1 support:   
    dnssd_set_airplay_features(dnssd, 19, 1); // Audio format 2 support: must be set for AirPlay 2 multiroom audio 

    dnssd_set_airplay_features(dnssd, 20, 1); // Audio format 3 support: must be set for AirPlay 2 multiroom audio 
    dnssd_set_airplay_features(dnssd, 21, 1); // Audio format 4 support:
    dnssd_set_airplay_features(dnssd, 22, 1); // Authentication type 4: FairPlay authentication
    dnssd_set_airplay_features(dnssd, 23, 0); // Authentication type 1: RSA Authentication

    dnssd_set_airplay_features(dnssd, 24, 0); // 
    dnssd_set_airplay_features(dnssd, 25, 1); // 
    dnssd_set_airplay_features(dnssd, 26, 0); // Has Unified Advertiser info
    dnssd_set_airplay_features(dnssd, 27, 1); // Supports Legacy Pairing

    dnssd_set_airplay_features(dnssd, 28, 1); //  
    dnssd_set_airplay_features(dnssd, 29, 0); // 
    dnssd_set_airplay_features(dnssd, 30, 1); // RAOP support: with this bit set, the AirTunes service is not required. 
    dnssd_set_airplay_features(dnssd, 31, 0); // 


    /*  bits 32-63: see  https://emanualcozzi.net/docs/airplay2/features 
    dnssd_set_airplay_features(dnssd, 32, 0); // isCarPlay when ON,; Supports InitialVolume when OFF
    dnssd_set_airplay_features(dnssd, 33, 0); // Supports Air Play Video Play Queue
    dnssd_set_airplay_features(dnssd, 34, 0); // Supports Air Play from cloud (requires that bit 6 is ON)
    dnssd_set_airplay_features(dnssd, 35, 0); // Supports TLS_PSK

    dnssd_set_airplay_features(dnssd, 36, 0); //
    dnssd_set_airplay_features(dnssd, 37, 0); //
    dnssd_set_airplay_features(dnssd, 38, 0); //  Supports Unified Media Control (CoreUtils Pairing and Encryption)
    dnssd_set_airplay_features(dnssd, 39, 0); //

    dnssd_set_airplay_features(dnssd, 40, 0); // Supports Buffered Audio
    dnssd_set_airplay_features(dnssd, 41, 0); // Supports PTP
    dnssd_set_airplay_features(dnssd, 42, 0); // Supports Screen Multi Codec (allows h265 video)
    dnssd_set_airplay_features(dnssd, 43, 0); // Supports System Pairing

    dnssd_set_airplay_features(dnssd, 44, 0); // is AP Valeria Screen Sender
    dnssd_set_airplay_features(dnssd, 45, 0); //
    dnssd_set_airplay_features(dnssd, 46, 0); // Supports HomeKit Pairing and Access Control
    dnssd_set_airplay_features(dnssd, 47, 0); //

    dnssd_set_airplay_features(dnssd, 48, 0); // Supports CoreUtils Pairing and Encryption
    dnssd_set_airplay_features(dnssd, 49, 0); //
    dnssd_set_airplay_features(dnssd, 50, 0); // Metadata bit 3: "Now Playing" info sent by bplist not DAACP test
    dnssd_set_airplay_features(dnssd, 51, 0); // Supports Unified Pair Setup and MFi Authentication

    dnssd_set_airplay_features(dnssd, 52, 0); // Supports Set Peers Extended Message
    dnssd_set_airplay_features(dnssd, 53, 0); //
    dnssd_set_airplay_features(dnssd, 54, 0); // Supports AP Sync
    dnssd_set_airplay_features(dnssd, 55, 0); // Supports WoL

    dnssd_set_airplay_features(dnssd, 56, 0); // Supports Wol
    dnssd_set_airplay_features(dnssd, 57, 0); //
    dnssd_set_airplay_features(dnssd, 58, 0); // Supports Hangdog Remote Control
    dnssd_set_airplay_features(dnssd, 59, 0); // Supports AudioStreamConnection setup

    dnssd_set_airplay_features(dnssd, 60, 0); // Supports Audo Media Data Control         
    dnssd_set_airplay_features(dnssd, 61, 0); // Supports RFC2198 redundancy
    */

    /* bits 0 and 4 (AirPlay video, HLS) stay off: HLS playback is not part
       of the Windows-native build */

    /* needed for h265 video support */
    dnssd_set_airplay_features(dnssd, 42, (int) h265_support);

    /* bit 27 of Features determines whether the AirPlay2 client-pairing protocol will be used (1) or not (0) */
    dnssd_set_airplay_features(dnssd, 27, (int) setup_legacy_pairing);
    return 0;
}

static bool check_client(char *deviceid) {
    bool ret = false;
    int list =  allowed_clients.size();
    for (int i = 0; i < list ; i++) {
        if (!strcmp(deviceid,allowed_clients[i].c_str())) {
	    ret = true;
	    break;
        }
    }
    return ret;
}

static bool check_blocked_client(char *deviceid) {
    bool ret = false;
    int list =  blocked_clients.size();
    for (int i = 0; i < list ; i++) {
        if (!strcmp(deviceid,blocked_clients[i].c_str())) {
	    ret = true;
	    break;
        }
    }
    return ret;
}

// Server callbacks


//to be simplified

extern "C" void video_reset(void *cls, reset_type_t type) {
    switch (type) {
    case RESET_TYPE_NOHOLD:
        LOGD("video_reset: type = NoHold");
        if (use_video) {
            /* stop the video immediately to avoid a timing issue if we wait for main_loop to reset */
            native_media_airplay_video_stop();
            close_window = false;  // we already closed the window
        }
        /* the old client's sound ends here, not in the relaunch block, which
           may run after the new client has configured its own */
        if (use_audio) {
            native_media_airplay_audio_stop();
        }
        /* the silence of the old connection is not the new one's */
        missed_feedback = 0;
        connection_handed_over = true;
        relaunch_video = true;
        break;
    case RESET_TYPE_RTP_SHUTDOWN:
        LOGD("video_reset: type = RTP_Shutdown");
        if (use_video) {
            native_media_airplay_video_stop();
        }
        relaunch_video = true;
        break;
    default:
        LOGE("video_reset: unexpected reset type %d: ignored", (int) type);
        return;
    }
    event_loop.request_reset();
}

extern "C" int video_set_codec_and_size(void *cls, video_codec_t codec, unsigned short width_source,
                                        unsigned short height_source, unsigned short x_offset,
                                        unsigned short y_offset, unsigned short width, unsigned short height) {
    if (use_video) {
        native_media_airplay_video_configure(codec == VIDEO_CODEC_H265 ? 1 : 0, width, height);
    }
    return 0;
}

extern "C" void display_pin(void *cls, char *pin) {
    int margin = 10;
    int spacing = 3;
    char *image = create_pin_display(pin, margin, spacing);
    if (!image) {
        LOGE("create_pin_display could not create pin image, pin = %s", pin);
    } else {
        LOGI("%s\n",image);
        free (image);
    }
    /* no console window in the native build: show the PIN as a notification */
    native_media_notify_pin(pin);
}

extern "C" const char *passwd(void *cls, int *len){
    if (pin_pw == 2) {
        *len = password.size();
        return password.c_str();
    } else if  (pin_pw == 3) {
        *len = -1;
    } else {
        *len = 0;   /* no password used */
    }
    return NULL;
}

/* this session's offer of media remote control was noted in the log */
static std::atomic<bool> remote_control_noted{false};

extern "C" void export_dacp(void *cls, const char *active_remote, const char *dacp_id) {
    /* A client that sends these identifiers accepts playback commands from
       the receiver (DACP: play/pause, next, previous, volume).  Audio-only
       AirPlay usually does; whether screen mirroring does is what this line
       shows.  The Active-Remote value is a session token and is not logged. */
    if (!remote_control_noted.exchange(true)) {
        LOGI("the client offers media remote control: Active-Remote present, DACP-ID %s",
             dacp_id ? "present" : "absent");
    }
      if (dacpfile.length()) {
        FILE *fp = fopen(dacpfile.c_str(), "w");
        if (fp) {
            fprintf(fp,"%s\n%s\n", dacp_id ? dacp_id : "", active_remote ? active_remote : "");
            fclose(fp);
        } else {
            LOGE("failed to open DACP export file \"%s\"", dacpfile.c_str());
        }
    }
}

extern "C" void conn_init (void *cls) {
    open_connections++;
    LOGD("Open connections: %u", open_connections.load());
    if (dnssd) {
        /* keep advertising on the session's link while it is open */
        dnssd_set_session_active(dnssd, 1);
    }
}

/* "Wi-Fi only" is a hard rule: an AirPlay connection that arrives over the
 * iPhone USB link is refused, whatever address the phone resolved or cached
 * (RAOP connection thread).  The phone then connects over Wi-Fi, where the
 * service is advertised. */
extern "C" bool conn_admit (void *cls, const char *local_address, const char *remote_address) {
    (void) cls;
    if (!native_media_prefer_usb_link() && native_media_is_usb_link_address(local_address)) {
        LOGI("refused an AirPlay connection from %s on the iPhone USB link (%s): the link preference is "
             "\"Wi-Fi only\"", remote_address, local_address);
        return false;
    }
    return true;
}

extern "C" void conn_transport (void *cls, const char *local_address) {
    (void) cls;
    /* label the window "USB" or "Wi-Fi" by the link the AirPlay client used */
    current_connection_over_usb = native_media_set_airplay_local_address(local_address) != 0;
}

extern "C" void conn_destroy (void *cls) {
    unsigned int const remaining = --open_connections;
    LOGD("Open connections: %u", remaining);
    if (remaining == 0) {
        remote_control_noted = false;
        if (dnssd) {
            /* free to move the advertisement to a newly preferred link */
            dnssd_set_session_active(dnssd, 0);
        }
        if (use_audio) {
            native_media_airplay_audio_stop();
        }
        if (dacpfile.length()) {
            remove (dacpfile.c_str());
        }
    }
}

extern "C" void conn_feedback (void *cls) {
    /* received client heartbeat signal: connection still exists */
    missed_feedback = 0;
}

extern "C" void conn_reset (void *cls, int reason) {
    switch (reason) {
    case 1:
        LOGI("*** ERROR lost connection with client (network problem?)");
        break;
    case 2:
        LOGI("*** ERROR the client's mirror stream was invalid or could not be read: resetting the connection");
        break;
    default:
        LOGI("*** ERROR connection reset requested (reason %d)", reason);
        break;
    }

    if (!nofreeze) {
        close_window = false;    /* leave "frozen" window open */
    }
    reset_httpd = true;
    relaunch_video = true;
    event_loop.request_reset();
}

extern "C" void report_client_request(void *cls, char *deviceid, char * model, char *name, bool * admit) {
    LOGI("connection request from %s (%s) with deviceID = %s\n", name, model, deviceid);
    if (restrict_clients) {
        *admit = check_client(deviceid);
        if (*admit == false) {
            LOGI("client connections have been restricted to those with listed deviceID,\nuse \"-allow %s\" to allow this client to connect.\n",
                 deviceid);
        }
    } else {
        *admit = true;
    }
    if (check_blocked_client(deviceid)) {
        *admit = false;
        LOGI("*** attempt to connect by blocked client (clientID %s): DENIED\n", deviceid);
    }
}

extern "C" void audio_process (void *cls, raop_ntp_t *ntp, audio_decode_struct *data) {
    if (dump_audio) {
        dump_audio_to_file(data->data, data->data_len, (data->data)[0] & 0xf0);
    }
    if (use_audio) {
        /* -vsync/-async delays are applied by the engine (native_media_airplay_audio_configure) */
        native_media_airplay_audio_packet(data->data, data->data_len, data->ntp_time_local, data->ntp_time_remote);
    }
}

extern "C" void video_process (void *cls, raop_ntp_t *ntp, video_decode_struct *data) {
    if (dump_video) {
        dump_video_to_file(data->data, data->data_len);
    }
    if (use_video) {
        native_media_airplay_video_frame(data->data, data->data_len, data->ntp_time_local, data->ntp_time_remote);
    }
}

extern "C" void video_pause (void *cls) {
    if (use_video) {
        native_media_airplay_video_pause();
    }
}

extern "C" void video_resume (void *cls) {
    if (use_video) {
        native_media_airplay_video_resume();
    }
}

extern "C" double audio_set_client_volume(void *cls) {
    return current_volume;
}

//callback from RAOP_SET_PARAMETER "volume" handler
extern "C" void audio_set_volume (void *cls, float volume) {
    double db, db_flat, frac, linear_gain;
    if (!use_audio) {
      return;
    }
    /* convert from AirPlay dB  volume in range {-30dB : 0dB}, to a linear gain */
    if (volume == -144.0f) {   /* AirPlay "mute" signal */
        frac = 0.0;
    } else if (volume < -30.0f) {
        LOGE(" invalid AirPlay volume %f < -30.0, use -30.0", volume);
        frac = 0.0;
        volume = -30.0f;
    } else if (volume > 0.0f) {
        LOGE(" invalid AirPlay volume %f > 0.0, use 0.0", volume);
        frac = 1.0;
        volume = 0.0f;
    } else if (volume == -30.0f) {
        frac = 0.0;
    } else if (volume == 0.0f) {
        frac = 1.0;
    } else {
        frac = (double) ( (30.0f + volume) / 30.0f);
        frac = (frac > 1.0) ? 1.0 : frac;
    }
    current_volume = (double) volume;
    /* frac is length of volume slider as fraction of max length */
    /* also (steps/16) where steps is number of discrete steps above mute (16 = full volume) */
    if (frac == 0.0) {
        linear_gain = 0.0;
    } else {
      /* flat rescaling of decibel range from {-30dB : 0dB} to {db_low : db_high} */  
        db_flat = db_low + (db_high-db_low) * frac;
        if (taper_volume) {
            /* taper the volume reduction by the (rescaled) Airplay {-30:0} range so each reduction of
             * the remaining slider length by 50% reduces the perceived volume by 50% (-10dB gain)
             * (This is the "dasl-tapering" scheme offered by shairport-sync) */
            db = db_high + 10.0 * (log10(frac) / log10(2.0));
            db = (db  > db_flat) ? db : db_flat;
        } else {
            db = db_flat;
        }
        /* conversion from (gain) decibels to a linear amplitude scale */
        linear_gain = pow(10.0, 0.05*db);
    }
    native_media_set_volume(linear_gain);
    LOGI("AirPlay volume %.1f dB (slider %.0f%%): gain %.3f", (double) volume, 100.0 * frac, linear_gain);
}

extern "C" void audio_get_format (void *cls, unsigned char *ct, unsigned short *spf, bool *usingScreen, bool *isMedia, uint64_t *audioFormat) {
    unsigned char type;
    LOGI("ct=%d spf=%d usingScreen=%d isMedia=%d  audioFormat=0x%lx",*ct, *spf, *usingScreen, *isMedia, (unsigned long) *audioFormat);
    switch (*ct) {
    case 2:
        type = 0x20;
        break;
    case 8:
        type = 0x80;
        break;
    default:
        type = 0x10;
        break;
    }
    if (audio_dumpfile && type != audio_type) {
        fclose(audio_dumpfile);
        audio_dumpfile = NULL;
    }
    audio_type = type;
    
    if (use_audio) {
        bool const alac = (*ct == 2);
        native_media_airplay_audio_configure(*ct, alac ? audio_sync : video_sync,
                                             (double) (alac ? audio_delay_alac : audio_delay_aac) / 1e6);
    }

    if (coverart_filename.length()) {
        write_coverart(coverart_filename.c_str(), (const void *) empty_image, sizeof(empty_image));
    }
    if (metadata_filename.length()) {
        write_metadata(metadata_filename.c_str(), "no data\n");
    }
}

extern "C" void audio_set_coverart(void *cls, const void *buffer, int buflen) {
    if (buffer && coverart_filename.length()) {
        write_coverart(coverart_filename.c_str(), buffer, buflen);
        LOGI("coverart size %d written to %s", buflen,  coverart_filename.c_str());
    }
}

/* track position reported by the client (ALAC audio, 44.1 kHz RTP clock) */
extern "C" void audio_set_progress(void *cls, uint32_t *start, uint32_t *curr, uint32_t *end) {
    if (*curr < *start || *curr > *end) {
        return;
    }
    int duration = (int) ((*end - *start) / 44100);
    int position = (int) ((*curr - *start) / 44100);
    int remain = duration - position;
    LOGI("audio progress (min:sec): %3d:%2.2d; remaining: %3d:%2.2d; track length %d:%2.2d",
         position / 60, position % 60, remain / 60, remain % 60, duration / 60, duration % 60);
}

extern "C" void audio_set_metadata(void *cls, const void *buffer, int buflen) {
    char dmap_tag[5] = {0x0};
    const unsigned char *metadata = (const  unsigned char *) buffer;
    int datalen;
    int count = 0;

    LOGI("====================Audio Metadata==================");

    if (buflen < 8) {
        LOGE("received invalid metadata, length %d < 8", buflen);
        return;
    } else if (parse_dmap_header(metadata, dmap_tag, &datalen)) {
        LOGE("received invalid metadata, tag [%s]  datalen %d", dmap_tag, datalen);
        return;
    }
    metadata += 8;
    buflen -= 8;

    if (strcmp(dmap_tag, "mlit") != 0 || datalen != buflen) {
        LOGE("received metadata with tag %s, but is not a DMAP listingitem, or datalen = %d !=  buflen %d",
             dmap_tag, datalen, buflen);
        return;
    }
    std::string metadata_text = "";
    while (buflen >= 8) {
        count++;
        if (parse_dmap_header(metadata, dmap_tag, &datalen)) {
            LOGE("received metadata with invalid DMAP header:  tag = [%s],  datalen = %d", dmap_tag, datalen);
            return;
        }
        metadata += 8;
        buflen -= 8;
        if (datalen < 0 || datalen > buflen) {
            /* the length comes from the client: never read past what was received */
            LOGE("received metadata whose item [%s] claims %d bytes while %d remain: ignored", dmap_tag, datalen,
                 buflen);
            return;
        }
        process_metadata(count, (const char *) dmap_tag, metadata, datalen, &metadata_text);
        metadata += datalen;
        buflen -= datalen;
    }
    LOGI("%s", metadata_text.c_str());
    if (metadata_filename.length()) {
        write_metadata(metadata_filename.c_str(), metadata_text.c_str());
    }
    if (buflen != 0) {
        LOGE("%d bytes of metadata were not processed", buflen);
    }
}

extern "C" void register_client(void *cls, const char *device_id, const char *client_pk, const char *client_name) {
    if (!registration_list) {
      /* we are not maintaining a list of registered clients */
        return;
    }
    LOGI("registered new client: %s DeviceID = %s PK = \n%s", client_name, device_id, client_pk);
    registered_keys.push_back(client_pk);
    if (strlen(pairing_register.c_str())) {
        FILE *fp = fopen600(pairing_register.c_str(), "a");
        if (fp) {
            time_t raw_time = time(NULL);
            struct tm *tm_info = localtime(&raw_time);
            char time_str[20];
            strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", tm_info);
            fprintf(fp, "%s,%s,%s,%s\n", client_pk, time_str, device_id, client_name);
            fclose(fp);
        }
    }
}

extern "C" bool check_register(void *cls, const char *client_pk) {
    if (!registration_list) {
        /* we are not maintaining a list of registered clients */
        return true;
    }
    LOGD("check returning client's pairing registration");
    std::string pk = client_pk;
    if (std::find(registered_keys.rbegin(), registered_keys.rend(), pk) != registered_keys.rend()) {
        LOGD("registration found: PK=%s", client_pk);
        return true;
    } else {
        LOGE("returning client's pairing registration not found: PK=%s", client_pk);
        return false;
    }
}

extern "C" void log_callback (void *cls, int level, const char *msg) {
    switch (level) {
    case LOGGER_DEBUG:
        LOGD("%s", msg);
        break;
    case LOGGER_WARNING:
        LOGW("%s", msg);
        break;
    case LOGGER_INFO:
        LOGI("%s", msg);
        break;
    case LOGGER_ERR:
        LOGE("%s", msg);
        break;
    default:
        break;
    }
}

static void native_log_callback(int level, const char *message) {
    log(level, "%s", message);
}

/* the user closed the video window while AirPlay was shown: same handling
 * as upstream UxPlay's "Output window was closed" (drop the client) */
static void on_window_closed() {
    LOGI("video window closed: disconnecting the AirPlay client");
    event_loop.quit();
}

/* "USB first" / "Wi-Fi only" chosen in the tray (main loop thread): move the
 * mDNS advertisement, and end a session that runs on the other link so the
 * iPhone reconnects on the chosen one (it does not reconnect by itself).
 * The USB device is never touched, so a charging cable keeps charging. */
static void on_link_preference_changed() {
    bool prefer_usb = native_media_prefer_usb_link();
    if (dnssd) {
        dnssd_set_usb_link_allowed(dnssd, prefer_usb ? 1 : 0);
    }
    bool wrong_link = false;
    if (open_connections > 0) {
        bool const over_usb = current_connection_over_usb.load();
        wrong_link = prefer_usb ? (!over_usb && native_media_usb_link_up()) : over_usb;
    }
    if (wrong_link) {
        LOGI("AirPlay link preference changed: ending the current %s session so the iPhone reconnects over %s",
             prefer_usb ? "Wi-Fi" : "USB", prefer_usb ? "USB" : "Wi-Fi");
    }
    native_media_link_preference_applied(wrong_link);
    if (wrong_link) {
        reset_httpd = true;
        relaunch_video = true;
        event_loop.quit();
    }
}

static void stop_raop_server () {
    if (raop) {
        raop_destroy(raop);
        raop = NULL;
    }
}

static int exit_code_requested = 0;   /* main thread: exit code of an exit that follows a failure */

/* ends the program from the main loop after a failure that was reported
   with startup_message()/LOGE (shown in a dialog when there is no terminal) */
static void exit_after_failure(void) {
    exit_code_requested = 1;
    exit_requested = true;
    event_loop.quit();
}

/* The picture size and rate asked from the client, by mode:
 *   quality, balanced: the monitor's resolution and refresh rate;
 *   performance:       at most 1920x1080, which the phone encodes and this PC
 *                      decodes with far less work (lowest delay and memory);
 *   balanced without any hardware video decoder uses that smaller box too.
 * "-s" and "-fps" given by the user are left alone.  The client reads these
 * values when it connects, so a change applies to the next connection. */
static void choose_stream_geometry(void) {
    native_media_display_info info;
    if (!native_media_query_display(&info)) {
        return;
    }
    int const mode = native_media_performance_mode();
    bool const software_decode = !info.h264_hardware && !(h265_support && info.hevc_hardware);
    bool const light_stream = mode == NATIVE_MEDIA_MODE_PERFORMANCE ||
                              (mode == NATIVE_MEDIA_MODE_BALANCED && software_decode);
    if (!user_set_display) {
        unsigned width = (unsigned) info.width;
        unsigned height = (unsigned) info.height;
        if (light_stream) {
            unsigned const long_limit = 1920;
            unsigned const short_limit = 1080;
            if (width >= height) {
                width = std::min(width, long_limit);
                height = std::min(height, short_limit);
            } else {
                width = std::min(width, short_limit);
                height = std::min(height, long_limit);
            }
        }
        display[0] = (unsigned short) std::min(width, 9999u);
        display[1] = (unsigned short) std::min(height, 9999u);
        display[2] = (unsigned short) std::min(info.refresh_hz, 255);
    }
    if (!user_set_fps) {
        display[3] = (unsigned short) std::min(info.refresh_hz, 60);
    }
    if (raop) {
        if (display[0]) raop_set_plist(raop, "width", (int) display[0]);
        if (display[1]) raop_set_plist(raop, "height", (int) display[1]);
        if (display[2]) raop_set_plist(raop, "refreshRate", (int) display[2]);
        if (display[3]) raop_set_plist(raop, "maxFPS", (int) display[3]);
    }
    LOGI("advertising display %ux%u @ %u Hz, max %u fps, codecs %s (mode %s%s%s)", display[0], display[1],
         display[2], display[3], h265_support ? "H.265 + H.264" : "H.264",
         mode == NATIVE_MEDIA_MODE_QUALITY ? "quality" : (mode == NATIVE_MEDIA_MODE_PERFORMANCE ? "performance"
                                                                                                : "balanced"),
         software_decode ? ", no hardware video decoder" : "", user_set_display ? ", size set with -s" : "");
}

/* a mode was picked in the menu (main loop thread) */
static void on_performance_mode_changed() {
    choose_stream_geometry();
    native_media_performance_mode_applied(open_connections > 0 && !user_set_display);
}

/* the engine could not start: there is no window and no tray icon, so the
   program must not stay behind as an invisible, advertised receiver */
static void on_engine_start_failed() {
    startup_message("the media engine could not start: %s", native_media_failure_text());
    exit_after_failure();
}

/* Windows shuts down or the user signs out: the engine's window thread waits
   (bounded) for the goodbye on the network before the process is ended */
static void on_session_ending() {
    LOGI("Windows session is ending: stopping the AirPlay server");
    stop_raop_server();
    stop_dnssd();
    native_media_network_stopped();
    exit_requested = true;
    event_loop.quit();
}

/* engine threads (window, notification area, statistics): handled on the main loop */
static void native_event_callback(native_media_event event, void *user_data) {
    (void) user_data;
    switch (event) {
    case NATIVE_MEDIA_EVENT_WINDOW_CLOSED:
        event_loop.post(on_window_closed);
        break;
    case NATIVE_MEDIA_EVENT_EXIT_REQUESTED:
        LOGI("exit requested from the notification-area menu");
        request_exit();
        break;
    case NATIVE_MEDIA_EVENT_LINK_PREFERENCE_CHANGED:
        event_loop.post(on_link_preference_changed);
        break;
    case NATIVE_MEDIA_EVENT_PERFORMANCE_MODE_CHANGED:
        event_loop.post(on_performance_mode_changed);
        break;
    case NATIVE_MEDIA_EVENT_DISPLAY_CHANGED:
        event_loop.post(choose_stream_geometry);
        break;
    case NATIVE_MEDIA_EVENT_START_FAILED:
        event_loop.post(on_engine_start_failed);
        break;
    case NATIVE_MEDIA_EVENT_DEVICE_LOST:
        /* the device cannot be re-created in place: start afresh (0.5 s),
           the phone then mirrors again when the user picks the receiver */
        LOGE("the graphics device was lost (driver update or reset): restarting the program");
        restart_after_exit = true;
        request_exit();
        break;
    case NATIVE_MEDIA_EVENT_SESSION_ENDING:
        event_loop.post(on_session_ending);
        break;
    default:
        break;
    }
}

/* Creates the AirPlay protocol core and starts its RTSP server.  On failure
 * the reason has been reported (startup_message) and the caller stops what
 * was created with stop_raop_server(). */
static int start_raop_server (unsigned short display[5], unsigned short tcp[3], unsigned short udp[3], bool debug_log) {
    (void) debug_log;
    raop_callbacks_t raop_cbs;
    memset(&raop_cbs, 0, sizeof(raop_cbs));
    raop_cbs.conn_init = conn_init;
    raop_cbs.conn_destroy = conn_destroy;
    raop_cbs.conn_transport = conn_transport;
    raop_cbs.conn_admit = conn_admit;
    raop_cbs.conn_reset = conn_reset;
    raop_cbs.conn_feedback = conn_feedback;
    raop_cbs.audio_process = audio_process;
    raop_cbs.video_process = video_process;
    raop_cbs.video_pause = video_pause;
    raop_cbs.video_resume = video_resume;
    raop_cbs.audio_set_client_volume = audio_set_client_volume;
    raop_cbs.audio_set_volume = audio_set_volume;
    raop_cbs.audio_get_format = audio_get_format;
    raop_cbs.audio_set_metadata = audio_set_metadata;
    raop_cbs.audio_set_coverart = audio_set_coverart;
    raop_cbs.audio_set_progress = audio_set_progress;
    raop_cbs.report_client_request = report_client_request;
    raop_cbs.display_pin = display_pin;
    raop_cbs.register_client = register_client;
    raop_cbs.check_register = check_register;
    raop_cbs.passwd = passwd;
    raop_cbs.export_dacp = export_dacp;
    raop_cbs.video_reset = video_reset;
    raop_cbs.video_set_codec_and_size = video_set_codec_and_size;

    raop = raop_init(&raop_cbs);
    if (raop == NULL) {
        startup_message("the AirPlay protocol core could not be created (raop_init)");
        return -1;
    }
    raop_set_log_callback(raop, log_callback, NULL);
    raop_set_log_level(raop, log_level);
    /* set nohold = 1 to allow  capture by new client */
    if (raop_init2(raop, nohold, mac_address.c_str(), keyfile.c_str())){
        startup_message("the AirPlay protocol core could not be initialized (raop_init2, key file \"%s\")",
                        keyfile.c_str());
        free (raop);
        raop = NULL;
        return -1;
    }

    /* write desired display pixel width, pixel height, refresh_rate, max_fps, overscanned.  */
    /* use 0 for default values 1920,1080,60,30,0; these are sent to the Airplay client      */

    if (display[0]) raop_set_plist(raop, "width", (int) display[0]);
    if (display[1]) raop_set_plist(raop, "height", (int) display[1]);
    if (display[2]) raop_set_plist(raop, "refreshRate", (int) display[2]);
    if (display[3]) raop_set_plist(raop, "maxFPS", (int) display[3]);
    if (display[4]) raop_set_plist(raop, "overscanned", (int) display[4]);

    if (show_client_FPS_data) raop_set_plist(raop, "clientFPSdata", 1);
    if (audiodelay >= 0) raop_set_plist(raop, "audio_delay_micros", audiodelay);
    if (pin_pw == 1) raop_set_plist(raop, "pin", (int) pin);

    /* network port selection (ports listed as "0" will be dynamically assigned) */
    raop_set_tcp_ports(raop, tcp);
    raop_set_udp_ports(raop, udp);

    /* before the first request can be served: the handlers read it */
    if (!dnssd) {
        startup_message("the AirPlay server has no mDNS service to advertise with");
        return -2;
    }
    raop_set_dnssd(raop, dnssd);

    raop_port = raop_get_port(raop);
    int const listening = raop_start_httpd(raop, &raop_port);
    if (listening < 0) {
        startup_message("the AirPlay (RTSP) server could not listen on TCP port %u (httpd error %d); "
                        "another program may be using the port, or the firewall refused it",
                        raop_port, listening);
        return -3;
    }
    raop_set_port(raop, raop_port);

    /* use raop_port for airplay_port (instead of tcp[2]) */
    airplay_port = raop_port;
    return 0;
}

static void read_config_file(const char * filename, const char * uxplay_name) {
    std::string config_file = filename;
    std::string option_char = "-";
    std::vector<std::string> options;
    options.push_back(uxplay_name);
    std::ifstream file(config_file);
    if (file.is_open()) {
        LOGI("reading configuration from %s", config_file.c_str());
        std::string line;
        while (std::getline(file, line)) {
            if (line[0] == '#') continue;
            //  first process line into separate option items with '\0' as delimiter
            bool is_part_of_item, in_quotes;
            char endchar;
            is_part_of_item = false;
            for (int i = 0; i < (int) line.size(); i++) {
                if (is_part_of_item == false) {
                    if (line[i] == ' ') {
                        line[i] = '\0';
                    } else {
                        // start of new item
                        is_part_of_item = true;
                        switch (line[i]) {
                        case '\'':
                        case '\"':
                            endchar = line[i];
                            line[i] = '\0';
                            in_quotes = true;
                            break;
                        default:
                            in_quotes = false;
		            endchar = ' ';
		            break;
                        }
                    }
                } else {
                    /* previous character was inside this item */
                    if (line[i] == endchar) {
                        if (in_quotes) {
                            /* cases where endchar is inside quoted item */
                            if (i > 0 && line[i - 1] == '\\') continue;
                            if (i + 1 < (int) line.size() && line[i + 1] != ' ') continue;
		        }
                        line[i] =  '\0';
                        is_part_of_item = false;
                    }
                }
            }

            // now tokenize the processed line   
            std::istringstream iss(line);
            std::string token;
            bool first = true;
            while (std::getline(iss, token, '\0')) {
                if (token.size() > 0) {
                    if (first) {
                        options.push_back(option_char + token.c_str());
                        first = false;
                    } else {
                        options.push_back(token.c_str());
                    }
                }
	    }
	}
        file.close();
    } else {
        /* not a reason to stop: the start goes on without the file */
        LOGW("failed to open configuration file at %s", config_file.c_str());
    }
    if (options.size() > 1) {

        int argc = options.size();
        char **argv = (char **) malloc(sizeof(char*) * (argc + 1));
        if (argv == NULL) {
            startup_message("memory allocation failure while reading the configuration file");
            startup_exit(1);
        }
        for (int i = 0; i < argc; i++) {
            argv[i] = (char *) options[i].c_str();
        }
        /* add an extra NULL option to terminate the simulated argv list
           (as protection if argv[++i] is referenced when i = argc -1) */
        argv[argc] = NULL;
        parse_arguments (argc, argv);
        free (argv);
    }
}

/* 64-bit FNV-1a of the receiver name: names the single-instance mutex */
static std::string receiver_name_tag(const std::string &name) {
    unsigned long long hash = 14695981039346656037ull;
    for (unsigned char c : name) {
        hash ^= c;
        hash *= 1099511628211ull;
    }
    char text[17];
    snprintf(text, sizeof(text), "%016llx", hash);
    return text;
}

/* One receiver per name and Windows session.  Returns true when this start
 * is (now) the only instance and owns the mutex until it exits; false when a
 * running instance was asked to show itself and this start must end.
 *
 * Another instance has the name: it is asked to show itself, then the mutex
 * is awaited for a moment.  An instance that is just ending (exit, or the
 * restart after a lost graphics device) releases it and this start takes
 * over; one that stays keeps it. */
static bool claim_single_instance(void) {
    std::string const name = "Local\\uxplay_native_" + receiver_name_tag(server_name);
    instance_mutex = CreateMutexA(NULL, TRUE, name.c_str());
    DWORD const error = GetLastError();
    if (instance_mutex == NULL) {
        /* cannot tell: a second receiver with the same name is at least visible on the phone */
        LOGW("single-instance check unavailable (CreateMutex: Windows error %lu); starting anyway",
             (unsigned long) error);
        instance_claimed = true;
        return true;
    }
    if (error != ERROR_ALREADY_EXISTS) {
        instance_claimed = true;
        return true;
    }
    /* An instance that is still starting has no window yet (the first start
       of a new download can take seconds while it is scanned): it is asked
       again for about 6 s.  Once it answered, the mutex is awaited 1.5 s in
       case it is just ending. */
    int notified = 0;
    for (int attempt = 0; attempt < 20; attempt++) {
        if (notified == 0) {
            notified = native_media_activate_running_instance(server_name.c_str());
        }
        DWORD const waited = WaitForSingleObject(instance_mutex, notified > 0 ? 1500 : 300);
        if (waited == WAIT_OBJECT_0 || waited == WAIT_ABANDONED) {
            instance_claimed = true;
            return true;
        }
        if (notified > 0) {
            break;
        }
    }
    CloseHandle(instance_mutex);
    instance_mutex = NULL;
    if (notified == 0) {
        startup_message("\"%s\" is already running but did not answer. End " UXPLAY_EXECUTABLE_NAME
                        " in Task Manager, "
                        "then start it again.", server_name.c_str());
        startup_exit(1);
    }
    LOGI("\"%s\" is already running: it was asked to show itself", server_name.c_str());
    return false;
}

/* Starts this program again with the same command line (the Direct3D device
 * was lost).  A second loss within a minute of such a restart is not followed
 * by another one: the cause is then not transient.  On failure the reason has
 * been reported with startup_message(). */
static bool restart_program(void) {
    static const char restarted_at[] = "UXPLAY_RESTARTED_AT";
    ULONGLONG const now = GetTickCount64();
    char previous[32];
    DWORD const length = GetEnvironmentVariableA(restarted_at, previous, sizeof(previous));
    if (length > 0 && length < sizeof(previous)) {
        ULONGLONG const then = strtoull(previous, NULL, 10);
        if (now >= then && now - then < 60000) {
            startup_message("the graphics device was lost again within a minute of the automatic restart, so "
                            "the program was not started again; check the display driver");
            return false;
        }
    }
    char stamp[32];
    snprintf(stamp, sizeof(stamp), "%llu", (unsigned long long) now);
    if (!SetEnvironmentVariableA(restarted_at, stamp)) {
        LOGW("restart: the restart time could not be recorded (Windows error %lu)", (unsigned long) GetLastError());
    }
    std::vector<wchar_t> path(32768);
    DWORD const used = GetModuleFileNameW(NULL, path.data(), (DWORD) path.size());
    if (used == 0 || used >= path.size()) {
        startup_message("restart: the program path is unavailable (Windows error %lu)", (unsigned long) GetLastError());
        return false;
    }
    std::wstring command_line = GetCommandLineW();
    STARTUPINFOW startup;
    memset(&startup, 0, sizeof(startup));
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process;
    memset(&process, 0, sizeof(process));
    if (!CreateProcessW(path.data(), command_line.data(), NULL, NULL, FALSE, 0, NULL, NULL, &startup, &process)) {
        startup_message("restart: the program could not be started again (Windows error %lu)",
                        (unsigned long) GetLastError());
        return false;
    }
    LOGI("restart: started process %lu (it writes its log file in %%LOCALAPPDATA%%\\" UXPLAY_PRODUCT_NAME
         " unless it can attach to "
         "this run's terminal; redirected output is not passed on)", (unsigned long) process.dwProcessId);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return true;
}

int main (int argc, char *argv[]) {
    std::vector<char> server_hw_addr;
    std::string config_file = "";

    /* initialise Windows kernel qpc frequency for recv timestamping */
    ntp_global_init();
    native_setup_output();
    /* from here on a crash leaves a record next to the log */
    if (!native_log_directory.empty()) {
        native_media_install_crash_report(native_log_directory.c_str());
    }
    shutdown_complete_event = CreateEventW(NULL, TRUE, FALSE, NULL);

    /* Winsock stays initialized for the whole run.  raop_destroy() ends with
       WSACleanup: without this reference that would close every socket of
       the process, the mDNS responder's too, before its goodbye is sent. */
    WSADATA winsock_data;
    int const winsock_error = WSAStartup(MAKEWORD(2, 2), &winsock_data);
    if (winsock_error != 0) {
        startup_message("Windows Sockets could not be started (WSAStartup error %d)", winsock_error);
        startup_exit(1);
    }

    if (!SetConsoleCtrlHandler(CtrlHandler, TRUE)) {
        startup_message("could not set the console control handler (Windows error %lu)",
                        (unsigned long) GetLastError());
        startup_exit(1);
    }

    char *rcfile = NULL;
    /* see if option -rc was given */
    for (int i = 1; i < argc ; i++) {
        std::string arg(argv[i]);
        if (arg == "-rc") {
            struct stat sb;
            if (i+1 == argc) {
                startup_message("option -rc requires a filename  (-rc <filename>)");
                startup_exit(1);
            }
            rcfile = argv[i+1];
            if (stat(rcfile, &sb) == -1) {
                startup_message("startup file %s specified by option -rc was not found", rcfile);
                startup_exit(1);
            }
            break;
        }
    }
    if (rcfile) {
        config_file = rcfile;
    } else {
        config_file = find_uxplay_config_file();
    }
    if (config_file.length()) {
        read_config_file(config_file.c_str(), argv[0]);
    }
    parse_arguments (argc, argv);

    log_level = (debug_log ? LOGGER_DEBUG_DATA : LOGGER_INFO);
    if (debug_log && suppress_packet_debug_data) {
        log_level = LOGGER_DEBUG;
    }

    SetConsoleOutputCP(CP_UTF8);

    /* the receiver name is final here: it identifies the running instance */
    if (do_append_hostname) {
        append_hostname(server_name);
    }
    /* the log file to fall back to when uxplay.log belongs to another
       running instance (a receiver with another name, an older version) */
    log_alternate_name = UXPLAY_PRODUCT_FILE_NAME "." + receiver_name_tag(server_name).substr(0, 8);
    if (!claim_single_instance()) {
        /* the log file belongs to the instance that is running: not opened */
        exit(0);
    }
    open_log_file();

    LOGI(UXPLAY_PRODUCT_NAME " %s (AirPlay protocol core: UxPlay %s), receiver name \"%s\"", UXPLAY_VERSION_TEXT,
         VERSION, server_name.c_str());

    if (dump_video) {
        if (video_dump_limit > 0) {
             LOGI("dump video using \"-vdmp %d %s\"", video_dump_limit, video_dumpfile_name.c_str());
        } else {
             LOGI("dump video using \"-vdmp %s\"", video_dumpfile_name.c_str());
        }
    }
    if (dump_audio) {
        if (audio_dump_limit > 0) {
            LOGI("dump audio using \"-admp %d %s\"", audio_dump_limit, audio_dumpfile_name.c_str());
        } else {
            LOGI("dump audio using \"-admp %s\"",  audio_dumpfile_name.c_str());
        }
    }

    if (!use_video) {
        LOGI("video_disabled");
        display[3] = 1; /* set fps to 1 frame per sec when no video will be shown */
        user_set_fps = true;
    }
    if (!use_audio) {
        LOGI("audio_disabled");
    }

    if (pin_pw == 1 && registration_list) {
        if (pairing_register == "") {
            const char * homedir = get_homedir();
            if (homedir) {
                pairing_register = homedir;
                pairing_register.append("/.uxplay.register");
             }
        }
    }

    /* read in public keys that were previously registered with pair-setup-pin */
    if (pin_pw == 1 && registration_list && strlen(pairing_register.c_str())) {
        int clients = 0;
        int malformed = 0;
        std::ifstream file(pairing_register);
        if (file.is_open()) {
            std::string line;
            while (std::getline(file, line)) {
                /* 32 bytes pk -> base64 -> 44 characters at the start of the line */
                if (line.size() < 44) {
                    if (!line.empty()) {
                        malformed++;
                    }
                    continue;
                }
                registered_keys.push_back(line.substr(0, 44));
                clients ++;
            }
            if (clients) {
                LOGI("Register %s lists %d pin-registered clients", pairing_register.c_str(), clients);
            }
            if (malformed) {
                LOGW("Register %s: %d line(s) shorter than a public key were ignored", pairing_register.c_str(),
                     malformed);
            }
            file.close();
        }
    }

    if (pin_pw == 1 && keyfile == "0") {
        const char * homedir = get_homedir();
        if (homedir) {
            keyfile.erase();
            keyfile = homedir;
            keyfile.append("/.uxplay.pem");
        } else {
            LOGE("could not determine $HOME: public key wiil not be saved, and so will not be persistent");
        }
    }

    if (keyfile != "") {
        LOGI("public key storage (for persistence) is in %s", keyfile.c_str());
    }

    /* fallback if the GPU query below fails; refined from real GPU HEVC
       capability once the engine has started */
    if (!force_h264_only) {
        h265_support = true;
    }

    native_media_settings native_settings;
    memset(&native_settings, 0, sizeof(native_settings));
    native_settings.window_title = server_name.c_str();
    native_settings.start_fullscreen = fullscreen;
    native_settings.timestamp_sync = video_sync;
    native_settings.audio_offset_ms = (double) audio_delay_aac / 1e6;
    native_settings.keep_window_on_stop = !new_window_closing_behavior;
    native_settings.log_level = log_level;
    native_settings.gpu_preference = gpu_preference;
    native_settings.ui_language = ui_language_choice;
    native_settings.performance_mode = mode_choice;
    native_settings.crop_mode = crop_choice;
    native_settings.tray_icon = true;
    native_settings.product_version = UXPLAY_VERSION_TEXT;
    native_settings.core_version = VERSION;
    native_settings.project_url = UXPLAY_PROJECT_URL;
    native_settings.log_path = native_log_path.empty() ? NULL : native_log_path.c_str();
    native_settings.log_callback = native_log_callback;
    native_settings.event_callback = native_event_callback;
    if (native_media_start(&native_settings) != 0) {
        startup_message("the Windows-native media engine could not be started");
        cleanup(1);
    }
    LOGI("Windows-native engine: fullscreen toggle = double-click / F / F11 / Alt+Enter, Esc leaves fullscreen");
    /* waits (bounded) for the engine, whose GPU decides the codec offer */
    native_media_display_info native_display;
    bool const display_known = native_media_query_display(&native_display);
    if (native_media_failure_text()[0] != '\0') {
        /* no window and no tray icon: never advertise such a receiver */
        startup_message("the media engine could not start: %s", native_media_failure_text());
        cleanup(1);
    }
    if (display_known) {
        /* adapt the offered codec to whatever GPU drives this machine's display:
           offer H.265 only if that GPU decodes HEVC in hardware (unless the user
           forced it with -h265, or disabled it with -h264) */
        if (force_h264_only) {
            h265_support = false;
        } else if (!h265_user_requested) {
            h265_support = (native_display.hevc_hardware != 0);
        }
        if (h265_support && !native_display.hevc_hardware) {
            LOGW("H.265 enabled but this GPU has no HEVC hardware decoder; HEVC will use the software fallback");
        }
    }
    /* ask the iPhone for this monitor's geometry and refresh rate, per mode */
    choose_stream_geometry();

    if (udp[0]) {
        LOGI("using network ports UDP %d %d %d TCP %d %d %d", udp[0], udp[1], udp[2], tcp[0], tcp[1], tcp[2]);
    }

    if (!use_random_hw_addr) {
        if (strlen(mac_address.c_str()) == 0) {
            mac_address = find_mac();
            LOGI("using system MAC address %s",mac_address.c_str());
        } else {
            LOGI("using user-set MAC address %s",mac_address.c_str());
        }
    }
    if (mac_address.empty()) {
        mac_address = random_mac();
        LOGI("using randomly-generated MAC address %s",mac_address.c_str());
    }
    parse_hw_addr(mac_address, server_hw_addr);

    if (coverart_filename.length()) {
        LOGI("any AirPlay audio cover-art will be written to file  %s",coverart_filename.c_str());
        write_coverart(coverart_filename.c_str(), (const void *) empty_image, sizeof(empty_image));
    }

    if (metadata_filename.length()) {
        LOGI("any AirPlay audio metadata text will be written to file  %s",metadata_filename.c_str());
        write_metadata(metadata_filename.c_str(), "no data\n");
    }

    /* set default resolutions for h264 or h265*/
    if (!display[0] && !display[1]) {
        if (h265_support) {
            display[0] = 3840;
            display[1] = 2160;
        } else {
            display[0] = 1920;
            display[1] = 1080;
        }
    }

    /* starting volume is the user-selected initial volume */
    current_volume = initial_volume;

    /* the session flags, before a server thread can set one */
    relaunch_video = use_video;
    reset_httpd = false;
    close_window = new_window_closing_behavior;

    if (start_dnssd(server_hw_addr, server_name)) {
        cleanup(1);
    }
    if (start_raop_server(display, tcp, udp, debug_log)) {
        stop_raop_server();
        stop_dnssd();
        cleanup(1);
    }
    if (register_dnssd()) {
        stop_raop_server();
        stop_dnssd();
        cleanup(1);
    }
    {
        /* startup time as the user perceives it: process creation -> advertised */
        FILETIME creation, exited, kernel, user, now;
        if (GetProcessTimes(GetCurrentProcess(), &creation, &exited, &kernel, &user)) {
            GetSystemTimePreciseAsFileTime(&now);
            ULARGE_INTEGER created_at, now_at;
            created_at.LowPart = creation.dwLowDateTime;
            created_at.HighPart = creation.dwHighDateTime;
            now_at.LowPart = now.dwLowDateTime;
            now_at.HighPart = now.dwHighDateTime;
            LOGI("AirPlay server \"%s\" is advertised and ready, %.0f ms after launch", server_name.c_str(),
                 (double) (now_at.QuadPart - created_at.QuadPart) / 1e4);
        }
    }

    /* session cycles: each ends with a client reset or teardown (relaunch) or
       with an exit request; an exit requested at any time (also during the
       start, or between two cycles) is never turned into a relaunch */
    while (!exit_requested) {
        main_loop();
        /* What this cycle's requests asked for is taken here: first the
           requests are acknowledged, then each flag is taken in one step.  A
           request made from here on belongs to the next cycle and ends it:
           request_reset keeps it and its flags stay set. */
        event_loop.acknowledge_resets();
        bool const relaunch = relaunch_video.exchange(use_video);
        if (exit_requested || !relaunch) {
            break;
        }
        bool const restart_server = reset_httpd.exchange(false);
        bool const handed_over = connection_handed_over.exchange(false);
        bool const close_now = close_window.exchange(new_window_closing_behavior);
        if (restart_server) {
            raop_stop_httpd(raop);
        }
        /* After a hand-over the AirPlay core already removed the old
           connections and video_reset stopped the old client's picture and
           sound: stopping the sound or removing "known" connections here
           would hit the new client that was just accepted. */
        if (use_audio && !handed_over) {
            native_media_airplay_audio_stop();
        }
        if (use_video && !handed_over && (close_now || full_video_reset)) {
            native_media_airplay_video_stop();
            raop_remove_known_connections(raop);
            full_video_reset = false;
        } else if (use_video && !handed_over) {
            /* the last picture stays on screen (connection lost), but the
               stream is over: paused, it does not keep the display awake;
               the next stream's configuration ends the pause */
            native_media_airplay_video_pause();
        }
        if (restart_server) {
            unsigned short port = raop_get_port(raop);
            int const listening = raop_start_httpd(raop, &port);
            if (listening < 0) {
                startup_message("after a client reset the AirPlay (RTSP) server could not listen again on TCP "
                                "port %u (httpd error %d)", port, listening);
                exit_code_requested = 1;
                break;
            }
            raop_set_port(raop, port);
        }
    }
    LOGI("Stopping RAOP Server...");
    stop_raop_server();
    stop_dnssd();
    cleanup(exit_code_requested);
}

/* Main thread only.  Ends the program: a non-zero exit code is a failure
 * whose reason was reported with startup_message()/LOGE and is shown in a
 * dialog when there is no terminal. */
static void cleanup(int exit_code) {
    if (use_audio) {
        native_media_airplay_audio_stop();
    }
    if (use_video)  {
        native_media_airplay_video_stop();
    }
    if(audio_dumpfile) {
        fclose(audio_dumpfile);
    }
    if (video_dumpfile) {
        fwrite(mark, 1, sizeof(mark), video_dumpfile);
        fclose(video_dumpfile);
    }
    if (coverart_filename.length()) {
        remove (coverart_filename.c_str());
    }
    if (metadata_filename.length()) {
        remove (metadata_filename.c_str());
    }
    native_media_stop();
    /* the window and the tray icon are gone.  A restart is started first: it
       waits (claim_single_instance) until this process lets go below. */
    if (exit_code == 0 && restart_after_exit && !restart_program()) {
        exit_code = 1;
    }
    if (shutdown_complete_event) {
        SetEvent(shutdown_complete_event);
    }
    if (exit_code != 0) {
        report_startup_failure();
    }
    /* the log first, then the mutex: the next instance (which may be waiting
       for the mutex right now) keeps this run's log as uxplay.previous.log */
    close_log_file();
    if (instance_mutex) {
        ReleaseMutex(instance_mutex);
        CloseHandle(instance_mutex);
        instance_mutex = NULL;
    }
    exit(exit_code);
}
