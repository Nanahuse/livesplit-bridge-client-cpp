find_package(Catch2 3 CONFIG REQUIRED)
find_package(Threads REQUIRED)

add_executable(livesplit_bridge_tests
    test_models.cpp
    test_client_mock.cpp
)

target_compile_features(livesplit_bridge_tests PRIVATE cxx_std_23)
target_link_libraries(livesplit_bridge_tests
    PRIVATE
        livesplit_bridge::livesplit_bridge
        Catch2::Catch2WithMain
        Threads::Threads
)
target_include_directories(livesplit_bridge_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/support
)
target_compile_definitions(livesplit_bridge_tests PRIVATE
    LIVESPLIT_BRIDGE_TEST_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/fixtures"
)

if(MSVC)
    target_compile_options(livesplit_bridge_tests PRIVATE /bigobj /external:W0 /wd4702)
    target_compile_definitions(livesplit_bridge_tests PRIVATE _WIN32_WINNT=0x0A00 WIN32_LEAN_AND_MEAN)
endif()

include(CTest)
include(Catch)
catch_discover_tests(livesplit_bridge_tests)

add_executable(livesplit_bridge_testhost_integration
    integration/testhost_integration.cpp
)
target_compile_features(livesplit_bridge_testhost_integration PRIVATE cxx_std_23)
target_link_libraries(livesplit_bridge_testhost_integration PRIVATE livesplit_bridge::livesplit_bridge)
if(MSVC)
    target_compile_options(livesplit_bridge_testhost_integration PRIVATE /bigobj)
    target_compile_definitions(livesplit_bridge_testhost_integration PRIVATE _WIN32_WINNT=0x0A00 WIN32_LEAN_AND_MEAN)
endif()

find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND AND DEFINED ENV{LIVESPLIT_BRIDGE_TESTHOST} AND NOT "$ENV{LIVESPLIT_BRIDGE_TESTHOST}" STREQUAL "")
    foreach(variant IN ITEMS default icons)
        if(variant STREQUAL "icons")
            set(extra_args --with-icons)
            set(test_name testhost_integration_icons)
        else()
            set(extra_args)
            set(test_name testhost_integration)
        endif()
        add_test(
            NAME ${test_name}
            COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/integration/run_testhost.py
                --client $<TARGET_FILE:livesplit_bridge_testhost_integration>
                ${extra_args}
        )
        set_tests_properties(${test_name} PROPERTIES SKIP_RETURN_CODE 77)
    endforeach()
endif()
