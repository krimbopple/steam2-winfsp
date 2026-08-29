cmake_minimum_required(VERSION 3.28)
project(steam2_winfsp VERSION 0.2.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
option(S2FS_BUILD_GUI "Build the Qt 6 manager" ON)


include(FetchContent)
set(ZLIB_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(ZLIB_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(ZLIB_BUILD_STATIC ON CACHE BOOL "" FORCE)
FetchContent_Declare(
    zlib
    GIT_REPOSITORY https://github.com/madler/zlib.git
    GIT_TAG v1.3.1
    GIT_SHALLOW TRUE
)
FetchContent_Declare(
    nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG v3.12.0
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(zlib nlohmann_json)

add_library(s2fs_core STATIC
    src/build_definition.cpp
    src/steam2_archive.cpp
    src/virtual_tree.cpp
    src/depot_keys.cpp
)
target_include_directories(s2fs_core PUBLIC include)
target_link_libraries(s2fs_core PUBLIC zlibstatic nlohmann_json::nlohmann_json)

if(WIN32)
    target_link_libraries(s2fs_core PUBLIC bcrypt)
    target_compile_definitions(s2fs_core PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)

    add_library(winfsp_sdk SHARED IMPORTED GLOBAL)
    set_target_properties(winfsp_sdk PROPERTIES
        IMPORTED_IMPLIB "${CMAKE_CURRENT_SOURCE_DIR}/external/winfsp/lib/winfsp-x64.lib"
        INTERFACE_INCLUDE_DIRECTORIES "${CMAKE_CURRENT_SOURCE_DIR}/external/winfsp/include"
    )

    add_executable(steam2fs
        src/main.cpp
        src/winfsp_filesystem.cpp
    )
    target_link_libraries(steam2fs PRIVATE s2fs_core winfsp_sdk delayimp)
    target_link_options(steam2fs PRIVATE "/DELAYLOAD:winfsp-x64.dll")
    target_compile_definitions(
        steam2fs PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
else()
    find_package(OpenSSL REQUIRED)
    find_package(Threads REQUIRED)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(FUSE3 REQUIRED IMPORTED_TARGET fuse3)
    target_link_libraries(s2fs_core PUBLIC OpenSSL::Crypto Threads::Threads)

    add_executable(steam2fs
        src/main_fuse.cpp
        src/fuse_filesystem.cpp
    )
    target_link_libraries(steam2fs PRIVATE s2fs_core PkgConfig::FUSE3)
    target_compile_definitions(steam2fs PRIVATE _FILE_OFFSET_BITS=64)
endif()

if(S2FS_BUILD_GUI)
    set(CMAKE_AUTOMOC ON)
    set(CMAKE_AUTORCC ON)

    find_package(Qt6 6.4 REQUIRED COMPONENTS Core Widgets Concurrent)
    if(WIN32)
        get_filename_component(QT_BIN_DIR "${Qt6_DIR}/../../../bin" ABSOLUTE)
    endif()

    add_library(s2fs_catalog STATIC gui/catalog.cpp gui/resources.qrc)
    target_include_directories(s2fs_catalog PUBLIC gui)
    target_link_libraries(s2fs_catalog PUBLIC s2fs_core Qt6::Core)

    if(WIN32)
        add_executable(steam2gui WIN32
            gui/main.cpp
            gui/main_window.cpp
            gui/main_window.hpp
        )
        target_link_libraries(
            steam2gui PRIVATE s2fs_catalog Qt6::Widgets Qt6::Concurrent shell32)
        target_compile_definitions(
            steam2gui PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)

        find_program(WINDEPLOYQT_EXECUTABLE
            NAMES windeployqt
            HINTS "${Qt6_DIR}/../../../bin"
            REQUIRED)
        set(S2FS_PACKAGE_DIR "${CMAKE_BINARY_DIR}/package/$<CONFIG>")
        add_custom_target(package_gui
            COMMAND "${CMAKE_COMMAND}" -E rm -rf "${S2FS_PACKAGE_DIR}"
            COMMAND "${CMAKE_COMMAND}" -E make_directory "${S2FS_PACKAGE_DIR}"
            COMMAND "${CMAKE_COMMAND}" -E copy "$<TARGET_FILE:steam2gui>" "${S2FS_PACKAGE_DIR}/steam2gui.exe"
            COMMAND "${CMAKE_COMMAND}" -E copy "$<TARGET_FILE:steam2fs>" "${S2FS_PACKAGE_DIR}/steam2fs.exe"
            COMMAND "${CMAKE_COMMAND}" -E copy "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE" "${S2FS_PACKAGE_DIR}/LICENSE"
            COMMAND "${CMAKE_COMMAND}" -E copy
                "${CMAKE_CURRENT_SOURCE_DIR}/THIRD_PARTY_NOTICES.txt"
                "${S2FS_PACKAGE_DIR}/THIRD_PARTY_NOTICES.txt"
            COMMAND "${CMAKE_COMMAND}" -E copy_directory
                "${CMAKE_CURRENT_SOURCE_DIR}/licenses"
                "${S2FS_PACKAGE_DIR}/licenses"
            COMMAND "${WINDEPLOYQT_EXECUTABLE}"
                --no-translations
                --dir "${S2FS_PACKAGE_DIR}"
                "${S2FS_PACKAGE_DIR}/steam2gui.exe"
            DEPENDS steam2gui steam2fs
            VERBATIM)
    else()
        add_executable(steam2gui
            gui/main.cpp
            gui/main_window.cpp
            gui/main_window.hpp
        )
        target_link_libraries(
            steam2gui PRIVATE s2fs_catalog Qt6::Widgets Qt6::Concurrent)
    endif()
endif()



include(CTest)
if(BUILD_TESTING)
    add_executable(steam2_archive_tests tests/steam2_archive_tests.cpp)
    target_link_libraries(steam2_archive_tests PRIVATE s2fs_core)
    add_test(NAME steam2_archive_tests COMMAND steam2_archive_tests)

    add_executable(virtual_tree_tests tests/virtual_tree_tests.cpp)
    target_link_libraries(virtual_tree_tests PRIVATE s2fs_core)
    add_test(NAME virtual_tree_tests COMMAND virtual_tree_tests)
    if(S2FS_BUILD_GUI)
        add_executable(catalog_tests tests/gui/catalog_tests.cpp)
        target_link_libraries(catalog_tests PRIVATE s2fs_catalog)
        add_test(NAME catalog_tests COMMAND catalog_tests)
        if(WIN32)
            set_tests_properties(catalog_tests PROPERTIES
                ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${QT_BIN_DIR}")
        endif()

    endif()

endif()

set(S2FS_WARNING_TARGETS
    s2fs_core steam2fs steam2_archive_tests virtual_tree_tests
    s2fs_catalog steam2gui catalog_tests)
foreach(target IN LISTS S2FS_WARNING_TARGETS)
    if(TARGET ${target})
        if(MSVC)
            target_compile_options(${target} PRIVATE /W4 /permissive- /EHsc)
        else()
            target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
        endif()
    endif()
endforeach()
