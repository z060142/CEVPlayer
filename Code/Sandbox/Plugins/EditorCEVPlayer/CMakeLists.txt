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
	SOURCE_GROUP "Resources"
		"EditorCEVPlayer.qrc"
		"icons/assets_video.ico"
		"icons/assets_video.png"
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
		"Asset/ImportQueue.cpp"
		"Asset/ImportQueue.h"
	SOURCE_GROUP "FFmpeg"
		"FFmpeg/FfmpegService.cpp"
		"FFmpeg/FfmpegService.h"
	SOURCE_GROUP "Settings"
		"Settings/EditorSettings.cpp"
		"Settings/EditorSettings.h"
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
		"Editor/TranscodeDialog.cpp"
		"Editor/TranscodeDialog.h"
		"Editor/PreviewAudio.cpp"
		"Editor/PreviewAudio.h"
		"Editor/PreviewMedia.cpp"
		"Editor/PreviewMedia.h"
		"Editor/PreviewSession.cpp"
		"Editor/PreviewSession.h"
		"Editor/VideoPreviewWidget.cpp"
		"Editor/VideoPreviewWidget.h"
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

# CEVPlayerFFmpeg (locate / run / download the external ffmpeg, M5): static, EXCLUDE_FROM_ALL in the engine tree, built because this
# plug-in links it. ffmpeg only ever runs as a child process: no libav* is linked, and the runtime plug-in does not link this.
# (Core/offline/CMakeLists.txt creates the target next to CEVPlayerOffline.)
target_link_libraries(${THIS_PROJECT} PRIVATE CEVPlayerFFmpeg)

# The video preview plays its sound through Qt Multimedia (QAudioOutput, E2): the import library of the Sandbox's own Qt SDK
# is linked the way EditorXTUI does it (Qt5Multimedia.dll and plugins/audio ship in bin/win_x64).
set(CEV_QT_LIB "${CRYENGINE_DIR}/Code/SDKs/Qt/5.12.3_pyside2/Qt/lib")
if (NOT EXISTS "${CEV_QT_LIB}/Qt5Multimedia.lib")
	message(FATAL_ERROR "EditorCEVPlayer: ${CEV_QT_LIB}/Qt5Multimedia.lib not found (Qt Multimedia of the Sandbox Qt SDK)")
endif()
target_link_libraries(${THIS_PROJECT} PRIVATE
	"$<$<CONFIG:Debug>:${CEV_QT_LIB}/Qt5Multimediad.lib>"
	"$<$<NOT:$<CONFIG:Debug>>:${CEV_QT_LIB}/Qt5Multimedia.lib>")
