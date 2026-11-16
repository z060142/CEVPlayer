# libogg: prebuilt static library, see CEVPlayerSDK.cmake for the SDK layout.
# Ogg bitstream library (needed by libvorbis, ogg_packet type).
if(NOT TARGET libogg)
	include("${CMAKE_CURRENT_LIST_DIR}/CEVPlayerSDK.cmake")
	cev_add_imported_static(libogg libogg LIBS "ogg|libogg")
endif()
