# LiteView 系統架構

Nov 1, 2026 · @偉欽

LiteView 是單一執行檔的 C++ Win32 程式，分成介面層與核心層兩層，全部只依賴 Windows 內建 API。功能範圍見 [第一版規格](https://claude.ai/artifact/K41pDhdcgmtJn34p1MB2gS)。

## 全貌

```text
介面層  ui/    MainWindow · FolderTree · FileListView · PreviewPane · ViewerView · dialogs/
           │ 呼叫                         ▲ PostMessage(WM_APP_*)
           ▼                              │
核心層  core/  FolderModel · ImageDecoder · ThumbnailService · ViewerCache · Metadata
               Rotator · FileOps · BatchRename · Settings · ThreadPool
           │
           ▼
Windows API    Win32 · ComCtl32 v6 · WIC · Direct2D · Shell (IFileOperation, IShellItemImageFactory)
```

依賴只往下走：介面層呼叫核心層，核心層呼叫 Windows API；核心層的背景工作完成後，只能用 PostMessage 通知介面。

## 技術選型與建置

| 項目 | 選擇 | 理由 |
| --- | --- | --- |
| 語言 | C++17 | 無執行環境、檔案小、速度最快 |
| 視窗 | 純 Win32 + ComCtl32 v6（TreeView、虛擬 ListView） | 不用 MFC / Qt，維持 2 MB 以內 |
| 解碼 | WIC（Windows Imaging Component） | 系統內建，裝擴充後自動支援 WebP、HEIC、RAW |
| 繪圖 | Direct2D | 硬體加速縮放，大圖拖曳流暢 |
| COM 指標 | WRL `Microsoft::WRL::ComPtr` | 系統 SDK 內建 |
| 建置 | CMake 3.25 + Visual Studio 2022（MSVC） | Codex 與 GitHub Actions 都能跑 |
| 連結 | 靜態 CRT（`/MT`）、`/O1`、`/GL` + `/LTCG` | 不需要 VC++ 可轉散發套件 |
| 測試 | doctest（單一標頭檔）測 core/ | 核心層不碰視窗，可自動測試 |
| 安裝檔 | Inno Setup 6 | 產出單一 setup.exe，可勾選檔案關聯 |
| CI | GitHub Actions `windows-latest` | 每次 push 建置並跑測試 |

執行檔附 manifest：啟用 ComCtl32 v6 樣式、Per-Monitor V2 DPI 感知、字串一律用 UTF-16（`wchar_t`）。

## 模組職責

介面層只負責顯示與輸入，所有檔案與圖片邏輯都在核心層；核心層不可 include `<windows.h>` 的視窗相關功能（HWND、訊息），只用回呼通知介面。

| 層 | 模組 | 職責 | 對應規格功能 |
| --- | --- | --- | --- |
| 介面 | `MainWindow` | 主框架、網址列、狀態列、瀏覽 ↔ 檢視切換、快捷鍵表（Accelerator） | 模式切換、快捷鍵 |
| 介面 | `CommandBar` | 自繪的 Windows 11 風格指令列（圖示字型、下拉按鈕、提示） | 工具按鈕 |
| 介面 | `Theme` | 深色／淺色配色、標題列、標準控制項與對話框的深色樣式 | 外觀 |
| 介面 | `FolderTree` | 左上資料夾樹，延遲展開子資料夾 | 資料夾樹 |
| 介面 | `FileListView` | 虛擬 ListView（`LVS_OWNERDATA`），四種檢視、多選、自繪縮圖 | 縮圖清單、四種檢視 |
| 介面 | `PreviewPane` | 右側預覽窗格（可隱藏、可調寬） | 預覽窗 |
| 介面 | `ViewerView` | Direct2D 看圖、縮放、拖曳、全螢幕、幻燈片計時器 | 檢視模式全部 |
| 介面 | `dialogs/` | 批次改名、圖片資訊、設定對話方塊 | 批次改名、圖片資訊 |
| 核心 | `FolderModel` | 列出圖檔、自然排序（`StrCmpLogicalW`）、監看資料夾變動 | 排序、F5 重整 |
| 核心 | `ImageDecoder` | WIC 解碼、按目標尺寸縮小解碼、依 EXIF 轉正 | 開圖、自動轉正 |
| 核心 | `ThumbnailService` | 背景執行緒池產生縮圖，記憶體 LRU + 磁碟快取 | 縮圖清單 |
| 核心 | `ViewerCache` | 預載前後各一張、保留最近 3 張已解碼圖 | 換圖不等待 |
| 核心 | `Metadata` | 讀 EXIF：拍攝日期、相機、方向 | 圖片資訊、依日期命名 |
| 核心 | `Rotator` | 旋轉存檔（JPEG 改 EXIF 方向，其他重新編碼） | 旋轉存檔 |
| 核心 | `FileOps` | 複製、移動、刪到回收筒、單檔改名、新增資料夾（`IFileOperation`） | 基本管理 |
| 核心 | `BatchRename` | 解析模板、產生預覽、衝突檢查、執行與復原 | 批次改名、復原 |
| 核心 | `Settings` | 讀寫 INI、判斷綠色版 | 記住路徑、綠色版 |

## 執行緒與資料流

只有 UI 執行緒碰視窗；解碼與檔案工作都在背景執行緒，做完用 `PostMessage` 把結果丟回 UI 執行緒。

**開資料夾與縮圖**

1. 使用者選資料夾，`FolderModel` 產生新的「世代編號」（generation），在背景列檔並排序。
2. 列檔完成後通知 `FileListView` 設定項目數，虛擬 ListView 只繪出可見的那一屏。
3. 繪製時沒有縮圖的項目先畫檔案類型圖示，並向 `ThumbnailService` 發出請求，可見項目優先。
4. 工作執行緒依序嘗試：記憶體 LRU → 磁碟快取 → Windows 系統縮圖（`IShellItemImageFactory`）→ WIC 縮小解碼。
5. 完成後 `PostMessage(WM_APP_THUMB_READY)`，帶上項目編號與世代編號；世代不符就丟棄。
6. 捲動或換資料夾時，佇列中尚未開始且已不可見的請求直接取消。

**檢視模式換圖**

1. 顯示第 N 張時，`ViewerCache` 在背景預載 N+1 與 N−1（依使用者前進方向優先）。
2. 解碼時若圖比螢幕大，先解碼成螢幕大小版；使用者放大到超過時才解碼原尺寸。
3. 按下一張時已在快取就直接繪出；不在就先顯示縮圖放大版，解碼完再替換。

執行緒數：縮圖用 `核心數 − 1`（最少 1、最多 4），檢視預載 1 條。執行緒池用 `std::thread` + 條件變數自寫，不用第三方函式庫。

## 專案目錄結構

```text
LiteView/
├─ CMakeLists.txt
├─ AGENTS.md              給 Codex 的開發規範（即本文最後一節）
├─ src/
│  ├─ main.cpp            wWinMain、COM 初始化、訊息迴圈
│  ├─ core/               不碰視窗，可單元測試
│  │  ├─ FolderModel.h/.cpp
│  │  ├─ ImageDecoder.h/.cpp
│  │  ├─ ThumbnailService.h/.cpp
│  │  ├─ ViewerCache.h/.cpp
│  │  ├─ Metadata.h/.cpp
│  │  ├─ Rotator.h/.cpp
│  │  ├─ FileOps.h/.cpp
│  │  ├─ BatchRename.h/.cpp
│  │  ├─ Settings.h/.cpp
│  │  └─ ThreadPool.h/.cpp
│  ├─ ui/
│  │  ├─ MainWindow.h/.cpp
│  │  ├─ FolderTree.h/.cpp
│  │  ├─ FileListView.h/.cpp
│  │  ├─ PreviewPane.h/.cpp
│  │  ├─ ViewerView.h/.cpp
│  │  ├─ Commands.h        所有指令 ID 與快捷鍵表
│  │  └─ dialogs/
│  └─ res/
│     ├─ LiteView.rc       字串表（繁體中文）、圖示、對話方塊
│     └─ LiteView.manifest
├─ tests/                 doctest，只測 core/
│  └─ fixtures/           測試用小圖（含各種 EXIF 方向）
├─ installer/LiteView.iss  Inno Setup 腳本
└─ .github/workflows/build.yml
```

## 核心介面約定

以下是介面層與核心層之間的合約草稿，Codex 實作時可加成員，但不改既有簽名。錯誤一律回傳 `HRESULT`，不丟例外。

```cpp
// core/FolderModel.h
struct FileItem {
    std::wstring name;          // 檔名（含副檔名）
    uint64_t     size;
    FILETIME     modified;
    uint32_t     id;            // 資料夾內唯一，重整後不重複
};
enum class SortKey { Name, Size, Type, Modified };

class FolderModel {
public:
    using OnLoaded = std::function<void(uint32_t generation)>;
    uint32_t Open(const std::wstring& path, OnLoaded cb); // 背景列檔，回傳世代編號
    void     Sort(SortKey key, bool ascending);
    size_t   Count() const;
    const FileItem& At(size_t index) const;
    std::wstring FullPath(size_t index) const;
};

// core/ImageDecoder.h
struct DecodedImage {
    Microsoft::WRL::ComPtr<IWICBitmapSource> bitmap; // 已依 EXIF 轉正，32bppPBGRA
    UINT width, height;                              // 解碼後尺寸
    UINT originalWidth, originalHeight;
};
class ImageDecoder {
public:
    // maxSide = 0 表示原尺寸；否則縮小到長邊不超過 maxSide
    HRESULT Decode(const std::wstring& path, UINT maxSide, DecodedImage& out);
};

// core/ThumbnailService.h
class ThumbnailService {
public:
    using OnReady = std::function<void(uint32_t generation, uint32_t itemId, HBITMAP bmp)>;
    explicit ThumbnailService(OnReady cb);   // cb 在背景執行緒呼叫，介面層自行 PostMessage
    void Request(uint32_t generation, uint32_t itemId, const std::wstring& path, UINT size, bool visible);
    void CancelAllExcept(uint32_t generation);
    HBITMAP TryGetCached(const std::wstring& path, UINT size); // 命中記憶體快取就立即回傳
};

// core/BatchRename.h
struct RenamePlanItem { std::wstring from, to; bool conflict; };
class BatchRename {
public:
    // pattern 例：「旅遊_###」、「{date:yyyyMMdd}_##」；# 為流水號位數
    std::vector<RenamePlanItem> Preview(const std::vector<std::wstring>& paths,
                                        const std::wstring& pattern, int startNumber);
    HRESULT Execute(const std::vector<RenamePlanItem>& plan); // 寫入復原紀錄
    HRESULT UndoLast();
};
```

`ViewerCache`、`Metadata`、`Rotator`、`FileOps`、`Settings` 的介面由 Codex 依模組職責表自行定義，審查時再確認。

## 關鍵設計決定

- **旋轉存檔**：JPEG 用 `IWICFastMetadataEncoder` 只改 EXIF Orientation，影像資料不動、不失真；若檔案沒有預留空間則用 `IWICComponentFactory` 複製中繼資料重寫。PNG、BMP、TIFF 重新編碼。先寫到暫存檔，成功後用 `ReplaceFileW` 取代原檔，並保留修改日期。
- **批次改名安全**：預覽時標出衝突（重名、非法字元、路徑過長），有衝突不能執行。執行分兩階段：先全部改成暫名，再改成目標名，避免 A→B、B→A 互踩。任一步失敗就全部還原。復原紀錄存在設定資料夾的 `rename-undo.txt`。
- **刪除**：一律經 `IFileOperation` 送到資源回收筒（`FOFX_RECYCLEONDELETE`），不直接刪檔。
- **設定位置**：程式資料夾有 `portable.ini` 就用程式資料夾；否則用 `%APPDATA%\LiteView\settings.ini`。
- **縮圖快取**：存在 `%LOCALAPPDATA%\LiteView\thumbs\`（綠色版則在程式資料夾 `thumbs\`），檔名為「完整路徑 + 修改時間 + 尺寸」的雜湊值，內容為 85% 品質 JPEG。超過 200 MB 時刪除最久沒用的。
- **檔案關聯**：由安裝檔寫入登錄檔（HKCU），程式本身不動登錄檔。命令列 `LiteView.exe <圖檔>` 直接進檢視模式，`LiteView.exe <資料夾>` 進瀏覽模式。
- **單一執行個體**：不限制，可同時開多個視窗（與 ACDSee 3.2 相同）。

## 給 Codex 的開發規範

這一節會原樣放進 repo 的 `AGENTS.md`，Codex 每次工作都會先讀。

1. 只用 C++17 與 Windows SDK，不引入任何第三方函式庫（doctest 只在 tests/ 內）。
2. `core/` 不可使用 HWND、視窗訊息或 `ui/` 的任何標頭檔。
3. 所有字串用 `std::wstring`；介面文字一律放在 `LiteView.rc` 字串表，程式碼內不寫死中文。
4. COM 物件一律用 `ComPtr`；GDI 控制代碼用 RAII 包裝，不可洩漏。
5. 錯誤回傳 `HRESULT`，不用例外；使用者看得到的錯誤要有中文訊息。
6. UI 執行緒上不可做解碼、列檔或任何可能超過 16 毫秒的工作。
7. 每個工作單只改該單列出的檔案；要改介面約定時先在 PR 說明理由。
8. `core/` 新增功能要附 doctest 測試，CI 全綠才算完成。
9. Release 版執行檔超過 2 MB 視為失敗，CI 會檢查。
10. 程式碼註解用繁體中文或英文皆可，識別字一律英文。
