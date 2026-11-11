# SPDX-License-Identifier: MIT
# Windows-native media engine for UxPlay (C++26 named modules).
#
# Inputs (pinned main-branch builds, see README_AI.md): FFmpeg (libavcodec,
# libavutil, libswresample), found through pkg-config; the Windows SDK shader
# compiler fxc.exe for offline HLSL compilation.

find_package( PkgConfig REQUIRED )
pkg_check_modules( UXPLAY_FFMPEG REQUIRED libavcodec libavutil libswresample )

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

# What every owned part of the program shares: failure outcomes and the log.
# A target of its own, so that phone_control (which the engine uses) can use
# it without depending on the engine.
add_library( uxplay_base STATIC )
target_sources( uxplay_base
  PUBLIC
    FILE_SET uxplay_base_modules TYPE CXX_MODULES FILES
      src/uxplay_status.cppm
      src/uxplay_log.cppm
  PRIVATE
    src/uxplay_status.cpp
    src/uxplay_log.cpp )
target_compile_features( uxplay_base PUBLIC cxx_std_26 )
set_target_properties( uxplay_base PROPERTIES CXX_MODULE_STD ON CXX_EXTENSIONS OFF )
# WIN32_LEAN_AND_MEAN / NOMINMAX are defined by each source before <windows.h>
target_compile_definitions( uxplay_base PUBLIC UNICODE _UNICODE _WIN32_WINNT=0x0A00 WINVER=0x0A00 )
uxplay_strict_warnings( uxplay_base uxplay_base_modules )

add_library( uxplay_native STATIC )
target_sources( uxplay_native
  PUBLIC
    FILE_SET uxplay_modules TYPE CXX_MODULES FILES
      src/uxplay_clock.cppm
      src/uxplay_system_tuning.cppm
      src/uxplay_media_types.cppm
      src/uxplay_gpu_device.cppm
      src/uxplay_ui_text.cppm
      src/uxplay_settings.cppm
      src/uxplay_crash_report.cppm
      src/uxplay_crop_detector.cppm
      src/uxplay_video_frame.cppm
      src/uxplay_video_decoder.cppm
      src/uxplay_video_overlay.cppm
      src/uxplay_video_window.cppm
      src/uxplay_video_presenter.cppm
      src/uxplay_audio_output.cppm
      src/uxplay_audio_decoder.cppm
      src/uxplay_media_engine.cppm
      src/uxplay_usb_link.cppm
  PRIVATE
    src/uxplay_clock.cpp
    src/uxplay_system_tuning.cpp
    src/uxplay_gpu_device.cpp
    src/uxplay_ui_text.cpp
    src/uxplay_settings.cpp
    src/uxplay_crash_report.cpp
    src/uxplay_crop_detector.cpp
    src/uxplay_video_frame.cpp
    src/uxplay_video_decoder.cpp
    src/uxplay_video_overlay.cpp
    src/uxplay_video_window.cpp
    src/uxplay_video_presenter.cpp
    src/uxplay_audio_output.cpp
    src/uxplay_audio_decoder.cpp
    src/uxplay_media_engine.cpp
    src/uxplay_usb_link.cpp
    src/native_media_bridge.cpp )
add_dependencies( uxplay_native uxplay_shaders )
target_compile_features( uxplay_native PUBLIC cxx_std_26 )
set_target_properties( uxplay_native PROPERTIES CXX_MODULE_STD ON CXX_EXTENSIONS OFF )
target_include_directories( uxplay_native PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include )
target_include_directories( uxplay_native PRIVATE ${UXPLAY_GENERATED} )
# module interfaces include FFmpeg headers in their global module
# fragment: importers (and CMake's synthesized BMIs) need them as system headers
target_include_directories( uxplay_native SYSTEM PUBLIC ${UXPLAY_FFMPEG_INCLUDE_DIRS} )
uxplay_strict_warnings( uxplay_native uxplay_modules )

target_link_directories( uxplay_native PUBLIC ${UXPLAY_FFMPEG_LIBRARY_DIRS} )
# phone_control: remote control of the phone (../phone_control, GPL-3.0)
target_link_libraries( uxplay_native PUBLIC
  uxplay_base phone_control
  avcodec swresample avutil
  d3d11 dxgi dwmapi avrt ole32 uuid user32 gdi32 bcrypt ws2_32 crypt32 iphlpapi cfgmgr32 d2d1 dwrite imm32 )

# ---- tests (not part of the shipped program) -------------------------------
option( UXPLAY_NATIVE_TESTS "Build the native engine tests" ON )
if ( UXPLAY_NATIVE_TESTS )
  enable_testing()
  # CXX_EXTENSIONS OFF as in the libraries whose modules the tests import: one
  # flavour of every module interface (and of the std module) serves them all.
  add_executable( engine_smoke_test tests/engine_smoke_test.cpp )
  target_link_libraries( engine_smoke_test PRIVATE uxplay_native )
  set_target_properties( engine_smoke_test PROPERTIES CXX_MODULE_STD ON CXX_EXTENSIONS OFF )

  add_executable( usb_link_tests tests/usb_link_tests.cpp )
  target_link_libraries( usb_link_tests PRIVATE uxplay_native )
  set_target_properties( usb_link_tests PROPERTIES CXX_MODULE_STD ON CXX_EXTENSIONS OFF )
  add_test( NAME usb_link_tests COMMAND usb_link_tests )

  add_executable( ui_text_tests tests/ui_text_tests.cpp )
  target_link_libraries( ui_text_tests PRIVATE uxplay_native )
  set_target_properties( ui_text_tests PROPERTIES CXX_MODULE_STD ON CXX_EXTENSIONS OFF )
  add_test( NAME ui_text_tests COMMAND ui_text_tests )

  foreach( test_source tests/engine_smoke_test.cpp tests/usb_link_tests.cpp tests/ui_text_tests.cpp )
    set_property( SOURCE ${test_source} APPEND PROPERTY COMPILE_OPTIONS
      -Wall -Wextra -Wpedantic -Wshadow -Wold-style-cast -Werror )
  endforeach()
endif()
