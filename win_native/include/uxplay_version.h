// SPDX-License-Identifier: MIT
/*
 * Identity of the Windows-native program: product name and version, shared
 * by uxplay.cpp, the media engine and the version resource
 * (win_native/resources/uxplay.rc).  The manifest (uxplay.manifest) cannot
 * include this file: its assemblyIdentity repeats the name and the version.
 *
 * The product is MirrorDock since 1.7; up to 1.6 it was published as "UxPlay
 * for Windows (native)".  UXPLAY_CORE_VERSION is the upstream UxPlay release
 * the AirPlay protocol core (lib/, uxplay.cpp) derives from.  Internal names
 * (modules, namespace, build targets, these macros) keep the uxplay prefix.
 */
#ifndef UXPLAY_VERSION_H
#define UXPLAY_VERSION_H

/* shown to the user; also the default receiver name on the iPhone */
#define UXPLAY_PRODUCT_NAME "MirrorDock"
/* executable, log and crash-record files (the resource compiler does not
   join string literals, hence the names written out) */
#define UXPLAY_PRODUCT_FILE_NAME "mirror_dock"
#define UXPLAY_EXECUTABLE_NAME "mirror_dock.exe"
#define UXPLAY_FILE_DESCRIPTION "MirrorDock screen mirroring receiver"
#define UXPLAY_COMPANY_NAME "MirrorDock open-source project"
#define UXPLAY_COPYRIGHT "UxPlay authors and MirrorDock contributors. GPL-3.0-or-later."
/* the name up to 1.6: its settings key and its log folder are taken over once */
#define UXPLAY_LEGACY_PRODUCT_NAME "UxPlay"

#define UXPLAY_VERSION_MAJOR 1
#define UXPLAY_VERSION_MINOR 9
#define UXPLAY_VERSION_PATCH 1
#define UXPLAY_VERSION_TEXT "1.9.1"
#define UXPLAY_CORE_VERSION "1.74"
#define UXPLAY_PROJECT_URL "https://github.com/bestkakkoii/MirrorDock"

#endif
