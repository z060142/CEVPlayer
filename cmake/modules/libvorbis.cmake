# libvorbis: prebuilt static library, see CEVPlayerSDK.cmake for the SDK layout.
# Vorbis decoder (packet level API: vorbis_synthesis_*). vorbisfile/vorbisenc libs exist in the SDK but are not
# exposed as targets because CEVPlayer does not use them. Needs libogg.
if(NOT TARGET libvorbis)
	include("${CMAKE_CURRENT_LIST_DIR}/CEVPlayerSDK.cmake")
	cev_add_imported_static(libvorbis libvorbis LIBS "vorbis|libvorbis")
	if(NOT TARGET libogg)
		include("${CMAKE_CURRENT_LIST_DIR}/libogg.cmake")
	endif()
	set_property(TARGET libvorbis APPEND PROPERTY INTERFACE_LINK_LIBRARIES libogg)
endif()
