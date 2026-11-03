# Windows-native media engine for UxPlay (C++26 named modules).
#
# Inputs (pinned main-branch builds, see README_AI.md): FFmpeg (libavcodec,
# libavutil, libswresample) and libusb-1.0, found through pkg-config; the
# Windows SDK shader compiler fxc.exe for offline HLSL compilation.

find_package( PkgConfig REQUIRED )
pkg_check_modules( UXPLAY_FFMPEG REQUIRED libavcodec libavutil libswresample )
pkg_check_modules( UXPLAY_LIBUSB REQUIRED libusb-1.0 )

file( GLOB UXPLAY_FXC_CANDIDATES "C:/Program Files (x86)/Windows Kits/10/bin/*/x64/fxc.exe" )
list( SORT UXPLAY_FXC_CANDIDATES COMPARE NATURAL ORDER DESCENDING )
find_program( UXPLAY_FXC NAMES fxc fxc.exe HINTS ${UXPLAY_FXC_CANDIDATES} )
if ( NOT UXPLAY_FXC AND UXPLAY_FXC_CANDIDATES )
  list( GET UXPLAY_FXC_CANDIDATES 0 UXPLAY_FXC )
endif()
if ( NOT UXPLAY_FXC )
  message( FATAL_ERROR "fxc.exe (Windows SDK) is required to compile win_native/shaders" )
endif()
message( STATUS "HLSL compiler: ${UXPLAY_FXC}" )

set( UXPLAY_SHADER_SOURCE ${CMAKE_CURRENT_SOURCE_DIR}/shaders/video_present.hlsl )
set( UXPLAY_GENERATED ${CMAKE_CURRENT_BINARY_DIR}/generated )
file( MAKE_DIRECTORY ${UXPLAY_GENERATED} )
set( UXPLAY_SHADER_HEADERS )
foreach( shader_spec "vs_fullscreen;vs_5_0;video_present_vs.h"
                     "ps_nv12;ps_5_0;video_present_ps_nv12.h"
                     "ps_i420;ps_5_0;video_present_ps_i420.h" )
  list( GET shader_spec 0 entry_point )
  list( GET shader_spec 1 profile )
  list( GET shader_spec 2 header )
  add_custom_command(
    OUTPUT ${UXPLAY_GENERATED}/${header}
    COMMAND ${UXPLAY_FXC} /nologo /O3 /WX /T ${profile} /E ${entry_point} /Vn shader_${entry_point}
            /Fh ${UXPLAY_GENERATED}/${header} ${UXPLAY_SHADER_SOURCE}
    DEPENDS ${UXPLAY_SHADER_SOURCE}
    COMMENT "fxc ${entry_point} (${profile})"
    VERBATIM )
  list( APPEND UXPLAY_SHADER_HEADERS ${UXPLAY_GENERATED}/${header} )
endforeach()
add_custom_target( uxplay_shaders DEPENDS ${UXPLAY_SHADER_HEADERS} )

add_library( uxplay_native STATIC )
target_sources( uxplay_native
  PUBLIC
    FILE_SET uxplay_modules TYPE CXX_MODULES FILES
      src/uxplay_status.cppm
      src/uxplay_log.cppm
      src/uxplay_clock.cppm
      src/uxplay_system_tuning.cppm
      src/uxplay_media_types.cppm
      src/uxplay_gpu_device.cppm
      src/uxplay_video_frame.cppm
      src/uxplay_video_decoder.cppm
      src/uxplay_video_overlay.cppm
      src/uxplay_video_window.cppm
      src/uxplay_video_presenter.cppm
      src/uxplay_audio_output.cppm
      src/uxplay_audio_decoder.cppm
      src/uxplay_media_engine.cppm
      src/uxplay_qt_protocol.cppm
      src/uxplay_usb_mirror.cppm
      src/uxplay_usb_link.cppm
  PRIVATE
    src/uxplay_status.cpp
    src/uxplay_log.cpp
    src/uxplay_clock.cpp
    src/uxplay_system_tuning.cpp
    src/uxplay_gpu_device.cpp
    src/uxplay_video_frame.cpp
    src/uxplay_video_decoder.cpp
    src/uxplay_video_overlay.cpp
    src/uxplay_video_window.cpp
    src/uxplay_video_presenter.cpp
    src/uxplay_audio_output.cpp
    src/uxplay_audio_decoder.cpp
    src/uxplay_media_engine.cpp
    src/uxplay_qt_protocol.cpp
    src/uxplay_usb_mirror.cpp
    src/uxplay_usb_link.cpp
    src/native_media_bridge.cpp
    src/renderer_compat.c )
add_dependencies( uxplay_native uxplay_shaders )
target_compile_features( uxplay_native PUBLIC cxx_std_26 )
set_target_properties( uxplay_native PROPERTIES CXX_MODULE_STD ON CXX_EXTENSIONS OFF )
target_include_directories( uxplay_native PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include )
target_include_directories( uxplay_native PRIVATE ${UXPLAY_GENERATED} )
# module interfaces include FFmpeg/libusb headers in their global module
# fragment: importers (and CMake's synthesized BMIs) need them as system headers
target_include_directories( uxplay_native SYSTEM PUBLIC ${UXPLAY_FFMPEG_INCLUDE_DIRS} ${UXPLAY_LIBUSB_INCLUDE_DIRS} )
# WIN32_LEAN_AND_MEAN / NOMINMAX are defined by each source before <windows.h>
target_compile_definitions( uxplay_native PUBLIC UNICODE _UNICODE _WIN32_WINNT=0x0A00 WINVER=0x0A00 )

# GLib: uxplay.cpp's main loop and renderer_compat.c's renderer headers.
# Linked statically with its own static dependencies (libintl -> libiconv,
# pcre2) so the program is one executable with no bundled DLLs;
# GLIB_STATIC_COMPILATION drops GLib's dllimport declarations (glib-2.0.pc
# does not carry it).
pkg_check_modules( UXPLAY_GLIB REQUIRED glib-2.0 )
add_library( uxplay_glib INTERFACE )
target_include_directories( uxplay_glib SYSTEM INTERFACE ${UXPLAY_GLIB_STATIC_INCLUDE_DIRS} )
target_compile_definitions( uxplay_glib INTERFACE GLIB_STATIC_COMPILATION )
target_link_directories( uxplay_glib INTERFACE ${UXPLAY_GLIB_STATIC_LIBRARY_DIRS} )
target_link_libraries( uxplay_glib INTERFACE ${UXPLAY_GLIB_STATIC_LIBRARIES} iconv )
target_link_libraries( uxplay_native PUBLIC uxplay_glib )

# Strongest practical warning set, as errors, on the owned sources only: the
# std module that CMake synthesizes from libc++'s std.cppm must not inherit
# it (libc++'s own `export module std;` triggers -Wreserved-module-identifier).
get_target_property( UXPLAY_NATIVE_SOURCES uxplay_native SOURCES )
get_target_property( UXPLAY_NATIVE_MODULES uxplay_native CXX_MODULE_SET_uxplay_modules )
foreach( owned_source ${UXPLAY_NATIVE_SOURCES} ${UXPLAY_NATIVE_MODULES} )
  if ( owned_source MATCHES "[.]c$" )
    set_property( SOURCE ${owned_source} APPEND PROPERTY COMPILE_OPTIONS -Wall -Wextra -Werror -Wno-unused-parameter )
  elseif ( owned_source MATCHES "[.](cpp|cppm)$" )
    set_property( SOURCE ${owned_source} APPEND PROPERTY COMPILE_OPTIONS
      -Wall -Wextra -Wpedantic -Wshadow -Wnon-virtual-dtor -Wold-style-cast -Wnull-dereference
      -Wimplicit-fallthrough -Wno-unused-parameter -Werror )
  endif()
endforeach()

target_link_directories( uxplay_native PUBLIC ${UXPLAY_FFMPEG_LIBRARY_DIRS} ${UXPLAY_LIBUSB_LIBRARY_DIRS} )
target_link_libraries( uxplay_native PUBLIC
  avcodec swresample avutil usb-1.0
  d3d11 dxgi dwmapi avrt ole32 uuid user32 gdi32 bcrypt ws2_32 crypt32 setupapi iphlpapi cfgmgr32 d2d1 dwrite )

# ---- tests (not part of the shipped program) -------------------------------
option( UXPLAY_NATIVE_TESTS "Build the native engine tests" ON )
if ( UXPLAY_NATIVE_TESTS )
  enable_testing()
  add_executable( qt_protocol_tests tests/qt_protocol_tests.cpp )
  target_link_libraries( qt_protocol_tests PRIVATE uxplay_native )
  set_target_properties( qt_protocol_tests PROPERTIES CXX_MODULE_STD ON )
  add_test( NAME qt_protocol_tests COMMAND qt_protocol_tests ${CMAKE_CURRENT_SOURCE_DIR}/tests/qt_fixtures )

  add_executable( engine_smoke_test tests/engine_smoke_test.cpp )
  target_link_libraries( engine_smoke_test PRIVATE uxplay_native )
  set_target_properties( engine_smoke_test PROPERTIES CXX_MODULE_STD ON )

  add_executable( usb_link_tests tests/usb_link_tests.cpp )
  target_link_libraries( usb_link_tests PRIVATE uxplay_native )
  set_target_properties( usb_link_tests PROPERTIES CXX_MODULE_STD ON )
  add_test( NAME usb_link_tests COMMAND usb_link_tests )

  foreach( test_source tests/qt_protocol_tests.cpp tests/engine_smoke_test.cpp tests/usb_link_tests.cpp )
    set_property( SOURCE ${test_source} APPEND PROPERTY COMPILE_OPTIONS
      -Wall -Wextra -Wpedantic -Wshadow -Wold-style-cast -Werror )
  endforeach()
endif()
