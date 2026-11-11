aux_source_directory( . dnssd_src )
add_library( dnssd STATIC ${dnssd_src} )
target_include_directories( dnssd PRIVATE . )
# see lib/CMakeLists.txt for -Wno-sign-compare
target_compile_options( dnssd PRIVATE -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare -Werror )
