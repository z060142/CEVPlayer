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
		"Render/XUI2DLClient.cpp"
		"Render/FullscreenOutput.h"
		"Render/RenderBridge.h"
		"Render/XUI2DLApi.h"
		"Render/XUI2DLClient.h"
)
add_sources("NoUberFile"
    SOURCE_GROUP "Bindings"
		"Bindings/ApiTable.cpp"
		"Bindings/ApiPlayers.cpp"
		"Bindings/ApiTest.cpp"
		"Bindings/ApiTest.h"
		"Bindings/FlowApiNodes.cpp"
		"Bindings/FlowTest.cpp"
		"Bindings/FlowTest.h"
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
# L5: the XUI2D fullscreen backend. Only built when the engine's IXUIRenderer.h (XTFrame's CE_XTUI patches) has the planar
# shading (EShading::TexturePlanar, patch 18); an engine without it builds the XUI2DL backend alone. The renderer is
# checked again at run time (XUI2D_Patched) before the interface is used.
set(CEV_HAVE_XUI2D OFF)
set(CEV_IXUIRENDERER_H "${CRYENGINE_DIR}/Code/CryEngine/CryCommon/CryRenderer/IXUIRenderer.h")
if(EXISTS "${CEV_IXUIRENDERER_H}")
	file(STRINGS "${CEV_IXUIRENDERER_H}" CEV_XUI2D_PLANAR_HITS REGEX "TexturePlanar")
	if(CEV_XUI2D_PLANAR_HITS)
		set(CEV_HAVE_XUI2D ON)
		add_sources("NoUberFile"
		    SOURCE_GROUP "Render"
				"Render/FullscreenXUI2D.cpp"
				"Render/FullscreenXUI2D.h"
		)
	endif()
endif()
add_sources("NoUberFile"
    SOURCE_GROUP "Components"
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
# M10: loading-loop callbacks (IRenderer::AddLoadtimeCallback, an engine patch). Only compiled when the engine's
# IRenderer.h declares it; a stock engine keeps the old behaviour (clocks paused while a level loads). The renderer is
# checked again at run time (XUI2D_Patched bit 7) before the interface is used.
set(CEV_HAVE_LOADTIME_CALLBACKS OFF)
set(CEV_IRENDERER_H "${CRYENGINE_DIR}/Code/CryEngine/CryCommon/CryRenderer/IRenderer.h")
if(EXISTS "${CEV_IRENDERER_H}")
	file(STRINGS "${CEV_IRENDERER_H}" CEV_LOADTIME_HITS REGEX "AddLoadtimeCallback")
	if(CEV_LOADTIME_HITS)
		set(CEV_HAVE_LOADTIME_CALLBACKS ON)
	endif()
endif()
if(CEV_HAVE_LOADTIME_CALLBACKS)
	message(STATUS "CEVPlayer: IRenderer.h declares AddLoadtimeCallback (CEV_HAVE_LOADTIME_CALLBACKS=1)")
	target_compile_definitions(${THIS_PROJECT} PRIVATE CEV_HAVE_LOADTIME_CALLBACKS=1)
else()
	message(STATUS "CEVPlayer: no AddLoadtimeCallback in IRenderer.h, building without loading-loop callbacks")
endif()
if(CEV_HAVE_XUI2D)
	message(STATUS "CEVPlayer: IXUIRenderer.h has EShading::TexturePlanar, building the XUI2D fullscreen backend (CEV_HAVE_XUI2D=1)")
	target_compile_definitions(${THIS_PROJECT} PRIVATE CEV_HAVE_XUI2D=1)
else()
	message(STATUS "CEVPlayer: no IXUIRenderer.h with TexturePlanar, building without the XUI2D fullscreen backend")
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
# M9: the Media Foundation decode backend of CEVPlayerCore needs these Windows SDK libraries (CEVPlayerCore links them
# PUBLIC as well; listed here so the engine-side module states its system dependencies explicitly).
target_link_libraries(${THIS_PROJECT} PRIVATE mfplat mfuuid d3d11 dxgi)

# SDL2 (audio output): the engine's own SDK/module (2.0.8)
if(NOT TARGET SDL2)
	include("${TOOLS_CMAKE_DIR}/modules/SDL2.cmake")
endif()
target_link_libraries(${THIS_PROJECT} PRIVATE SDL2)
