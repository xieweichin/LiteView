cmake_minimum_required(VERSION 3.25)

# Static CRT (/MT) for every target; must be set before project().
cmake_policy(SET CMP0091 NEW)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

project(LiteView VERSION 0.1.0 LANGUAGES CXX RC)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT WIN32)
    message(FATAL_ERROR "LiteView is Windows only.")
endif()

option(LITEVIEW_BUILD_TESTS "Build doctest unit tests" ON)

# ---- Common compile settings ------------------------------------------------
add_library(liteview_options INTERFACE)
target_compile_definitions(liteview_options INTERFACE
    UNICODE _UNICODE
    WIN32_LEAN_AND_MEAN NOMINMAX
    _WIN32_WINNT=0x0A00 WINVER=0x0A00 NTDDI_VERSION=0x0A000006)

if(MSVC)
    target_compile_options(liteview_options INTERFACE
        /utf-8 /W4 /permissive- /Zc:__cplusplus
        $<$<CONFIG:Release>:/O1 /GL /Gy /Gw>)
    target_link_options(liteview_options INTERFACE
        $<$<CONFIG:Release>:/LTCG /OPT:REF /OPT:ICF>)
else()
    # MinGW is only used for quick local compile checks.
    target_compile_options(liteview_options INTERFACE -Wall -Wextra -Wno-missing-field-initializers)
    target_link_options(liteview_options INTERFACE -static)
endif()

# ---- Core layer (no UI, unit tested) -----------------------------------------
add_library(LiteViewCore STATIC
    src/core/FolderModel.cpp
    src/core/ImageDecoder.cpp
    src/core/Metadata.cpp
    src/core/Settings.cpp
    src/core/ThreadPool.cpp
    src/core/ThumbnailService.cpp
    src/core/ViewerCache.cpp)
target_include_directories(LiteViewCore PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(LiteViewCore PUBLIC liteview_options windowscodecs shlwapi shell32 ole32 uuid)

# ---- Application ------------------------------------------------------------
add_executable(LiteView WIN32
    src/main.cpp
    src/ui/MainWindow.cpp
    src/ui/MainWindow.h
    src/ui/FileListView.cpp
    src/ui/FileListView.h
    src/ui/FolderTree.cpp
    src/ui/FolderTree.h
    src/ui/PreviewPane.cpp
    src/ui/PreviewPane.h
    src/ui/ViewerView.cpp
    src/ui/ViewerView.h
    src/ui/Messages.h
    src/ui/Commands.h
    src/res/resource.h
    src/res/LiteView.rc)
target_include_directories(LiteView PRIVATE src src/res)
target_link_libraries(LiteView PRIVATE LiteViewCore liteview_options
    comctl32 shlwapi ole32 shell32 uuid uxtheme msimg32 d2d1)

if(MSVC)
    # The manifest is embedded by LiteView.rc.
    target_link_options(LiteView PRIVATE /MANIFEST:NO)
else()
    target_link_options(LiteView PRIVATE -municode)
    # windres needs an explicit code page even when the .rc has a pragma.
    set_source_files_properties(src/res/LiteView.rc PROPERTIES COMPILE_FLAGS "--codepage=65001")
endif()

# ---- Tests ------------------------------------------------------------------
if(LITEVIEW_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
