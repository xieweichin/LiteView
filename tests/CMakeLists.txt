add_executable(LiteViewTests
    test_main.cpp
    test_smoke.cpp)
target_include_directories(LiteViewTests PRIVATE third_party)
target_link_libraries(LiteViewTests PRIVATE LiteViewCore)

add_test(NAME LiteViewTests COMMAND LiteViewTests)
set_tests_properties(LiteViewTests PROPERTIES
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
