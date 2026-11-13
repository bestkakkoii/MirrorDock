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

# ---- what the program is built from (the tables of "About") -----------------
# build_scripts/build_inputs.tsv is the one list of the pinned inputs:
# fetch_deps.sh fetches from it, and here its rows become data of the program
# (src/uxplay_build_info.cpp).  Where this configuration knows the version it
# really builds with (the "check" column), that version is the one shown; a
# date or a revision the list records for another version is left out, with a
# warning.  The headings of CHANGELOG.md become the list of the program's own
# versions and their dates.
set( UXPLAY_BUILD_INPUTS ${CMAKE_CURRENT_SOURCE_DIR}/build_scripts/build_inputs.tsv )
set( UXPLAY_CHANGELOG ${CMAKE_SOURCE_DIR}/CHANGELOG.md )
set_property( DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
  ${UXPLAY_BUILD_INPUTS} ${UXPLAY_CHANGELOG}
  ${CMAKE_CURRENT_SOURCE_DIR}/include/uxplay_version.h ${CMAKE_SOURCE_DIR}/lib/llhttp/llhttp.h )
pkg_check_modules( UXPLAY_INPUT_OPENSSL QUIET openssl )
pkg_check_modules( UXPLAY_INPUT_LIBPLIST QUIET libplist-2.0 )

function( uxplay_generate_build_info )
  # the versions this configuration knows, by "check" key (empty: not known)
  set( actual_libavcodec "${UXPLAY_FFMPEG_libavcodec_VERSION}" )
  set( actual_openssl "${UXPLAY_INPUT_OPENSSL_VERSION}" )
  set( actual_libplist "${UXPLAY_INPUT_LIBPLIST_VERSION}" )
  set( actual_clang "${CMAKE_CXX_COMPILER_VERSION}" )
  set( actual_cmake "${CMAKE_VERSION}" )
  set( actual_ninja "" )
  if ( CMAKE_GENERATOR MATCHES "Ninja" AND CMAKE_MAKE_PROGRAM )
    execute_process( COMMAND ${CMAKE_MAKE_PROGRAM} --version OUTPUT_VARIABLE actual_ninja
                     OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET )
  endif()
  set( actual_fxc "" )
  if ( UXPLAY_FXC MATCHES "/bin/([0-9]+[.][0-9]+[.][0-9]+[.][0-9]+)/" )
    set( actual_fxc "${CMAKE_MATCH_1}" )
  endif()
  set( actual_uxplay_core "" )
  file( STRINGS ${CMAKE_CURRENT_SOURCE_DIR}/include/uxplay_version.h core_line REGEX "#define UXPLAY_CORE_VERSION " )
  if ( core_line MATCHES "\"([^\"]+)\"" )
    set( actual_uxplay_core "${CMAKE_MATCH_1}" )
  endif()
  set( actual_llhttp "" )
  file( STRINGS ${CMAKE_SOURCE_DIR}/lib/llhttp/llhttp.h llhttp_lines REGEX "#define LLHTTP_VERSION_(MAJOR|MINOR|PATCH) " )
  if ( "${llhttp_lines}" MATCHES "LLHTTP_VERSION_MAJOR ([0-9]+)" )
    set( llhttp_major "${CMAKE_MATCH_1}" )
    if ( "${llhttp_lines}" MATCHES "LLHTTP_VERSION_MINOR ([0-9]+)" )
      set( llhttp_minor "${CMAKE_MATCH_1}" )
      if ( "${llhttp_lines}" MATCHES "LLHTTP_VERSION_PATCH ([0-9]+)" )
        set( actual_llhttp "${llhttp_major}.${llhttp_minor}.${CMAKE_MATCH_1}" )
      endif()
    endif()
  endif()

  file( STRINGS ${UXPLAY_BUILD_INPUTS} input_lines ENCODING UTF-8 )
  set( rows "" )
  foreach( line IN LISTS input_lines )
    if ( line MATCHES "^#" OR line STREQUAL "" )
      continue()
    endif()
    string( REPLACE "\t" ";" fields "${line}" )
    list( LENGTH fields field_count )
    if ( NOT field_count EQUAL 10 )
      message( FATAL_ERROR "build_inputs.tsv: a line with ${field_count} fields instead of 10: ${line}" )
    endif()
    list( GET fields 0 kind )
    list( GET fields 1 name )
    list( GET fields 2 check )
    list( GET fields 3 version )
    list( GET fields 4 revision )
    list( GET fields 5 date )
    list( GET fields 6 licence )
    list( GET fields 7 homepage )
    if ( NOT kind MATCHES "^(library|tool|system|run_time|credit)$" )
      message( FATAL_ERROR "build_inputs.tsv: unknown kind \"${kind}\" of ${name}" )
    endif()
    if ( "${name}${version}${revision}${date}${licence}${homepage}" MATCHES "[\"\\\\]" )
      message( FATAL_ERROR "build_inputs.tsv: ${name}: a quote or a backslash cannot be written into the program" )
    endif()
    if ( NOT check STREQUAL "-" )
      if ( NOT DEFINED actual_${check} )
        message( FATAL_ERROR "build_inputs.tsv: ${name}: no version is known under the check key \"${check}\"" )
      endif()
      set( actual "${actual_${check}}" )
      if ( actual AND NOT actual STREQUAL version )
        message( WARNING "build_inputs.tsv records ${name} ${version}, this build uses ${actual}: "
                         "\"About\" shows ${actual} without the recorded date and revision. Update the list." )
        set( version "${actual}" )
        set( revision "-" )
        set( date "-" )
      endif()
    endif()
    foreach( field version revision date licence homepage )
      if ( ${field} STREQUAL "-" )
        set( ${field} "" )
      endif()
    endforeach()
    string( APPEND rows "            {build_input_kind::${kind}, \"${name}\", \"${version}\", \"${revision}\", "
                        "\"${date}\", \"${licence}\", \"${homepage}\"},\n" )
  endforeach()
  if ( rows STREQUAL "" )
    message( FATAL_ERROR "build_inputs.tsv names no input" )
  endif()
  file( CONFIGURE OUTPUT ${UXPLAY_GENERATED}/uxplay_build_inputs.inc CONTENT "${rows}" @ONLY )

  file( STRINGS ${UXPLAY_CHANGELOG} headings REGEX "^## [0-9]" ENCODING UTF-8 )
  set( releases "" )
  foreach( heading IN LISTS headings )
    if ( heading MATCHES "^## ([0-9]+[.][0-9]+([.][0-9]+)?)[^0-9]+([0-9][0-9][0-9][0-9]-[0-9][0-9]-[0-9][0-9])" )
      string( APPEND releases "            {\"${CMAKE_MATCH_1}\", \"${CMAKE_MATCH_3}\"},\n" )
    else()
      message( FATAL_ERROR "CHANGELOG.md: a release heading without \"<version> - <yyyy-mm-dd>\": ${heading}" )
    endif()
  endforeach()
  if ( releases STREQUAL "" )
    message( FATAL_ERROR "CHANGELOG.md names no release" )
  endif()
  file( CONFIGURE OUTPUT ${UXPLAY_GENERATED}/uxplay_release_history.inc CONTENT "${releases}" @ONLY )
endfunction()
uxplay_generate_build_info()

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
      src/uxplay_update_check.cppm
      src/uxplay_build_info.cppm
      src/uxplay_system_report.cppm
      src/uxplay_about_window.cppm
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
    src/uxplay_update_check.cpp
    src/uxplay_build_info.cpp
    src/uxplay_system_report.cpp
    src/uxplay_about_window.cpp
    src/native_media_bridge.cpp )
# the two tables written above (the compiler's own list of included files does
# not exist before the first build)
set_property( SOURCE src/uxplay_build_info.cpp APPEND PROPERTY OBJECT_DEPENDS
  ${UXPLAY_GENERATED}/uxplay_build_inputs.inc ${UXPLAY_GENERATED}/uxplay_release_history.inc )
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
  d3d11 dxgi dwmapi avrt ole32 oleaut32 uuid user32 gdi32 bcrypt ws2_32 crypt32 iphlpapi cfgmgr32 d2d1 dwrite
  imm32 shcore )

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
