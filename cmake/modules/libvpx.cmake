# libvpx: prebuilt static library, see CEVPlayerSDK.cmake for the SDK layout.
# VP8/VP9 codec. The library is named vpx.lib (vcpkg/CMake builds) or libvpx.lib (upstream MSVC builds).
if(NOT TARGET libvpx)
	include("${CMAKE_CURRENT_LIST_DIR}/CEVPlayerSDK.cmake")
	cev_add_imported_static(libvpx libvpx LIBS "vpx|libvpx")
endif()
