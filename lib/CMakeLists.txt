include_directories( playfair llhttp )

# Optimization and the portable CPU baseline (x86-64-v2) come from the build
# script's CMAKE_C_FLAGS and the build type; no -march=native here.
aux_source_directory( . play_src )
add_library( airplay STATIC ${play_src} )
target_compile_definitions( airplay PRIVATE
  STANDALONE __STDC_CONSTANT_MACROS __STDC_LIMIT_MACROS TARGET_POSIX _REENTRANT _LARGEFILE64_SOURCE
  _FILE_OFFSET_BITS=64
  # drop existing connections if a third connection is made
  NOHOLD )
# -Wsign-compare is off: the upstream socket code keeps Windows SOCKET handles
# (unsigned) in int variables, which every FD_SET/INVALID_SOCKET use flags
target_compile_options( airplay PRIVATE -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare -Werror )
# libplist and OpenSSL are linked statically (one executable)
target_compile_definitions( airplay PUBLIC LIBPLIST_STATIC )
target_link_libraries( airplay
  pthread
  playfair
  dnssd
  llhttp
  iphlpapi
  ws2_32 )

# libplist (pinned main-branch build, static; LIBPLIST is set by the build
# script); the code uses the libplist >= 2.3 API (plist_mem_free)
find_package( PkgConfig REQUIRED )
pkg_search_module( PLIST REQUIRED libplist-2.0>=2.3.0 )
find_library( LIBPLIST ${PLIST_LIBRARIES} PATH ${PLIST_LIBDIR} )
target_link_libraries( airplay ${LIBPLIST} )
message( STATUS "found libplist-${PLIST_VERSION}" )
target_include_directories( airplay PRIVATE ${PLIST_INCLUDE_DIRS} )

# libcrypto (pinned main-branch OpenSSL, static)
find_package( OpenSSL 1.1.1 REQUIRED )
target_compile_definitions( airplay PUBLIC OPENSSL_API_COMPAT=0x10101000L )
target_link_libraries( airplay OpenSSL::Crypto )
