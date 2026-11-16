# Shared helper for the CEVPlayer third-party SDK modules (libvpx, libwebm, libogg, libvorbis, libopus).
#
# Layout of every SDK, relative to ${SDK_DIR} (= <engine>/Code/SDKs by default):
#   CEVPlayer/<name>/include/...                      headers (upstream-relative include paths, e.g. vpx/vpx_decoder.h)
#   CEVPlayer/<name>/lib/win64/release/<lib>.lib      static lib for Release and Profile  (/MD)
#   CEVPlayer/<name>/lib/win64/debug/<lib>.lib        static lib for Debug                (/MDd)
#   CEVPlayer/<name>/VERSION, LICENSE
# The libraries are produced by tools/build_sdks.sh and synced into the engine by tools/sync_sources.sh.
#
# cev_add_imported_static(<target> <sdk dir name> LIBS "<lib base name>[|<alternative base name>...]")
#   Creates "add_library(<target> STATIC IMPORTED GLOBAL)" with INTERFACE_INCLUDE_DIRECTORIES and
#   IMPORTED_LOCATION_{DEBUG,PROFILE,RELEASE}. "a|b" lists alternative file names (first one that
#   exists in lib/win64/release wins). One imported target per .lib file; dependencies between
#   targets go through INTERFACE_LINK_LIBRARIES.
if(COMMAND cev_add_imported_static)
	return()
endif()

function(cev_add_imported_static TARGET_NAME SDK_NAME LIB_KEYWORD LIB_SPEC)
	if(NOT LIB_KEYWORD STREQUAL "LIBS")
		message(FATAL_ERROR "cev_add_imported_static(<target> <sdk> LIBS <lib>): bad arguments")
	endif()
	if(TARGET ${TARGET_NAME})
		return()
	endif()

	set(SDK_ROOT "${SDK_DIR}/CEVPlayer/${SDK_NAME}")
	set(SDK_LIB_ROOT "${SDK_ROOT}/lib/win64")
	if(NOT EXISTS "${SDK_ROOT}/include")
		message(FATAL_ERROR "CEVPlayer: SDK '${SDK_NAME}' not found at ${SDK_ROOT} (expected include/ and lib/win64/). "
			"Run tools/sync_sources.sh <engine-root> from the CEVPlayer repository, or pass -DSDK_DIR=... .")
	endif()

	string(REPLACE "|" ";" LIB_ALTERNATIVES "${LIB_SPEC}")

	# Release (also used for Profile)
	set(RELEASE_LIB "")
	foreach(ALT IN LISTS LIB_ALTERNATIVES)
		if(EXISTS "${SDK_LIB_ROOT}/release/${ALT}.lib")
			set(RELEASE_LIB "${SDK_LIB_ROOT}/release/${ALT}.lib")
			set(MATCHED_ALT "${ALT}")
			break()
		endif()
	endforeach()
	if(NOT RELEASE_LIB)
		message(FATAL_ERROR "CEVPlayer: ${SDK_NAME}: none of [${LIB_SPEC}].lib found in ${SDK_LIB_ROOT}/release")
	endif()

	# Debug: prefer the /MDd build, fall back to the release lib (CRT mismatch warning LNK4098 is ignored by the plugin)
	set(DEBUG_LIB "${SDK_LIB_ROOT}/debug/${MATCHED_ALT}.lib")
	if(NOT EXISTS "${DEBUG_LIB}")
		message(WARNING "CEVPlayer: ${SDK_NAME}: no debug library (${DEBUG_LIB}), using the release library in Debug builds")
		set(DEBUG_LIB "${RELEASE_LIB}")
	endif()

	add_library(${TARGET_NAME} STATIC IMPORTED GLOBAL)
	set_target_properties(${TARGET_NAME} PROPERTIES INTERFACE_INCLUDE_DIRECTORIES "${SDK_ROOT}/include")
	set_property(TARGET ${TARGET_NAME} APPEND PROPERTY IMPORTED_CONFIGURATIONS DEBUG PROFILE RELEASE)
	set_target_properties(${TARGET_NAME} PROPERTIES
		IMPORTED_LOCATION_DEBUG   "${DEBUG_LIB}"
		IMPORTED_LOCATION_PROFILE "${RELEASE_LIB}"
		IMPORTED_LOCATION_RELEASE "${RELEASE_LIB}")
endfunction()
