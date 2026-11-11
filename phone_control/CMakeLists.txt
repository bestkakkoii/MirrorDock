# SPDX-License-Identifier: GPL-3.0-or-later
# Remote control of the iPhone through the developer services of iOS (C++26
# named modules): usbmuxd, lockdown, the developer tunnel with its own
# IPv6/TCP, RemoteXPC, CoreDevice and the HID reports for touch and keyboard;
# the Developer Disk Image those services come with (fetched over WinHTTP,
# signed by Apple, installed through the phone's cryptex service).
#
# Inputs (pinned main-branch builds, see README_AI.md): libplist and OpenSSL
# (libssl, libcrypto), both static.  Uses uxplay_base (failure outcomes, log).

find_package( PkgConfig REQUIRED )
pkg_search_module( PLIST REQUIRED libplist-2.0>=2.3.0 )
# (LIBPLIST is set by the build script: the static library of the pinned build)
find_library( LIBPLIST ${PLIST_LIBRARIES} PATH ${PLIST_LIBDIR} )
find_package( OpenSSL 1.1.1 REQUIRED )

add_library( phone_control STATIC )
target_sources( phone_control
  PUBLIC
    FILE_SET phone_control_modules TYPE CXX_MODULES FILES
      src/uxplay_byte_stream.cppm
      src/uxplay_property_list.cppm
      src/uxplay_usbmux.cppm
      src/uxplay_device_stream.cppm
      src/uxplay_lockdown.cppm
      src/uxplay_device_tunnel.cppm
      src/uxplay_remote_xpc.cppm
      src/uxplay_core_device.cppm
      src/uxplay_cryptex.cppm
      src/uxplay_http_client.cppm
      src/uxplay_developer_image.cppm
      src/uxplay_remote_control.cppm
  PRIVATE
    src/uxplay_byte_stream.cpp
    src/uxplay_property_list.cpp
    src/uxplay_usbmux.cpp
    src/uxplay_device_stream.cpp
    src/uxplay_lockdown.cpp
    src/uxplay_device_tunnel.cpp
    src/uxplay_remote_xpc.cpp
    src/uxplay_core_device.cpp
    src/uxplay_cryptex.cpp
    src/uxplay_http_client.cpp
    src/uxplay_developer_image.cpp
    src/uxplay_remote_control.cpp )
target_compile_features( phone_control PUBLIC cxx_std_26 )
set_target_properties( phone_control PROPERTIES CXX_MODULE_STD ON CXX_EXTENSIONS OFF )
# libplist and OpenSSL are included by implementation units only
target_compile_definitions( phone_control PRIVATE LIBPLIST_STATIC )
target_include_directories( phone_control SYSTEM PRIVATE ${PLIST_INCLUDE_DIRS} ${OPENSSL_INCLUDE_DIR} )
uxplay_strict_warnings( phone_control phone_control_modules )
target_link_libraries( phone_control PUBLIC
  uxplay_base ${LIBPLIST} OpenSSL::SSL OpenSSL::Crypto ws2_32 crypt32 bcrypt winhttp )
