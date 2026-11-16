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
add_sources("NoUberFile"
    SOURCE_GROUP "Root"
		"Plugin.cpp"
		"StdAfx.cpp"
		"Plugin.h"
		"PluginEnv.h"
		"StdAfx.h"
)
add_sources("CEVPlayer_audio_uber_0.cpp"
    SOURCE_GROUP "Audio"
		"Audio/AudioDecoder.cpp"
		"Audio/AudioWrapper.cpp"
		"Audio/AudioDecoder.h"
		"Audio/AudioWrapper.h"
)
add_sources("CEVPlayer_core_uber_0.cpp"
    SOURCE_GROUP "Core"
		"Core/DecoderThread.cpp"
		"Core/ThreadManager.cpp"
		"Core/Timer.cpp"
		"Core/DecoderThread.h"
		"Core/ThreadManager.h"
		"Core/Timer.h"
)
add_sources("CEVPlayer_flownodes_uber_0.cpp"
    SOURCE_GROUP "FlowNodes"
		"FlowNodes/FlowNodes.cpp"
)
add_sources("CEVPlayer_input_uber_0.cpp"
    SOURCE_GROUP "Input"
		"Input/InputDispatcher.cpp"
		"Input/InputDispatcher.h"
)
add_sources("CEVPlayer_render_uber_0.cpp"
    SOURCE_GROUP "Render"
		"Render/Frame.cpp"
		"Render/FrameBuffer.cpp"
		"Render/RenderWrapper.cpp"
		"Render/Frame.h"
		"Render/FrameBuffer.h"
		"Render/RenderWrapper.h"
)
add_sources("CEVPlayer_components_uber_0.cpp"
    SOURCE_GROUP "Components"
		"Components/TextureVideoPlayerComponent.cpp"
		"Components/TextureVideoPlayerComponent.h"
		"Components/ILightComponent.h"
)
add_sources("CEVPlayer_utils_uber_0.cpp"
    SOURCE_GROUP "Utils"
		"Utils/Buffer.h"
		"Utils/ObjectsPool.h"
		"Utils/PacketQueue.h"
		"Utils/SafeQueue.h"
)
add_sources("CEVPlayer_video_uber_0.cpp"
    SOURCE_GROUP "Video"
		"Video/2DVideoQueue.cpp"
		"Video/Packet.cpp"
		"Video/TextureVideoQueue.cpp"
		"Video/VideoFileReader.cpp"
		"Video/VideoPlayer.cpp"
		"Video/YuvToRgba.cpp"
		"Video/2DVideoQueue.h"
		"Video/Packet.h"
		"Video/TextureVideoQueue.h"
		"Video/VideoFileReader.h"
		"Video/VideoPlayer.h"
		"Video/YuvToRgba.h"
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

# Third-party SDKs (prebuilt static libs under ${SDK_DIR}/<name>, see Tools/CMake/modules/CEVPlayerSDK.cmake).
# Libav/swscale is no longer used: the I420 -> RGBA conversion is Video/YuvToRgba.cpp.
foreach(CEV_SDK libvpx libwebm libogg libvorbis)
	if(NOT TARGET ${CEV_SDK})
		include("${TOOLS_CMAKE_DIR}/modules/${CEV_SDK}.cmake")
	endif()
endforeach()

# libvorbis (packet level API only, no vorbisfile) pulls in libogg. libopus is not used by this code yet.
target_link_libraries(${THIS_PROJECT} PRIVATE libvpx libwebm libvorbis libogg)

# SDL2 (audio output): the engine's own SDK/module (2.0.8)
if(NOT TARGET SDL2)
	include("${TOOLS_CMAKE_DIR}/modules/SDL2.cmake")
endif()
target_link_libraries(${THIS_PROJECT} PRIVATE SDL2)
