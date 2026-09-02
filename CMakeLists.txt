cmake_minimum_required(VERSION 3.20)

project(go_bigger_boy VERSION 0.26.0 LANGUAGES CXX)

configure_file(settings.ini settings.ini COPYONLY)

include(GNUInstallDirs)

option(GAMEBOY_BUILD_TESTS "Build the unit tests" ON)
option(GAMEBOY_BUILD_SDL "Build the SDL3 desktop frontend when SDL3 is available" ON)
set(GAMEBOY_TEST_ROM_DIR "" CACHE PATH
    "Optional root of the c-sp Game Boy test ROM bundle")
set(GAMEBOY_SAMESUITE_DIR "" CACHE PATH
    "Optional checkout of LIJI32/SameSuite for experimental APU tests")

add_library(gameboy_core
    src/apu.cpp
    src/cartridge.cpp
    src/cpu.cpp
    src/dmg_palette.cpp
    src/emulator.cpp
    src/gameshark.cpp
    src/joypad.cpp
    src/link_session.cpp
    src/link_transport.cpp
    src/memory_bus.cpp
    src/ppu.cpp
    src/printer.cpp
    src/save_state.cpp
    src/serial.cpp
    src/timer.cpp
    src/tcp_link_channel.cpp
    src/tcp_serial_endpoint.cpp
)

# Stable, system-neutral boundary consumed by frontends. The current GB/GBC
# implementation is adapted behind this target; future cores can register
# without introducing their hardware types into SDL, Android, or Web code.
add_library(gbb_core_api
    src/audio.cpp
    src/core_registry.cpp
    src/gameboy_scene.cpp
    src/rom_library.cpp
    src/scene_json.cpp
    src/voxel_profile.cpp
)
target_include_directories(gbb_core_api PUBLIC include)
target_compile_features(gbb_core_api PUBLIC cxx_std_17)
target_link_libraries(gbb_core_api PUBLIC gameboy_core)
add_library(gb_core ALIAS gameboy_core)

if(MSVC)
    target_compile_options(gbb_core_api PRIVATE /W4 /permissive-)
else()
    target_compile_options(gbb_core_api PRIVATE -Wall -Wextra -Wpedantic)
endif()

target_include_directories(gameboy_core PUBLIC include)
target_compile_features(gameboy_core PUBLIC cxx_std_17)

if(WIN32)
    target_link_libraries(gameboy_core PUBLIC ws2_32)
endif()

if(EMSCRIPTEN)
    target_compile_options(gameboy_core PRIVATE -fexceptions)
endif()

if(MSVC)
    target_compile_options(gameboy_core PRIVATE /W4 /permissive-)
else()
    target_compile_options(gameboy_core PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(NOT ANDROID)
    add_executable(gameboy_cli apps/cli/main.cpp)
    target_link_libraries(gameboy_cli PRIVATE gbb_core_api)
    set_target_properties(gameboy_cli PROPERTIES OUTPUT_NAME gbb_cli)

    add_executable(gameboy_test_runner apps/test_runner/main.cpp)
    target_link_libraries(gameboy_test_runner PRIVATE gbb_core_api)
    set_target_properties(gameboy_test_runner PROPERTIES OUTPUT_NAME gbb_test_runner)

    add_executable(gameboy_link_harness apps/link_harness/main.cpp)
    target_link_libraries(gameboy_link_harness PRIVATE gameboy_core)
    set_target_properties(gameboy_link_harness PROPERTIES OUTPUT_NAME gbb_link_harness)
endif()

if(GAMEBOY_BUILD_SDL)
    if(EMSCRIPTEN)
        find_package(SDL3 3.2 REQUIRED CONFIG)
    else()
        find_package(SDL3 3.2 QUIET CONFIG)
    endif()
    if(SDL3_FOUND)
        find_package(SDL3_ttf 3.2 CONFIG QUIET)
        if(EMSCRIPTEN)
            add_executable(gameboy_web apps/web/main.cpp)
            target_link_libraries(gameboy_web PRIVATE gbb_core_api SDL3::SDL3)
            target_compile_options(gameboy_web PRIVATE -fexceptions)
            target_link_options(gameboy_web PRIVATE
                -fexceptions
                --bind
                "--shell-file=${CMAKE_CURRENT_SOURCE_DIR}/web/shell.html"
                "-sALLOW_MEMORY_GROWTH=1"
                "-sMAXIMUM_MEMORY=536870912"
                "-sENVIRONMENT=web"
                "-sEXPORTED_FUNCTIONS=['_main','_gbb_resume_audio','_gbb_start_rom','_gbb_pause_rom','_gbb_set_palette','_gbb_set_video_mode','_gbb_set_voxel_camera','_gbb_reset_voxel_camera','_gbb_export_scene_snapshot']"
            )
            set_target_properties(gameboy_web PROPERTIES
                OUTPUT_NAME index
                SUFFIX ".html"
                RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/web"
            )
            add_custom_command(TARGET gameboy_web POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                        "${CMAKE_CURRENT_SOURCE_DIR}/web/go_bigger_boy_icon.png"
                        "${CMAKE_BINARY_DIR}/web/go_bigger_boy_icon.png"
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                        "${CMAKE_CURRENT_SOURCE_DIR}/web/go_bigger_boy_logo.png"
                        "${CMAKE_BINARY_DIR}/web/go_bigger_boy_logo.png"
                VERBATIM
            )
        elseif(ANDROID)
            add_library(gameboy_sdl SHARED apps/sdl/main.cpp)
            set_target_properties(gameboy_core PROPERTIES
                                  POSITION_INDEPENDENT_CODE ON)
        elseif(WIN32)
            enable_language(RC)
            add_executable(gameboy_sdl WIN32
                apps/sdl/main.cpp
                apps/sdl/windows_dashboard.cpp
                apps/sdl/update_checker.cpp
                packaging/windows/go-bigger-boy.rc
            )
            add_executable(gbb_updater WIN32 apps/sdl/windows_updater.cpp)
            set_target_properties(gbb_updater PROPERTIES
                OUTPUT_NAME "gbb-updater"
                CXX_STANDARD 17
                CXX_STANDARD_REQUIRED ON
                CXX_EXTENSIONS OFF
            )
            target_compile_features(gbb_updater PRIVATE cxx_std_17)
            target_link_libraries(gbb_updater PRIVATE bcrypt shell32)
        elseif(APPLE)
            add_executable(gameboy_sdl MACOSX_BUNDLE
                apps/sdl/main.cpp
                apps/sdl/update_checker.cpp
                packaging/macos/go-bigger-boy.icns
            )
            set_source_files_properties(
                packaging/macos/go-bigger-boy.icns
                PROPERTIES MACOSX_PACKAGE_LOCATION Resources
            )
        else()
            add_executable(gameboy_sdl
                apps/sdl/main.cpp
                apps/sdl/update_checker.cpp
            )
        endif()
        if(NOT EMSCRIPTEN)
            set(GAMEBOY_DESKTOP_VERSION "${PROJECT_VERSION}")
            if(DEFINED ENV{GITHUB_REF_NAME} AND
               "$ENV{GITHUB_REF_NAME}" MATCHES "^v([0-9]+\\.[0-9]+\\.[0-9]+)$")
                set(GAMEBOY_DESKTOP_VERSION "${CMAKE_MATCH_1}")
            endif()
            target_link_libraries(gameboy_sdl PRIVATE gbb_core_api SDL3::SDL3)
            if(SDL3_ttf_FOUND AND TARGET SDL3_ttf::SDL3_ttf)
                target_link_libraries(gameboy_sdl PRIVATE SDL3_ttf::SDL3_ttf)
                target_compile_definitions(gameboy_sdl PRIVATE GBB_HAS_SDL_TTF=1)
            endif()
            target_compile_definitions(gameboy_sdl PRIVATE
                GBB_VERSION="${GAMEBOY_DESKTOP_VERSION}"
            )
            if(WIN32)
                target_link_libraries(gameboy_sdl PRIVATE
                    winhttp bcrypt comctl32 comdlg32 ole32 windowscodecs)
                target_include_directories(gameboy_sdl PRIVATE
                    "${CMAKE_CURRENT_SOURCE_DIR}/packaging/windows"
                )
                if(MSVC)
                    # The resource script embeds our DPI/common-controls
                    # manifest; prevent MSVC from adding a second copy.
                    target_link_options(gameboy_sdl PRIVATE "/MANIFEST:NO")
                endif()
            endif()
            if(ANDROID)
                set_target_properties(gameboy_sdl PROPERTIES OUTPUT_NAME main)
            elseif(APPLE)
                set_target_properties(gameboy_sdl PROPERTIES
                    OUTPUT_NAME "Go Bigger Boy"
                    MACOSX_BUNDLE_BUNDLE_NAME "Go Bigger Boy"
                    MACOSX_BUNDLE_GUI_IDENTIFIER
                        "com.danielseim.go-bigger-boy"
                    MACOSX_BUNDLE_ICON_FILE "go-bigger-boy.icns"
                    MACOSX_BUNDLE_SHORT_VERSION_STRING
                        "${GAMEBOY_DESKTOP_VERSION}"
                    MACOSX_BUNDLE_BUNDLE_VERSION
                        "${GAMEBOY_DESKTOP_VERSION}"
                )
            else()
                set_target_properties(gameboy_sdl PROPERTIES OUTPUT_NAME gbb)
            endif()
            if(NOT ANDROID)
                if(WIN32)
                    install(TARGETS gameboy_sdl RUNTIME DESTINATION .)
                    install(TARGETS gbb_updater RUNTIME DESTINATION .)
                elseif(APPLE)
                    set_target_properties(gameboy_sdl PROPERTIES
                        INSTALL_RPATH "@executable_path/../Frameworks"
                    )
                    install(TARGETS gameboy_sdl BUNDLE DESTINATION .)
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
                    install(FILES $<TARGET_FILE:SDL3::SDL3-shared>
                            DESTINATION
                                "Go Bigger Boy.app/Contents/Frameworks")
                elseif(UNIX AND TARGET SDL3::SDL3-shared)
                    install(FILES
                            $<TARGET_FILE:SDL3::SDL3-shared>
                            $<TARGET_SONAME_FILE:SDL3::SDL3-shared>
                            DESTINATION ${CMAKE_INSTALL_LIBDIR})
                endif()
                if(UNIX AND NOT APPLE)
                    install(FILES packaging/linux/go-bigger-boy.desktop
                            DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
                    install(FILES packaging/linux/go-bigger-boy.png
                            DESTINATION
                                ${CMAKE_INSTALL_DATADIR}/icons/hicolor/512x512/apps
                            RENAME go-bigger-boy.png)
                endif()
                install(FILES settings.ini DESTINATION .)
                install(FILES README.md DESTINATION .)
            endif()
        endif()
    else()
        message(STATUS "SDL3 not found; skipping the GBB SDL frontend")
    endif()
endif()

if(GAMEBOY_BUILD_TESTS)
    enable_testing()
    add_executable(gameboy_tests tests/core_tests.cpp)
    target_link_libraries(gameboy_tests PRIVATE gbb_core_api)
    add_custom_command(TARGET gameboy_tests POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory
                "$<TARGET_FILE_DIR:gameboy_tests>/audio-fixtures"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/audio/dmg-pulse.txt"
                "$<TARGET_FILE_DIR:gameboy_tests>/audio-fixtures/dmg-pulse.txt"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/audio/dmg-wave.txt"
                "$<TARGET_FILE_DIR:gameboy_tests>/audio-fixtures/dmg-wave.txt"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/audio/dmg-noise.txt"
                "$<TARGET_FILE_DIR:gameboy_tests>/audio-fixtures/dmg-noise.txt"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/audio/cgb-pulse.txt"
                "$<TARGET_FILE_DIR:gameboy_tests>/audio-fixtures/cgb-pulse.txt"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/audio/cgb-wave.txt"
                "$<TARGET_FILE_DIR:gameboy_tests>/audio-fixtures/cgb-wave.txt"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/audio/cgb-noise.txt"
                "$<TARGET_FILE_DIR:gameboy_tests>/audio-fixtures/cgb-noise.txt"
        VERBATIM)
    add_test(NAME gameboy_core_tests COMMAND gameboy_tests)

    # The version path intentionally runs before SDL video initialization, so
    # it is safe in headless CI and catches broken desktop entry-point builds.
    if(TARGET gameboy_sdl)
        add_test(NAME gameboy_sdl_version COMMAND gameboy_sdl --version)
    endif()

    if(GAMEBOY_TEST_ROM_DIR OR GAMEBOY_SAMESUITE_DIR)
        include(cmake/ConformanceTests.cmake)
    endif()
endif()
