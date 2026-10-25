cmake_minimum_required(VERSION 3.25)

project(
    livesplit_bridge_client
    VERSION 0.1.0
    DESCRIPTION "C++23 client library for the LiveSplit.Bridge JSON protocol v4"
    HOMEPAGE_URL "https://github.com/Nanahuse/livesplit-bridge-client-cpp"
    LANGUAGES CXX
)

if(PROJECT_IS_TOP_LEVEL)
    option(LIVESPLIT_BRIDGE_BUILD_CLI "Build the verification CLI" ON)
    option(LIVESPLIT_BRIDGE_BUILD_TESTS "Build tests" ON)
else()
    option(LIVESPLIT_BRIDGE_BUILD_CLI "Build the verification CLI" OFF)
    option(LIVESPLIT_BRIDGE_BUILD_TESTS "Build tests" OFF)
endif()

find_package(Boost CONFIG REQUIRED COMPONENTS asio beast)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(Threads REQUIRED)

add_library(livesplit_bridge STATIC
    src/client.cpp
    src/client_impl.cpp
    src/error.cpp
    src/icons.cpp
)

add_library(livesplit_bridge::livesplit_bridge ALIAS livesplit_bridge)

target_compile_features(livesplit_bridge PUBLIC cxx_std_23)
target_include_directories(livesplit_bridge
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(livesplit_bridge
    PUBLIC
        Boost::asio
        Boost::beast
        nlohmann_json::nlohmann_json
        Threads::Threads
    PRIVATE
        $<$<PLATFORM_ID:Windows>:ws2_32>
        $<$<PLATFORM_ID:Windows>:mswsock>
)

if(MSVC)
    target_compile_options(livesplit_bridge PRIVATE /W4 /permissive- /Zc:__cplusplus /bigobj /external:W0 /wd4702)
    target_compile_definitions(livesplit_bridge PRIVATE _WIN32_WINNT=0x0A00 WIN32_LEAN_AND_MEAN)
else()
    target_compile_options(livesplit_bridge PRIVATE -Wall -Wextra -Wpedantic)
endif()

set_target_properties(livesplit_bridge PROPERTIES
    CXX_EXTENSIONS OFF
    POSITION_INDEPENDENT_CODE ON
)

if(LIVESPLIT_BRIDGE_BUILD_CLI)
    add_executable(livesplit_bridge_cli cli/main.cpp)
    target_compile_features(livesplit_bridge_cli PRIVATE cxx_std_23)
    target_link_libraries(livesplit_bridge_cli PRIVATE livesplit_bridge::livesplit_bridge)
    if(MSVC)
        target_compile_options(livesplit_bridge_cli PRIVATE /bigobj)
    endif()
    set_target_properties(livesplit_bridge_cli PROPERTIES CXX_EXTENSIONS OFF)
endif()

if(LIVESPLIT_BRIDGE_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

install(
    TARGETS livesplit_bridge
    EXPORT livesplit_bridgeTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)
install(
    DIRECTORY include/
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)
install(
    EXPORT livesplit_bridgeTargets
    FILE livesplit_bridgeTargets.cmake
    NAMESPACE livesplit_bridge::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/livesplit_bridge
)

configure_package_config_file(
    cmake/livesplit_bridgeConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/livesplit_bridgeConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/livesplit_bridge
)
write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/livesplit_bridgeConfigVersion.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
)
install(
    FILES
        ${CMAKE_CURRENT_BINARY_DIR}/livesplit_bridgeConfig.cmake
        ${CMAKE_CURRENT_BINARY_DIR}/livesplit_bridgeConfigVersion.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/livesplit_bridge
)
