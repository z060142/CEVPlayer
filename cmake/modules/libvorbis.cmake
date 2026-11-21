# libvorbis: prebuilt static library, see CEVPlayerSDK.cmake for the SDK layout.
# Vorbis decoder (packet level API: vorbis_synthesis_*). vorbisenc.lib / vorbisfile.lib have their own targets
# (libvorbisenc.cmake, libvorbisfile.cmake: editor side only, the runtime does not use them). Needs libogg.
if(NOT TARGET libvorbis)
	include("${CMAKE_CURRENT_LIST_DIR}/CEVPlayerSDK.cmake")
	cev_add_imported_static(libvorbis libvorbis LIBS "vorbis|libvorbis")
	if(NOT TARGET libogg)
		include("${CMAKE_CURRENT_LIST_DIR}/libogg.cmake")
	endif()
	set_property(TARGET libvorbis APPEND PROPERTY INTERFACE_LINK_LIBRARIES libogg)
endif()
