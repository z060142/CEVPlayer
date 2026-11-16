# libwebm: prebuilt static library, see CEVPlayerSDK.cmake for the SDK layout.
# WebM container: mkvparser + mkvmuxer + common in one webm.lib. Headers: <mkvparser/mkvparser.h>, <mkvparser/mkvreader.h>.
if(NOT TARGET libwebm)
	include("${CMAKE_CURRENT_LIST_DIR}/CEVPlayerSDK.cmake")
	cev_add_imported_static(libwebm libwebm LIBS "webm|libwebm")
endif()
