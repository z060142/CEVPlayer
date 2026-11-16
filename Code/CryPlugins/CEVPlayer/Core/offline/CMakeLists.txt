# CEVPlayerOffline - offline media tools (probe, WebM audio extract / strip, WAV, Ogg Vorbis) on top of
# CEVPlayerCore. CRYENGINE independent, editor side only: the runtime plugin does not link it.
#
# Included by ../CMakeLists.txt (add_subdirectory(offline)). Standalone it is built with everything else
# (plus cevoffline_selftest); inside an engine CMake tree the target is EXCLUDE_FROM_ALL, so it is only built
# when something depends on it (the editor plugin: target_link_libraries(<editor plugin> PRIVATE CEVPlayerOffline)).
#
#   <build dir>/Release/cevoffline_selftest.exe --media <dir with the make_test_media.sh clips>

if(NOT TARGET CEVPlayerCore)
	message(FATAL_ERROR "CEVPlayerOffline: include it from Core/CMakeLists.txt (CEVPlayerCore must exist first)")
endif()

# libvorbisenc / libvorbisfile are not part of the engine's module directory until sync_sources.sh copied them:
# fall back to this repository's cmake/modules.
get_filename_component(_cev_offline_repo_root "${CMAKE_CURRENT_LIST_DIR}/../../../../.." ABSOLUTE)
foreach(_cev_sdk libvorbisenc libvorbisfile)
	if(NOT TARGET ${_cev_sdk})
		if(DEFINED _cev_modules AND EXISTS "${_cev_modules}/${_cev_sdk}.cmake")
			include("${_cev_modules}/${_cev_sdk}.cmake")
		else()
			include("${_cev_offline_repo_root}/cmake/modules/${_cev_sdk}.cmake")
		endif()
	endif()
endforeach()

set(CEV_OFFLINE_SOURCES
	src/AudioExport.cpp
	src/Common.cpp
	src/Internal.h
	src/Probe.cpp
	src/Remux.cpp)

set(CEV_OFFLINE_HEADERS
	include/CEVOffline/Offline.h)

if(CEV_CORE_STANDALONE)
	add_library(CEVPlayerOffline STATIC ${CEV_OFFLINE_SOURCES} ${CEV_OFFLINE_HEADERS})
else()
	add_library(CEVPlayerOffline STATIC EXCLUDE_FROM_ALL ${CEV_OFFLINE_SOURCES} ${CEV_OFFLINE_HEADERS})
endif()
target_include_directories(CEVPlayerOffline PUBLIC "${CMAKE_CURRENT_LIST_DIR}/include" PRIVATE "${CMAKE_CURRENT_LIST_DIR}/src")
target_link_libraries(CEVPlayerOffline PUBLIC CEVPlayerCore libwebm libvorbisenc libvorbis libogg)
set_target_properties(CEVPlayerOffline PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
if(NOT CMAKE_VERSION VERSION_LESS 3.8)
	target_compile_features(CEVPlayerOffline PUBLIC cxx_std_17)
endif()
source_group("Include" FILES ${CEV_OFFLINE_HEADERS})

if(MSVC)
	target_compile_options(CEVPlayerOffline PRIVATE /W4 /permissive- /utf-8 /EHsc /external:W0 /wd4505)
	if(CEV_CORE_STANDALONE)
		target_compile_options(CEVPlayerOffline PRIVATE /WX /MP)
	endif()
	# _wfopen / fopen on UTF-8 converted paths (the engine builds with /WX: no C4996 there either).
	target_compile_definitions(CEVPlayerOffline PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
else()
	target_compile_options(CEVPlayerOffline PRIVATE -Wall -Wextra)
endif()

# ---------------------------------------------------------------------------------------------------------------
# Selftest (standalone only)
# ---------------------------------------------------------------------------------------------------------------
if(CEV_CORE_STANDALONE AND CEV_BUILD_SELFTEST)
	add_executable(cevoffline_selftest tests/cevoffline_selftest.cpp)
	target_link_libraries(cevoffline_selftest PRIVATE CEVPlayerOffline libvorbisfile)
	target_compile_definitions(cevoffline_selftest PRIVATE CEV_DEFAULT_MEDIA_DIR="${CEV_TEST_MEDIA_DIR}")
	if(MSVC)
		target_compile_options(cevoffline_selftest PRIVATE /W4 /WX /permissive- /utf-8 /EHsc /external:W0)
		target_compile_definitions(cevoffline_selftest PRIVATE _CRT_SECURE_NO_WARNINGS)
		if(CEV_ENABLE_ASAN)
			target_compile_options(cevoffline_selftest PRIVATE /fsanitize=address /Zi)
			target_compile_options(CEVPlayerOffline PRIVATE /fsanitize=address /Zi)
			set_target_properties(cevoffline_selftest PROPERTIES LINK_FLAGS "/INCREMENTAL:NO")
		endif()
	endif()
	enable_testing()
	add_test(NAME cevoffline_selftest COMMAND cevoffline_selftest --media "${CEV_TEST_MEDIA_DIR}")
endif()
