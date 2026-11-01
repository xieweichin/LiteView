add_executable(LiteViewTests
    test_main.cpp
    test_smoke.cpp
    test_folder_model.cpp
    test_settings.cpp
    test_thread_pool.cpp)
target_include_directories(LiteViewTests PRIVATE third_party)
target_link_libraries(LiteViewTests PRIVATE LiteViewCore)

add_test(NAME LiteViewTests COMMAND LiteViewTests)
set_tests_properties(LiteViewTests PROPERTIES
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    TIMEOUT 30)

option(LITEVIEW_BUILD_BENCHMARKS "Build manual performance checks" OFF)
if(LITEVIEW_BUILD_BENCHMARKS)
    add_executable(LiteViewFolderBenchmark benchmark_folder.cpp)
    target_link_libraries(LiteViewFolderBenchmark PRIVATE LiteViewCore)
    if(NOT MSVC)
        target_link_options(LiteViewFolderBenchmark PRIVATE -municode)
    endif()
endif()
