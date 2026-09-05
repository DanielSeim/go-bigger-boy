cmake_minimum_required(VERSION 3.20)

file(STRINGS "${CMAKE_CURRENT_LIST_DIR}/VERSION" GBB_PROJECT_VERSION
     LIMIT_COUNT 1)
if(NOT GBB_PROJECT_VERSION MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+$")
    message(FATAL_ERROR "VERSION must contain a semantic version (x.y.z)")
endif()
project(go_bigger_boy VERSION ${GBB_PROJECT_VERSION} LANGUAGES CXX)

# Every target, including focused SDL tests and optional fuzzers that do not
# link a library carrying usage requirements, must use the same language
# standard on all supported toolchains. GCC's default currently happens to be
# newer than C++17; AppleClang's is not.
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

configure_file(settings.ini settings.ini COPYONLY)

include(GNUInstallDirs)

option(GAMEBOY_BUILD_TESTS "Build the unit tests" ON)
option(GAMEBOY_BUILD_SDL "Build the SDL3 desktop frontend when SDL3 is available" ON)
option(GAMEBOY_ENABLE_SANITIZERS
       "Enable AddressSanitizer and UndefinedBehaviorSanitizer for native builds"
       OFF)
option(GAMEBOY_ENABLE_THREAD_SANITIZER
       "Enable ThreadSanitizer for native GCC/Clang builds"
       OFF)
option(GAMEBOY_BUILD_FUZZERS
       "Build opt-in libFuzzer parser and protocol targets"
       OFF)
set(GAMEBOY_TEST_ROM_DIR "" CACHE PATH
    "Optional root of the c-sp Game Boy test ROM bundle")
set(GAMEBOY_SAMESUITE_DIR "" CACHE PATH
    "Optional checkout of LIJI32/SameSuite for experimental APU tests")

# Keep sanitizer instrumentation opt-in: it is invaluable for CI/debugging,
# but it changes runtime and memory behavior enough that release builds should
# remain untouched. MSVC currently provides AddressSanitizer only; GCC/Clang
# can run both address and undefined-behavior checks.
if(GAMEBOY_ENABLE_SANITIZERS AND NOT EMSCRIPTEN AND NOT ANDROID)
    if(MSVC)
        add_compile_options(/fsanitize=address)
        add_link_options(/fsanitize=address)
    else()
        add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
        add_link_options(-fsanitize=address,undefined)
    endif()
endif()

if(GAMEBOY_ENABLE_THREAD_SANITIZER AND GAMEBOY_ENABLE_SANITIZERS)
    message(FATAL_ERROR
            "GAMEBOY_ENABLE_THREAD_SANITIZER cannot be combined with "
            "GAMEBOY_ENABLE_SANITIZERS")
endif()

if(GAMEBOY_BUILD_FUZZERS AND GAMEBOY_ENABLE_THREAD_SANITIZER)
    message(FATAL_ERROR
            "GAMEBOY_BUILD_FUZZERS cannot be combined with ThreadSanitizer")
endif()

if(GAMEBOY_ENABLE_THREAD_SANITIZER AND (EMSCRIPTEN OR ANDROID))
    message(FATAL_ERROR
            "ThreadSanitizer is only supported for native desktop builds")
endif()

if(GAMEBOY_ENABLE_THREAD_SANITIZER)
    if(MSVC)
        message(FATAL_ERROR
                "ThreadSanitizer is not supported by the MSVC toolchain")
    elseif(NOT CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang|AppleClang")
        message(FATAL_ERROR
                "ThreadSanitizer requires a GCC or Clang-compatible compiler")
    else()
        add_compile_options(-fsanitize=thread -fno-omit-frame-pointer)
        add_link_options(-fsanitize=thread)
    endif()
endif()

# Fuzzing needs sanitizer instrumentation in the libraries under test, not
# only in the libFuzzer entry point. Reuse the explicit sanitizer option when
# supplied; otherwise make fuzzing self-contained for native Clang builds.
if(GAMEBOY_BUILD_FUZZERS AND NOT GAMEBOY_ENABLE_SANITIZERS AND
   NOT EMSCRIPTEN AND NOT ANDROID)
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address,undefined)
endif()

add_library(gameboy_core
    src/apu.cpp
    src/cartridge.cpp
    src/cartridge_persistence.cpp
    src/cpu.cpp
    src/dmg_palette.cpp
    src/emulator.cpp
    src/gameboy_link_endpoint.cpp
    src/gameshark.cpp
    src/joypad.cpp
    src/link_packet_codec.cpp
    src/link_session.cpp
    src/link_scheduler.cpp
    src/link_transport.cpp
    src/log.cpp
    src/memory_bus.cpp
    src/ppu.cpp
    src/ppu_timing.cpp
    src/ppu_sgb.cpp
    src/printer.cpp
    src/save_state.cpp
    src/save_state_apu.cpp
    src/save_state_bus.cpp
    src/save_state_bus_read.cpp
    src/save_state_cartridge.cpp
    src/save_state_cpu.cpp
    src/save_state_joypad.cpp
    src/save_state_ppu.cpp
    src/save_state_timer.cpp
    src/serial.cpp
    src/save_state_container.cpp
    src/timer.cpp
    src/tcp_link_channel.cpp
    src/tcp_serial_endpoint.cpp
)

# Stable, system-neutral boundary consumed by frontends. The current GB/GBC
# implementation is adapted behind this target; future cores can register
# without introducing their hardware types into SDL, Android, or Web code.
add_library(gbb_core_api
    src/audio.cpp
    src/core_contract.cpp
    src/core_contributors.cpp
    src/core_registry.cpp
    src/gameboy_core_factory.cpp
    src/core_runtime.cpp
    src/plugin_loader.cpp
    src/plugin_discovery.cpp
    src/plugin_trust.cpp
    src/lan_discovery.cpp
    src/gameboy_scene.cpp
    src/video.cpp
    src/rom_library.cpp
    src/scene_json.cpp
    src/settings.cpp
    src/trace_parser.cpp
    src/voxel_profile.cpp
)
target_include_directories(gbb_core_api PUBLIC include)
target_compile_features(gbb_core_api PUBLIC cxx_std_17)
target_link_libraries(gbb_core_api PUBLIC gameboy_core)
target_link_libraries(gbb_core_api PUBLIC ${CMAKE_DL_LIBS})
add_library(gb_core ALIAS gameboy_core)

# The dynamic-core boundary is intentionally independent of the C++ library;
# install its one public header so external fixture plug-ins can compile
# against the same declarations as the in-tree loader tests.
install(FILES include/gbb/plugin_abi.h
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/gbb)

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

    add_executable(gameboy_trace_report apps/cli/trace_report.cpp)
    target_link_libraries(gameboy_trace_report PRIVATE gbb_core_api)
    set_target_properties(gameboy_trace_report PROPERTIES OUTPUT_NAME gbb_trace_report)
    if(WIN32)
        install(TARGETS gameboy_trace_report RUNTIME DESTINATION .)
    else()
        install(TARGETS gameboy_trace_report RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
    endif()

    add_executable(gameboy_test_runner apps/test_runner/main.cpp)
    target_link_libraries(gameboy_test_runner PRIVATE gbb_core_api)
    set_target_properties(gameboy_test_runner PROPERTIES OUTPUT_NAME gbb_test_runner)

    add_executable(gameboy_link_harness
                apps/link_harness/main.cpp
        apps/link_harness/harness_io.cpp
        apps/link_harness/options.cpp
        apps/link_harness/pokemon_automation.cpp
        apps/link_harness/pokemon_state.cpp
        apps/link_harness/semantic_tracker.cpp
        apps/link_harness/scenario_runner.cpp
        apps/link_harness/scenario_state.cpp
        apps/link_harness/scenario_trace.cpp
        apps/link_harness/scenario_trace_writer.cpp
    )
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
            target_compile_definitions(gameboy_web PRIVATE
                GBB_VERSION="${PROJECT_VERSION}"
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
            add_library(gameboy_sdl SHARED
                apps/sdl/main.cpp
                apps/sdl/presentation.cpp
                apps/sdl/frame_pacer.cpp
                apps/sdl/emulation_session.cpp
                apps/sdl/event_dispatch.cpp
                apps/sdl/event_policy.cpp
                apps/sdl/event_lifecycle.cpp
                apps/sdl/android_bridge.cpp
                apps/sdl/sdl_resources.cpp
                apps/sdl/desktop_storage.cpp
                apps/sdl/window_event.cpp
                apps/sdl/tool_window_support.cpp
                apps/sdl/input_mapping.cpp
                apps/sdl/android_touch_input.cpp
                apps/sdl/input_configuration.cpp
                apps/sdl/input_lifecycle.cpp
                apps/sdl/camera_capture.cpp
                apps/sdl/audio_output.cpp
                apps/sdl/frame_presenter.cpp
                apps/sdl/voxel_renderer.cpp
                apps/sdl/input_movie.cpp
                apps/sdl/link_trace_file.cpp
                apps/sdl/pokemon_link_diagnostics.cpp
                apps/sdl/settings_persistence.cpp
                apps/sdl/dialogs.cpp
                apps/sdl/dashboard_controller.cpp
            )
            set_target_properties(gameboy_core PROPERTIES
                                  POSITION_INDEPENDENT_CODE ON)
        elseif(WIN32)
            enable_language(RC)
            add_executable(gameboy_sdl WIN32
                apps/sdl/main.cpp
                apps/sdl/advanced_tools.cpp
                apps/sdl/link_controller.cpp
                apps/sdl/presentation.cpp
                apps/sdl/frame_pacer.cpp
                apps/sdl/emulation_session.cpp
                apps/sdl/event_dispatch.cpp
                apps/sdl/event_policy.cpp
                apps/sdl/event_lifecycle.cpp
                apps/sdl/sdl_resources.cpp
                apps/sdl/desktop_storage.cpp
                apps/sdl/window_event.cpp
                apps/sdl/tool_window_support.cpp
                apps/sdl/input_mapping.cpp
                apps/sdl/android_touch_input.cpp
                apps/sdl/input_configuration.cpp
                apps/sdl/input_lifecycle.cpp
                apps/sdl/camera_capture.cpp
                apps/sdl/audio_output.cpp
                apps/sdl/frame_presenter.cpp
                apps/sdl/voxel_renderer.cpp
                apps/sdl/input_movie.cpp
                apps/sdl/link_trace_file.cpp
                apps/sdl/pokemon_link_diagnostics.cpp
                apps/sdl/settings_persistence.cpp
                apps/sdl/dialogs.cpp
                apps/sdl/dashboard_controller.cpp
                apps/sdl/windows_dashboard.cpp
                apps/sdl/windows_menu_bar.cpp
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
                apps/sdl/advanced_tools.cpp
                apps/sdl/link_controller.cpp
                apps/sdl/presentation.cpp
                apps/sdl/frame_pacer.cpp
                apps/sdl/emulation_session.cpp
                apps/sdl/event_dispatch.cpp
                apps/sdl/event_policy.cpp
                apps/sdl/event_lifecycle.cpp
                apps/sdl/sdl_resources.cpp
                apps/sdl/desktop_storage.cpp
                apps/sdl/window_event.cpp
                apps/sdl/tool_window_support.cpp
                apps/sdl/input_mapping.cpp
                apps/sdl/android_touch_input.cpp
                apps/sdl/input_configuration.cpp
                apps/sdl/input_lifecycle.cpp
                apps/sdl/camera_capture.cpp
                apps/sdl/audio_output.cpp
                apps/sdl/frame_presenter.cpp
                apps/sdl/voxel_renderer.cpp
                apps/sdl/input_movie.cpp
                apps/sdl/link_trace_file.cpp
                apps/sdl/pokemon_link_diagnostics.cpp
                apps/sdl/settings_persistence.cpp
                apps/sdl/dialogs.cpp
                apps/sdl/dashboard_controller.cpp
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
                apps/sdl/advanced_tools.cpp
                apps/sdl/link_controller.cpp
                apps/sdl/presentation.cpp
                apps/sdl/frame_pacer.cpp
                apps/sdl/emulation_session.cpp
                apps/sdl/event_dispatch.cpp
                apps/sdl/event_policy.cpp
                apps/sdl/event_lifecycle.cpp
                apps/sdl/sdl_resources.cpp
                apps/sdl/desktop_storage.cpp
                apps/sdl/window_event.cpp
                apps/sdl/tool_window_support.cpp
                apps/sdl/input_mapping.cpp
                apps/sdl/android_touch_input.cpp
                apps/sdl/input_configuration.cpp
                apps/sdl/input_lifecycle.cpp
                apps/sdl/camera_capture.cpp
                apps/sdl/audio_output.cpp
                apps/sdl/frame_presenter.cpp
                apps/sdl/voxel_renderer.cpp
                apps/sdl/input_movie.cpp
                apps/sdl/link_trace_file.cpp
                apps/sdl/pokemon_link_diagnostics.cpp
                apps/sdl/settings_persistence.cpp
                apps/sdl/dialogs.cpp
                apps/sdl/dashboard_controller.cpp
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

    add_executable(gameboy_frontend_contract_tests
        tests/frontend_contract_tests.cpp)
    target_link_libraries(gameboy_frontend_contract_tests PRIVATE gbb_core_api)
    add_test(NAME gameboy_frontend_contract
             COMMAND gameboy_frontend_contract_tests)

    add_executable(gameboy_core_contract_tests
        tests/core_contract_tests.cpp)
    target_link_libraries(gameboy_core_contract_tests PRIVATE gbb_core_api)
    add_test(NAME gameboy_core_contract
             COMMAND gameboy_core_contract_tests)

    if(NOT EMSCRIPTEN AND NOT ANDROID)
        enable_language(C)

        add_executable(gameboy_plugin_abi_header_c_tests
            tests/plugin_abi_header_c_tests.c)
        set_target_properties(gameboy_plugin_abi_header_c_tests PROPERTIES
            C_STANDARD 11
            C_STANDARD_REQUIRED ON
            C_EXTENSIONS OFF)
        target_include_directories(gameboy_plugin_abi_header_c_tests
            PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
        add_test(NAME gameboy_plugin_abi_header_c
                 COMMAND gameboy_plugin_abi_header_c_tests)

        function(add_plugin_fixture target mode)
            add_library(${target} SHARED
                tests/plugin_fixture/plugin.cpp)
            target_compile_definitions(${target}
                PRIVATE GBB_PLUGIN_FIXTURE_MODE=${mode})
            target_include_directories(${target}
                PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
        endfunction()

        add_plugin_fixture(gbb_plugin_fixture 0)
        add_plugin_fixture(gbb_plugin_fixture_bad_capability 1)
        add_plugin_fixture(gbb_plugin_fixture_missing_callback 2)
        add_plugin_fixture(gbb_plugin_fixture_truncated_table 3)
        add_plugin_fixture(gbb_plugin_fixture_bad_frame 4)
        add_plugin_fixture(gbb_plugin_fixture_bad_blob 5)
        add_plugin_fixture(gbb_plugin_fixture_create_failure 6)
        add_plugin_fixture(gbb_plugin_fixture_concurrent 7)
        add_plugin_fixture(gbb_plugin_fixture_bad_utf8 9)
        add_plugin_fixture(gbb_plugin_fixture_unterminated_string 10)
        add_plugin_fixture(gbb_plugin_fixture_exception 11)
        add_plugin_fixture(gbb_plugin_fixture_allocator_alignment 12)
        add_plugin_fixture(gbb_plugin_fixture_capability 13)
        add_plugin_fixture(gbb_plugin_fixture_oversized_tables 8)
        add_library(gbb_plugin_fixture_no_query SHARED
            tests/plugin_fixture/no_query.cpp)

        add_executable(gameboy_plugin_abi_tests
            tests/plugin_abi_tests.cpp)
        target_include_directories(gameboy_plugin_abi_tests
            PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
        target_link_libraries(gameboy_plugin_abi_tests PRIVATE ${CMAKE_DL_LIBS})
        add_test(NAME gameboy_plugin_abi
                 COMMAND gameboy_plugin_abi_tests
                         $<TARGET_FILE:gbb_plugin_fixture>)

        add_executable(gameboy_lan_discovery_tests
            tests/lan_discovery_tests.cpp)
        target_link_libraries(gameboy_lan_discovery_tests PRIVATE gbb_core_api)
        add_test(NAME gameboy_lan_discovery
                 COMMAND gameboy_lan_discovery_tests)
        set_tests_properties(gameboy_lan_discovery PROPERTIES
                             SKIP_RETURN_CODE 77)

        add_executable(gameboy_plugin_loader_tests
            tests/plugin_loader_tests.cpp)
        target_link_libraries(gameboy_plugin_loader_tests PRIVATE gbb_core_api)
        add_test(NAME gameboy_plugin_loader
                 COMMAND gameboy_plugin_loader_tests
                         $<TARGET_FILE:gbb_plugin_fixture>
                         $<TARGET_FILE:gbb_plugin_fixture_oversized_tables>)

        add_executable(gameboy_plugin_discovery_tests
            tests/plugin_discovery_tests.cpp)
        target_link_libraries(gameboy_plugin_discovery_tests PRIVATE gbb_core_api)
        add_test(NAME gameboy_plugin_discovery
                 COMMAND gameboy_plugin_discovery_tests
                         $<TARGET_FILE:gbb_plugin_fixture>
                         $<TARGET_FILE:gbb_plugin_fixture_capability>)

        add_executable(gameboy_plugin_loader_rejection_tests
            tests/plugin_loader_rejection_tests.cpp)
        target_link_libraries(gameboy_plugin_loader_rejection_tests
            PRIVATE gbb_core_api)
        add_test(NAME gameboy_plugin_loader_rejections
                 COMMAND gameboy_plugin_loader_rejection_tests
                         $<TARGET_FILE:gbb_plugin_fixture_bad_capability>
                         $<TARGET_FILE:gbb_plugin_fixture_missing_callback>
                         $<TARGET_FILE:gbb_plugin_fixture_truncated_table>
                         $<TARGET_FILE:gbb_plugin_fixture_no_query>
                         $<TARGET_FILE:gbb_plugin_fixture_bad_utf8>
                         $<TARGET_FILE:gbb_plugin_fixture_unterminated_string>)

        add_executable(gameboy_plugin_loader_runtime_tests
            tests/plugin_loader_runtime_tests.cpp)
        target_link_libraries(gameboy_plugin_loader_runtime_tests
            PRIVATE gbb_core_api)
        add_test(NAME gameboy_plugin_loader_runtime_validation
                 COMMAND gameboy_plugin_loader_runtime_tests
                         $<TARGET_FILE:gbb_plugin_fixture_bad_frame>
                         $<TARGET_FILE:gbb_plugin_fixture_bad_blob>
                         $<TARGET_FILE:gbb_plugin_fixture_create_failure>
                         $<TARGET_FILE:gbb_plugin_fixture_concurrent>
                         $<TARGET_FILE:gbb_plugin_fixture_exception>
                         $<TARGET_FILE:gbb_plugin_fixture_allocator_alignment>)
    endif()

    add_executable(gameboy_sdl_core_capability_tests
        tests/sdl_core_capability_tests.cpp)
    target_include_directories(gameboy_sdl_core_capability_tests
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/apps/sdl)
    target_link_libraries(gameboy_sdl_core_capability_tests PRIVATE gbb_core_api)
    add_test(NAME gameboy_sdl_core_capability
             COMMAND gameboy_sdl_core_capability_tests)

    add_executable(gameboy_rom_library_contract_tests
        tests/rom_library_contract_tests.cpp)
    target_link_libraries(gameboy_rom_library_contract_tests PRIVATE gbb_core_api)
    add_test(NAME gameboy_rom_library_contract
             COMMAND gameboy_rom_library_contract_tests)

    add_executable(gameboy_gameshark_contract_tests
        tests/gameshark_contract_tests.cpp)
    target_link_libraries(gameboy_gameshark_contract_tests PRIVATE gameboy_core)
    add_test(NAME gameboy_gameshark_contract
             COMMAND gameboy_gameshark_contract_tests)

    add_executable(gameboy_apu_cycle_contract_tests
        tests/apu_cycle_contract_tests.cpp)
    target_link_libraries(gameboy_apu_cycle_contract_tests PRIVATE gameboy_core)
    add_test(NAME gameboy_apu_cycle_contract
             COMMAND gameboy_apu_cycle_contract_tests)

    add_executable(gameboy_apu_waveform_contract_tests
        tests/apu_waveform_contract_tests.cpp)
    target_link_libraries(gameboy_apu_waveform_contract_tests PRIVATE gameboy_core)
    add_custom_command(TARGET gameboy_apu_waveform_contract_tests POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory
                "$<TARGET_FILE_DIR:gameboy_apu_waveform_contract_tests>/audio-fixtures"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/audio/dmg-pulse.txt"
                "$<TARGET_FILE_DIR:gameboy_apu_waveform_contract_tests>/audio-fixtures/dmg-pulse.txt"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/audio/dmg-wave.txt"
                "$<TARGET_FILE_DIR:gameboy_apu_waveform_contract_tests>/audio-fixtures/dmg-wave.txt"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/audio/dmg-noise.txt"
                "$<TARGET_FILE_DIR:gameboy_apu_waveform_contract_tests>/audio-fixtures/dmg-noise.txt"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/audio/cgb-pulse.txt"
                "$<TARGET_FILE_DIR:gameboy_apu_waveform_contract_tests>/audio-fixtures/cgb-pulse.txt"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/audio/cgb-wave.txt"
                "$<TARGET_FILE_DIR:gameboy_apu_waveform_contract_tests>/audio-fixtures/cgb-wave.txt"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/audio/cgb-noise.txt"
                "$<TARGET_FILE_DIR:gameboy_apu_waveform_contract_tests>/audio-fixtures/cgb-noise.txt"
        VERBATIM)
    add_test(NAME gameboy_apu_waveform_contract
             COMMAND gameboy_apu_waveform_contract_tests)

    add_executable(gameboy_apu_hardware_contract_tests
        tests/apu_hardware_contract_tests.cpp)
    target_link_libraries(gameboy_apu_hardware_contract_tests PRIVATE gameboy_core)
    add_test(NAME gameboy_apu_hardware_contract
             COMMAND gameboy_apu_hardware_contract_tests)

    add_executable(gameboy_cartridge_file_contract_tests
        tests/cartridge_file_contract_tests.cpp)
    target_link_libraries(gameboy_cartridge_file_contract_tests PRIVATE gameboy_core)
    add_test(NAME gameboy_cartridge_file_contract
             COMMAND gameboy_cartridge_file_contract_tests)

    add_executable(gameboy_cartridge_mapper_contract_tests
        tests/cartridge_mapper_contract_tests.cpp)
    target_link_libraries(gameboy_cartridge_mapper_contract_tests PRIVATE gameboy_core)
    add_test(NAME gameboy_cartridge_mapper_contract
             COMMAND gameboy_cartridge_mapper_contract_tests)

    add_executable(gameboy_serial_link_contract_tests
        tests/serial_link_contract_tests.cpp)
    target_link_libraries(gameboy_serial_link_contract_tests PRIVATE gameboy_core)
    add_test(NAME gameboy_serial_link_contract
             COMMAND gameboy_serial_link_contract_tests)

    add_executable(gameboy_printer_contract_tests
        tests/printer_contract_tests.cpp)
    target_link_libraries(gameboy_printer_contract_tests PRIVATE gameboy_core)
    add_test(NAME gameboy_printer_contract
             COMMAND gameboy_printer_contract_tests)

    add_executable(gameboy_cpu_register_contract_tests
        tests/cpu_register_contract_tests.cpp)
    target_link_libraries(gameboy_cpu_register_contract_tests PRIVATE gameboy_core)
    add_test(NAME gameboy_cpu_register_contract
             COMMAND gameboy_cpu_register_contract_tests)

    add_executable(gameboy_cpu_alu_contract_tests
        tests/cpu_alu_contract_tests.cpp)
    target_link_libraries(gameboy_cpu_alu_contract_tests PRIVATE gameboy_core)
    add_test(NAME gameboy_cpu_alu_contract
             COMMAND gameboy_cpu_alu_contract_tests)

    add_executable(gameboy_cpu_execution_contract_tests
        tests/cpu_execution_contract_tests.cpp)
    target_link_libraries(gameboy_cpu_execution_contract_tests PRIVATE gameboy_core)
    add_test(NAME gameboy_cpu_execution_contract
             COMMAND gameboy_cpu_execution_contract_tests)

    add_executable(gameboy_timer_contract_tests
        tests/timer_contract_tests.cpp)
    target_link_libraries(gameboy_timer_contract_tests PRIVATE gameboy_core)
    add_test(NAME gameboy_timer_contract
             COMMAND gameboy_timer_contract_tests)

    add_executable(gameboy_ppu_timing_contract_tests
        tests/ppu_timing_contract_tests.cpp)
    target_link_libraries(gameboy_ppu_timing_contract_tests PRIVATE gameboy_core)
    add_test(NAME gameboy_ppu_timing_contract
             COMMAND gameboy_ppu_timing_contract_tests)

    add_executable(gameboy_settings_video_contract_tests
        tests/settings_video_contract_tests.cpp)
    target_link_libraries(gameboy_settings_video_contract_tests
        PRIVATE gbb_core_api)
    add_test(NAME gameboy_settings_video_contract
             COMMAND gameboy_settings_video_contract_tests)

    add_executable(gameboy_core_runtime_link_scheduler_tests
        tests/core_runtime_link_scheduler_tests.cpp)
    target_link_libraries(gameboy_core_runtime_link_scheduler_tests
        PRIVATE gbb_core_api)
    add_test(NAME gameboy_core_runtime_link_scheduler
             COMMAND gameboy_core_runtime_link_scheduler_tests)

    add_executable(gameboy_link_packet_codec_tests
        tests/link_packet_codec_tests.cpp)
    target_link_libraries(gameboy_link_packet_codec_tests PRIVATE gameboy_core)
    add_test(NAME gameboy_link_packet_codec
             COMMAND gameboy_link_packet_codec_tests)

    add_executable(gameboy_save_state_format_tests
        tests/save_state_format_tests.cpp)
    target_include_directories(gameboy_save_state_format_tests
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(gameboy_save_state_format_tests PRIVATE gameboy_core)
    add_test(NAME gameboy_save_state_format
             COMMAND gameboy_save_state_format_tests)

    add_executable(gameboy_save_state_contract_tests
        tests/save_state_contract_tests.cpp)
    target_link_libraries(gameboy_save_state_contract_tests PRIVATE gameboy_core)
    add_test(NAME gameboy_save_state_contract
             COMMAND gameboy_save_state_contract_tests)

    add_executable(gameboy_cgb_contract_tests
        tests/cgb_contract_tests.cpp)
    target_link_libraries(gameboy_cgb_contract_tests PRIVATE gameboy_core)
    add_test(NAME gameboy_cgb_contract
             COMMAND gameboy_cgb_contract_tests)

    add_executable(gameboy_ppu_sgb_contract_tests
        tests/ppu_sgb_contract_tests.cpp)
    target_link_libraries(gameboy_ppu_sgb_contract_tests PRIVATE gameboy_core)
    add_test(NAME gameboy_ppu_sgb_contract
             COMMAND gameboy_ppu_sgb_contract_tests)

    add_executable(gameboy_logging_contract_tests
        tests/logging_contract_tests.cpp)
    target_link_libraries(gameboy_logging_contract_tests PRIVATE gbb_core_api)
    add_test(NAME gameboy_logging_contract
             COMMAND gameboy_logging_contract_tests)

    add_executable(gameboy_trace_parser_tests
        tests/trace_parser_tests.cpp)
    target_link_libraries(gameboy_trace_parser_tests PRIVATE gbb_core_api)
    add_test(NAME gameboy_trace_parser
             COMMAND gameboy_trace_parser_tests)

    if(NOT ANDROID)
        add_executable(gameboy_input_movie_contract_tests
            tests/input_movie_contract_tests.cpp
            apps/sdl/input_movie.cpp)
        target_include_directories(gameboy_input_movie_contract_tests
            PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/apps/sdl)
        target_link_libraries(gameboy_input_movie_contract_tests PRIVATE gameboy_core)
        add_test(NAME gameboy_input_movie_contract
                 COMMAND gameboy_input_movie_contract_tests)

        add_executable(gameboy_link_harness_options_tests
            tests/link_harness_options_tests.cpp
            apps/link_harness/options.cpp
        )
        target_include_directories(gameboy_link_harness_options_tests
            PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/apps/link_harness)
        add_test(NAME gameboy_link_harness_options
                 COMMAND gameboy_link_harness_options_tests)

        add_executable(gameboy_link_harness_trace_tests
            tests/link_harness_trace_tests.cpp
            apps/link_harness/scenario_trace_writer.cpp)
        target_include_directories(gameboy_link_harness_trace_tests
            PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/apps/link_harness
                    ${CMAKE_CURRENT_SOURCE_DIR}/include)
        add_test(NAME gameboy_link_harness_trace
                 COMMAND gameboy_link_harness_trace_tests)

        add_executable(gameboy_sdl_link_trace_file_tests
            tests/link_trace_file_tests.cpp
            apps/sdl/link_trace_file.cpp)
        target_include_directories(gameboy_sdl_link_trace_file_tests
            PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/apps/sdl)
        target_link_libraries(gameboy_sdl_link_trace_file_tests PRIVATE gameboy_core)
        add_test(NAME gameboy_sdl_link_trace_file
                 COMMAND gameboy_sdl_link_trace_file_tests)

        add_executable(gameboy_link_harness_support_tests
            tests/link_harness_support_tests.cpp
            apps/link_harness/harness_io.cpp
            apps/link_harness/pokemon_state.cpp
            apps/link_harness/semantic_tracker.cpp
            apps/link_harness/scenario_runner.cpp
            apps/link_harness/scenario_state.cpp)
        target_include_directories(gameboy_link_harness_support_tests
            PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/apps/link_harness)
        target_link_libraries(gameboy_link_harness_support_tests PRIVATE gameboy_core)
        add_test(NAME gameboy_link_harness_support
                 COMMAND gameboy_link_harness_support_tests)
    endif()

    # The version path intentionally runs before SDL video initialization, so
    # it is safe in headless CI and catches broken desktop entry-point builds.
    if(TARGET SDL3::SDL3)
        add_executable(gameboy_sdl_input_mapping_tests
            tests/sdl_input_mapping_tests.cpp
            apps/sdl/input_mapping.cpp)
        target_include_directories(gameboy_sdl_input_mapping_tests
            PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/apps/sdl)
        target_link_libraries(gameboy_sdl_input_mapping_tests PRIVATE gameboy_core SDL3::SDL3)
        add_test(NAME gameboy_sdl_input_mapping
                 COMMAND gameboy_sdl_input_mapping_tests)

        add_executable(gameboy_sdl_window_event_tests
            tests/sdl_window_event_tests.cpp
            apps/sdl/window_event.cpp)
        target_include_directories(gameboy_sdl_window_event_tests
            PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/apps/sdl)
        target_link_libraries(gameboy_sdl_window_event_tests PRIVATE SDL3::SDL3)
        add_test(NAME gameboy_sdl_window_event
                 COMMAND gameboy_sdl_window_event_tests)

        add_executable(gameboy_sdl_input_configuration_tests
            tests/sdl_input_configuration_tests.cpp
            apps/sdl/input_configuration.cpp)
        target_include_directories(gameboy_sdl_input_configuration_tests
            PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/apps/sdl)
        target_link_libraries(gameboy_sdl_input_configuration_tests PRIVATE SDL3::SDL3)
        add_test(NAME gameboy_sdl_input_configuration
                 COMMAND gameboy_sdl_input_configuration_tests)

        add_executable(gameboy_sdl_frame_pacer_tests
            tests/sdl_frame_pacer_tests.cpp
            apps/sdl/frame_pacer.cpp)
        target_include_directories(gameboy_sdl_frame_pacer_tests
            PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/apps/sdl)
        add_test(NAME gameboy_sdl_frame_pacer
                 COMMAND gameboy_sdl_frame_pacer_tests)

        add_executable(gameboy_sdl_event_dispatch_tests
            tests/sdl_event_dispatch_tests.cpp
            apps/sdl/event_dispatch.cpp)
        target_include_directories(gameboy_sdl_event_dispatch_tests
            PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/apps/sdl
                    ${CMAKE_CURRENT_SOURCE_DIR}/include)
        # The focused pump/coordinate test intentionally excludes handlers
        # whose full UI and emulator dependencies belong to the SDL target.
        target_compile_definitions(gameboy_sdl_event_dispatch_tests
            PRIVATE GBB_EVENT_DISPATCH_CORE_ONLY)
        target_link_libraries(gameboy_sdl_event_dispatch_tests PRIVATE SDL3::SDL3)
        add_test(NAME gameboy_sdl_event_dispatch
                 COMMAND gameboy_sdl_event_dispatch_tests)
    endif()
    if(TARGET gameboy_sdl)
        add_test(NAME gameboy_sdl_version COMMAND gameboy_sdl --version)
    endif()

    if(GAMEBOY_TEST_ROM_DIR OR GAMEBOY_SAMESUITE_DIR)
        include(cmake/ConformanceTests.cmake)
    endif()
endif()

if(GAMEBOY_BUILD_FUZZERS)
    if(EMSCRIPTEN OR ANDROID)
        message(FATAL_ERROR "libFuzzer targets require a native desktop build")
    elseif(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang|AppleClang")
        message(FATAL_ERROR "GAMEBOY_BUILD_FUZZERS requires Clang/libFuzzer")
    endif()

    add_executable(gameboy_parser_fuzzers tests/fuzz/parsers_fuzz.cpp)
    target_include_directories(gameboy_parser_fuzzers PRIVATE
                               ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(gameboy_parser_fuzzers PRIVATE gbb_core_api)
    target_compile_options(gameboy_parser_fuzzers PRIVATE
        -fsanitize=fuzzer)
    target_link_options(gameboy_parser_fuzzers PRIVATE -fsanitize=fuzzer)
endif()

# The semantic corpus reviewer is a normal native command-line tool so it
# remains available even on systems without Clang/libFuzzer. It exercises the
# same parser entry points as the optional fuzz target.
if(NOT EMSCRIPTEN AND NOT ANDROID)
    add_executable(gameboy_parser_review tests/fuzz/review_corpus.cpp)
    target_include_directories(gameboy_parser_review PRIVATE
                               ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(gameboy_parser_review PRIVATE gbb_core_api)
endif()
