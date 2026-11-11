cmake_minimum_required( VERSION 4.4 )

# MirrorDock (up to 1.6: "UxPlay for Windows (native)"; see README_AI.md).
# Internal names (targets, modules) keep the uxplay prefix:
#   uxplay.cpp   program: options, AirPlay callbacks, main loop (GPL-3.0)
#   lib/         AirPlay protocol core (upstream UxPlay; GPL-3.0 / LGPL-2.1+)
#   win_native/  C++26 media engine: Direct3D 11 + FFmpeg D3D11VA, WASAPI (MIT)
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

# service discovery: UxPlay's own minimal mDNS responder (lib/mdnsd)
add_subdirectory( lib/mdnsd )
add_subdirectory( lib/llhttp )
add_subdirectory( lib/playfair )
add_subdirectory( lib )
add_subdirectory( win_native )

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
