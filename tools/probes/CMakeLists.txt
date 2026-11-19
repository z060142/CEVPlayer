# S0 measurement probes (not part of any product target).
#   cmake -S tools/probes -B <build dir> -G "Visual Studio 17 2022" -A x64 -DCEV_SDK_DIR=<dir with CEVPlayer/...>
#   cmake --build <build dir> --config Release
cmake_minimum_required(VERSION 3.14)
project(CEVProbes LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MAP_IMPORTED_CONFIG_RELWITHDEBINFO "RELEASE;PROFILE")
set(CMAKE_MAP_IMPORTED_CONFIG_MINSIZEREL "RELEASE")
get_filename_component(CEV_PROBES_REPO_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
add_subdirectory("${CEV_PROBES_REPO_ROOT}/Code/CryPlugins/CEVPlayer/Core" CEVPlayerCore)

add_executable(core_bench core_bench/core_bench.cpp)
target_link_libraries(core_bench PRIVATE CEVPlayerCore)

if(WIN32)
	add_executable(mf_decode_probe mf_decode_probe/mf_decode_probe.cpp)
	target_link_libraries(mf_decode_probe PRIVATE CEVPlayerCore mfplat mfuuid d3d11 dxgi psapi winmm)
endif()
if(MSVC)
	target_compile_options(core_bench PRIVATE /W4 /utf-8 /external:W0 /wd4505)
	target_compile_definitions(core_bench PRIVATE NOMINMAX _CRT_SECURE_NO_WARNINGS)
	if(TARGET mf_decode_probe)
		target_compile_options(mf_decode_probe PRIVATE /W4 /utf-8 /external:W0 /wd4505)
		target_compile_definitions(mf_decode_probe PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
	endif()
endif()
