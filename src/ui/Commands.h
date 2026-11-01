// Command IDs for every menu item and shortcut in docs/spec.md.
// This header is also included by LiteView.rc, so it must contain only
// preprocessor definitions outside the RC_INVOKED guard.
#pragma once

// ---- Accelerator tables -----------------------------------------------------
#define IDR_ACCEL_BROWSER           200   // 瀏覽模式快捷鍵
#define IDR_ACCEL_VIEWER            201   // 檢視模式快捷鍵

// ---- File menu --------------------------------------------------------------
#define IDM_FILE_OPEN_FOLDER        40001
#define IDM_FILE_NEW_FOLDER         40002
#define IDM_FILE_PROPERTIES         40003 // Alt+Enter 圖片資訊
#define IDM_FILE_EXIT               40004

// ---- Edit menu --------------------------------------------------------------
#define IDM_EDIT_COPY_TO            40101
#define IDM_EDIT_MOVE_TO            40102
#define IDM_EDIT_DELETE             40103 // Delete
#define IDM_EDIT_RENAME             40104 // F2
#define IDM_EDIT_BATCH_RENAME       40105 // Ctrl+F2
#define IDM_EDIT_UNDO_BATCH_RENAME  40106
#define IDM_EDIT_SELECT_ALL         40107 // Ctrl+A

// ---- View menu (browser) ----------------------------------------------------
#define IDM_VIEW_THUMBNAILS         40201
#define IDM_VIEW_LARGE_ICONS        40202
#define IDM_VIEW_LIST               40203
#define IDM_VIEW_DETAILS            40204
#define IDM_SORT_NAME               40211
#define IDM_SORT_SIZE               40212
#define IDM_SORT_TYPE               40213
#define IDM_SORT_MODIFIED           40214
#define IDM_VIEW_REFRESH            40221 // F5

// ---- Navigation -------------------------------------------------------------
#define IDM_NAV_TOGGLE_MODE         40301 // Enter（瀏覽 ↔ 檢視）；檢視模式另有 Esc
#define IDM_NAV_EXIT_VIEWER         40302 // Esc（只在檢視模式）
#define IDM_NAV_PARENT_FOLDER       40303 // Backspace（只在瀏覽模式）
#define IDM_NAV_NEXT                40304 // PageDown、Space、滾輪向下
#define IDM_NAV_PREV                40305 // PageUp、Backspace、滾輪向上
#define IDM_NAV_FIRST               40306 // Home
#define IDM_NAV_LAST                40307 // End

// ---- Viewer -----------------------------------------------------------------
#define IDM_ZOOM_IN                 40401 // +、Ctrl+滾輪
#define IDM_ZOOM_OUT                40402 // -、Ctrl+滾輪
#define IDM_ZOOM_TOGGLE_FIT         40403 // /（數字鍵盤）、滑鼠中鍵
#define IDM_VIEW_FULLSCREEN         40404 // Ctrl+F
#define IDM_VIEW_SLIDESHOW          40405 // Ctrl+S
#define IDM_SLIDESHOW_2S            40411 // slideshow interval
#define IDM_SLIDESHOW_3S            40412
#define IDM_SLIDESHOW_5S            40413
#define IDM_SLIDESHOW_10S           40414

// ---- Tools ------------------------------------------------------------------
#define IDM_ROTATE_LEFT             40501 // Ctrl+[
#define IDM_ROTATE_RIGHT            40502 // Ctrl+]

// ---- Command bar drop-downs ------------------------------------------------
#define IDM_BAR_SORT                40601 // 排序方式選單
#define IDM_BAR_VIEW                40602 // 檢視方式選單
#define IDM_BAR_MORE                40603 // 完整選單（Alt、F10）

// ---- Help -------------------------------------------------------------------
#define IDM_HELP_ABOUT              40901
