# libvorbisfile: prebuilt static library (vorbisfile.lib of the libvorbis SDK), see CEVPlayerSDK.cmake for the SDK layout.
# High level Vorbis file decoder (vorbis/vorbisfile.h: ov_open & co). Used by the CEVPlayerOffline selftest only.
# Needs libvorbis (and through it libogg).
if(NOT TARGET libvorbisfile)
	include("${CMAKE_CURRENT_LIST_DIR}/CEVPlayerSDK.cmake")
	if(NOT TARGET libvorbis)
		include("${CMAKE_CURRENT_LIST_DIR}/libvorbis.cmake")
	endif()
	cev_add_imported_static(libvorbisfile libvorbis LIBS "vorbisfile")
	set_property(TARGET libvorbisfile APPEND PROPERTY INTERFACE_LINK_LIBRARIES libvorbis)
endif()
