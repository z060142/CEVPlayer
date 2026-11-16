# libopus: prebuilt static library, see CEVPlayerSDK.cmake for the SDK layout.
# Opus decoder (used from M1 on).
if(NOT TARGET libopus)
	include("${CMAKE_CURRENT_LIST_DIR}/CEVPlayerSDK.cmake")
	cev_add_imported_static(libopus libopus LIBS "opus|libopus")
endif()
