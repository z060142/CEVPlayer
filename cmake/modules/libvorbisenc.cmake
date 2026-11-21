# libvorbisenc: prebuilt static library (vorbisenc.lib of the libvorbis SDK), see CEVPlayerSDK.cmake for the SDK layout.
# Vorbis encoder (vorbis/vorbisenc.h: vorbis_encode_init_vbr & co). Used by CEVPlayerOffline only (editor side),
# never by the runtime plugin. Needs libvorbis (and through it libogg).
if(NOT TARGET libvorbisenc)
	include("${CMAKE_CURRENT_LIST_DIR}/CEVPlayerSDK.cmake")
	if(NOT TARGET libvorbis)
		include("${CMAKE_CURRENT_LIST_DIR}/libvorbis.cmake")
	endif()
	cev_add_imported_static(libvorbisenc libvorbis LIBS "vorbisenc")
	set_property(TARGET libvorbisenc APPEND PROPERTY INTERFACE_LINK_LIBRARIES libvorbis)
endif()
