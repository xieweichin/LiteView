// Private window messages used between the UI and background work.
#pragma once

#include <windows.h>

// MainWindow
constexpr UINT WM_APP_SETTINGS_READY = WM_APP + 1;   // wParam: settings token
constexpr UINT WM_APP_SETTINGS_SAVED = WM_APP + 2;   // wParam: settings token
constexpr UINT WM_APP_FOLDER_LOADED = WM_APP + 3;    // wParam: FolderModel generation
constexpr UINT WM_APP_TREE_CHILDREN = WM_APP + 4;    // FolderTree results are queued; drain them
constexpr UINT WM_APP_UPDATE_STATUS = WM_APP + 5;    // coalesced status bar refresh
constexpr UINT WM_APP_PATH_ENTERED = WM_APP + 6;     // path bar Enter; text is read from the bar
constexpr UINT WM_APP_THUMB_READY = WM_APP + 7;      // wParam: generation, lParam: item id
constexpr UINT WM_APP_PREVIEW_READY = WM_APP + 8;    // PreviewPane: its decode finished
constexpr UINT WM_APP_VIEWER_READY = WM_APP + 9;     // ViewerCache finished a decode
constexpr UINT WM_APP_ROTATED = WM_APP + 10;         // a background rotation finished
constexpr UINT WM_APP_TEXT_READY = WM_APP + 11;      // TextView: its file finished loading
constexpr UINT WM_APP_DOCUMENT_LOAD = WM_APP + 12;   // DocumentView: start the preview handler
