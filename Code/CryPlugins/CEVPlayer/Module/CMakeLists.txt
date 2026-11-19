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
		"Player/VideoDescriptor.cpp"
		"Player/CryPakByteSource.h"
		"Player/PlayerManager.h"
		"Player/VideoPlayerImpl.h"
		"Player/VideoDescriptor.h"
)
add_sources("NoUberFile"
    SOURCE_GROUP "Contract"
		"Contract/TexSource.cpp"
		"Contract/ContractTest.cpp"
		"Contract/TexSource.h"
		"Contract/ContractTest.h"
)
add_sources("NoUberFile"
    SOURCE_GROUP "Audio"
		"Audio/AudioMixer.cpp"
		"Audio/AudioMixer.h"
		"Audio/CueRoute.cpp"
		"Audio/CueRoute.h"
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
    SOURCE_GROUP "Bindings"
		"Bindings/ApiTable.cpp"
		"Bindings/ApiPlayers.cpp"
		"Bindings/ApiTest.cpp"
		"Bindings/ApiTest.h"
		"Bindings/FlowApiNodes.cpp"
		"Bindings/ApiTable.h"
		"Bindings/ApiPlayers.h"
		"Bindings/CryVSEnv.h"
)
# CryVS (the community tree's Schematyc2 fork): only built when the tree ships its public headers. The vanilla engine
# has none of them, so CryVSEnv.cpp is not compiled and no CryVS header is ever included there (CEV_HAVE_CRYVS stays
# undefined, Plugin.cpp only includes the header-free Bindings/CryVSEnv.h under #if CEV_HAVE_CRYVS).
set(CEV_HAVE_CRYVS OFF)
if(EXISTS "${CRYENGINE_DIR}/Code/CryEngine/CryCommon/CryVS")
	set(CEV_HAVE_CRYVS ON)
	add_sources("NoUberFile"
	    SOURCE_GROUP "Bindings"
			"Bindings/CryVSEnv.cpp"
	)
endif()
add_sources("NoUberFile"
    SOURCE_GROUP "Input"
		"Input/InputDispatcher.cpp"
		"Input/InputDispatcher.h"
)
add_sources("NoUberFile"
    SOURCE_GROUP "Components"
		"Components/TextureVideoPlayerComponent.cpp"
		"Components/TextureVideoPlayerComponent.h"
		"Components/VideoPlayerComponent.cpp"
		"Components/VideoPlayerComponent.h"
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
if(CEV_HAVE_CRYVS)
	message(STATUS "CEVPlayer: CryVS headers found, building the CryVS bindings (CEV_HAVE_CRYVS=1)")
	target_compile_definitions(${THIS_PROJECT} PRIVATE CEV_HAVE_CRYVS=1)
else()
	message(STATUS "CEVPlayer: no CryVS headers, building without the CryVS bindings")
endif()
set_target_properties(${THIS_PROJECT} PROPERTIES LINK_FLAGS "/ignore:4099 /ignore:4075 /ignore:4098")

# CEVPlayerCore: CE independent decoding core (static library). It links the imported SDK targets libvpx / libwebm /
# libogg / libvorbis / libopus (Tools/CMake/modules/*.cmake, included by Core/CMakeLists.txt when they do not exist yet).
# The Sandbox plug-in EditorCEVPlayer adds the same sub directory (BuildSandbox.cmake is processed before Code/CryPlugins),
# so whoever comes first creates the target and the other one skips: both sides use this guard.
if(NOT TARGET CEVPlayerCore)
	add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../Core" "${CMAKE_CURRENT_BINARY_DIR}/CEVPlayerCore")
	set_target_properties(CEVPlayerCore PROPERTIES FOLDER "Project/Plugins")
endif()
target_link_libraries(${THIS_PROJECT} PRIVATE CEVPlayerCore)

# SDL2 (audio output): the engine's own SDK/module (2.0.8)
if(NOT TARGET SDL2)
	include("${TOOLS_CMAKE_DIR}/modules/SDL2.cmake")
endif()
target_link_libraries(${THIS_PROJECT} PRIVATE SDL2)
