cmake_minimum_required( VERSION 4.4 )

# UxPlay, Windows-native build (see README_AI.md):
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
# icon, DPI/UTF-8 manifest, version info; GUI subsystem (no console window:
# the log goes to the launching terminal or %LOCALAPPDATA%/UxPlay/uxplay.log)
enable_language( RC )
target_sources( uxplay PRIVATE win_native/resources/uxplay.rc )
target_link_options( uxplay PRIVATE -mwindows )
