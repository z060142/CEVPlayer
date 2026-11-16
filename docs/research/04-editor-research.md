# 04 · M4 編輯器研究：影片資產型別 + Sandbox 編輯工具（唯讀研究）

範圍：CRYENGINE 5.7.1 Sandbox 插件 `Code/Sandbox/Plugins/EditorCEVPlayer`（三欄視窗、影片資產型別、WebM 匯入、ACE 觸發器自動建立）。本檔只有讀碼結論與骨架，不含實作。

## 路徑縮寫

- `V:` = `D:/coding11/Cryengine_5.7.1`（官方版，主線）
- `C:` = `D:/coding11/CryENGINE-5-7-community`
- `X:` = `D:/coding11/XTFrame-CRYENGINE`（姊妹專案，有可運作的 Sandbox 插件 EditorXTUI）
- `P:` = `D:/coding11/CEVPlayer`
- `EC:` = `V:Code/Sandbox/Plugins/EditorCommon`；`AS:` = `EC:AssetSystem`

---

## 0. 結論一覽

1. **工具視窗基底類別選 `CAssetEditor`**（`AS:AssetEditor.h:18`），不是單純的 `CDockableEditor`。它自帶 docking 容器與「已依型別過濾、和編輯器同步選取」的 Asset Browser 面板（`AS:AssetEditor.cpp:835-841` 註冊 `CAssetBrowserWidget`，`AS:Browser/AssetBrowserWidget.cpp:17`），左欄免費得到；中欄／右欄用 `RegisterDockableWidget` 加。Material Editor 是同樣模式（`V:Code/Sandbox/Plugins/MaterialEditorPlugin/MaterialEditor.cpp:248-275`）。
2. **資產型別**：`CAssetType` 子類，`GetFileExtension()` 回 `"cevvideo"`；`.cryasset` 檔名是 `<name>.cevvideo.cryasset`（`AS:AssetType.cpp:MakeMetadataFilename`，`AssetType.h:201`），`Files[0]` = `<name>.cevvideo` = 執行期描述檔。副檔名與實際檔案名無必然關係（SamplePlugin 的型別副檔名是 `smpl`，檔案是 `.txt`：`V:Code/Sandbox/Plugins/SamplePlugin/SampleAssetType.cpp:34-47`），所以我們的型別可以掛多個檔。
3. **多檔同動**：改名／搬移／刪除由 `CAssetType::RenameAsset/MoveAsset`（`virtual`，`AssetType.h:116,122`）與 `CAssetManager::DeleteAssetsWithFiles`（`AssetManager.cpp:458`）處理，對 `CAsset::GetFiles()` 內**每個檔**操作；但改名只改「檔名以舊資產名開頭」的檔（`AssetType.cpp:26-36,472-553`），且**不會改檔案內容**——`.cevvideo` 內以檔名引用的 `video=`/`file=` 要在覆寫的 `RenameAsset` 內自己改寫。
4. **拖 `.webm` 進 Asset Browser 會觸發 importer**（依副檔名比對，`AS:Browser/AssetDropHandler.cpp:214-345`）：註冊 `CAssetImporter` 子類即可。Importer 的 `ImportAssets` 在**背景執行緒**（`AssetDropHandler.cpp:175-186` 用 `ThreadingUtils::AsyncQueue`）；要在裡面跳選項對話框必須 `PostOnMainThread(...).get()`；引擎預設那個「Importing / Save to / 型別勾選」對話框無法擴充，選項對話框得自己在 importer 內再跳一次。
5. **執行期不讀 `.cryasset`**（整個 `V:Code/CryEngine` 只有 `RenderDll/Common/Textures/TextureCompiler.cpp:281` 提到，是寫檔），引擎也沒有任何自動「打包時排除 cryasset／來源」的機制（5.7.1 的 `Tools/cryassets/rcjob_cryassets.xml` 只是產生 cryasset 的 RC job）；打包排除屬專案端設定（見 §2.7）。
6. **預覽視口（依使用者指示）**：採 XTFrame 所仿的 Character Tool／Mesh Importer 做法——`QViewport`（`EC:QViewport.h`）+ `SignalRender` 回呼 + `eNotify_OnIdleUpdate` 驅動 `Update()`；在回呼裡用 `rc.pAuxGeom->PushImage(SRender2DImageDescription)` 畫棋盤與影片貼圖（§4）。**premultiplied 問題有解**：aux 的 `SRender2DImageDescription` 每張圖有自己的 `renderFlags`（`CryCommon/CryRenderer/IRenderAuxGeom.h:964`），雖然沒有 premultiplied 混合模式（只有 None/Additive/Blended，`:540-544`），但「同一張貼圖畫兩次」可以得到精確的 premultiplied 合成（§4.6）。
7. **編輯模式的 tick**：`CGameEngine::Update` 編輯分支確實每個 idle 呼叫 `UpdateBeforeSystem/AfterSystem/BeforeFinalizeCamera/BeforeRender/AfterRender/AfterRenderSubmit`（`V:Code/Sandbox/EditorQt/GameEngine.cpp:1455-1569`，BeforeRender 在 1561），所以 CEVPlayer 的 `EnableUpdate(BeforeRender/AfterRender)` 在編輯模式有效（受 `sys_MaxFPS` 節流，1490-1510 行）。**遊戲模式不跑這條分支**（走 `DoFrame`），但那時引擎本身 tick 我們的插件，工具視窗的 idle 不會來（`eNotify_OnIdleUpdate` 在遊戲模式不送，見 `X:docs/08-xtframe-integration-log.md:1097`）。
8. **ACE 沒有可供其他插件呼叫的 API**（ACE 的類別都沒匯出，只有 `IImpl` 帶 `ACE_API`，`V:.../EditorAudioControlsEditor/common/IImpl.h:10-15`）→ 走「直接寫 ACE XML + 發 `ESYSTEM_EVENT_AUDIO_REFRESH`」的後備方案（§6）。
9. **WebM 重封裝**：`webm.lib` 內有 mkvmuxer（含 `AddFrameWithDiscardPadding`、`Track::set_codec_delay/set_seek_pre_roll`），可不重編碼寫出只有音軌的 WebM；`libvorbis`/`vorbisenc`/`libogg` 都有，Vorbis 來源可無損封成 Ogg（要自己算 granulepos），Opus 來源一律走「解碼 → 對齊 → vorbisenc」。需補一個 `vorbisenc` 的 CMake imported target（現有 `libvorbis.cmake` 只暴露 `vorbis`）。

---

## 1. Sandbox 插件骨架（5.7.1）

### 1.1 CMake

範本：`V:Code/Sandbox/Plugins/SamplePlugin/CMakeLists.txt`（最小）、`X:Code/Sandbox/Plugins/EditorXTUI/CMakeLists.txt`（靜態庫＋Qt 擴充，已實際建過）。`CryEditorPlugin` 巨集（`V:Tools/CMake/CommonMacros.cmake:1048-1058`）已做：建 shared lib、輸出到 `bin/win_x64/EditorPlugins`、`-DSANDBOX_IMPORTS -DEDITOR_COMMON_IMPORTS`、`/EHsc /GR /bigobj`、`use_qt()`（AUTOMOC/AUTORCC，Qt 5.12.3，`CommonMacros.cmake:1003-1031,1111-1121`）、`target_link_libraries(PRIVATE EditorCommon)`、MFC 旗標（`_AFXDLL`）。**編輯器模組不加 `/permissive-`**（`CommonMacros.cmake:219-224` 的 EDITOR_MODULE_FLAGS 例外），EditorCommon 標頭依賴這點。

建議的 `Code/Sandbox/Plugins/EditorCEVPlayer/CMakeLists.txt`：

```cmake
start_sources()
sources_platform(ALL)
add_sources("NoUberFile"
	SOURCE_GROUP "Root"
		"StdAfx.cpp" "StdAfx.h"
		"EditorCEVPlayerPlugin.cpp" "EditorCEVPlayerPlugin.h"
		"VideoAssetType.cpp" "VideoAssetType.h"
		"VideoAssetImporter.cpp" "VideoAssetImporter.h"
		"VideoEditor.cpp" "VideoEditor.h"
		"VideoViewport.cpp" "VideoViewport.h"
		# ... Import/ Audio/ Ace/ 子目錄
)
end_sources()

CryEditorPlugin(EditorCEVPlayer PCH "StdAfx.cpp" SOLUTION_FOLDER "Sandbox/Plugins")

target_include_directories(${THIS_PROJECT} PRIVATE
	"${CRYENGINE_DIR}/Code/CryPlugins/CEVPlayer/Interface"     # IVideoPlayer.h / IVideoPlugin.h
)

# CEVPlayerCore（靜態庫）。建置順序：BuildSandbox.cmake 在 Code/CryPlugins 之前被處理
# （V:Tools/CMake/Build.cmake:35 先於 :51），Module/CMakeLists.txt 也會 add_subdirectory(Core)，
# 所以兩邊都要用同一個 guard，先到的建，後到的略過（否則 "target already exists"）。
if(NOT TARGET CEVPlayerCore)
	add_subdirectory("${CRYENGINE_DIR}/Code/CryPlugins/CEVPlayer/Core" "${CMAKE_BINARY_DIR}/CEVPlayerCore")
	set_target_properties(CEVPlayerCore PROPERTIES FOLDER "Project/Plugins")
endif()
target_link_libraries(${THIS_PROJECT} PRIVATE CEVPlayerCore)
```

- 必改：`P:Code/CryPlugins/CEVPlayer/Module/CMakeLists.txt` 的 `add_subdirectory(.../Core ...)` 要包 `if(NOT TARGET CEVPlayerCore)`（同樣的 guard）。目前（`Module/CMakeLists.txt` 尾段）沒有 guard。
- Core 內的 `include("${_cev_modules}/libogg.cmake")` 等在引擎樹用 `TOOLS_CMAKE_DIR/modules/*.cmake`（`Core/CMakeLists.txt` 註解），不論誰先 add 都能工作（`if(NOT TARGET ...)`）。
- Core 以引擎預設旗標編（CRT /MD、無 `/permissive-` 例外），編輯器插件是 `/GR /EHsc`；Core 的公開標頭只有 C++17 STL＋自己的 `cev::` 型別，混用沒問題（XTFrame 靜態庫混 CE 同樣可行，`X:docs/01-stage1-design.md:27`）。
- **不需要連結 `Sandbox`**（EditorXTUI 連是因為用到 `SANDBOX_API` 的類別）。只有要用 `EditorQt/` 的 `QPreviewWidget` 等才加 `../../EditorQt` include 與 `target_link_libraries(... Sandbox)`（`V:.../PrefabAssetType/CMakeLists.txt:16-21`）。本插件走 `QViewport`（EditorCommon 匯出），不需要。
- 寫 WebM／Ogg 需要的函式庫：`libwebm`（含 `mkvmuxer`，`webm.lib` 內有，已用二進位 grep 確認 `mkvmuxer`/`DiscardPadding`/`SeekPreRoll` 符號）、`libvorbis`、`libogg`、`libopus` 都已是 Core 的 PUBLIC 依賴，編輯器插件經 `CEVPlayerCore` 自動得到 include 與 lib。**缺 `vorbisenc`**：`P:cmake/modules/libvorbis.cmake` 註解明說 `vorbisenc/vorbisfile` 不是 target（SDK 有 `vorbisenc.lib`、`include/vorbis/vorbisenc.h`）→ 新增 `libvorbisenc.cmake`（`cev_add_imported_static(libvorbisenc libvorbis LIBS "vorbisenc")`，`CEVPlayerSDK.cmake:19`），只給編輯器插件連。
- 編輯器模組沒有 `/WX` 豁免問題：官方版 `CRYENGINE-MSVC.cmake:14` 有 `/WX`，社群版改成 `/WX-`（§8）。EditorXTUI 的 C++20 旗標處理在 `X:.../EditorXTUI/CMakeLists.txt:80-108`，我們用 C++17，不需要。

### 1.2 插件類別與註冊

`V:Code/Sandbox/Plugins/SamplePlugin/SamplePlugin.{h,cpp}`、`EditorCommon/IPlugin.h:15-88`。

```cpp
// StdAfx.h
#pragma once
#include <CryCore/Platform/platform.h>
#ifdef CRY_PLATFORM_WINDOWS
#include <CryCore/Platform/CryWindows.h>
#endif
#include "EditorCommon.h"
```

```cpp
// EditorCEVPlayerPlugin.h
#include "IPlugin.h"
#if defined(EditorCEVPlayer_EXPORTS)
	#define EDITOR_CEV_API DLL_EXPORT
#else
	#define EDITOR_CEV_API DLL_IMPORT
#endif
class EDITOR_CEV_API CEditorCEVPlayerPlugin : public IPlugin
{
public:
	CEditorCEVPlayerPlugin();   // 進入點
	~CEditorCEVPlayerPlugin();
	int32       GetPluginVersion() override     { return 1; }
	const char* GetPluginName() override        { return "CEVPlayer"; }
	const char* GetPluginDescription() override { return "CEVPlayer video asset type and editor"; }
};
```

```cpp
// EditorCEVPlayerPlugin.cpp
#include "StdAfx.h"
#include "EditorCEVPlayerPlugin.h"
#include <CryCore/Platform/platform_impl.inl>   // 每個 DLL 一次
REGISTER_PLUGIN(CEditorCEVPlayerPlugin);        // 展開成 CreatePluginInstance + RegisterPlugin() 自動註冊
```

`REGISTER_PLUGIN` 會呼叫 `CAutoRegisterClassHelper::RegisterAll()` 等（`IPlugin.h:61-76`），所以 `REGISTER_ASSET_TYPE`、`REGISTER_ASSET_IMPORTER`、`REGISTER_VIEWPANE_FACTORY` 只要寫在任一 .cpp 的全域範圍就會在插件載入時註冊；`CAssetManager::Init` 在插件載入後才跑（`AssetManager.cpp:146-148` UpdateAssetTypes/Importers），所以順序沒問題。

插件在引擎插件沒載入時要能降級：`gEnv->pSystem->GetIPluginManager()->QueryPlugin<CEVPlayer::IVideoPlugin>()`（`X:.../EditorXTUIPlugin.cpp:50-56` 同樣寫法）回 nullptr 時，視窗顯示「請在 .cryproject 啟用 CEVPlayer 外掛」，資產型別與匯入（不需要播放器）仍可用。

### 1.3 註冊可停靠視窗

`V:EC:QtViewPane.h:135-156`：

```cpp
REGISTER_VIEWPANE_FACTORY(CCevVideoEditor, "CEV Video Editor", "Tools", false);   // false = 可多開
```

巨集建立的 pane 類別以 `new widget()` 建立（`QtViewPane.h:145`），所以編輯器類別要有**無參數建構子**。`ClassName()` 的字串必須與 `CAssetType::Edit` 傳給 `CAssetEditor::OpenAssetForEdit("CEV Video Editor", pAsset)`（`AssetEditor.cpp:127-140`）的名字相同。

### 1.4 基底類別比較

| 選項 | 優點 | 缺點 |
|---|---|---|
| `CAssetEditor`（建議） | 內建 docking、左欄 `CAssetBrowserWidget`（型別過濾、遞迴、縮圖、選取同步開啟、雙擊開啟）、File 選單、儲存／關閉的未存檔保護、`OnOpenAsset/OnCloseAsset` 生命週期、`CAsset::Edit()`／雙擊資產會開到這個視窗、拖資產進視窗＝開啟（`AssetEditor.cpp:539-620`） | 一次一個資產（`AssetEditor.h:13-14`，`OpenAsset` 若該資產已在別的編輯器開著會回 false，`AssetEditor.cpp:168-175`）；左欄是引擎做的，不能改欄位 |
| `CDockableEditor` | 彈性最大 | 自己建 `CAssetBrowser`（§3）、自己處理資產開啟／同步／儲存 |
| 純 `QWidget` + `REGISTER_VIEWPANE_FACTORY`（EditorXTUI 的 `CDockableWidget`） | 最簡單 | 沒有任何資產整合 |

`CAssetEditor` 必須覆寫的純虛擬：`GetEditorName()`、`OnOpenAsset(CAsset*)`、`OnCloseAsset()`（`AssetEditor.h:107,114`）；**`OnSaveAsset(CEditableAsset&)` 必須覆寫**——預設實作直接 `GetEditingSession()->OnSaveAsset(...)`（`AssetEditor.cpp:817-820`），沒有 session 會空指標崩潰（SampleAssetEditor 覆寫，`SampleAssetEditor.cpp:99-118`）。

骨架（模仿 `MaterialEditor.cpp:103-290`）：

```cpp
class CCevVideoEditor : public CAssetEditor
{
	Q_OBJECT
public:
	CCevVideoEditor() : CAssetEditor("CEVVideo") {}               // 資產型別名稱 = GetTypeName()
	const char* GetEditorName() const override { return "CEV Video Editor"; }
protected:
	void OnInitialize() override                                    // AssetEditor.cpp:842 在 Initialize() 末尾呼叫
	{
		RegisterDockableWidget("Viewer", [this]() { return new CCevViewerPanel(this); }, true);
		RegisterDockableWidget("Info",   [this]() { return new CCevInfoPanel(this);   }, true);
	}
	void OnCreateDefaultLayout(CDockableContainer* pSender, QWidget* pAssetBrowser) override
	{
		// 左：資產瀏覽器（pAssetBrowser，已建好）；中：Viewer；右：Info
		QWidget* pViewer = pSender->SpawnWidget("Viewer", pAssetBrowser, QToolWindowAreaReference::VSplitRight);
		pSender->SpawnWidget("Info", pViewer, QToolWindowAreaReference::Right);
	}
	bool OnOpenAsset(CAsset* pAsset) override;                      // 讀 .cevvideo、通知三個面板
	void OnCloseAsset() override;                                   // 停止播放、釋放播放器
	bool OnSaveAsset(CEditableAsset& editAsset) override;           // 寫 .cevvideo、editAsset.SetFiles/SetDetails
	void dragEnterEvent(QDragEnterEvent*) override;                 // 加上 .webm 檔案拖入 = 匯入；其餘轉 CAssetEditor::dragEnterEvent
	void dropEvent(QDropEvent*) override;
};
REGISTER_VIEWPANE_FACTORY(CCevVideoEditor, "CEV Video Editor", "Tools", false);
```

`SpawnWidget` 簽名：`EC:DockingSystem/DockableContainer.h:69-71`；`SetSplitterSizes`：`:79`。拖放：`dragEnterEvent/dropEvent` 在 `AssetEditor.h:126-127` 是 `override` 而非 `final`，可再覆寫；檔案路徑用 `CDragDropData::FromMimeData(...)->GetFilePaths()`（`AssetEditor.cpp:539-596` 同樣用法）。

---

## 2. 資產型別

### 2.1 `CAssetType` 子類

簽名見 `AS:AssetType.h:57-250`。必要與建議覆寫：

```cpp
class CVideoAssetType : public CAssetType
{
public:
	DECLARE_ASSET_TYPE_DESC(CVideoAssetType);                     // ClassName() + CreateObject()
	const char* GetTypeName() const override      { return "CEVVideo"; }        // 寫進 .cryasset，不可改
	const char* GetUiTypeName() const override    { return QT_TR_NOOP("Video"); }
	const char* GetFileExtension() const override { return "cevvideo"; }        // 小寫、無點、全域唯一
	bool CanBeEdited() const override             { return true; }
	CAssetEditor* Edit(CAsset* pAsset) const override { return CAssetEditor::OpenAssetForEdit("CEV Video Editor", pAsset); }
	bool IsImported() const override              { return false; }              // 見 §2.4
	bool CanAutoRepairMetadata() const override   { return false; }              // 見 §2.5（重要）
	bool HasThumbnail() const override            { return true; }
	void GenerateThumbnail(const CAsset* pAsset) const override;                // 寫 pAsset->GetThumbnailPath()
	std::vector<CItemModelAttribute*> GetDetails() const override;
	QVariant GetDetailValue(const CAsset*, const CItemModelAttribute*) const override;
	bool RenameAsset(CAsset*, const char* newName) const override;              // §2.3
	bool MoveAsset(CAsset*, const char* destFolder, bool moveSource) const override;
	void PreDeleteAssetFiles(const CAsset&) const override;                     // §2.6
	// bool IsAssetValid(CAsset*, string&) const override;   // 預設已檢查所有 Files 存在
	QColor GetThumbnailColor() const override;
private:
	CryIcon GetIconInternal() const override;
};
REGISTER_ASSET_TYPE(CVideoAssetType)                                          // = REGISTER_CLASS_DESC
```

- 副檔名驗證在 `CAssetManager::UpdateAssetTypes`（`AssetManager.cpp:~620-700`）：小寫、無前導點、不以 `cryasset` 結尾、唯一。`cevvideo` 與引擎現有型別不衝突（引擎型別：cgf/chr/skin/dds/mtl/cga/anm/cdf/caf/wav/ogg/cax/pfx/cs/schematyc_ent/level/…，見 `V:Tools/cryassets/rcjob_cryassets.xml`）。
- 資產名稱規則（`CAssetType::IsValidAssetPath`，`AssetType.cpp` 約 `:600-650`）：**名稱不可含 `.`**，只允許英數、底線、連字號。所以變體／語言檔的檔名不能靠「資產名.xxx」切字解析資產名；要靠 `.cevvideo` 的內容與 Files 清單。
- 縮圖路徑固定 `ReplaceExtension(metadataFile, "thmb.png")` = `<name>.cevvideo.thmb.png`（`AS:Asset.cpp:463-467`）；大小 256（`ASSET_THUMBNAIL_SIZE`，`AssetType.h:17`）。`GenerateThumbnail` 範例：`V:Code/Sandbox/EditorQt/AssetSystem/TextureAssetType.cpp:47-79`（存 PNG 到 `PathUtil::Make(GetGameProjectAssetsPath(), pAsset->GetThumbnailPath())`）。我們用 Core（`WebmDemuxer` + `VideoDecoder` + `ColorConvert`）解第一個畫面，縮放成 QImage 存檔，**不需要 GPU 或 IVideoPlayer**，可在背景執行緒做。
- Detail 欄位（解析度、長度、codec、有無 alpha、語言數）：`CItemModelAttribute` 宣告方式見 `TextureAssetType.cpp:34-38`（`static CItemModelAttribute s_widthAttribute("Width", &Attributes::s_intAttributeType, CItemModelAttribute::StartHidden);`）。資料存 `.cryasset` 的 `Details`（`IEditableAsset::SetDetails`，`AssetType.h:36`；`Details` 只是字串名值對，`Metadata.cpp:19-130`），`GetDetailValue` 用 `CAsset::GetDetailValue(name)` 取。

### 2.2 `.cryasset` 格式與哪些資訊放哪

`AS:Loader/Metadata.cpp:19-130`：XML `AssetMetadata`（屬性 `version/type/guid/source/timestamp`），子節點 `Files/File@path`、`WorkFiles/WorkFile@path`、`Details/Detail@name,value`、`Dependencies/Path@...`。所有路徑相對於 `PathUtil::GetGameProjectAssetsPath()`；`AddFile` 會把與 `.cryasset` 同資料夾的前綴去掉（`AssetType.cpp:Private_AssetType::CAssetMetadata::AddFile`）。

建議分工：

| 資訊 | 放哪 | 理由 |
|---|---|---|
| 主影片、變體、語言音軌（WebM）、（可選）WAV/OGG | `Files`（`Files[0]` = `.cevvideo`） | 同動；`Files[0]` 也是依賴追蹤的鍵（`AssetManager.cpp:744-761` 以 `GetFile(0)` 查反向依賴） |
| 路線、語言、觸發器名、變體清單、標記 | `.cevvideo`（執行期描述檔，格式見 `P:docs/design/M2-audio-and-bindings.md` 與 `Module/Player/VideoDescriptor.h`） | 隨遊戲發行 |
| 來源路徑、來源雜湊、匯入設定（CRF、解析度、變體規則、是否產生 WAV…）、解析度／長度等顯示資訊、匯入狀態、ACE 登記資訊 | `Details`（純字串；巢狀設定序列化成 JSON/`key=value;` 單一字串，或拆成多個 name） | 僅編輯器；`.cryasset` 不發行 |
| 縮圖 | `<name>.cevvideo.thmb.png`（`GetAssetFiles` 自動算進移動／刪除，`AssetType.cpp:296-345`） | 內建機制 |

### 2.3 多檔的改名／搬移／刪除如何傳遞（**逐行確認過**）

- **改名**（`CAssetType::RenameAsset`，`AssetType.cpp:472-553`）：先檢查所有檔案可寫、不在 pak；改 `.cryasset`、縮圖，然後對 `Files` 的每個檔，用 `TryReplaceFilename(舊名,新名,…)`（`:26-36`，`strnicmp` 檔名前綴比對）。**只有以舊資產名開頭的檔會被改名**；不是的原地不動。所以建議**所有檔案命名 `<asset>.cevvideo`、`<asset>.webm`（主影片）、`<asset>_720.webm`、`<asset>_english.webm`、`<asset>_english.wav`…**（全部以資產名開頭，且資產名不含 `.`）。`SetFiles` 後 `WriteToFile`。
- **搬移**（`MoveAsset`，`:555-640`）：對 `GetAssetFiles(asset, bMoveSourcefile, true, true, true)` 檢查唯讀；`.cryasset`、縮圖、來源檔（若 `IsImported()` 且有 `GetSourceFile()`）、`Files` 每個檔都以 `PathUtil::GetFile(file)` 平搬到目標資料夾——**所有檔必須與資產在同一資料夾（平放）**。
- **刪除**（`CAssetManager::DeleteAssetsWithFiles`，`AssetManager.cpp:458-476`）：`ComposeFileGroupsForDeletion`（`:71`）依 `GetAssetFiles` 組檔案群，先呼叫每個資產的 `PreDeleteAssetFiles`（`AssetType.h:125`），再丟給 `CFileOperationExecutor::Delete`（背景佇列）。
- **兩個坑**：(a) 描述檔內的檔名引用不會被更新 → 覆寫 `RenameAsset`：先呼叫 `CAssetType::RenameAsset(...)`，成功後重新讀 `.cevvideo`、把 `video/file` 屬性裡的舊前綴換成新前綴、存檔（`CEditableAsset` 的建構子是 private，friend 只有 `CAsset/CAssetType/...`（`EditableAsset.h:13-24`），**子類不繼承 friend**；用公開的 `CAssetImportContext::CreateEditableAsset(CAsset&)`（`AssetImportContext.h:129`）取得，或改在 `OnSaveAsset` 內處理，或直接 `tinyxml`/`XmlNodeRef` 改檔後讓檔案監看器合併）。(b) `MoveAsset` 不需要改描述檔，因為描述檔引用的是「相對於描述檔所在資料夾」（`VideoDescriptor.h` 檔頭註解）。

### 2.4 `IsImported()` 與「來源檔」

`IsImported()` 為 true 時（`AssetType.h:157`；`Asset.cpp:140` `HasSourceFile()`、`AssetManager.cpp:548-556`）：Asset Browser 顯示「Reimport」（`AssetBrowser.cpp:1640`）、搬移時一併搬／複製 `GetSourceFile()`（`:530`）、`Reimport` 走 `CAssetImporter::ReimportAsset`（`AssetImporter.cpp:48-70`）。**來源檔必須與資產同資料夾**（`Asset.h` 註解）。貼圖流程就是這樣：`foo.tif`（來源，放在 Assets 內）+ `foo.dds.cryasset` + 衍生 `foo.dds`（`HasDerivedFiles`，放在快取，不在 Assets）。

本專案需求「來源影片不進 pak、來源雜湊變了顯示需重匯入」，而 M4 是 WebM-only（無轉檔），主影片＝來源的複本。建議：

- **不使用 `SetSourceFile`，`IsImported()` 回 false**；來源路徑（絕對路徑，使用者的原始位置）與 SHA-256 存在 `Details`（`sourcePath`、`sourceHash`、`sourceSize`、`sourceMTime`）。好處：來源不在 Assets 內 → 不可能被打包進 pak，也不受「來源必須同資料夾」限制。
- 「需重匯入」提示：編輯器視窗開啟資產、或 Asset Browser 的 `GetDetailValue("Source status")`，比較來源檔的 `size+mtime`（便宜）與（必要時）雜湊，不符顯示警告 + 右欄「重新匯入」按鈕。自己做 Reimport 動作（`AppendContextMenuActions` 加選單項，`AssetType.h:166-168`），不依賴引擎的 Reimport 鍵。
- 若之後要「來源存在 Assets 內」的模型（FFmpeg 轉檔期）：改 `IsImported() = true`、`SetSourceFile`，並在專案打包 job 排除該副檔名（§2.7）。這兩種模型的切換只影響 importer 與 `IsImported`，不影響執行期。

### 2.5 `CanAutoRepairMetadata()` 必須回 false（**重要，避免意外 RC 呼叫**）

`CAssetGenerator`（`AS:AssetGenerator.cpp:149-176`）對**每個** `CanAutoRepairMetadata()==true` 的型別，把 `<ext>,<TypeName>` 加進 RC 設定並對該副檔名 `RegisterListener`：任何新建／修改的 `*.cevvideo` 若沒有 `.cryasset` 就會呼叫 `rc.exe /overwriteextension=cryasset /assettypes=cevvideo,CEVVideo`（RC 不認得我們的型別，會失敗或產生空殼）。預設值是 true（`AssetType.h:206`）。回 false → 不註冊監看。

另一個相關的坑：引擎對**所有 `.wav`、`.ogg`** 檔案（Sound 型別，另有硬編的 `ogg,Sound`，`AssetGenerator.cpp:172-173`）自動產生 `<file>.wav.cryasset` / `.ogg.cryasset`。因此：
- 把 WAV/OGG 放在影片資產資料夾並當作 `Files` 成員，會**同時**冒出另一個 Sound 資產（兩個資產認領同一檔；刪除影片時 Sound 的 cryasset 殘留）。
- 解法 A（建議）：**資產自己的 `Files` 只放 `.cevvideo`、主影片／變體／語言音軌（都是 `.webm`）**；SDL_mixer 用的 WAV/OGG 是「登錄副本」，只放進 `audio/sdlmixer/assets/cev/<資產名>/`（本來就是 Sound 資產的合法位置），其路徑清單記在 `Details`（`sdlFiles=…`），由 `PreDeleteAssetFiles`／`RenameAsset`／`MoveAsset` 覆寫維護（刪除時一併刪副本與其 `.cryasset`、ACE 檔；改名／搬移時重新登錄）。這同時是 SDL_mixer 的硬性要求（它只掃 `audio/sdlmixer/assets/`，§6）。
- 解法 B：資產資料夾內放 `.wav`，接受 Sound 資產並行出現，並在 `PreDeleteAssetFiles` 手動刪 `<wav>.cryasset`。較亂，不建議。
- 需要與需求單對照處：需求寫「WAV 是同一個 .cryasset 的檔案」。依上述，M4 可讓 `Files` 列出 WAV（`.wav` 副檔名）而承受 Sound 資產並行問題，或採解法 A 讓它成為登錄副本。**這是需要使用者決定的設計取捨，見 §9.4。**

### 2.6 以程式建立資產（匯入器路徑）

三種方式，從簡到繁：

1. **`CAssetType::Create(path, params)`**（`AssetType.cpp:203-268`）：建立 `INewAsset`，呼叫 `OnCreate`，寫 `.cryasset`，`MergeAssets`。需要 `CanBeCreated()`／`OnCreate` 覆寫，且 `MergeAssets` 會動資產管理器 → **只能主執行緒**。用於「新增空白影片資產」之類，不適合背景匯入。
2. **Substance 匯入器模式**（建議給匯入器用，`V:Code/Sandbox/Plugins/EditorSubstance/AssetImporters/SubstanceArchiveImporter.cpp:48-92`）：
   ```cpp
   CAsset* pAsset = new CAsset("CEVVideo", CryGUID::Create(), ctx.GetAssetName());
   CEditableAsset edit = ctx.CreateEditableAsset(*pAsset);          // 公開，AssetImportContext.h:129
   edit.SetMetadataFile(relDir + name + ".cevvideo.cryasset");
   edit.SetFiles({ relDir + name + ".cevvideo", relDir + name + ".webm" });
   edit.SetDetails({ {"sourcePath", src}, {"sourceHash", hash}, {"importSettings", json}, ... });
   edit.WriteToFile();                                              // CEditableAsset::WriteToFile 是 public
   return { pAsset };                                               // 拖放流程再 MergeAssets（AssetDropHandler.cpp:175-186）
   ```
   或寫好 `.cryasset` 後 `ctx.LoadAsset(path)`（`AssetImportContext.cpp:LoadAsset`，GeometryCache 匯入器用法 `GeometryCacheAssetType.cpp:83`）。`.cryasset` 一寫出，`CAssetFileMonitor` 也會偵測並合併（`AssetFileMonitor.cpp:24-80`，貼圖匯入器就只靠這個，`AssetImporterImage.cpp:111-113` 註解）。
3. 完全手寫 `.cryasset` XML：不建議。

匯入器骨架（`AS:AssetImporter.h:21-52`、範例 `V:Code/Sandbox/Plugins/MeshImporter/AssetImporterImage.{h,cpp}`）：

```cpp
class CVideoAssetImporter : public CAssetImporter
{
public:
	DECLARE_ASSET_IMPORTER_DESC(CVideoAssetImporter);
	std::vector<string> GetFileExtensions() const override { return { "webm" }; }     // 小寫、無點
	std::vector<string> GetAssetTypes() const override      { return { "CEVVideo" }; }
private:
	std::vector<string> GetAssetNames(const std::vector<string>&, CAssetImportContext& ctx) override
	{ return { ctx.GetOutputFilePath(".cevvideo.cryasset") }; }                         // 相對 Assets；Import() 會對它 CanWrite 檢查
	std::vector<CAsset*> ImportAssets(const std::vector<string>& paths, CAssetImportContext& ctx) override;
};
REGISTER_ASSET_IMPORTER(CVideoAssetImporter)
```

必知行為：
- **`.webm` 其他型別沒有 importer**（引擎的圖片 importer 副檔名清單不含 webm，`AssetImporterImage.cpp:35-76`），不會衝突；`UpdateAssetImporters` 的驗證只是 TODO（`AssetManager.cpp:706-716`）。
- 拖放流程：`AssetFoldersModel.cpp:290-344` / `AssetBrowser.cpp:2022-2037` → `CAssetDropHandler::ImportAsync` → `AsyncQueue`（**所有匯入共用單一佇列，`ThreadingUtils.h:117-125`**）→ `ImportExt`（`AssetDropHandler.cpp:214-345`）：先在主執行緒彈引擎的 `CAssetImportDialog`（`PostOnMainThread(...).get()`；內容只有輸出資料夾與型別勾選，`AssetImportDialog.cpp:27-140`，無擴充點）→ `pAssetImporter->Import(...)` → `CAssetImporter::Import`（`AssetImporter.cpp:20-46`）對每個潛在資產路徑呼叫 `ctx.CanWrite()`（已存在則**彈覆寫確認，同樣 `PostOnMainThread().get()`**，`AssetImportContext.cpp:136-161`）→ `ImportAssets`。
- **死鎖陷阱**：`PostOnMainThread` 永遠是「投遞再回傳 future」（`ThreadingUtils.h:131-146`），在主執行緒上對它 `.get()` 會永遠卡住。因此：(a) **不要在主執行緒呼叫 `CAssetImporter::Import`/`CAssetType::Import`**（輸出檔已存在時 `CanWrite` 會自己死鎖，除非先 `ctx.SetOverwriteAll(true)`，`AssetImportContext.h:79-82`）；(b) 在 `ImportAssets` 內彈自己的選項對話框要先檢查 `QThread::currentThread() == qApp->thread()`，是就直接 `exec()`，否則 `ThreadingUtils::PostOnMainThread(...).get()`。
- **從我們自己的視窗拖入（中欄／右欄）**不能呼叫 `CAssetDropHandler`：它沒有 `EDITOR_COMMON_API`（`AssetDropHandler.h:17`，未匯出）。做法：自己彈選項對話框（主執行緒），然後把「複製檔案、寫 `.cevvideo`/`.cryasset`」做成與 importer 共用的函式，在自己的工作執行緒（§5）跑，完成後 `PostOnMainThread` 回主執行緒 `CAssetManager::GetInstance()->MergeAssets({pAsset})`。
- 長工作（音訊分離、WAV、Ogg）不要塞在 `ImportAssets`（單一共用佇列會擋住縮圖、其他匯入）：`ImportAssets` 只做「複製 WebM、寫 `.cevvideo` + `.cryasset`（Details `importState=processing`）」立即回傳，然後啟動自己的背景 job，完成後在主執行緒更新檔案清單與 Details（`edit.SetFiles(...)`、`WriteToFile()`；檔案監看器會合併）。

### 2.7 打包與「來源不進 pak」

- 5.7.1 引擎沒有內建的 pak 排除表；Sandbox 內只有匯入／產生 cryasset 時用到 RC（`PakImporter.cpp:164-199` 是**讀** pak 並產生 `*.cryasset.pak`）。貼圖的 `.tif` 沒被打包，是因為專案的打包 job 只收 `.dds` 等（沒有程式碼把來源「標記」為不打包）。`WorkFiles`（`SetWorkFiles`）是 DCC 檔案管理（`ManageWorkFilesDialog`），與打包無關。
- 因此「來源不進 pak」＝**來源不放在 Assets 內**（§2.4 建議）+ 文件建議專案打包時排除 `*.cryasset`、`*.thmb.png`、`*.cevsrc`（如果日後有）。
- **執行期只讀 `.cevvideo`（及其引用的 `.webm`／`audio/…` 檔）**；`.cryasset` 與縮圖不需要出貨。已確認 `V:Code/CryEngine` 內沒有任何執行期程式讀 `.cryasset`（grep 僅 `TextureCompiler.cpp`）。
- 依賴追蹤：`SetDependencies`（`AssetType.h:37`）宣告「此資產用到哪些資產」；關卡實體引用影片資產目前沒人寫 dependency（Schematyc 元件的 `.cevvideo` 路徑屬性）。可選：影片資產不宣告依賴；若要讓刪除時顯示「被誰使用」，需在元件序列化側登記（超出 M4）。反向依賴查詢鍵為 `Files[0]`（`AssetManager.cpp:748`）。

---

## 3. 內嵌 Asset Browser

優先用 §1.4 的 `CAssetEditor`（`CAssetBrowserWidget` 現成）。若不用 `CAssetEditor`：

```cpp
// 子類化，取得雙擊回呼與（選用）停用拖出
class CVideoBrowser : public CAssetBrowser
{
public:
	CVideoBrowser(QWidget* pParent, std::function<void(CAsset*)> onActivated)
		: CAssetBrowser(std::vector<CAssetType*>{ CAssetManager::GetInstance()->FindAssetType("CEVVideo") },
		                /*bHideEngineFolder=*/false, pParent)
		, m_onActivated(std::move(onActivated)) {}
protected:
	void OnActivated(CAsset* pAsset) override { if (m_onActivated) m_onActivated(pAsset); }   // AssetBrowser.h:122，雙擊/Enter
	void UpdatePreview(const QModelIndex&) override {}                                       // :126 選取變更時預設會開預覽；空實作＝不動
private:
	std::function<void(CAsset*)> m_onActivated;
};
// 使用
auto* pBrowser = new CVideoBrowser(this, ...);
pBrowser->Initialize();                          // CEditor::Initialize，必須呼叫（AssetBrowserDialog.cpp:127 同樣）
connect(pBrowser, &CAssetBrowser::SelectionChanged, this, [=]{ auto assets = pBrowser->GetSelectedAssets(); ... });  // AssetBrowser.h:106
pBrowser->SelectAsset(*pAsset);                  // :58-62
```

- 建構子：`CAssetBrowser(const std::vector<CAssetType*>&, bool bHideEngineFolder=false, QWidget* = nullptr)`（`AssetBrowser.h:50`）；傳入型別清單即過濾（`AssetBrowser.cpp:549-575`，`InitAssetTypeFilter`），只有一個型別時有型別欄位覆寫。
- 「唯讀模式」：沒有旗標。`CAssetBrowserDialog::CBrowser`（`AssetBrowserDialog.cpp:71-105`）是 private 巢狀類別，不能用；它做的事——`GetDetailsView()->setSelectionMode(SingleSelection)`、`GetThumbnailsView()->GetInternalView()->setSelectionMode(...)`、`setDragEnabled(false)`——這些是 `protected` accessor（`AssetBrowser.h:~130-140`），子類別可做。選單的刪除／改名無法關（想關可在 `CAssetBrowser::s_signalContextMenuRequested` 靜態訊號 `AssetBrowser.h:41-42` 側處理）。
- 嵌入沒有問題的證據：`CAssetBrowserDialog` 把 `CAssetBrowser` 放在 `QVBoxLayout`（`AssetBrowserDialog.cpp:226-238`）；`CAssetBrowserWidget` 放在 docking（`AssetEditor.cpp:835-841`）。它是 `CDockableEditor`（`AssetBrowser.h:45`），含自己的選單與 `setAcceptDrops(true)`，所以**拖 .webm 進左欄 = 引擎的匯入流程**（我們的 importer）。
- 若要自己做簡單清單（備案）：`CAssetManager::GetInstance()->ForeachAssetOfType(pType, f)`（`AssetManager.h:ForeachAssetOfType`）+ `signalAfterAssetsInserted/signalAssetsUpdated/signalAfterAssetsRemoved`（`AssetManager.h:~190-205`）自己維護 `QListView`。不建議（失去縮圖、搜尋、資料夾、右鍵）。

---

## 4. 預覽視口（依使用者指示：仿 XTFrame / Character Tool 的 QViewport）

### 4.1 XTFrame 實際怎麼做（讀碼＋文件）

- 設計文件：`X:docs/05-stage2-design.md:455-466`（§5.5.2）：「窗格預覽照 **Mesh Importer 的 QViewport**：自己的 swap chain 與 display context、Minimum 管線、每 tick 一組 `BeginFrame(K,P)`／`EndFrame`；瀏覽器窗格照 `QPreviewWidget`」。引擎端被仿的程式：
  - `QViewport`（`EC:QViewport.cpp:178-223` 建構、`:298-331` `CreateRenderContext`、`:411-458` `Update`、`:987-1030` `RenderInternal`、`:1208-1225` `resizeEvent`）；Character Tool 用法 `V:Code/Sandbox/Plugins/EditorAnimation/CharacterTool/BlendSpacePreview.cpp:59-68`（Minimum 管線、關 grid、連 `SignalRender`）、`:130`（idle → `Update()`）、`CharacterToolForm.cpp:537-560`。
  - `QPreviewWidget`（`V:Code/Sandbox/EditorQt/QT/Widgets/QPreviewWidget.cpp:268-290`、`:800-930`、`:1376-1392`；資產瀏覽器內的模型預覽）。
- **XTFrame 自己最後並沒有用 `QViewport` 類別**（`X:.../EditorPreview.cpp:262-293` 用裸 `CreateSwapChainBackedContext` + `CreateGraphicsPipeline`），原因是它要把 XUI2D 的 surface 掛在 K 上；但它的資料流、更新時機、教訓都對我們適用（§4.7）。我們沒有 XUI2D 需求，所以**直接用 `QViewport`** 最省事，且正是使用者指定的 Character Tool 做法。
- 驅動：XTFrame 窗格用 `IAutoEditorNotifyListener` 的 `eNotify_OnIdleUpdate`（`X:.../EditorXTUIPlugin.h:11-26`、`X:docs/05-stage2-design.md:419`：不可見不 tick、1/60 s 節流）；Character Tool 同（`CharacterToolForm::OnIdleUpdate`）。

### 4.2 建立視口

```cpp
IRenderer::SGraphicsPipelineDescription desc;
desc.type        = EGraphicsPipelineType::Minimum;
desc.shaderFlags = SHDF_SECONDARY_VIEWPORT | SHDF_ALLOWHDR | SHDF_FORWARD_MINIMAL;    // BlendSpacePreview.cpp:59-61 完全相同

m_pViewport = new QViewport(gEnv, desc, this);                    // 建構子內就 CreateRenderContext()（QViewport.cpp:222）
SViewportSettings s = m_pViewport->GetSettings();
s.grid.showGrid                      = false;
s.grid.origin                        = false;
s.camera.showViewportOrientation     = false;
s.rendering.fps                      = false;                     // 否則左上角畫 FPS（QViewport.cpp:972-975）
s.background.useGradient             = false;
s.background.topColor                = ColorB(24, 24, 24, 255);   // 同時是 swap chain 的 clear color 與 Qt 背景（QViewport.cpp:230-239,306）
m_pViewport->SetSettings(s);
connect(m_pViewport, &QViewport::SignalRender, this, &CCevViewerPanel::OnRender);       // QViewport.h:SignalRender(const SRenderContext&)
layout->addWidget(m_pViewport, 1);
```

- `QViewport` 是 `EDITOR_COMMON_API`，Q_OBJECT；可子類別化。**建議子類別化並覆寫 `mousePressEvent/mouseMoveEvent/mouseReleaseEvent/wheelEvent/keyPressEvent`**：基底在右鍵／中鍵按下時 `QApplication::setOverrideCursor(Qt::BlankCursor)` 並進入相機操作模式（`QViewport.cpp:1109-1127`），我們用不到（影片是 2D），而且放開失敗會卡住游標。縮放／平移影片由我們自己的 wheel/drag 處理。
- 不會用到的成本：每個 `Render` 仍會做環境探針／方向光設定（`QViewport.cpp:811-908`）與 `EF_StartEf/EF_EndEf3D`，Character Tool 一樣；影片預覽可接受。
- 建構時機：`new QViewport` 在 Qt 事件迴圈（不在任何 `BeginFrame/EndFrame` 之內）才合法——`CreateSwapChainBackedContext`／`DeleteContext` 只能在其他視口的 Begin/End 之外（XTFrame 為此把建／刪排到 `UpdateAfterRenderSubmit`，`X:.../EditorPreview.cpp:256-257`）。**不要在 `OnRender` 回呼內或 `eNotify_OnIdleUpdate` 裡正在別的視口 Begin/End 之間建立或銷毀 `QViewport`**；在 dock 面板工廠 lambda 裡建立（Qt 事件）是 Character Tool 的作法，OK。

### 4.3 驅動與尺寸

```cpp
class CCevViewerPanel : public QWidget, public IAutoEditorNotifyListener   // IEditor.h:491
{
	void OnEditorNotifyEvent(EEditorNotifyEvent e) override
	{
		if (e != eNotify_OnIdleUpdate) return;
		if (!isVisible() || !m_pViewport) return;               // 隱藏即不動（QViewport::Update 自己也檢查 isVisible，:427）
		if (GetIEditor()->IsInGameMode()) return;                // 遊戲模式不送 idle，保險
		TickTransport();                                         // 取 IVideoPlayer 時間、更新時間軸 UI
		m_pViewport->Update();                                   // 內部 RenderInternal：CScopedRenderLock → RenderBegin → Render → RenderEnd
	}
};
```

- `Update()`（`QViewport.cpp:411-458`）：不可見／尺寸 0／context 未建 → 跳過或建 context；`m_updating` 防重入；`CScopedRenderLock`（`RenderLock.h`）拿不到時整幀略過（別的視口正在渲染），**不會**累積。
- 尺寸：`QViewport::Width()/Height()`（`QViewport.h`，裝置像素，`QtUtil::PixelScale`）；`resizeEvent` 呼叫 `ResizePipelineAndContext`（`QViewport.cpp:1208-1225`）並**對整個引擎發 `ESYSTEM_EVENT_RESIZE`**（`:1220`）——Character Tool 同樣如此，已知行為；我們的插件不處理此事件，無影響。HiDPI：回呼內用 `rc.viewport->Width()/Height()` 當像素尺寸，不要用 `width()`。
- 節流：`Update()` 每個 idle 都會渲染（受 `sys_MaxFPS` 影響整體 idle 頻率，`GameEngine.cpp:1490-1510`）。影片 30/60 fps 夠；暫停且無互動時可略過 `Update()`（但 resize、拖時間軸要重畫：以 `m_dirty` 旗標＋播放中才每 idle 更新）。
- 遊戲模式：不送 idle，視窗不更新（可接受；顯示「遊戲模式中，預覽暫停」）。XTFrame 為了遊戲模式另做了 `IGameFrameworkListener::OnPostUpdate` 轉 Qt 佇列（`X:.../EditorXTUIPlugin.h:11-26`，CE-72b），我們不需要。

### 4.4 在 `SignalRender` 回呼內畫影片貼圖

回呼在 `QViewport::Render` 的 3D 階段（`EF_StartEf` 與 `EF_EndEf3D` 之間，`QViewport.cpp:938-948`）被呼叫；**2D 圖片（`PushImage`）只是記錄，真正繪製在 `aux->Submit()` 內 `CAuxGeomCB::Draw2dImages`**（`V:Code/CryEngine/RenderDll/Common/RenderAuxGeom.cpp:2209-2280`，`QViewport.cpp:970`），也就是 3D 場景之後、直接畫到後緩衝（與文字同階段），所以不經過 HDR／色調映射，**顏色是原樣輸出、座標是像素（Mode2D，`IRenderAuxGeom.h:530`）**。`BlendSpacePreview::OnRender` 在同一回呼內畫 `IRenderAuxText::Draw2dLabel`（`BlendSpacePreview.cpp:80-107`）證明這個階段安全。

```cpp
void CCevViewerPanel::OnRender(const SRenderContext& rc)
{
	IRenderAuxGeom* aux = rc.pAuxGeom;                      // 這個視口專屬的 aux（QViewport.cpp:999,1015）
	if (!aux) return;
	const float W = float(rc.viewport->Width());
	const float H = float(rc.viewport->Height());

	// --- 計算影片矩形（fit / 1:1 / 縮放 / 平移）---
	CEVPlayer::IVideoPlayer* p = m_pPlayer;                 // 可能為 nullptr
	const int texId = (p && p->IsLoaded()) ? p->GetTextureId() : 0;   // **每幀重取**：Load/換變體會重建貼圖（尺寸變）
	// ... dst = { x, y, w, h } 像素

	// --- 1. 棋盤格（alpha 底）---
	DrawChecker(aux, dst);                                   // §4.5
	// --- 2. 影片（premultiplied，兩次繪製）---
	if (texId > 0) DrawPremultiplied(aux, texId, dst);       // §4.6
}
```

`SRender2DImageDescription`（公開結構，`IRenderAuxGeom.h:~945-965`）欄位：`x,y,z,w,h,uv[2],angle,stereoDepth,color(ColorB),textureId,targetId,displayContextKey,renderFlags`。**直接填它並呼叫 `aux->PushImage(img)`**，而不是用 `IRenderAuxImage::Draw2dImage`（`IRenderAuxGeom.h:988-1030`）：後者走 `IRenderAuxGeom::GetAux()`（全域當前 aux）且不能設 `renderFlags`。

貼圖方向：runtime 插件的貼圖是上到下的 CPU 上傳，`FullscreenOutput.cpp:81` 的做法是 `Draw2dImage(..., s0=0, t0=1, s1=1, t1=0, ...)`（aux 四邊形垂直翻轉，`RenderAuxGeom.cpp:2257-2263` 註解「uv.y 被 1-y」）。即 `img.uv[0]=(0,1)`、`img.uv[1]=(1,0)`。

貼圖取得：`IVideoPlayer::GetTextureId()`／`GetTexture()`（`Interface/CEVPlayer/IVideoPlayer.h`）。貼圖是 `UploadToVideoMemory(nullptr, w, h, eTF_R8G8B8A8, …, FILTER_BILINEAR, …, FT_NOMIPS)` 建立並由 `UpdateTextureInVideoMemory` 每幀更新（`Module/Render/RenderBridge.cpp:121,150`）。插件在 `UpdateBeforeRender`（`GameEngine.cpp:1561`，在 `eNotify_OnIdleUpdate` 之前）上傳，所以同一個 idle 內我們畫的就是最新幀。播放器必須在主執行緒操作（API 註解）；我們的 idle 在主執行緒。

### 4.5 棋盤格（alpha 底）

aux 沒有「棋盤」圖元。兩個做法：

- **A（建議）**：自己建一張 256×256 的 `$cev_editor_checker` 貼圖（內容就是棋盤），用與插件相同的 API：`gEnv->pRenderer->UploadToVideoMemory(pixels, 256, 256, eTF_R8G8B8A8, eTF_R8G8B8A8, 0, false, FILTER_POINT, 0, "$cev_editor_checker", FT_NOMIPS)`（`RenderBridge.cpp:121` 簽名，這次給資料指標）。繪製時以 256×256 像素區塊鋪滿影片矩形（1080p 約 30 個 `PushImage`），邊緣區塊用 uv 截斷；**不依賴貼圖 wrap 模式**（aux sampler 的 wrap/clamp 未驗證）。顏色用 sRGB 值無轉換（2D 階段原樣輸出）。
- B：用 `textureId = -1`（白色填色，`FullscreenOutput.cpp:24` 的 `FillRect` 做法）+ `img.color` 逐格畫方塊，格子數可能上萬；只當備案。
- 建立貼圖要在 context 之外的主執行緒（插件自己也是在 tick 內建）；`UploadToVideoMemory` 在 `BeginFrame` 之外或之內皆為資料請求，但**不要在 `OnRender` 內第一次建立**，在面板建構時建立並在析構釋放（`ReleaseTexture` 做法見 `RenderBridge.cpp:154`）。

### 4.6 premultiplied 與 aux 混合（**有解，不需引擎 patch**）

事實：
- 我們的貼圖是 premultiplied RGBA（`IVideoPlayer.h`「Pixel format … IsPremultiplied() is always true」）。
- aux 圖片的混合由 `SAuxGeomRenderFlags::GetAlphaBlendMode()` 決定，只有三種：`e_AlphaNone`、`e_AlphaAdditive`（`GS_BLSRC_ONE | GS_BLDST_ONE`）、`e_AlphaBlended`（`GS_BLSRC_SRCALPHA | GS_BLDST_ONEMINUSSRCALPHA`）（`IRenderAuxGeom.h:540-544`；`XRenderD3D9/D3DRenderAuxGeom.cpp:569-573`）。**沒有 premultiplied（ONE, ONE_MINUS_SRC_ALPHA）**。
- 像素著色器是 `return col.rgba * tex.rgba;`（`V:Engine/Shaders/HWScripts/CryFX/AuxGeom.cfx:110-114`）：純調製，不再乘 alpha。
- 但每張圖有自己的 `renderFlags`（`SRender2DImageDescription::renderFlags`，預設 `e_Def2DImageRenderflags = e_Mode2D | e_DepthTestOff | e_AlphaBlended | e_CullModeNone …`，`IRenderAuxGeom.h:615,964`；`Draw2dImages` 逐圖切換，`RenderAuxGeom.cpp:2246-2252`）。

因此用 **兩次繪製** 得到精確的 premultiplied over（`out = src.rgb + dst * (1 - src.a)`）：

1. **第一遍（變暗背景）**：`color = (0,0,0,1)`，`e_AlphaBlended`。像素輸出 `rgb = 0*tex.rgb = 0`、`a = tex.a`；混合 `out = 0*a + dst*(1-a) = dst*(1-a)`。
2. **第二遍（加上預乘顏色）**：`color = (1,1,1,1)`，`e_AlphaAdditive`。輸出 `rgb = tex.rgb`（已預乘）；混合 `out = tex.rgb*1 + dst*1`。
3. 合計 `dst*(1-a) + tex.rgb` = premultiplied over。

```cpp
static void DrawPremultiplied(IRenderAuxGeom* aux, int texId, const Rect& r)
{
	SRender2DImageDescription img;
	img.x = r.x; img.y = r.y; img.w = r.w; img.h = r.h; img.z = 1.0f;
	img.textureId = texId;
	img.uv[0] = Vec2(0.f, 1.f);  img.uv[1] = Vec2(1.f, 0.f);          // 垂直翻轉
	// pass 1: dst *= (1 - a)
	img.color = ColorB(0, 0, 0, 255);
	img.renderFlags = e_Mode2D | e_DepthTestOff | e_AlphaBlended  | e_CullModeNone | e_FillModeSolid;   // 欄位組合照 e_Def2DImageRenderflags，IRenderAuxGeom.h:615
	aux->PushImage(img);
	// pass 2: dst += premultiplied rgb
	img.color = ColorB(255, 255, 255, 255);
	img.renderFlags = e_Mode2D | e_DepthTestOff | e_AlphaAdditive | e_CullModeNone | e_FillModeSolid;
	aux->PushImage(img);
}
```
- `Draw2dImages` 依 push 順序執行，所以先棋盤、再兩遍影片。`e_Def2DImageRenderflags` 的完整組合請以 `IRenderAuxGeom.h:615-617` 為準複製（上面省略號部分，不要自己漏掉 `e_DepthWriteOff` 之類的位元）。
- 不透明影片（`HasAlpha()==false`）：單遍 `e_AlphaNone` 最便宜。
- 純「straight alpha 近似」（單遍 `e_AlphaBlended`）會把半透明邊緣變暗（`rgb*a*a`），Stage 1 預覽不要用；若要顯示「影片原樣被遊戲全螢幕疊在場景上」的外觀（`FullscreenOutput` 走預設 flags，其實有同樣的偏暗問題）可作對照。
- 未驗證點（需實機）：加法遍的 `GS_BLDST_ONE` 與 swap chain 後緩衝格式（若為 sRGB 格式，加法在線性空間，兩遍仍一致）；`e_AlphaNone` 圖片的 z 排序。上述兩遍法只依賴 `AuxGeom.cfx` 的 `col*tex`，不依賴 HDR。

### 4.7 生命週期／拆除的坑（XTFrame 付出 GPU fault 的教訓）

來源：`X:docs/05-stage2-design.md:521-535`（§5.5.3「GPU fault 根因」）、`X:docs/tasks/CE-65.md`、`X:.../EditorPreview.cpp:296-330,480-560,640-720`。

1. **三次 NVIDIA event 153 / 崩潰的根因排名第一**：窗格 widget 解構時它的原生 HWND 先死，`DeleteContext` 晚一個主影格；render thread 仍可能 Present 到已死視窗；HWND 值被回收使同一視窗短暫掛兩條 swap chain。XTFrame 的修正（CE-65）：swap chain 掛在**插件自己建立的 `WS_CHILD` 子視窗**（Qt 不擁有它），detach 時先 `SetParent` 到隱藏 holder、延後 `DestroySurface → DeleteContext → DeleteGraphicsPipeline → DestroyWindow`。固定拆除順序：surface → context → pipeline。
2. **Qt 在 dock 重新 parent 時會重建原生視窗**（XTFrame CE-65 實測：`beginFrame` 要檢查 `IsWindow(hSurface)`，否則 `BeginFrame(K)` 打在死視窗上）。`QViewport` 的 context 綁在 `QWidget::winId()`（`QViewport.cpp:303-306`）而且**只在建構與析構建／刪**（沒有處理 `WinIdChange`）。→ 我們的視口面板是 docking 容器的一員，使用者拖曳／浮動時有同樣風險。緩解：
   - (a) 視口面板註冊為 `isUnique=true` 並盡量避免浮動（`QToolWindowManager` 是否可禁止浮動未驗證）；
   - (b) 在 `QViewport` 子類別攔 `QEvent::WinIdChange`／`ParentChange`：標記 `m_needRecreate`，下一個 idle 在**不在任何 Begin/End 內**時 `delete` 舊 `QViewport`（其析構的 `DeleteContext` 會 flush render thread，`DestroyRenderContext` `QViewport.cpp:334-344`）並建新的；
   - (c) **若 M4 驗證出現 event 153 或黑畫面，改走 XTFrame 的備案**：自有 `WS_CHILD` 子視窗 + 裸 `CreateSwapChainBackedContext`/`CreateGraphicsPipeline`（`X:.../EditorPreview.cpp` 範本，約 250 行，去掉 XUI2D 部分；建／刪排到 `IPlugin::DeferAfterRenderSubmit` 類似時機——CEVPlayer 有自己的 `UpdateAfterRenderSubmit` 機會，或用 `GetIEditor()->PostOnMainThread` 於 idle 之外）。此備案會失去 `QViewport` 的 `SignalRender` 便利，但繪製程式（§4.4-4.6）完全相同，只換 `BeginFrame/EndFrame` 包裝。
3. **`CreateSwapChainBackedContext`／`DeleteContext` 只能在其他 display context 的 BeginFrame/EndFrame 之外**（`X:.../EditorPreview.cpp:256-257`）。`eNotify_OnIdleUpdate` 內，`CViewManager` 會在這個通知裡讓主視口各自 Begin/End（`X:docs/08-xtframe-integration-log.md:736`），所以：不要在 `OnEditorNotifyEvent` 中同步 new／delete `QViewport`；用 `QTimer::singleShot(0)`／`PostOnMainThread` 延到 Qt 事件迴圈。
4. **render lock 拿不到時**（`CScopedRenderLock`，別的視口正在渲染）：XTFrame 回報「該幀不 present」且不視為失敗（`EditorPreview.cpp:673-684`）；`QViewport::RenderInternal` 同樣直接略過（`QViewport.cpp:990`）。我們也只要略過，下個 idle 再來。
5. **多執行緒 renderer 的既有競態**：`XUI2D::EndFrame` 讀 `m_activeContextKey`（`X:docs/05:457`）是 XUI2D patch 範圍；我們沒有 XUI2D，不受影響。
6. 視窗被隱藏／最小化：`isVisible()` 為 false 時不 `Update()`；`QViewport` 自己也跳過（`:427`）。
7. 銷毀順序（我們的視窗關閉時）：先 `DestroyPlayer`／解除 `IVideoPlayer` 引用 → 釋放棋盤貼圖 → `delete` 視口（`DeleteContext` flush render thread）。播放器貼圖在插件釋放播放器時由插件管（API 註解：aux 繪過的貼圖由 renderer 持有到不再引用，名稱重用）。**不要在 `ESYSTEM_EVENT_FULL_SHUTDOWN` 之後使用 `IVideoPlayer*`**（API 註解）：視窗的 `OnCloseAsset`／析構要在編輯器退出前執行（`CAssetEditor` 的 `AboutToQuit` 路徑保證關閉；仍用 `QPointer`/null 檢查）。
8. 與 `QPreviewWidget` 的比較（備案 2）：`QPreviewWidget` 自己 `WA_PaintOnScreen|WA_NativeWindow`、第一次繪製才建 context、`BeginFrame/EndFrame` 直接呼叫（`QPreviewWidget.cpp:268-290,800-930`），其 `DrawBackground()`（`:1376-1392`）用 `IRenderAuxImage::Draw2dImage` 畫背景貼圖——**原始碼自己註解「透明度與背景貼圖不如 MFC 版好用」**（`QPreviewWidget.h` 的 `//TODO: transparency and background texture don't work as they used to`），所以不當首選。

### 4.8 播放控制與 IVideoPlayer 的缺口

`IVideoPlayer` 沒有「逐幀步進」「路線覆寫」「取得幀率」：
- **步進**：暫停後 `Seek(t ± 1/fps)`；fps 取自 Core `WebmDemuxer::Video().frameRate`（`Core/include/CEVCore/WebmDemuxer.h`，編輯器自己也要開檔取資訊，用 `CryPakByteSource` 或 `FileByteSource` 開一次即可）。`Seek` 在暫停時會解碼到目標幀並顯示（`PlayerCore::ApplySeek`，`Core/src/PlayerCore.cpp:410-425`）。
- **聲音路線切換（SDL vs CryAudio 觸發器）**：路線由描述檔 `route=`／語言的 `trigger=` 決定（`Module/Player/VideoPlayerImpl.h:117-123`），API 無覆寫。預覽時：編輯器寫一個**暫存描述檔**（`Assets/.cevpreview/<name>.cevvideo`，`route` 與 `video=`/`file=` 用 CryPak 相對 Assets 的完整路徑，`ResolveSiblingPath` 接受「以給定路徑相對遊戲根存在」，`VideoDescriptor.h` 註解）載入它。或下一版介面追加 `IVideoPlayer::SetRouteOverride`（interface v5，appended virtual）。M4 先用暫存描述檔。
- 靜音／全域：`SetVolume(0)`、cvar `cev_audio_enable`（`Module/Console.cpp:132`）。

---

## 5. 背景工作（進度／取消／日誌）

可用的引擎設施：

| 設施 | 位置 | 評價 |
|---|---|---|
| `ThreadingUtils::Async`／`AsyncQueue`／`PostOnMainThread`／`AsyncFinalize`／`AsyncNotify` | `EC:ThreadingUtils.h:96-190`（header-only；**它 `#include "StdAfx.h"`，所以插件要有 `StdAfx.h` 在 include 路徑**） | `AsyncQueue` 是**全編輯器單一佇列**（IO 執行緒，`eTaskThreadMask_IO`），音訊轉檔會擋縮圖產生、其他匯入；不用於長工作。`Async`（`eTaskThreadMask_Any`）可用但沒有取消、沒有進度 |
| `IBackgroundTaskManager`／`IBackgroundTask`（`Work()` 可回 `Resume` 分段、`Cancel`） | `EC:IBackgroundTaskManager.h` | 有取消，但共用引擎的背景執行緒集；可選 |
| `CProgressNotification` | `EC:Notifications/NotificationCenter.h:28-45`（`SetProgress(0..1)`、`SetMessage`；`AssetGenerator.cpp:21-48`、`AssetDropHandler.cpp:99-130` 用法） | **沒有取消**；物件有 `QObject` 親和性，要在主執行緒建立（匯入器內用 `PostOnMainThread` 建，見 `AssetDropHandler.cpp:107-111`）。適合「顯示在通知中心」 |
| `INotificationCenter::ShowInfo/Warning/Critical`（`GetIEditor()->GetNotificationCenter()`） | 同檔 `:52-62` | 結果通知 |
| `CryWarning(VALIDATOR_MODULE_EDITOR, …)` | — | 會被通知中心自動收進歷史（同檔註解） |

建議自建一個小型 job 系統（M4 只有 3-4 種 job，不必泛化）：

```cpp
struct CJob : QObject {                       // 活在主執行緒
	std::atomic<bool> cancel{false};
	std::atomic<float> progress{0};
	QString title;
	// 日誌：工作執行緒只呼叫 Log()；Log() 以 QMetaObject::invokeMethod(this, ..., Qt::QueuedConnection) 回主執行緒，追加到 QPlainTextEdit 與 CryLog
	void Log(const QString& line);
signals: void Finished(bool ok); void LogLine(QString);
};
// 執行：std::thread 或 QThread（獨立於引擎佇列），內部迴圈每 N 個 packet 檢查 cancel，更新 progress
// UI：右欄「工作」清單：QProgressBar + Cancel 按鈕 + 日誌視窗；另建一個 CProgressNotification 在主執行緒同步顯示摘要（可選）
```

- 取消：Core 的 `WebmDemuxer::Open` 已接受 `std::atomic<bool>* cancel`（`WebmDemuxer.h`）；自寫迴圈（remux、解碼、WAV、Ogg 編碼）每個 packet 檢查。
- 主執行緒回呼：`QMetaObject::invokeMethod(pQObject, lambda, Qt::QueuedConnection)`（Qt 5.10+ 支援 functor）或 `GetIEditor()->PostOnMainThread(std::function<void()>)`（`IEditor.h:335`）。注意 job 完成回呼時視窗可能已關：持 `QPointer`。
- 不要在工作執行緒呼叫：`CAssetManager::MergeAssets`、`CEditableAsset::WriteToFile`（寫 `.cryasset` 本身是檔案操作，但之後觸發資產管理器更新；保險起見主執行緒做）、任何 Qt widget、`gEnv->pRenderer`。純 STL＋Core＋檔案 I/O 可在工作執行緒。檔案路徑：用 `PathUtil::GetGameProjectAssetsPath()`（`PathUtils.h`）組絕對路徑，工作執行緒用 `std::ofstream`/`fopen` 直接寫，不經 CryPak。
- 寫完檔案後引擎的檔案監看器（資產、ACE `FileMonitorSystem`）會自動反應（§6）。

---

## 6. ACE（Audio Controls Editor）觸發器自動建立（SDL_mixer）

### 6.1 沒有可呼叫的 API → 直接寫 XML

- ACE 是獨立插件 DLL（`EditorAudioControlsEditor`），其類別（`CAudioControlsEditorPlugin`、`CAssetsManager`、`g_assetsManager`、`g_pIImpl`…）**都沒有 export 巨集**（`AudioControlsEditorPlugin.h:12-60`、`AssetsManager.h`）；`ACE_API` 只標在 `IImpl`（`common/IImpl.h:10-15`，給中介軟體實作插件用）。另一個插件無法連結。
- 因此作法：**寫 `Assets/audio/sdlmixer/ace/<library>.xml`** 並**通知更新**。ACE 自己存檔就是寫這個資料夾：`CAssetsManager::UpdateConfigFolderPath`（`AssetsManager.cpp:295-305`）→ `CRY_AUDIO_DATA_ROOT "/" + implFolderName + "/" + g_szConfigFolderName("ace") + "/"`（`CryCommon/CryAudio/IAudioInterfacesCommonData.h:97`），實作資料夾名 `"sdlmixer"`（`EditorSDLMixer/Impl.cpp:1018-1023`，`GlobalData.h:14`）；寫檔位置 `PathUtil::GetGameFolder() + "/" + libraryPath + ".xml"`（`FileWriter.cpp:584`）。`CRY_AUDIO_DATA_ROOT` = `"audio"`。

### 6.2 檔案格式（ACE 與執行期共用同一份 XML）

`FileWriter.cpp:422-600` 寫出的 `<AudioSystemData name=… version=… triggers="N" …>`，`<Triggers>` 下每個 `<Trigger name=…>` 含 SDL_mixer 的 `<Event>` 子節點（`EditorSDLMixer/Impl.cpp:572-670` `CreateXMLNodeFromConnection`：`name`（檔名）、`path`（`assets/` 內子路徑，空則省略）、`type="start|stop|pause|resume"`、start 時 `volume`、`loop_count`（0 = 無限）、可選 `panning_enabled`、`attenuation_*`、`fade_in_time`、`fade_out_time`、`localized="true"`），`<ImplData events=… …/>`（`SetDataNode`）。`<EditorData>` 節點（資料夾／描述編輯器資料）**可省略**（`FileLoader.cpp:482-484` 有才讀）。專案已有手寫的最小範本 `P:tools/test_audio/cev_test.xml`（3 個 trigger：start/pause/resume，已在冒煙測試用過）：

```xml
<?xml version="1.0" encoding="utf-8"?>
<AudioSystemData name="cev_<assetname>" version="2" triggers="3" parameters="0" switches="0" states="0" environments="0" preloads="0" settings="0" files="0">
	<Triggers>
		<Trigger name="Play_<assetname>_<lang>">
			<Event name="<file>.wav" path="cev/<assetname>/" type="start" volume="0" loop_count="1"/>
		</Trigger>
		<Trigger name="Pause_…"><Event name="<file>.wav" path="cev/<assetname>/" type="pause"/></Trigger>
		<Trigger name="Resume_…"><Event name="<file>.wav" path="cev/<assetname>/" type="resume"/></Trigger>
	</Triggers>
	<ImplData events="3" parameters="0" parametersadvanced="0" switchstates="0" files="0"/>
</AudioSystemData>
```
（`path` 的格式需對照 ACE 載入：`Utils::GetId(EItemType::Event, name, path, isLocalized)` 用 name+path 組 id，`Impl.cpp:442`；`path` 相對 `audio/sdlmixer/assets/`，結尾斜線與否以 `cev_test.xml` 的 `path=""` 與 ACE 實際輸出對照確認，**實作前請先用 ACE 手動建一個含子資料夾的 trigger 存檔，複製其 `path` 寫法**。）

- **一個資產一個 library 檔**（`<name>.xml`，library 名＝檔名；`name` 屬性＝library 名）：避免多資產共用一檔時的讀改寫競態，也讓刪除資產只刪自己的檔。每個 library 的 `triggers="N"`、`events="N"` 計數屬性要與內容一致（`FileWriter` 依 `libScope.numTriggers` 寫；執行期預先配置池用，`CryAudioSystem` 讀）。觸發器名稱全域唯一：用 `Play_<資產名>` 之類加前綴，匯入時檢查既有 ACE 檔案避免重複。
- 語言：每種語言一個 event 檔＋trigger；描述檔 `<Language trigger="…">`（`VideoDescriptor.h` 註解）。SDL_mixer 本地化目錄（`localized="true"` 對應 `assets/<language>/`，`research/01` §7）若要用 `g_languageAudio` 切換則檔案放語言資料夾；M4 先用每語言獨立 trigger（描述檔已支援 per-language trigger）。

### 6.3 通知更新

- **執行期（Sandbox 內的 CryAudioSystem）**：`ESYSTEM_EVENT_AUDIO_REFRESH`（`CryCommon/CrySystem/ISystem.h:474`）；ACE 自己存檔後就是這樣通知（`EditorAudioControlsEditor/MainWindow.cpp:495`、`EditorQt/QT/PythonUIActionBinds.cpp:160`）：
  ```cpp
  gEnv->pSystem->GetISystemEventDispatcher()->OnSystemEvent(ESYSTEM_EVENT_AUDIO_REFRESH, 0, 0);
  ```
  **注意**：`CSystem::OnSystemEvent` 內處理 `AUDIO_REFRESH` 與 `AUDIO_RELOAD_CONTROLS_DATA` 被 `#if defined(CRY_AUDIO_USE_DEBUG_CODE)` 包住（`CryAudioSystem/System.cpp:1074-1087`；同樣 `s_Refresh` 命令只在該巨集下註冊，`CVars.cpp:501-503`）。Profile／Debug 建置有此巨集；**Release 建置無，需重啟編輯器**。M4 以 Profile 為準。
- **ACE 視窗**：`CFileMonitorSystem` 對 ACE 資料夾的 `xml` 做 `RegisterListener`（`FileMonitorSystem.cpp:20-23`），檔案一變就在 ACE 視窗開著時重載；ACE 沒開時下次開啟自然讀到。若 ACE 視窗內有**未儲存的修改**，外部改檔會讓使用者看到衝突／重載提示（檔案監看行為由 `FileMonitorBase` 處理）——UI 上提醒「請先儲存 ACE」。
- **SDL_mixer 音檔**：`LoadMetadata` 以 `ICryPak::FindFirst` 遞迴掃 `audio/sdlmixer/assets/`，副檔名白名單含 `wav`、`ogg`、`opus`（`research/01` §7，`Impl.cpp:80-125`、`GlobalData.h`）。檔案寫入後需要上面的 refresh 才進入。

### 6.4 程式化 API 的替代（若之後要）

- 在 `EditorAudioControlsEditor` 追加匯出一個窄介面（如 `ACE::RegisterGeneratedTriggers(...)`）需改引擎（patch），且還要處理 ACE 內部資料模型與其存檔的一致性，風險遠大於直接寫檔。M4 不採用。

---

## 7. 不用 FFmpeg 的 WebM 重封裝、WAV、Ogg

### 7.1 mkvmuxer 能做什麼（已確認）

`P:Code/SDKs/CEVPlayer/libwebm/include/mkvmuxer/mkvmuxer.h`；`webm.lib` 內含 mkvmuxer 實作（符號 grep 確認）。要點：
- `mkvmuxer::MkvWriter w; w.Open(path)`（`mkvwriter.h:20-36`，`FILE*`，可 seek）→ `mkvmuxer::Segment seg; seg.Init(&w); seg.set_mode(mkvmuxer::Segment::kFile);`（`:1501,1511`）。
- 音軌：`uint64_t n = seg.AddAudioTrack(sampleRate, channels, 0);`（`:1529`；註解寫「Vorbis audio track」，只是預設 codec id）→ `auto* t = static_cast<mkvmuxer::AudioTrack*>(seg.GetTrackByNumber(n));` → `t->set_codec_id(mkvmuxer::Tracks::kOpusCodecId)` 或 `kVorbisCodecId`（`:875-876`）、`t->SetCodecPrivate(bytes, len)`（`:679`）、`t->set_codec_delay(ns)`、`t->set_seek_pre_roll(ns)`（`:698-703`；Opus 來源）、`t->set_bit_depth(...)`、`t->set_sample_rate(double)`、`t->set_channels(...)`（`:852-856`）、`t->set_language("eng")`（`:~690`）、`t->set_default_duration(ns)`（可選）。
- 逐 packet：`seg.AddFrame(data, len, track, timestamp_ns, true)`（`:1555`，音訊 `is_key=true`），**含 DiscardPadding**用 `seg.AddFrameWithDiscardPadding(data, len, discardPaddingNs, track, timestamp_ns, true)`（`:1605`；`discard_padding` 單位 ns，與 Core 的 `PacketRef::discardPaddingNs` 相同定義，`WebmDemuxer.h`）。時間戳單位是**奈秒**（Segment 層 API），Segment 預設 timecode scale 1 ms → 區塊時間會被量化到 1 ms；Opus 20 ms 封包、Vorbis 的封包 pts 在來源也已是 ms 量化，沒有損失。若來源 timecode scale 不是 1 ms（罕見），`seg.GetSegmentInfo()->set_timecode_scale(src)`（`:1467`）；Core 目前不暴露來源的 scale，M4 假設 1 ms，並在匯入記錄一則警告若偵測到 pts 非 ms 倍數。
- `seg.Finalize()`（`:1648`）寫 Cues／Duration（`kFile` 模式需可 seek 的 writer；`seg.set_estimate_file_duration(true)` 可選）。
- 資料來源：**Core 已能把所有音訊 packet 索引並讀出**：`WebmDemuxer::Open(source, out, detail, cancel)`、`Audio()`（`codec/sampleRate/channels/codecPrivate/codecDelayNs/seekPreRollNs`）、`AudioPacket(i)`（`ptsNs/size/discardPaddingNs`）、`ReadAudioPacket(i, dst, cap)`（`WebmDemuxer.h`）。remux = 開來源 → 建 AudioTrack（照抄 `AudioStreamInfo`）→ 迴圈讀 packet 寫入。**不需要 mkvparser 以外的任何東西、不重新編碼**。

### 7.2 時間零點對齊（sample 0 = 影片 0 s）

執行期對時間軸的定義（`Core/src/PlayerCore.cpp:386-398` `ApplyAudioSeek` 與 `AudioDecoder.h` 檔頭）：
- 容器時間軸為絕對時間；影片以 `PopForTime(max(t, firstVideoPts))`（`PlayerCore.cpp:853`）顯示。
- 音訊「一個 packet 第一個被解碼取樣的實際時間 = block 時間 − CodecDelay」；對 packet 0，Opus pre-skip（＝CodecDelay）被丟掉，所以**第一個被保留的取樣在 block 時間（packet 0 的 pts）**；Vorbis `CodecDelay = 0`，第一個輸出取樣在 pts。
- 結論：**音訊時間零點由 packet 0 的 `pts` 決定**，而不是檔案開頭。

對 **WebM 輸出（執行期外部語言檔、聲音只有 WebM）**：保持每個 packet 的原 `ptsNs`、`CodecDelay`、`SeekPreRoll`、`DiscardPadding`、CodecPrivate **原封不動**，則外部音檔與主影片共用同一個容器時間軸（兩者來自同一來源檔），執行期（主影片 `PlayerCore` 時鐘＝外部音訊 `AudioClock`，M2 設計）的處理方式與內嵌音軌完全一致。**因此 WebM 路徑不需要任何時間編輯**。M4 唯一建議的調整：
- 若來源音軌第一個 packet 的 pts 為負值或遠離 0（不常見），可整體平移使「第一個被保留取樣」落在 ≥ 0：對所有 packet 減去同一偏移（量化為 ms），並在匯入設定的 `Details` 記錄偏移。mkvmuxer 的 `AddFrame` 要求時間戳非遞減（`mkvmuxer` 內部 `Cluster` 以 packet 順序寫）；偏移後仍遞增。
- 若要「修剪頭尾」（使用者指定起迄）：**只能在 packet 邊界**（Opus 20 ms、Vorbis 約 1024–2048 取樣的塊）；起點精確修剪的做法：丟掉起點之前的 packet 但保留足夠 pre-roll（Opus：`SeekPreRoll` 80 ms；Vorbis：多保留 1 個 packet）並把第一個保留 packet 的 `CodecDelay` 調成「pre-skip + 落在該 packet 內的待丟取樣」（Opus 的 pre-skip 必須同時寫入 OpusHead CodecPrivate 的 `pre_skip` 欄位與 `CodecDelay` 元素，兩者要一致，Core 兩者都讀：`AudioDecoder.cpp:71-79`）；終點精確修剪：最後一個 packet 的 `DiscardPadding`（ns，Core 轉成 `trimEndFrames`）。**Vorbis 沒有 pre-skip 概念**（Matroska 只靠 `CodecDelay=0` 與 packet 時間戳），起點只能整 packet 對齊；亞 packet 精度要靠 WAV/重編碼路徑。**這是 WebM 無重編碼修剪的上限：Opus 可達取樣精度（pre-skip＋DiscardPadding），Vorbis 只到 packet（~21–43 ms）**。M4 建議：WebM 路徑不提供修剪，只保留原 pts；修剪（若要）只用於 WAV/OGG 輸出（解碼後取樣級修剪，免費）。

對 **WAV／OGG 輸出（取樣級對齊，M4 必做）**：用 Core 的 `AudioDecoder`（`Create(stream)`、`Reset(true)`、逐 packet `Decode(data,size,trimEndFrames,&pcm,&frames)`，輸出 float 交錯，Opus 固定 48 kHz，Vorbis 為容器取樣率）：
1. `startFrames = llround((pts0 − codecDelay) * rate) + (Opus ? preSkipFrames : 0)`（同 `ApplyAudioSeek` s==0，等於 `llround(pts0*rate)` 對 Opus）。這是「第一個被保留取樣」在容器時間軸上的取樣索引。
2. 輸出緩衝 sample 0 對應影片 0 s：`startFrames > 0` → 前置 `startFrames` 個靜音取樣；`startFrames < 0`（pts0 為負）→ 丟掉前 `−startFrames` 個取樣。Vorbis 的前置 priming（第一個 packet 輸出 0 取樣）由 `Decode` 內處理，`*frames` 可為 0。
3. 每個 packet 的 `discardPaddingNs` → `trimEndFrames = round(ns*1e-9*rate)`（Core 的 PlayerCore 已這樣用）。
4. 聲道／取樣率：WAV 目標 48 kHz 16-bit：Opus 已 48 kHz；Vorbis 44.1 kHz 等用 Core 的 `Resampler`（`Core/include/CEVCore/Resampler.h`，串流式、含 rate=1）轉成 48 kHz；聲道保留（SDL_mixer 支援 1/2 聲道；> 2 聲道用 Core 降混到立體聲或報錯）。float → int16：`clamp(x*32767)` + TPDF dither 可選（預設 round，不 dither，避免靜音段雜訊）。
5. **取樣精度極限**：`llround` 到整數取樣，誤差 ≤ 0.5 取樣；Vorbis 的 priming 已由 decoder 吸收；Opus 的 pre-skip 在 OpusHead 與 CodecDelay 不一致時 Core 以 OpusHead 為準（`AudioDecoder.cpp:77-79`）。

### 7.3 WAV 寫入

不需要函式庫。44 字節 RIFF 標頭 + `data`：`fmt `（PCM=1、聲道、48000、byteRate、blockAlign、16 bits）、`data` 大小；先寫佔位標頭，寫完 PCM 後回頭填大小（`FILE*` + `fseek`）。大於 4 GiB 不可能（影片音訊）。可串流寫（每個 packet 解碼完就寫），低記憶體；進度＝packet 索引／總數；取消＝刪暫存檔。寫到 `*.tmp` 再 `rename`，避免半成品被引擎檔案監看當成 Sound 資產（§2.5）。

### 7.4 Ogg Vorbis（SDL_mixer 用）

- **Vorbis 來源，無重編碼封裝成 `.ogg`：可行**，以 libogg + libvorbis 的「packet API」：
  1. 解 CodecPrivate 的 Xiph lacing（`count−1 = 2`，接著兩個 packet 長度，再三個 header packet 串接；Core 的 `AudioDecoder.cpp` 已有這段解析，`Core/src/AudioDecoder.cpp`「ogg_packet built from the CodecPrivate Xiph lacing」）。
  2. `vorbis_info_init`/`vorbis_comment_init` 後對三個 header 呼叫 `vorbis_synthesis_headerin`（`vorbis/codec.h:203`），取得 `vorbis_info`。
  3. `ogg_stream_init(&os, serial)`；`ogg_stream_packetin(identification, b_o_s=1)`，`ogg_stream_flush` 強制 ID header 單獨成頁；comment+setup 兩個 header 送入後 `ogg_stream_flush`（規格要求）。
  4. 每個音訊 packet：`op.granulepos` = 累計輸出取樣數：以 `vorbis_packet_blocksize(vi, &op)`（`codec.h:214`，已確認 `vorbis.lib` 含此符號）取得塊大小，`granule += prevBlock/4 + curBlock/4`（第一個 packet 不累加；標準公式）。`ogg_stream_packetin` 後 `while(ogg_stream_pageout(&os,&og)>0)` 寫頁；最後一個 packet `e_o_s=1`，**最後一頁的 granulepos 設為「總取樣數 − 尾端修剪」即可實現精確尾端修剪**（Ogg Vorbis 規定最後頁 granulepos 小於實際可解碼取樣數時，解碼器丟掉多餘取樣）。
  5. 限制：Ogg Vorbis **沒有 pre-skip**，所以亞頁精度的起點偏移（正偏移＝前置靜音）無法無重編碼完成。pts0 恰為 0（或可忽略的 ≤ 1 取樣）的來源可無損 remux；否則走下一條。
- **Opus 來源（或需要對齊／重取樣／降混的 Vorbis）：解碼 → 取樣級對齊（§7.2）→ `vorbisenc` 編碼**：
  ```cpp
  vorbis_info vi; vorbis_info_init(&vi);
  vorbis_encode_init_vbr(&vi, channels, 48000, /*quality=*/0.4f);   // vorbis/vorbisenc.h:157；SDK 有 vorbisenc.lib
  vorbis_comment vc; vorbis_comment_init(&vc);
  vorbis_dsp_state vd; vorbis_analysis_init(&vd, &vi); vorbis_block vb; vorbis_block_init(&vd, &vb);
  ogg_stream_init(&os, serial);
  vorbis_analysis_headerout(&vd, &vc, &h1, &h2, &h3);                // codec.h:187；三個 header 入流並 flush
  // 以 float** 填 vorbis_analysis_buffer(&vd, n)，vorbis_analysis_wrote(&vd, n)；
  // 迴圈 vorbis_analysis_blockout / vorbis_analysis(&vb,nullptr) / vorbis_bitrate_addblock / vorbis_bitrate_flushpacket → ogg_stream_packetin → pageout
  // 結束 vorbis_analysis_wrote(&vd, 0)
  ```
  Vorbis 編碼器**沒有編碼延遲偏移**（granulepos 精確定義取樣數，解碼端 sample 0 = 輸入 sample 0），所以對齊後的 PCM 編成 `.ogg` 即 sample-accurate。有損二次編碼（Opus/Vorbis → Vorbis）：品質預設 0.4–0.5，使用者可在匯入選項調。
- **Opus 來源也可無損封成 Ogg Opus（`.opus`）**：OpusHead 取自 CodecPrivate（含 pre-skip）、`OpusTags` 自寫、granulepos = 48 kHz 取樣數（含 pre-skip）；SDL_mixer 實作有 `opus` 副檔名且 `Mix_Init` 旗標含 `OPUS`（`research/01` §7）。但需求指定 Ogg Vorbis，且 Opus 對齊同樣無法做正偏移，故列為可選優化。
- 輸出檔位置與 `.ogg`/`.wav` 的 Sound 資產副作用見 §2.5（A 解）。

### 7.5 匯入流程（M4）建議管線

`複製 WebM（或原地參考）→ WebmDemuxer 開檔取資訊 → 寫 .cevvideo/.cryasset（state=processing）→ [job] 語言音軌 remux（每個選取的音軌一個 WebM）→ [job] 解碼對齊 → WAV 與 OGG → [job] 登錄到 SDL_mixer assets/ + 寫 ACE XML + 發 AUDIO_REFRESH → 主執行緒更新 Files/Details，state=ready`。每步可取消；取消時刪暫存輸出並把 state 設為 `cancelled`，保持 `.cevvideo` 可用（只有內嵌音軌的 embedded 路線）。

---

## 8. 社群樹差異（對編輯器插件有意義的部分）

比對 `V:` 與 `C:`（`diff -rq`）：

- **`EditorCommon/AssetSystem` 只有 `Browser/AssetBrowser.cpp` 不同，且只是縮排／空白**（同 `research/01` §6）。`EditorCommon/CMakeLists.txt` 只多 `NodeGraph/AbstractShortcutNodeItem`/`ShortcutNodeEditorData`；`NodeGraph/*` 有差異（與我們無關）。所以本檔所有資產系統／`CAssetEditor`／`QViewport`／`ThreadingUtils` 結論兩棵樹皆成立。
- `EditorAudioControlsEditor`（含 `EditorSDLMixer`）、`EditorQt/AssetSystem` **無差異**（`diff -rq` 無輸出）。ACE 結論兩邊相同。
- Qt 兩邊都是 `Code/SDKs/Qt/5.12.3_pyside2`（`Tools/CMake/Build.cmake:21-22` 指向 `SDK_DIR/Qt/5.12.3_pyside2`）。Qt 5.12 注意：`QMetaObject::invokeMethod` 的 functor 多載可用（5.10+）；不要用 Qt 6 的 API；若引入 C++20 才會遇到 `qlist.h` 的 `checked_array_iterator` 警告（XTUI 的處理），C++17 不會。
- **CMake**：`Tools/CMake/BuildSandbox.cmake` 社群版多了 `PLUGIN_HPI_PLUGIN`、`PLUGIN_LOD_GENERATOR`、`PLUGIN_CRYVS`（`EditorCryVS` 取代官方版的 `EditorSchematyc2`）；兩邊都已含 `EditorCEVPlayer` 的 `if(EXISTS …)` 註冊區塊（社群版 `BuildSandbox.cmake:56-59` 已套 patch）。`CRYENGINE-MSVC.cmake:14`：官方 `/WX`，社群 `/WX-`（社群版容錯較高，**官方版要 0 警告**才能過；編輯器插件的警告以官方版為準）。
- 社群版另有 `EditorCryVS`（CryVS 資產型別 `CryVSAssetType.cpp`）、`HPiPlugin`、`LodGeneratorPlugin`、`SchematycEditor`：可當 `CAssetType` 的社群版範例，但我們的型別名稱／副檔名不可與其衝突（`cevvideo` 不衝突）。
- 官方版與社群版 `Build.cmake` 的 `Code/CryPlugins` 都在 Sandbox 之後（官方 `:35` vs `:51`），§1.1 的「兩邊 guard」同時適用。
- 其他：`.cryasset`／ACE 格式、`IAutoEditorNotifyListener`、`CGameEngine::Update` 編輯分支兩棵樹行為一致（未逐行 diff `GameEngine.cpp`，但 `EditorQt/` 內其他檔差異不在本檔使用範圍；實作前以 `diff` 再確認 `EditorQt/GameEngine.cpp`）。

---

## 9. 建議

### 9.1 類別配置（`Code/Sandbox/Plugins/EditorCEVPlayer`）

```
EditorCEVPlayer/
  CMakeLists.txt            // §1.1；Core guard；vorbisenc 另外連
  StdAfx.h/.cpp
  EditorCEVPlayerPlugin.h/.cpp        // CEditorCEVPlayerPlugin : IPlugin；REGISTER_PLUGIN
  Asset/
    VideoAssetType.h/.cpp             // CVideoAssetType : CAssetType（REGISTER_ASSET_TYPE）
    VideoAssetImporter.h/.cpp         // CVideoAssetImporter : CAssetImporter（.webm）
    VideoDescriptorIO.h/.cpp          // .cevvideo 讀寫（與 Module/Player/VideoDescriptor 格式一致；不連結 Module，另寫小型寫入器，讀可共用解析邏輯的複製）
    VideoAssetMeta.h/.cpp             // Details 的序列化（匯入設定、來源雜湊、ACE 登錄資訊）
  Editor/
    VideoEditor.h/.cpp                // CCevVideoEditor : CAssetEditor（REGISTER_VIEWPANE_FACTORY）
    ViewerPanel.h/.cpp                // 中欄：QViewport 子類別 + 傳輸列（播放/暫停/拖曳/步進/循環/時間碼/音訊路線）
    VideoViewport.h/.cpp              // CVideoViewport : QViewport（停用相機操作；WinIdChange 重建保護）
    InfoPanel.h/.cpp                  // 右欄：資訊 + 操作（重新匯入、重新產生音訊、登錄 ACE、開啟來源資料夾）+ Jobs 清單
    ImportOptionsDialog.h/.cpp        // CRF/解析度/變體/WAV/分離音訊/語言/路線/觸發器名
  Jobs/
    JobRunner.h/.cpp                  // CJob、std::thread 管理、取消、日誌
    RemuxAudioJob.cpp                 // Core WebmDemuxer → mkvmuxer
    DecodeAlignJob.cpp                // Core AudioDecoder/Resampler → 取樣對齊 PCM
    WavWriter.cpp、OggVorbisWriter.cpp
  Audio/
    AceRegistrar.h/.cpp               // 寫 audio/sdlmixer/ace/<lib>.xml、複製音檔到 sdlmixer/assets、AUDIO_REFRESH
  Preview/
    PreviewSession.h/.cpp             // 擁有 IVideoPlayer*（CreatePlayer/DestroyPlayer）、暫存描述檔、貼圖/棋盤資源
```

重點原則：UI 與引擎物件只在主執行緒；Jobs 只碰 STL + Core + 檔案；`PreviewSession` 是唯一持有 `IVideoPlayer*` 的地方（`QPointer` 與 shutdown 檢查）。

### 9.2 M4 能在「編譯＋不崩潰」驗證下實際交付的內容（無法目視）

可穩穩交付、可用自動化／log 驗證：
1. 插件可載入，Sandbox 啟動／關閉不崩（log：`[CEVEditor] plugin loaded`）。
2. `CVideoAssetType` 註冊，Asset Browser 能列出（以腳本／程式化建立資產後 `FindAssetForMetadata` 確認）；`CanAutoRepairMetadata=false` 無 RC 呼叫（log 確認）。
3. 匯入核心（與 UI 分離的純函式）：複製 WebM、寫 `.cevvideo` + `.cryasset`、記 Details、來源雜湊；單元測試放 Core 旁（類似 `cevcore_selftest`，可獨立 CMake 編譯，不需引擎）。
4. **音訊分離／WAV／OGG 的離線工具函式 + selftest**：remux 後以 Core `WebmDemuxer`/`AudioDecoder` 讀回，比對 packet 數、pts、CodecDelay；WAV：與 Core 直接解碼輸出逐取樣比較（含對齊偏移案例）；Ogg：以 libvorbis 解碼讀回，比較取樣數與取樣值誤差；這些可以在 `cevcore_selftest` 式獨立可執行檔內全自動驗證，不需肉眼。
5. ACE XML 產生（字串比對 `cev_test.xml` 風格）與 `AUDIO_REFRESH` 發送（log）；以 `cev_audio` 冒煙腳本（已有 `P:tools/test_audio`）在 GameLauncher 內觸發 trigger 驗證有 `TriggerExecuted`。
6. 三欄視窗能開、關、重開、切換資產、dock 拖曳 20 次不崩（用 log 的 `[CEVEditor] viewport created/destroyed` 配對與 `xtui` 式 pending-delete 計數；關注 Windows 事件記錄 event 153，§4.7）。
7. 預覽：`IVideoPlayer` 載入→`Play`→log `OnNewFrame` 序號遞增、`GetTextureId()>0`、`OnRender` 內 `PushImage` 被呼叫的計數 log；畫面正確性（棋盤、方向、premultiplied 外觀）**無法目視驗證**，用「讀回一幀」輔助：`gEnv->pRenderer` 的 `ReadFrameBuffer` 類 API 對 swap chain 不一定可用，退而求其次用 `DumpMemory`/截圖 console 命令不可靠——**建議把視覺驗證留給使用者，M4 交付時附一份「三分鐘人工驗收清單」**。

M4 不建議承諾：變體自動產生（需要轉檔，屬 FFmpeg 階段）、ACE 視窗開著時即時熱更新的完美行為、Release 建置的 audio refresh、CryAudio 觸發器路線的預覽音訊同步精度。

### 9.3 風險清單（依嚴重度）

1. **GPU fault／黑畫面（viewport swap chain 與 dock 重新 parent）**：§4.7 第 2 點。緩解 (a)(b)，備案 (c)＝XTFrame 子視窗法。建議 M4 先做好 (b) 並在 log 計數 `viewport recreated`；若實測不穩，改備案。
2. **premultiplied 兩遍法未實機驗證**：理論根據是 `AuxGeom.cfx` 的 `col*tex` 與 `D3DRenderAuxGeom.cpp:569-573` 的混合表；風險在 `e_Def2DImageRenderflags` 其他位元（深度寫入、cull）與 `PushImage` 的 flags 欄位是否真的逐圖生效（`RenderAuxGeom.cpp:2246-2252` 讀碼確認會切換）。保底：預覽不透明影片用單遍，有 alpha 時兩遍；兩遍失敗時退回單遍並在狀態列標註「預覽 alpha 近似」。
3. **WAV／OGG 與 Sound 資產自動產生互撞**（§2.5）：需使用者決定資產內的 WAV 去向（建議 A：登錄副本）。
4. **重命名後 `.cevvideo` 內檔名不同步**（§2.3）：必須覆寫 `RenameAsset`；若漏掉，資產改名後執行期載入失敗（`ResolveSiblingPath` 找不到檔）。要有 selftest。
5. **匯入器死鎖**：`PostOnMainThread().get()` 在主執行緒（§2.6）；`ImportAssets` 內判斷執行緒；自己的 UI 路徑不呼叫 `CAssetImporter::Import`。
6. **共用 `AsyncQueue` 被長工作霸佔**：用自建執行緒（§5）。
7. **ACE 的未儲存修改與外部寫檔衝突**；`AUDIO_REFRESH` 只在 `CRY_AUDIO_USE_DEBUG_CODE` 建置有效（§6.3）。
8. **CMake 順序／重複 target**（§1.1）：Module 與 Editor 兩邊 `if(NOT TARGET CEVPlayerCore)` guard，漏掉一邊就 configure 失敗；官方版 `/WX` 對編輯器插件也生效。
9. **執行期介面缺口**：步進、路線覆寫、幀率；用暫存描述檔與 Core 取得，長期應在 `IVideoPlayer` 追加（interface v5）。
10. **`ESYSTEM_EVENT_RESIZE` 全域廣播**（`QViewport::resizeEvent`）：dock 拖曳時頻繁；我們的插件不聽，風險低，但留意 log 噪音。
11. **遊戲模式下視窗不更新**（無 idle）：行為可接受，需在 UI 上明示。
12. **已確認需要補的建置件**：`libvorbisenc.cmake`、Module 的 Core guard、`EditorCEVPlayer/CMakeLists.txt`。

### 9.4 待使用者決定

1. 資產內 WAV/OGG 的位置：登錄副本（建議，§2.5 A）或放資產資料夾（§2.5 B，Sound 資產並行）。
2. 來源模型：外部參照＋雜湊（建議，M4 WebM-only 時主影片＝來源複本）或將來放入 Assets 並走 `IsImported`。
3. 預覽視口是否接受「備案＝XTFrame 子視窗法」作為驗證失敗時的第二階段（會多約 250 行）。
