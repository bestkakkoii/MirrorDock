cmake_minimum_required( VERSION 4.4 )

# MirrorDock (up to 1.6: "UxPlay for Windows (native)"; see README_AI.md).
# Internal names (targets, modules) keep the uxplay prefix:
#   uxplay.cpp      program: options, AirPlay callbacks, main loop (GPL-3.0)
#   lib/            AirPlay protocol core (upstream UxPlay; GPL-3.0 / LGPL-2.1+)
#   win_native/     C++26 media engine: Direct3D 11 + FFmpeg D3D11VA, WASAPI (MIT)
#   phone_control/  C++26 remote control of the iPhone over its developer services (GPL-3.0)
if ( NOT CMAKE_HOST_SYSTEM_NAME STREQUAL "Windows" )
  message( FATAL_ERROR "This tree builds the Windows-native UxPlay only (MSYS2 CLANG64); see README_AI.md." )
endif()

# C++26 named modules with the toolchain's standard library module
set( CMAKE_EXPERIMENTAL_CXX_IMPORT_STD "f35a9ac6-8463-4d38-8eec-5d6008153e7d" )

project( uxplay C CXX )

set( CMAKE_C_STANDARD 99 )
set( CMAKE_C_STANDARD_REQUIRED ON )
set( CMAKE_C_EXTENSIONS ON )
set( CMAKE_CXX_STANDARD 26 )
set( CMAKE_CXX_STANDARD_REQUIRED ON )
set( CMAKE_CXX_EXTENSIONS ON )
set( CMAKE_CXX_MODULE_STD ON )

# Strongest practical warning set, as errors, on a target's owned sources
# only: the std module that CMake synthesizes from libc++'s std.cppm must not
# inherit it (libc++'s own `export module std;` triggers
# -Wreserved-module-identifier).  To be called from the directory that
# defines the target, after its sources are named.
function( uxplay_strict_warnings target module_set )
  get_target_property( owned_sources ${target} SOURCES )
  get_target_property( owned_modules ${target} CXX_MODULE_SET_${module_set} )
  foreach( owned_source ${owned_sources} ${owned_modules} )
    if ( owned_source MATCHES "[.]c$" )
      set_property( SOURCE ${owned_source} APPEND PROPERTY COMPILE_OPTIONS -Wall -Wextra -Werror -Wno-unused-parameter )
    elseif ( owned_source MATCHES "[.](cpp|cppm)$" )
      set_property( SOURCE ${owned_source} APPEND PROPERTY COMPILE_OPTIONS
        -Wall -Wextra -Wpedantic -Wshadow -Wnon-virtual-dtor -Wold-style-cast -Wnull-dereference
        -Wimplicit-fallthrough -Wno-unused-parameter -Werror )
    endif()
  endforeach()
endfunction()

# service discovery: UxPlay's own minimal mDNS responder (lib/mdnsd)
add_subdirectory( lib/mdnsd )
add_subdirectory( lib/llhttp )
add_subdirectory( lib/playfair )
add_subdirectory( lib )
# win_native defines uxplay_base (which phone_control uses) and the engine
# (which uses phone_control)
add_subdirectory( win_native )
add_subdirectory( phone_control )

add_executable( uxplay uxplay.cpp )
set_property( SOURCE uxplay.cpp APPEND PROPERTY COMPILE_OPTIONS -Wall -Wextra -Wno-unused-parameter -Werror )
target_link_libraries( uxplay uxplay_native airplay )
# the product's executable name (win_native/include/uxplay_version.h)
set_target_properties( uxplay PROPERTIES OUTPUT_NAME mirror_dock )
# icon, DPI/UTF-8 manifest, version info; GUI subsystem (no console window: the
# log goes to the launching terminal or %LOCALAPPDATA%/MirrorDock/mirror_dock.log)
enable_language( RC )
target_sources( uxplay PRIVATE win_native/resources/uxplay.rc )
# The RC rule does not scan what the script includes.  Named here, so that a
# new version, manifest or icon rebuilds the resources (without this the
# executable kept the version resource of the previous release).
set_property( SOURCE win_native/resources/uxplay.rc APPEND PROPERTY OBJECT_DEPENDS
  ${CMAKE_CURRENT_SOURCE_DIR}/win_native/include/uxplay_version.h
  ${CMAKE_CURRENT_SOURCE_DIR}/win_native/resources/uxplay.manifest
  ${CMAKE_CURRENT_SOURCE_DIR}/win_native/resources/uxplay.ico )
target_link_options( uxplay PRIVATE -mwindows )
