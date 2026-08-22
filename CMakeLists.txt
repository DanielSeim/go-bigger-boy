cmake_minimum_required(VERSION 3.20)

project(go_bigger_boy VERSION 0.1.0 LANGUAGES CXX)

include(GNUInstallDirs)

option(GAMEBOY_BUILD_TESTS "Build the unit tests" ON)
option(GAMEBOY_BUILD_SDL "Build the SDL3 desktop frontend when SDL3 is available" ON)
set(GAMEBOY_TEST_ROM_DIR "" CACHE PATH
    "Optional directory of conformance .gb ROMs to register with CTest")

add_library(gameboy_core
    src/cartridge.cpp
    src/cpu.cpp
    src/emulator.cpp
    src/joypad.cpp
    src/memory_bus.cpp
    src/ppu.cpp
    src/timer.cpp
)

target_include_directories(gameboy_core PUBLIC include)
target_compile_features(gameboy_core PUBLIC cxx_std_17)

if(MSVC)
    target_compile_options(gameboy_core PRIVATE /W4 /permissive-)
else()
    target_compile_options(gameboy_core PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_executable(gameboy_cli apps/cli/main.cpp)
target_link_libraries(gameboy_cli PRIVATE gameboy_core)
set_target_properties(gameboy_cli PROPERTIES OUTPUT_NAME gbb_cli)

add_executable(gameboy_test_runner apps/test_runner/main.cpp)
target_link_libraries(gameboy_test_runner PRIVATE gameboy_core)
set_target_properties(gameboy_test_runner PROPERTIES OUTPUT_NAME gbb_test_runner)

if(GAMEBOY_BUILD_SDL)
    find_package(SDL3 3.2 QUIET CONFIG)
    if(SDL3_FOUND)
        if(WIN32)
            add_executable(gameboy_sdl WIN32 apps/sdl/main.cpp)
        else()
            add_executable(gameboy_sdl apps/sdl/main.cpp)
        endif()
        target_link_libraries(gameboy_sdl PRIVATE gameboy_core SDL3::SDL3)
        set_target_properties(gameboy_sdl PROPERTIES OUTPUT_NAME gbb)
        if(WIN32)
            install(TARGETS gameboy_sdl RUNTIME DESTINATION .)
        elseif(APPLE)
            set_target_properties(gameboy_sdl PROPERTIES
                                  INSTALL_RPATH "@loader_path")
            install(TARGETS gameboy_sdl RUNTIME DESTINATION .)
        else()
            set_target_properties(gameboy_sdl PROPERTIES
                                  INSTALL_RPATH "$ORIGIN/../lib")
            install(TARGETS gameboy_sdl
                    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
        endif()

        if(WIN32 AND TARGET SDL3::SDL3-shared)
            add_custom_command(TARGET gameboy_sdl POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                        $<TARGET_FILE:SDL3::SDL3-shared>
                        $<TARGET_FILE_DIR:gameboy_sdl>
                VERBATIM)
            install(FILES $<TARGET_FILE:SDL3::SDL3-shared> DESTINATION .)
        elseif(APPLE AND TARGET SDL3::SDL3-shared)
            install(FILES $<TARGET_FILE:SDL3::SDL3-shared> DESTINATION .)
        elseif(UNIX AND TARGET SDL3::SDL3-shared)
            install(FILES
                    $<TARGET_FILE:SDL3::SDL3-shared>
                    $<TARGET_SONAME_FILE:SDL3::SDL3-shared>
                    DESTINATION ${CMAKE_INSTALL_LIBDIR})
        endif()
        if(UNIX AND NOT APPLE)
            install(FILES packaging/linux/go-bigger-boy.desktop
                    DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
            install(FILES packaging/linux/go-bigger-boy.svg
                    DESTINATION
                        ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps)
        endif()
        install(FILES README.md DESTINATION .)
    else()
        message(STATUS "SDL3 not found; skipping the GBB desktop frontend")
    endif()
endif()

if(GAMEBOY_BUILD_TESTS)
    enable_testing()
    add_executable(gameboy_tests tests/core_tests.cpp)
    target_link_libraries(gameboy_tests PRIVATE gameboy_core)
    add_test(NAME gameboy_core_tests COMMAND gameboy_tests)

    if(GAMEBOY_TEST_ROM_DIR)
        file(GLOB_RECURSE GAMEBOY_CONFORMANCE_ROMS CONFIGURE_DEPENDS
             "${GAMEBOY_TEST_ROM_DIR}/*.gb")
        foreach(ROM IN LISTS GAMEBOY_CONFORMANCE_ROMS)
            file(RELATIVE_PATH ROM_NAME "${GAMEBOY_TEST_ROM_DIR}" "${ROM}")
            string(MAKE_C_IDENTIFIER "${ROM_NAME}" TEST_NAME)
            add_test(NAME "conformance_${TEST_NAME}"
                     COMMAND gameboy_test_runner "${ROM}"
                             --max-cycles 100000000)
            set_tests_properties("conformance_${TEST_NAME}"
                                 PROPERTIES LABELS conformance TIMEOUT 30)
        endforeach()
    endif()
endif()
