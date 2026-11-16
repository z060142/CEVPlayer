# EditorCEVPlayer: Sandbox plug-in of the CEVPlayer video plug-in (docs/design/M4-asset-and-editor.md, M4b).
# Registered by patches/01-register-plugins.patch (BuildSandbox.cmake: if(EXISTS .../EditorCEVPlayer/CMakeLists.txt)).

start_sources()
sources_platform(ALL)
add_sources("NoUberFile"
	SOURCE_GROUP "Root"
		"StdAfx.cpp"
		"StdAfx.h"
		"EditorCEVPlayerPlugin.cpp"
		"EditorCEVPlayerPlugin.h"
		"AutoScript.cpp"
		"AutoScript.h"
	SOURCE_GROUP "Asset"
		"Asset/Sha256.cpp"
		"Asset/Sha256.h"
		"Asset/VideoDescriptorIO.cpp"
		"Asset/VideoDescriptorIO.h"
		"Asset/VideoAssetMeta.cpp"
		"Asset/VideoAssetMeta.h"
		"Asset/VideoImport.cpp"
		"Asset/VideoImport.h"
		"Asset/VideoAssetType.cpp"
		"Asset/VideoAssetType.h"
		"Asset/VideoAssetImporter.cpp"
		"Asset/VideoAssetImporter.h"
	SOURCE_GROUP "Audio"
		"Audio/AceRegistrar.cpp"
		"Audio/AceRegistrar.h"
	SOURCE_GROUP "Offline"
		"Offline/IOfflineOps.h"
		"Offline/OfflineOps.cpp"
		"Offline/OfflineOpsReal.cpp"
		"Offline/OfflineOpsReal.h"
		"Offline/MediaInfo.cpp"
		"Offline/MediaInfo.h"
	SOURCE_GROUP "Jobs"
		"Jobs/JobRunner.cpp"
		"Jobs/JobRunner.h"
	SOURCE_GROUP "Editor"
		"Editor/ImportOptionsDialog.cpp"
		"Editor/ImportOptionsDialog.h"
		"Editor/PreviewSession.cpp"
		"Editor/PreviewSession.h"
		"Editor/VideoViewport.cpp"
		"Editor/VideoViewport.h"
		"Editor/PreviewPanel.cpp"
		"Editor/PreviewPanel.h"
		"Editor/InfoPanel.cpp"
		"Editor/InfoPanel.h"
		"Editor/VideoEditor.cpp"
		"Editor/VideoEditor.h"
)
end_sources()

CryEditorPlugin(EditorCEVPlayer PCH "StdAfx.cpp" SOLUTION_FOLDER "Sandbox/Plugins")

target_include_directories(${THIS_PROJECT} PRIVATE
	"${CMAKE_CURRENT_SOURCE_DIR}"
	"${CRYENGINE_DIR}/Code/CryPlugins/CEVPlayer/Interface"   # CEVPlayer/IVideoPlayer.h, IVideoPlugin.h
)

# CEVPlayerCore (static library, CE independent: used here for the stream info of a WebM file).
# Build order: BuildSandbox.cmake is processed before Code/CryPlugins (Tools/CMake/Build.cmake), and the runtime plug-in's
# Module/CMakeLists.txt adds the same sub directory. Both sides use the same guard: whoever comes first creates the
# target, the other one skips (otherwise "target already exists").
if(NOT TARGET CEVPlayerCore)
	add_subdirectory("${CRYENGINE_DIR}/Code/CryPlugins/CEVPlayer/Core" "${CMAKE_BINARY_DIR}/CEVPlayerCore")
	set_target_properties(CEVPlayerCore PROPERTIES FOLDER "Project/Plugins")
endif()
target_link_libraries(${THIS_PROJECT} PRIVATE CEVPlayerCore)

# CEVPlayerOffline (probe, extract / strip audio, WAV, Ogg): EXCLUDE_FROM_ALL in the engine tree, built because this plug-in links it.
target_link_libraries(${THIS_PROJECT} PRIVATE CEVPlayerOffline)
