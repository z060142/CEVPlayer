# CEVPlayerCore - CRYENGINE independent decoding core (static library) + selftest.
#
# Two ways to build it:
#
# (a) Standalone (selftest, CI, development without the engine). System CMake >= 3.14:
#       cmake -S Code/CryPlugins/CEVPlayer/Core -B <build dir> -G "Visual Studio 17 2022" -A x64
#       cmake --build <build dir> --config Release -- -m:4
#       <build dir>/Release/cevcore_selftest.exe --media <dir with the make_test_media.sh clips>
#     SDKs are taken from <repo>/Code/SDKs (override: -DCEV_SDK_DIR=...), the imported SDK targets from
#     <repo>/cmake/modules (override: -DCEV_CMAKE_MODULES_DIR=...).
#     Extra options: -DCEV_ENABLE_ASAN=ON (MSVC /fsanitize=address), -DCEV_BUILD_SELFTEST=OFF.
#
# (b) Inside a CRYENGINE CMake tree (CMake 3.22 there), from the plugin's Module/CMakeLists.txt, after
#     CryEngineModule(CEVPlayer ...):
#
#         # CEVPlayerCore: CE independent decoding core. It links the imported SDK targets libvpx / libwebm /
#         # libogg / libvorbis / libopus (Tools/CMake/modules/*.cmake, included here when they do not exist yet).
#         add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../Core" "${CMAKE_CURRENT_BINARY_DIR}/CEVPlayerCore")
#         target_link_libraries(${THIS_PROJECT} PRIVATE CEVPlayerCore)
#         set_target_properties(CEVPlayerCore PROPERTIES FOLDER "Project/Plugins")
#
#     In this mode no selftest is built and no warning-as-error flag is set; CEV_SDK_DIR / SDK_DIR and the
#     engine's CMake module directory (TOOLS_CMAKE_DIR) are used when present. The library is plain
#     C++17 without CRYENGINE headers (it never throws; the STL it uses may still throw bad_alloc).

cmake_minimum_required(VERSION 3.14)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
	set(CEV_CORE_STANDALONE ON)
else()
	set(CEV_CORE_STANDALONE OFF)
endif()

if(CEV_CORE_STANDALONE)
	project(CEVPlayerCore LANGUAGES CXX)
	set(CMAKE_CXX_STANDARD 17)
	set(CMAKE_CXX_STANDARD_REQUIRED ON)
	set(CMAKE_CXX_EXTENSIONS OFF)
	set_property(GLOBAL PROPERTY USE_FOLDERS ON)
	# The imported SDK targets only define DEBUG / PROFILE / RELEASE locations.
	set(CMAKE_MAP_IMPORTED_CONFIG_RELWITHDEBINFO "RELEASE;PROFILE")
	set(CMAKE_MAP_IMPORTED_CONFIG_MINSIZEREL "RELEASE")
endif()

# ---------------------------------------------------------------------------------------------------------------
# SDK locations
# ---------------------------------------------------------------------------------------------------------------
get_filename_component(CEV_CORE_REPO_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../../.." ABSOLUTE)
set(CEV_SDK_DIR "${CEV_CORE_REPO_ROOT}/Code/SDKs" CACHE PATH "Directory that contains libvpx/ libwebm/ libogg/ libvorbis/ libopus/")
if(NOT DEFINED SDK_DIR)
	set(SDK_DIR "${CEV_SDK_DIR}") # variable name used by cmake/modules/CEVPlayerSDK.cmake
endif()

set(CEV_CMAKE_MODULES_DIR "" CACHE PATH "Directory with libvpx.cmake & co (default: TOOLS_CMAKE_DIR/modules in the engine, <repo>/cmake/modules standalone)")
if(CEV_CMAKE_MODULES_DIR)
	set(_cev_modules "${CEV_CMAKE_MODULES_DIR}")
elseif(DEFINED TOOLS_CMAKE_DIR AND EXISTS "${TOOLS_CMAKE_DIR}/modules/libvpx.cmake")
	set(_cev_modules "${TOOLS_CMAKE_DIR}/modules")
else()
	set(_cev_modules "${CEV_CORE_REPO_ROOT}/cmake/modules")
endif()

foreach(_cev_sdk libogg libvpx libwebm libvorbis libopus)
	if(NOT TARGET ${_cev_sdk})
		include("${_cev_modules}/${_cev_sdk}.cmake")
	endif()
endforeach()

# ---------------------------------------------------------------------------------------------------------------
# Library
# ---------------------------------------------------------------------------------------------------------------
set(CEV_CORE_SOURCES
	src/AudioClock.cpp
	src/AudioDecoder.cpp
	src/AudioRing.h
	src/Clock.cpp
	src/ColorConvert.cpp
	src/DecodePool.cpp
	src/FrameRing.cpp
	src/IByteSource.cpp
	src/PlayerCore.cpp
	src/Resampler.cpp
	src/Stats.cpp
	src/Status.cpp
	src/VideoDecoder.cpp
	src/WebmDemuxer.cpp)

set(CEV_CORE_HEADERS
	include/CEVCore/AudioClock.h
	include/CEVCore/AudioDecoder.h
	include/CEVCore/ColorConvert.h
	include/CEVCore/DecodePool.h
	include/CEVCore/FrameRing.h
	include/CEVCore/IByteSource.h
	include/CEVCore/IClock.h
	include/CEVCore/PlayerCore.h
	include/CEVCore/Resampler.h
	include/CEVCore/Stats.h
	include/CEVCore/Status.h
	include/CEVCore/VideoDecoder.h
	include/CEVCore/WebmDemuxer.h)

add_library(CEVPlayerCore STATIC ${CEV_CORE_SOURCES} ${CEV_CORE_HEADERS})
target_include_directories(CEVPlayerCore PUBLIC "${CMAKE_CURRENT_LIST_DIR}/include" PRIVATE "${CMAKE_CURRENT_LIST_DIR}/src")
target_link_libraries(CEVPlayerCore PUBLIC libvpx libwebm libvorbis libogg libopus)
if(CEV_CORE_STANDALONE AND NOT MSVC)
	find_package(Threads REQUIRED)
	target_link_libraries(CEVPlayerCore PUBLIC Threads::Threads)
endif()
set_target_properties(CEVPlayerCore PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
if(NOT CMAKE_VERSION VERSION_LESS 3.8)
	target_compile_features(CEVPlayerCore PUBLIC cxx_std_17)
endif()
source_group("Include" FILES ${CEV_CORE_HEADERS})

if(MSVC)
	target_compile_options(CEVPlayerCore PRIVATE /W4 /permissive- /utf-8)
	# The engine builds plugins without /EHsc; Core catches allocation failures internally
	# (no exception crosses its API), so it needs unwind semantics of its own.
	target_compile_options(CEVPlayerCore PRIVATE /EHsc)
	# Third party headers (mkvparser, libvpx, ...) come in through imported targets; keep their warnings out.
	target_compile_options(CEVPlayerCore PRIVATE /external:W0)
	# libvpx headers define unused static inline control wrappers; C4505 ignores /external:W0 and the engine builds with /WX.
	target_compile_options(CEVPlayerCore PRIVATE /wd4505)
	if(CEV_CORE_STANDALONE)
		target_compile_options(CEVPlayerCore PRIVATE /WX /MP)
		target_compile_definitions(CEVPlayerCore PRIVATE _CRT_SECURE_NO_WARNINGS)
	endif()
	target_compile_definitions(CEVPlayerCore PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
else()
	target_compile_options(CEVPlayerCore PRIVATE -Wall -Wextra)
endif()

# ---------------------------------------------------------------------------------------------------------------
# Selftest (standalone only)
# ---------------------------------------------------------------------------------------------------------------
if(CEV_CORE_STANDALONE)
	option(CEV_BUILD_SELFTEST "Build cevcore_selftest" ON)
	option(CEV_ENABLE_ASAN "MSVC: build with /fsanitize=address" OFF)
	set(CEV_TEST_MEDIA_DIR "${CEV_CORE_REPO_ROOT}/test_media" CACHE PATH "Default clip directory baked into the selftest (--media overrides)")

	if(CEV_ENABLE_ASAN AND MSVC)
		# ASan needs no /RTC1 and no incremental linking; keep debug info for readable reports.
		string(REGEX REPLACE "/RTC[1csu]+" "" CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG}")
		target_compile_options(CEVPlayerCore PRIVATE /fsanitize=address /Zi)
	endif()

	if(CEV_BUILD_SELFTEST)
		add_executable(cevcore_selftest tests/cevcore_selftest.cpp)
		target_link_libraries(cevcore_selftest PRIVATE CEVPlayerCore)
		target_compile_definitions(cevcore_selftest PRIVATE CEV_DEFAULT_MEDIA_DIR="${CEV_TEST_MEDIA_DIR}")
		if(MSVC)
			target_compile_options(cevcore_selftest PRIVATE /W4 /WX /permissive- /utf-8 /external:W0)
			target_compile_definitions(cevcore_selftest PRIVATE _CRT_SECURE_NO_WARNINGS)
			if(CEV_ENABLE_ASAN)
				target_compile_options(cevcore_selftest PRIVATE /fsanitize=address /Zi)
				set_target_properties(cevcore_selftest PROPERTIES LINK_FLAGS "/INCREMENTAL:NO")
			endif()
		endif()
		enable_testing()
		add_test(NAME cevcore_selftest COMMAND cevcore_selftest --media "${CEV_TEST_MEDIA_DIR}")
	endif()
endif()
