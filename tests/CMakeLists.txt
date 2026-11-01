add_executable(LiteViewTests
    test_main.cpp
    test_smoke.cpp
    test_folder_model.cpp
    test_format_support.cpp
    test_text_document.cpp
    test_pdf_renderer.cpp
    test_vector_renderer.cpp
    test_model_loader.cpp
    test_image_decoder.cpp
    test_settings.cpp
    test_thread_pool.cpp
    test_thumbnail_service.cpp
    test_viewer_cache.cpp
    test_rotator.cpp
    test_file_ops.cpp
    test_batch_rename.cpp)
target_include_directories(LiteViewTests PRIVATE third_party)
target_link_libraries(LiteViewTests PRIVATE LiteViewCore)
add_custom_target(LiteViewTestFixtures
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/fixtures" "$<TARGET_FILE_DIR:LiteViewTests>/fixtures")
add_dependencies(LiteViewTests LiteViewTestFixtures)

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
