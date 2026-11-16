if (WINDOWS AND OPTION_ENGINE AND NOT OPTION_DEDICATED_SERVER)
	option(PLUGIN_CEVPLAYER "Enable CEVPlayer plug-in for video playing" ON)
else()
	option(PLUGIN_CEVPLAYER "Enable CEVPlayer plug-in for video playing" OFF)
endif()
	
if (NOT PLUGIN_CEVPLAYER)
	return()
endif()

start_sources()

sources_platform(ALL)
# Every file is its own translation unit (no uber files): the bridge files use anonymous namespaces with common names.
add_sources("NoUberFile"
    SOURCE_GROUP "Root"
		"Plugin.cpp"
		"StdAfx.cpp"
		"Console.cpp"
		"Plugin.h"
		"PluginEnv.h"
		"StdAfx.h"
		"Console.h"
)
add_sources("NoUberFile"
    SOURCE_GROUP "Player"
		"Player/CryPakByteSource.cpp"
		"Player/PlayerManager.cpp"
		"Player/VideoPlayerImpl.cpp"
		"Player/CryPakByteSource.h"
		"Player/PlayerManager.h"
		"Player/VideoPlayerImpl.h"
)
add_sources("NoUberFile"
    SOURCE_GROUP "Audio"
		"Audio/AudioMixer.cpp"
		"Audio/AudioMixer.h"
)
add_sources("NoUberFile"
    SOURCE_GROUP "Render"
		"Render/FullscreenOutput.cpp"
		"Render/RenderBridge.cpp"
		"Render/FullscreenOutput.h"
		"Render/RenderBridge.h"
)
add_sources("NoUberFile"
    SOURCE_GROUP "Compat"
		"Compat/LegacyPlayback.cpp"
		"Compat/LegacyPlayback.h"
)
add_sources("NoUberFile"
    SOURCE_GROUP "FlowNodes"
		"FlowNodes/FlowNodes.cpp"
)
add_sources("NoUberFile"
    SOURCE_GROUP "Input"
		"Input/InputDispatcher.cpp"
		"Input/InputDispatcher.h"
)
add_sources("NoUberFile"
    SOURCE_GROUP "Components"
		"Components/TextureVideoPlayerComponent.cpp"
		"Components/TextureVideoPlayerComponent.h"
		"Components/ILightComponent.h"
)
end_sources()

CryEngineModule(CEVPlayer FORCE_SHARED_WIN PCH "StdAfx.cpp" SOLUTION_FOLDER "Project/Plugins")

target_include_directories(${THIS_PROJECT} 
	PRIVATE 
		"${CRYENGINE_DIR}/Code/CryEngine/CryCommon"
		"${CRYENGINE_DIR}/Code/CryEngine/CryAction"
		"${CRYENGINE_DIR}/Code/CryEngine/CrySchematyc/Core/Interface"
	PUBLIC
		../Interface
)

add_definitions("/wd4996 /wd4800")
set_target_properties(${THIS_PROJECT} PROPERTIES LINK_FLAGS "/ignore:4099 /ignore:4075 /ignore:4098")

# CEVPlayerCore: CE independent decoding core (static library). It links the imported SDK targets libvpx / libwebm /
# libogg / libvorbis / libopus (Tools/CMake/modules/*.cmake, included by Core/CMakeLists.txt when they do not exist yet).
add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../Core" "${CMAKE_CURRENT_BINARY_DIR}/CEVPlayerCore")
target_link_libraries(${THIS_PROJECT} PRIVATE CEVPlayerCore)
set_target_properties(CEVPlayerCore PROPERTIES FOLDER "Project/Plugins")

# SDL2 (audio output): the engine's own SDK/module (2.0.8)
if(NOT TARGET SDL2)
	include("${TOOLS_CMAKE_DIR}/modules/SDL2.cmake")
endif()
target_link_libraries(${THIS_PROJECT} PRIVATE SDL2)
