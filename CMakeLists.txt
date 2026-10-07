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
option(GAMEBOY_ENABLE_MODEL_MATRIX
       "Run deterministic ROM suites across every hardware model" OFF)
option(GAMEBOY_ENABLE_DISCOVERED_CONFORMANCE
       "Register every discovered external ROM as an individual CTest case" OFF)
option(GAMEBOY_ENABLE_EXTERNAL_SUITES
       "Run metadata-driven AGE and SameSuite research suites" OFF)
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
option(GAMEBOY_BUILD_PERFORMANCE_TESTS
       "Build deterministic emulation load and FPS performance tests"
       ON)
option(GAMEBOY_ENABLE_RELEASE_IPO
       "Use supported native release link-time optimization without fast-math"
       ON)
set(GBB_RELEASE_IPO_SUPPORTED FALSE)
if(GAMEBOY_ENABLE_RELEASE_IPO AND NOT EMSCRIPTEN AND
   NOT GAMEBOY_ENABLE_SANITIZERS AND NOT GAMEBOY_ENABLE_THREAD_SANITIZER)
    include(CheckIPOSupported)
    check_ipo_supported(RESULT GBB_RELEASE_IPO_SUPPORTED
                        OUTPUT GBB_RELEASE_IPO_ERROR LANGUAGES CXX)
    if(NOT GBB_RELEASE_IPO_SUPPORTED)
        message(STATUS "Release IPO unavailable: ${GBB_RELEASE_IPO_ERROR}")
    endif()
endif()
function(gbb_enable_release_ipo target)
    if(GBB_RELEASE_IPO_SUPPORTED)
        set_property(TARGET ${target} PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
        # Android's packaged release configuration uses RelWithDebInfo.
        if(ANDROID)
            set_property(TARGET ${target} PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELWITHDEBINFO TRUE)
            if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
                # CMake selects ThinLTO for Clang by default. Full LTO lets
                # LLVM inline the CPU/APU/GB coordination paths together;
                # measured ARM64 playback needs that additional headroom.
                # The last flag overrides CMake's -flto=thin, without ISA
                # tuning, fast-math or changes to emulated timing/output.
                target_compile_options(${target} PRIVATE
                    "$<$<OR:$<CONFIG:Release>,$<CONFIG:RelWithDebInfo>>:-flto=full>")
                # Consumers of the static core also need the LTO driver
                # option: otherwise Android's emulated-TLS linker settings
                # are lost when LLVM bitcode is linked by a non-IPO target.
                target_link_options(${target} PUBLIC
                    "$<$<OR:$<CONFIG:Release>,$<CONFIG:RelWithDebInfo>>:-flto=full>")
            endif()
        endif()
    endif()
endfunction()
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
    src/snes_audio_host.cpp
    src/snes_host_cpu.cpp
    src/sgb_icd_gb_source.cpp
    src/sgb_host.cpp
    src/sgb_audio_mixer.cpp
    src/snes_dsp_pcm_renderer.cpp
    src/snes_dsp_audio_engine.cpp
    src/snes_apu_audio_engine.cpp
    src/snes_spc700.cpp
    src/snes_brr.cpp
    src/snes_dsp_brr_stream.cpp
    src/snes_dsp_brr_group_stream.cpp
    src/snes_dsp_voice_math.cpp
    src/snes_dsp_envelope.cpp
    src/snes_dsp_gaussian.cpp
    src/snes_dsp_sample_ring.cpp
    src/snes_dsp_key_on_sequence.cpp
    src/snes_dsp_key_control.cpp
    src/snes_dsp_end_state.cpp
    src/sgb_sound.cpp
    src/boot_rom.cpp
    src/cartridge.cpp
    src/cartridge_persistence.cpp
    src/cpu.cpp
    src/dmg_palette.cpp
    src/emulator.cpp
    src/boot_splash.cpp
    src/gameboy_link_endpoint.cpp
    src/gameshark.cpp
    src/joypad.cpp
    src/sgb_adapter.cpp
    src/sgb_trace.cpp
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
    src/bluetooth_link_channel.cpp
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
    src/sgb_firmware_core.cpp
    src/core_runtime.cpp
    src/plugin_loader.cpp
    src/plugin_discovery.cpp
    src/plugin_trust.cpp
    src/lan_discovery.cpp
    src/gameboy_scene.cpp
    src/video.cpp
    src/rom_library.cpp
    src/scene_json.cpp
    src/save_sync.cpp
    src/settings.cpp
    src/trace_parser.cpp
    src/voxel_profile.cpp
    src/voxel_scene.cpp
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
gbb_enable_release_ipo(gameboy_core)
gbb_enable_release_ipo(gbb_core_api)

if(WIN32)
    target_link_libraries(gameboy_core PUBLIC ws2_32 uuid iphlpapi)
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
    gbb_enable_release_ipo(gameboy_cli)
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

    add_executable(gameboy_sgb_trace_diff apps/cli/sgb_trace_diff.cpp)
    target_link_libraries(gameboy_sgb_trace_diff PRIVATE gameboy_core)
    set_target_properties(gameboy_sgb_trace_diff PROPERTIES
                          OUTPUT_NAME gbb_sgb_trace_diff)
    if(WIN32)
        install(TARGETS gameboy_sgb_trace_diff RUNTIME DESTINATION .)
    else()
        install(TARGETS gameboy_sgb_trace_diff
                RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
    endif()

    add_executable(gameboy_test_runner apps/test_runner/main.cpp)
    target_include_directories(gameboy_test_runner PRIVATE scripts)
    target_link_libraries(gameboy_test_runner PRIVATE gbb_core_api)
    set_target_properties(gameboy_test_runner PROPERTIES OUTPUT_NAME gbb_test_runner)

    add_executable(gameboy_link_harness
                apps/link_harness/main.cpp
        apps/link_harness/harness_io.cpp
        apps/link_harness/fault_channel.cpp
        apps/link_harness/options.cpp
        apps/link_harness/pokemon_automation.cpp
        apps/link_harness/pokemon_state.cpp
        apps/link_harness/semantic_tracker.cpp
        apps/link_harness/scenario_runner.cpp
        apps/link_harness/scenario_state.cpp
        apps/link_harness/scenario_trace.cpp
        apps/link_harness/scenario_trace_writer.cpp
    )
    target_link_libraries(gameboy_link_harness PRIVATE gbb_core_api)
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
                "-sEXPORTED_FUNCTIONS=['_main','_gbb_resume_audio','_gbb_start_rom','_gbb_pause_rom','_gbb_set_palette','_gbb_set_video_mode','_gbb_set_hardware_model','_gbb_set_startup_mode','_gbb_set_audio_enabled','_gbb_set_show_fps','_gbb_set_voxel_camera','_gbb_reset_voxel_camera','_gbb_export_scene_snapshot']"
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
                apps/sdl/emulation_runtime.cpp
                apps/sdl/presentation.cpp
                apps/sdl/frame_pacer.cpp
                apps/sdl/frame_timing_trace.cpp
                apps/sdl/emulation_session.cpp
                apps/sdl/remote_link_controller.cpp
                apps/sdl/event_dispatch.cpp
                apps/sdl/event_policy.cpp
                apps/sdl/event_lifecycle.cpp
                apps/sdl/android_bridge.cpp
                apps/sdl/android_link_bridge.cpp
                apps/sdl/android_lifecycle.cpp
                apps/sdl/android_touch_renderer.cpp
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
                apps/sdl/emulation_runtime.cpp
                apps/sdl/desktop_debugger.cpp
                apps/sdl/sprite_editor.cpp
                apps/sdl/video_viewer.cpp
                apps/sdl/tas_editor.cpp
                apps/sdl/cheat_manager.cpp
                apps/sdl/advanced_tools.cpp
                apps/sdl/link_controller.cpp
                apps/sdl/presentation.cpp
                apps/sdl/frame_pacer.cpp
                apps/sdl/frame_timing_trace.cpp
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
                apps/sdl/windows_dashboard_artwork.cpp
                apps/sdl/windows_dashboard_smoke.cpp
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
                apps/sdl/emulation_runtime.cpp
                apps/sdl/desktop_debugger.cpp
                apps/sdl/sprite_editor.cpp
                apps/sdl/video_viewer.cpp
                apps/sdl/tas_editor.cpp
                apps/sdl/cheat_manager.cpp
                apps/sdl/advanced_tools.cpp
                apps/sdl/link_controller.cpp
                apps/sdl/presentation.cpp
                apps/sdl/frame_pacer.cpp
                apps/sdl/frame_timing_trace.cpp
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
                apps/sdl/emulation_runtime.cpp
                apps/sdl/desktop_debugger.cpp
                apps/sdl/sprite_editor.cpp
                apps/sdl/video_viewer.cpp
                apps/sdl/tas_editor.cpp
                apps/sdl/cheat_manager.cpp
                apps/sdl/advanced_tools.cpp
                apps/sdl/link_controller.cpp
                apps/sdl/presentation.cpp
                apps/sdl/frame_pacer.cpp
                apps/sdl/frame_timing_trace.cpp
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
            gbb_enable_release_ipo(gameboy_sdl)
            if(SDL3_ttf_FOUND AND TARGET SDL3_ttf::SDL3_ttf)
                target_link_libraries(gameboy_sdl PRIVATE SDL3_ttf::SDL3_ttf)
                target_compile_definitions(gameboy_sdl PRIVATE GBB_HAS_SDL_TTF=1)
            endif()
            target_compile_definitions(gameboy_sdl PRIVATE
                GBB_VERSION="${GAMEBOY_DESKTOP_VERSION}"
            )
            if(WIN32)
                target_link_libraries(gameboy_sdl PRIVATE
                    winhttp bcrypt comctl32 comdlg32 ole32 shell32
                    windowscodecs winmm)
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
                if(WIN32 AND MINGW)
                    # MinGW-linked Windows builds depend on the compiler
                    # runtimes at launch. Keep the release archive
                    # self-contained instead of requiring users to install
                    # the MinGW toolchain separately.
                    foreach(runtime_name IN ITEMS
                            libgcc_s_seh-1.dll
                            libstdc++-6.dll
                            libwinpthread-1.dll)
                        execute_process(
                            COMMAND ${CMAKE_CXX_COMPILER}
                                    -print-file-name=${runtime_name}
                            OUTPUT_VARIABLE runtime_path
                            OUTPUT_STRIP_TRAILING_WHITESPACE
                            ERROR_QUIET)
                        if(EXISTS "${runtime_path}")
                            add_custom_command(TARGET gameboy_sdl POST_BUILD
                                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                                        "${runtime_path}"
                                        $<TARGET_FILE_DIR:gameboy_sdl>
                                VERBATIM)
                            install(FILES "${runtime_path}" DESTINATION .)
                        endif()
                    endforeach()
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
    add_executable(gameboy_dmg_boot_probe tests/dmg_boot_probe.cpp)
    target_link_libraries(gameboy_dmg_boot_probe PRIVATE gameboy_core)
    set_target_properties(gameboy_dmg_boot_probe PROPERTIES OUTPUT_NAME gbb_dmg_boot_probe)
    add_executable(gameboy_stat_irq_probe tests/stat_irq_probe.cpp)
    target_link_libraries(gameboy_stat_irq_probe PRIVATE gameboy_core)
    set_target_properties(gameboy_stat_irq_probe PROPERTIES OUTPUT_NAME gbb_stat_irq_probe)
    find_package(Python3 COMPONENTS Interpreter QUIET)
    if(Python3_Interpreter_FOUND)
        add_test(NAME gameboy_stat_irq_tool
                 COMMAND ${Python3_EXECUTABLE}
                         ${CMAKE_CURRENT_SOURCE_DIR}/tests/stat_irq_tool_tests.py
                         $<TARGET_FILE:gameboy_stat_irq_probe>)
        add_test(NAME gameboy_dmg_boot_probe
                 COMMAND ${Python3_EXECUTABLE}
                         ${CMAKE_CURRENT_SOURCE_DIR}/tests/dmg_boot_probe_tests.py
                         $<TARGET_FILE:gameboy_dmg_boot_probe>)
        set_tests_properties(gameboy_dmg_boot_probe PROPERTIES LABELS "firmware;boot")
        add_test(NAME gameboy_dmg_boot_audio
                 COMMAND ${Python3_EXECUTABLE}
                         ${CMAKE_CURRENT_SOURCE_DIR}/tests/dmg_boot_audio_tests.py
                         $<TARGET_FILE:gameboy_dmg_boot_probe>)
        set_tests_properties(gameboy_dmg_boot_audio PROPERTIES LABELS "firmware;boot;audio")
        add_test(NAME gameboy_dmg_boot_serial
                 COMMAND ${Python3_EXECUTABLE}
                         ${CMAKE_CURRENT_SOURCE_DIR}/tests/dmg_boot_serial_tests.py
                         $<TARGET_FILE:gameboy_dmg_boot_probe>)
        set_tests_properties(gameboy_dmg_boot_serial PROPERTIES LABELS "firmware;boot;link")
        add_test(NAME gameboy_dmg_firmware_source
                 COMMAND ${Python3_EXECUTABLE}
                         ${CMAKE_CURRENT_SOURCE_DIR}/scripts/build_dmg_boot_rom.py --check-source)
        set_tests_properties(gameboy_dmg_firmware_source PROPERTIES LABELS "firmware;boot")
        add_test(NAME gameboy_mgb_firmware_source
                 COMMAND ${Python3_EXECUTABLE}
                         ${CMAKE_CURRENT_SOURCE_DIR}/scripts/build_dmg_boot_rom.py --model mgb --check-source)
        set_tests_properties(gameboy_mgb_firmware_source PROPERTIES LABELS "firmware;boot")
        add_test(NAME gameboy_dmg0_firmware_source
                 COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/scripts/build_dmg_boot_rom.py --model dmg0 --check-source)
        set_tests_properties(gameboy_dmg0_firmware_source PROPERTIES LABELS "firmware;boot")
        foreach(boot_model cgb0 cgb agb0 agb sgb sgb2)
            add_test(NAME gameboy_${boot_model}_firmware_source
                     COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/scripts/build_dmg_boot_rom.py
                             --model ${boot_model} --check-source)
            set_tests_properties(gameboy_${boot_model}_firmware_source PROPERTIES LABELS "firmware;boot")
        endforeach()
        add_test(NAME gameboy_sgb_boot_validation_tool
                 COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/sgb_boot_validation_tests.py)
        set_tests_properties(gameboy_sgb_boot_validation_tool PROPERTIES LABELS "firmware;boot;sgb")
        foreach(boot_model dmg0 dmg mgb cgb0 cgb agb0 agb)
            add_test(NAME gameboy_${boot_model}_animated_firmware_source
                     COMMAND ${Python3_EXECUTABLE}
                             ${CMAKE_CURRENT_SOURCE_DIR}/scripts/build_dmg_boot_rom.py
                             --model ${boot_model} --animated --check-source)
            set_tests_properties(gameboy_${boot_model}_animated_firmware_source PROPERTIES LABELS "firmware;boot")
        endforeach()
        if(TARGET gameboy_test_runner)
            add_test(NAME gameboy_dmg_boot_cli
                     COMMAND ${Python3_EXECUTABLE}
                             ${CMAKE_CURRENT_SOURCE_DIR}/tests/dmg_boot_cli_tests.py
                             $<TARGET_FILE:gameboy_test_runner>)
            set_tests_properties(gameboy_dmg_boot_cli PROPERTIES LABELS "firmware;boot")
        endif()
        find_program(GBB_RGBASM rgbasm)
        find_program(GBB_RGBLINK rgblink)
        if(GBB_RGBASM AND GBB_RGBLINK)
            add_test(NAME gameboy_dmg_firmware_reproducible
                     COMMAND ${Python3_EXECUTABLE}
                             ${CMAKE_CURRENT_SOURCE_DIR}/scripts/build_dmg_boot_rom.py --check
                             --rgbasm ${GBB_RGBASM} --rgblink ${GBB_RGBLINK})
            set_tests_properties(gameboy_dmg_firmware_reproducible
                                 PROPERTIES LABELS "firmware;boot")
            add_test(NAME gameboy_mgb_firmware_reproducible
                     COMMAND ${Python3_EXECUTABLE}
                             ${CMAKE_CURRENT_SOURCE_DIR}/scripts/build_dmg_boot_rom.py --model mgb --check
                             --rgbasm ${GBB_RGBASM} --rgblink ${GBB_RGBLINK})
            set_tests_properties(gameboy_mgb_firmware_reproducible PROPERTIES LABELS "firmware;boot")
            add_test(NAME gameboy_dmg0_firmware_reproducible
                     COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/scripts/build_dmg_boot_rom.py --model dmg0 --check
                             --rgbasm ${GBB_RGBASM} --rgblink ${GBB_RGBLINK})
            set_tests_properties(gameboy_dmg0_firmware_reproducible PROPERTIES LABELS "firmware;boot")
            foreach(boot_model cgb0 cgb agb0 agb)
                add_test(NAME gameboy_${boot_model}_firmware_reproducible
                         COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/scripts/build_dmg_boot_rom.py
                                 --model ${boot_model} --check --rgbasm ${GBB_RGBASM} --rgblink ${GBB_RGBLINK})
                set_tests_properties(gameboy_${boot_model}_firmware_reproducible PROPERTIES LABELS "firmware;boot")
            endforeach()
            foreach(boot_model dmg0 dmg mgb cgb0 cgb agb0 agb)
                add_test(NAME gameboy_${boot_model}_animated_firmware_reproducible
                         COMMAND ${Python3_EXECUTABLE}
                                 ${CMAKE_CURRENT_SOURCE_DIR}/scripts/build_dmg_boot_rom.py
                                 --model ${boot_model} --animated --check
                                 --rgbasm ${GBB_RGBASM} --rgblink ${GBB_RGBLINK})
                set_tests_properties(gameboy_${boot_model}_animated_firmware_reproducible PROPERTIES LABELS "firmware;boot")
            endforeach()
        endif()
        add_test(NAME gameboy_audio_reference_tool
                 COMMAND ${Python3_EXECUTABLE}
                         ${CMAKE_CURRENT_SOURCE_DIR}/tests/audio_reference_tool_tests.py)
        add_test(NAME gameboy_external_suite_harness
                 COMMAND ${Python3_EXECUTABLE}
                         ${CMAKE_CURRENT_SOURCE_DIR}/tests/external_suite_tests.py)
        add_test(NAME gameboy_differential_test_harness
                 COMMAND ${Python3_EXECUTABLE}
                         ${CMAKE_CURRENT_SOURCE_DIR}/tests/differential_test_tests.py)
        add_test(NAME gameboy_ci_regression_report
                 COMMAND ${Python3_EXECUTABLE}
                         ${CMAKE_CURRENT_SOURCE_DIR}/tests/ci_regression_report_tests.py)
        add_test(NAME gameboy_model_matrix_contract
                 COMMAND ${Python3_EXECUTABLE}
                         ${CMAKE_CURRENT_SOURCE_DIR}/tests/model_matrix_contract_tests.py)
        add_test(NAME gameboy_render_performance_report
                 COMMAND ${Python3_EXECUTABLE}
                         ${CMAKE_CURRENT_SOURCE_DIR}/tests/render_performance_report_tests.py)
        add_test(NAME gameboy_android_sgb_performance_gate
                 COMMAND ${Python3_EXECUTABLE}
                         ${CMAKE_CURRENT_SOURCE_DIR}/tests/android_sgb_performance_gate_tests.py)
        set_tests_properties(gameboy_android_sgb_performance_gate
                             PROPERTIES LABELS "performance;sgb")
        add_test(NAME gameboy_sgb_host_performance_gate
                 COMMAND ${Python3_EXECUTABLE}
                         ${CMAKE_CURRENT_SOURCE_DIR}/tests/sgb_host_performance_gate_tests.py)
        set_tests_properties(gameboy_sgb_host_performance_gate
                             PROPERTIES LABELS "performance;sgb")
        add_test(NAME gameboy_sgb_frontend_playback_gate
                 COMMAND ${Python3_EXECUTABLE}
                         ${CMAKE_CURRENT_SOURCE_DIR}/tests/sgb_frontend_playback_gate_tests.py)
        set_tests_properties(gameboy_sgb_frontend_playback_gate
                             PROPERTIES LABELS "performance;sgb")
        add_test(NAME gameboy_sgb_command_report
                 COMMAND ${Python3_EXECUTABLE}
                         ${CMAKE_CURRENT_SOURCE_DIR}/tests/sgb_command_report_tests.py)
        set_tests_properties(gameboy_sgb_command_report
                             PROPERTIES LABELS "sgb;trace")
        add_test(NAME gameboy_voxel_visual_regression
                 COMMAND ${Python3_EXECUTABLE}
                         ${CMAKE_CURRENT_SOURCE_DIR}/tests/voxel_visual_regression_tests.py)
        add_test(NAME gameboy_visual_regression_gate
                 COMMAND ${Python3_EXECUTABLE}
                         ${CMAKE_CURRENT_SOURCE_DIR}/tests/visual_regression_gate_tests.py)
        if(NOT ANDROID)
            add_test(NAME gameboy_sgb_frame_alignment
                     COMMAND ${Python3_EXECUTABLE}
                             ${CMAKE_CURRENT_SOURCE_DIR}/tests/sgb_frame_alignment_tests.py)
            set_tests_properties(gameboy_sgb_frame_alignment
                                 PROPERTIES LABELS "sgb;visual")
            add_test(NAME gameboy_sgb_title_validation
                     COMMAND ${Python3_EXECUTABLE}
                             ${CMAKE_CURRENT_SOURCE_DIR}/tests/sgb_title_validation_tests.py
                             $<TARGET_FILE:gameboy_test_runner>)
            set_tests_properties(gameboy_sgb_title_validation
                                 PROPERTIES LABELS "sgb;trace")
            add_test(NAME gameboy_sgb_sound_report
                     COMMAND ${Python3_EXECUTABLE}
                             ${CMAKE_CURRENT_SOURCE_DIR}/tests/sgb_sound_report_tests.py
                             $<TARGET_FILE:gameboy_test_runner>)
            add_test(NAME gameboy_sgb_audio_reference_tools
                     COMMAND ${Python3_EXECUTABLE}
                             ${CMAKE_CURRENT_SOURCE_DIR}/tests/sgb_audio_reference_tools_tests.py)
            set_tests_properties(gameboy_sgb_sound_report
                                 PROPERTIES LABELS "sgb;audio;trace")
            set_tests_properties(gameboy_sgb_audio_reference_tools
                                 PROPERTIES LABELS "sgb;audio;trace")
            add_test(NAME gameboy_sgb_trace_fixture
                     COMMAND ${Python3_EXECUTABLE}
                             ${CMAKE_CURRENT_SOURCE_DIR}/tests/sgb_trace_fixture_test.py
                             $<TARGET_FILE:gameboy_test_runner>
                             $<TARGET_FILE:gameboy_sgb_trace_diff>
                             ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/sgb/trace_fixture.hex
                             ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/sgb/trace_fixture.trace)
            set_tests_properties(gameboy_sgb_trace_fixture
                                 PROPERTIES LABELS "sgb;trace")
        endif()
    endif()
    add_executable(gameboy_tests tests/core_tests.cpp)
    add_executable(gameboy_dmg_boot_tests tests/dmg_boot_tests.cpp)
    target_link_libraries(gameboy_dmg_boot_tests PRIVATE gameboy_core)
    add_test(NAME gameboy_dmg_boot COMMAND gameboy_dmg_boot_tests)
    add_executable(gameboy_boot_splash_tests tests/boot_splash_tests.cpp)
    target_link_libraries(gameboy_boot_splash_tests PRIVATE gameboy_core)
    add_test(NAME gameboy_boot_splash COMMAND gameboy_boot_splash_tests)
    add_executable(gameboy_mgb_boot_tests tests/mgb_boot_tests.cpp)
    add_executable(gameboy_cgb_boot_tests tests/cgb_boot_tests.cpp)
    add_executable(gameboy_sgb_replacement_boot_tests tests/sgb_replacement_boot_tests.cpp)
    target_link_libraries(gameboy_sgb_replacement_boot_tests PRIVATE gbb_core_api)
    add_test(NAME gameboy_sgb_replacement_boot COMMAND gameboy_sgb_replacement_boot_tests)
    set_tests_properties(gameboy_sgb_replacement_boot PROPERTIES LABELS "firmware;boot;sgb")
    target_link_libraries(gameboy_cgb_boot_tests PRIVATE gbb_core_api)
    add_test(NAME gameboy_cgb_boot COMMAND gameboy_cgb_boot_tests)
    set_tests_properties(gameboy_cgb_boot PROPERTIES LABELS "firmware;boot")
    add_executable(gameboy_dmg0_boot_tests tests/dmg0_boot_tests.cpp)
    target_link_libraries(gameboy_dmg0_boot_tests PRIVATE gbb_core_api)
    add_test(NAME gameboy_dmg0_boot COMMAND gameboy_dmg0_boot_tests)
    set_tests_properties(gameboy_dmg0_boot PROPERTIES LABELS "firmware;boot")
    target_link_libraries(gameboy_mgb_boot_tests PRIVATE gbb_core_api)
    add_test(NAME gameboy_mgb_boot COMMAND gameboy_mgb_boot_tests)
    set_tests_properties(gameboy_mgb_boot PROPERTIES LABELS "firmware;boot")
    set_tests_properties(gameboy_dmg_boot gameboy_boot_splash PROPERTIES LABELS "firmware;boot")
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

    add_executable(gameboy_windows_dashboard_contract_tests
        tests/windows_dashboard_contract_tests.cpp)
    target_include_directories(gameboy_windows_dashboard_contract_tests
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/apps/sdl
                ${CMAKE_CURRENT_SOURCE_DIR}/include)
    add_test(NAME gameboy_windows_dashboard_contract
             COMMAND gameboy_windows_dashboard_contract_tests)

    add_executable(gameboy_desktop_breakpoint_contract_tests
        tests/desktop_breakpoint_contract_tests.cpp)
    target_include_directories(gameboy_desktop_breakpoint_contract_tests
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/apps/sdl)
    add_test(NAME gameboy_desktop_breakpoint_contract
             COMMAND gameboy_desktop_breakpoint_contract_tests)

    add_executable(gameboy_android_touch_layout_contract_tests
        tests/android_touch_layout_contract_tests.cpp)
    target_include_directories(gameboy_android_touch_layout_contract_tests
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/apps/sdl)
    add_test(NAME gameboy_android_touch_layout_contract
             COMMAND gameboy_android_touch_layout_contract_tests)

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

    add_executable(gameboy_desktop_disassembler_contract_tests
        tests/desktop_disassembler_contract_tests.cpp)
    target_include_directories(gameboy_desktop_disassembler_contract_tests
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/apps/sdl)
    target_link_libraries(gameboy_desktop_disassembler_contract_tests
        PRIVATE gameboy_core)
    add_test(NAME gameboy_desktop_disassembler_contract
             COMMAND gameboy_desktop_disassembler_contract_tests)

    add_executable(gameboy_desktop_viewport_contract_tests
        tests/desktop_viewport_contract_tests.cpp)
    target_include_directories(gameboy_desktop_viewport_contract_tests
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/apps/sdl)
    target_link_libraries(gameboy_desktop_viewport_contract_tests
        PRIVATE gameboy_core)
    add_test(NAME gameboy_desktop_viewport_contract
             COMMAND gameboy_desktop_viewport_contract_tests)

    add_executable(gameboy_desktop_memory_view_contract_tests
        tests/desktop_memory_view_contract_tests.cpp)
    target_include_directories(gameboy_desktop_memory_view_contract_tests
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/apps/sdl)
    add_test(NAME gameboy_desktop_memory_view_contract
             COMMAND gameboy_desktop_memory_view_contract_tests)

    add_executable(gameboy_sdl_emulation_policy_tests
        tests/sdl_emulation_policy_tests.cpp)
    target_include_directories(gameboy_sdl_emulation_policy_tests
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/apps/sdl)
    add_test(NAME gameboy_sdl_emulation_policy
             COMMAND gameboy_sdl_emulation_policy_tests)

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

    add_executable(gameboy_link_fault_harness_tests
        tests/link_fault_harness_tests.cpp)
    target_link_libraries(gameboy_link_fault_harness_tests PRIVATE gbb_core_api)
    set_target_properties(gameboy_link_fault_harness_tests
                          PROPERTIES OUTPUT_NAME gbb_link_fault_harness)
    add_test(NAME gameboy_link_fault_harness
             COMMAND gameboy_link_fault_harness_tests)
    if(WIN32)
        install(TARGETS gameboy_link_fault_harness_tests RUNTIME DESTINATION .)
    elseif(NOT EMSCRIPTEN AND NOT ANDROID)
        install(TARGETS gameboy_link_fault_harness_tests
                RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
    endif()

    if(NOT EMSCRIPTEN)
        add_executable(gameboy_link_end_to_end_tests
            tests/link_end_to_end_tests.cpp)
        target_link_libraries(gameboy_link_end_to_end_tests PRIVATE gameboy_core)
        if(NOT ANDROID)
            add_test(NAME gameboy_link_end_to_end_local
                     COMMAND gameboy_link_end_to_end_tests local)
            add_test(NAME gameboy_link_end_to_end_tcp
                     COMMAND gameboy_link_end_to_end_tests tcp)
            set_tests_properties(gameboy_link_end_to_end_tcp PROPERTIES
                                 SKIP_RETURN_CODE 77)
        endif()
    endif()

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

    add_executable(gameboy_stat_irq_contract_tests tests/stat_irq_contract_tests.cpp)
    target_include_directories(gameboy_stat_irq_contract_tests PRIVATE src)
    target_link_libraries(gameboy_stat_irq_contract_tests PRIVATE gameboy_core)
    add_test(NAME gameboy_stat_irq_contract COMMAND gameboy_stat_irq_contract_tests)

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

    if(GAMEBOY_BUILD_PERFORMANCE_TESTS AND NOT EMSCRIPTEN AND NOT ANDROID AND
       NOT GAMEBOY_ENABLE_SANITIZERS AND NOT GAMEBOY_ENABLE_THREAD_SANITIZER)
        add_executable(gameboy_frame_rate_metrics_tests
            tests/frame_rate_metrics_tests.cpp)
        target_include_directories(gameboy_frame_rate_metrics_tests
            PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/apps/sdl)
        add_test(NAME gameboy_frame_rate_metrics
                 COMMAND gameboy_frame_rate_metrics_tests)
        set_tests_properties(gameboy_frame_rate_metrics
                             PROPERTIES LABELS "performance")

        add_executable(gameboy_emulation_performance_tests
            tests/emulation_performance_tests.cpp)
        target_include_directories(gameboy_emulation_performance_tests
            PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/apps/sdl)
        target_link_libraries(gameboy_emulation_performance_tests
            PRIVATE gbb_core_api)
        add_test(NAME gameboy_emulation_performance
                 COMMAND gameboy_emulation_performance_tests)
        set_tests_properties(gameboy_emulation_performance
                             PROPERTIES LABELS "performance" TIMEOUT 30)

        add_executable(gameboy_sgb_performance_tests
            tests/sgb_performance_tests.cpp)
        target_link_libraries(gameboy_sgb_performance_tests
            PRIVATE gbb_core_api)
        add_test(NAME gameboy_sgb_performance
                 COMMAND gameboy_sgb_performance_tests)
        set_tests_properties(gameboy_sgb_performance
                             PROPERTIES LABELS "performance;sgb" TIMEOUT 45
                                        RUN_SERIAL TRUE)
    endif()

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

    add_executable(gameboy_save_sync_contract_tests
        tests/save_sync_contract_tests.cpp)
    target_link_libraries(gameboy_save_sync_contract_tests PRIVATE gbb_core_api)
    add_test(NAME gameboy_save_sync_contract
             COMMAND gameboy_save_sync_contract_tests)

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

    add_executable(gameboy_hardware_model_matrix_contract_tests
        tests/hardware_model_matrix_contract_tests.cpp)
    target_link_libraries(gameboy_hardware_model_matrix_contract_tests
        PRIVATE gameboy_core)
    add_test(NAME gameboy_hardware_model_matrix_contract
             COMMAND gameboy_hardware_model_matrix_contract_tests)

    add_executable(gameboy_ppu_sgb_contract_tests
        tests/ppu_sgb_contract_tests.cpp)
    target_link_libraries(gameboy_ppu_sgb_contract_tests PRIVATE gameboy_core)
    add_test(NAME gameboy_ppu_sgb_contract
             COMMAND gameboy_ppu_sgb_contract_tests)

    add_executable(gameboy_sgb_sound_contract_tests
        tests/sgb_sound_contract_tests.cpp)
    target_link_libraries(gameboy_sgb_sound_contract_tests PRIVATE gameboy_core)
    add_test(NAME gameboy_sgb_sound_contract
             COMMAND gameboy_sgb_sound_contract_tests)

    add_executable(gameboy_snes_audio_host_tests
        tests/snes_audio_host_tests.cpp)
    target_link_libraries(gameboy_snes_audio_host_tests PRIVATE gameboy_core)
    add_test(NAME gameboy_snes_audio_host_contract
             COMMAND gameboy_snes_audio_host_tests)
    add_executable(gameboy_sgb_host_tests tests/sgb_host_tests.cpp)
    add_executable(gameboy_sgb_original_title_probe tests/support/sgb_original_title_probe.cpp)
    target_link_libraries(gameboy_sgb_original_title_probe PRIVATE gameboy_core)
    add_executable(gameboy_sgb_original_transfer_tests tests/sgb_original_transfer_tests.cpp)
    target_link_libraries(gameboy_sgb_original_transfer_tests PRIVATE gameboy_core)
    add_test(NAME gameboy_sgb_original_transfer_contract COMMAND gameboy_sgb_original_transfer_tests)
    add_executable(gameboy_sgb_original_firmware_tests tests/sgb_original_firmware_tests.cpp)
    target_link_libraries(gameboy_sgb_original_firmware_tests PRIVATE gameboy_core)
    add_test(NAME gameboy_sgb_original_firmware_contract COMMAND gameboy_sgb_original_firmware_tests)
    if(Python3_Interpreter_FOUND)
        add_test(NAME gameboy_sgb_score_decoder_contract
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/sgb_score_decoder_tests.py)
        add_test(NAME gameboy_sgb_score_demand_inventory
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/sgb_score_inventory_tests.py)
        if(TARGET gameboy_test_runner)
            add_test(NAME gameboy_sgb_song_selection_fixture
                COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/sgb_song_selection_fixture_tests.py
                    --runner $<TARGET_FILE:gameboy_test_runner>)
        else()
            add_test(NAME gameboy_sgb_song_selection_fixture
                COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/sgb_song_selection_fixture_tests.py)
        endif()
        add_test(NAME gameboy_sgb_pitch_fixture
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/sgb_pitch_fixture_tests.py)
        add_test(NAME gameboy_sgb_gate_timing_fixture
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/sgb_gate_timing_fixture_tests.py)
        add_test(NAME gameboy_sgb_timing_fixture
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/sgb_timing_fixture_tests.py)
        add_test(NAME gameboy_sgb_volume_pan_fixture
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/sgb_volume_pan_fixture_tests.py)
        add_test(NAME gameboy_sgb_echo_fixture
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/sgb_echo_fixture_tests.py)
        add_test(NAME gameboy_sgb_phrase_fixture
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/sgb_phrase_fixture_tests.py)
        add_test(NAME gameboy_sgb_subroutine_fixture
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/sgb_subroutine_fixture_tests.py)
        add_test(NAME gameboy_sgb_score_scheduler
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/sgb_score_scheduler_tests.py)
        add_test(NAME gameboy_sgb_resident_score_inspection
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/sgb_resident_score_inspection_tests.py)
        add_test(NAME gameboy_sgb_resident_score_contract
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/sgb_resident_score_tests.py
                    --runner $<TARGET_FILE:gameboy_sgb_original_transfer_tests>)
        add_test(NAME gameboy_sgb_original_compatibility_report
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/sgb_original_compatibility_report_tests.py
                    --probe $<TARGET_FILE:gameboy_sgb_original_title_probe>)
        add_test(NAME gameboy_sgb_original_score_transfer
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/sgb_original_score_transfer_tests.py
                    --runner $<TARGET_FILE:gameboy_sgb_original_transfer_tests>)
        add_test(NAME gameboy_sgb_original_firmware_build
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/sgb_original_firmware_build_tests.py)
        add_test(NAME gameboy_sgb_original_firmware_reproducible
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/scripts/build_sgb_prototype.py --check)
    endif()
    if(NOT ANDROID AND NOT EMSCRIPTEN)
        add_executable(gameboy_sgb_firmware_core_tests tests/sgb_firmware_core_tests.cpp)
        target_include_directories(gameboy_sgb_firmware_core_tests PRIVATE apps/sdl scripts)
        target_link_libraries(gameboy_sgb_firmware_core_tests PRIVATE gbb_core_api)
        add_test(NAME gameboy_sgb_firmware_core_contract COMMAND gameboy_sgb_firmware_core_tests)
        if(Python3_Interpreter_FOUND)
            add_test(NAME gameboy_sgb_production_report_contract
                COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/sgb_production_report_tests.py)
        endif()
        if(Python3_Interpreter_FOUND AND
           EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/roms/sgb1.program.rom" AND
           EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/roms/sgb2.program.rom" AND
           EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/roms/Donkey Kong (JU) (V1.1) [S][!].gb" AND
           EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/roms/Donkey Kong (JU) (V1.1) [S][!].sav")
            if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/roms/spc700.rom")
                add_test(NAME gameboy_sgb_production_local_titles
                    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/sgb_production_local_title_tests.py
                    --runner $<TARGET_FILE:gameboy_sgb_firmware_core_tests>
                    --roms ${CMAKE_CURRENT_SOURCE_DIR}/roms)
                # Allow the subprocess timeout to reap its runner and remove private
                # temporary images before CTest's outer watchdog expires.
                set_tests_properties(gameboy_sgb_production_local_titles PROPERTIES TIMEOUT 960 LABELS "sgb;private")
            endif()
            add_test(NAME gameboy_sgb_production_bundled_ipl_local_titles
                COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/sgb_production_local_title_tests.py
                --runner $<TARGET_FILE:gameboy_sgb_firmware_core_tests>
                --roms ${CMAKE_CURRENT_SOURCE_DIR}/roms --bundled-ipl)
            set_tests_properties(gameboy_sgb_production_bundled_ipl_local_titles PROPERTIES TIMEOUT 960 LABELS "sgb;private")
        endif()
    endif()
    target_link_libraries(gameboy_sgb_host_tests PRIVATE gameboy_core)
    add_test(NAME gameboy_sgb_host_contract COMMAND gameboy_sgb_host_tests)
    add_executable(gameboy_apu_mixer_cache_tests tests/apu_mixer_cache_tests.cpp)
    target_link_libraries(gameboy_apu_mixer_cache_tests PRIVATE gameboy_core)
    add_test(NAME gameboy_apu_mixer_cache_contract COMMAND gameboy_apu_mixer_cache_tests)
    add_executable(gameboy_snes_spc_replay_cache_tests tests/snes_spc_replay_cache_tests.cpp)
    target_link_libraries(gameboy_snes_spc_replay_cache_tests PRIVATE gameboy_core)
    add_test(NAME gameboy_snes_spc_replay_cache_contract COMMAND gameboy_snes_spc_replay_cache_tests)
    add_executable(gameboy_sgb_execution_cache_tests tests/sgb_execution_cache_tests.cpp)
    target_link_libraries(gameboy_sgb_execution_cache_tests PRIVATE gameboy_core)
    add_test(NAME gameboy_sgb_execution_cache_contract COMMAND gameboy_sgb_execution_cache_tests)
    add_executable(gameboy_sgb_host_title_runner tests/support/sgb_host_title_runner.cpp)
    gbb_enable_release_ipo(gameboy_sgb_host_title_runner)
    target_include_directories(gameboy_sgb_host_title_runner PRIVATE scripts)
    target_link_libraries(gameboy_sgb_host_title_runner PRIVATE gameboy_core)
    if(UNIX OR WIN32)
        add_test(NAME gameboy_sgb_host_metrics_contract
                 COMMAND gameboy_sgb_host_title_runner --metrics-self-test)
    endif()
    if(Python3_Interpreter_FOUND AND
       EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/roms/sgb1.program.rom" AND
       EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/roms/sgb2.program.rom" AND
       EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/roms/sgb.boot.rom" AND
       EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/roms/sgb2.boot.rom" AND
       EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/roms/Donkey Kong (JU) (V1.1) [S][!].gb")
        if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/roms/spc700.rom")
            add_test(NAME gameboy_sgb_host_local_titles
                     COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/sgb_host_local_title_tests.py
                             $<TARGET_FILE:gameboy_sgb_host_title_runner> ${CMAKE_CURRENT_SOURCE_DIR}/roms
                             ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/sgb/titles/donkey-kong-gameplay.script)
            set_tests_properties(gameboy_sgb_host_local_titles PROPERTIES TIMEOUT 1500)
            add_test(NAME gameboy_sgb_host_combined_local_titles
                     COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/sgb_host_combined_title_tests.py
                             $<TARGET_FILE:gameboy_sgb_host_title_runner> ${CMAKE_CURRENT_SOURCE_DIR}/roms
                             ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/sgb/titles/donkey-kong-gameplay.script)
            set_tests_properties(gameboy_sgb_host_combined_local_titles PROPERTIES TIMEOUT 3700)
        endif()
        add_test(NAME gameboy_sgb_replacement_ipl_combined_local_titles
                 COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/sgb_host_combined_title_tests.py
                         $<TARGET_FILE:gameboy_sgb_host_title_runner> ${CMAKE_CURRENT_SOURCE_DIR}/roms
                         ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/sgb/titles/donkey-kong-gameplay.script
                         --replacement-ipl)
        set_tests_properties(gameboy_sgb_replacement_ipl_combined_local_titles PROPERTIES TIMEOUT 3700 LABELS "sgb;private")
    endif()

    add_executable(gameboy_snes_spc700_tests
        tests/snes_spc700_tests.cpp)
    target_link_libraries(gameboy_snes_spc700_tests PRIVATE gameboy_core)
    add_test(NAME gameboy_snes_spc700_contract
             COMMAND gameboy_snes_spc700_tests)
    add_executable(gameboy_snes_spc_write_fixture_runner
        tests/support/snes_spc_write_fixture_runner.cpp)
    target_include_directories(gameboy_snes_spc_write_fixture_runner PRIVATE tests/support)
    target_link_libraries(gameboy_snes_spc_write_fixture_runner PRIVATE gameboy_core)

    add_executable(gameboy_snes_brr_tests
        tests/snes_brr_tests.cpp)
    target_link_libraries(gameboy_snes_brr_tests PRIVATE gameboy_core)
    add_test(NAME gameboy_snes_brr_contract
             COMMAND gameboy_snes_brr_tests)

    add_executable(gameboy_snes_dsp_brr_stream_tests
        tests/snes_dsp_brr_stream_tests.cpp)
    target_link_libraries(gameboy_snes_dsp_brr_stream_tests PRIVATE gameboy_core)
    add_test(NAME gameboy_snes_dsp_brr_stream_contract
             COMMAND gameboy_snes_dsp_brr_stream_tests)

    add_executable(gameboy_snes_dsp_brr_group_stream_tests
        tests/snes_dsp_brr_group_stream_tests.cpp)
    target_link_libraries(gameboy_snes_dsp_brr_group_stream_tests PRIVATE gameboy_core)
    add_test(NAME gameboy_snes_dsp_brr_group_stream_contract
             COMMAND gameboy_snes_dsp_brr_group_stream_tests)

    add_executable(gameboy_snes_dsp_voice_math_tests
        tests/snes_dsp_voice_math_tests.cpp)
    target_link_libraries(gameboy_snes_dsp_voice_math_tests PRIVATE gameboy_core)
    add_test(NAME gameboy_snes_dsp_voice_math_contract
             COMMAND gameboy_snes_dsp_voice_math_tests)

    add_executable(gameboy_snes_dsp_envelope_tests
        tests/snes_dsp_envelope_tests.cpp)
    target_link_libraries(gameboy_snes_dsp_envelope_tests PRIVATE gameboy_core)
    add_test(NAME gameboy_snes_dsp_envelope_contract
             COMMAND gameboy_snes_dsp_envelope_tests)

    add_executable(gameboy_snes_dsp_gaussian_tests
        tests/snes_dsp_gaussian_tests.cpp)
    target_link_libraries(gameboy_snes_dsp_gaussian_tests PRIVATE gameboy_core)
    add_test(NAME gameboy_snes_dsp_gaussian_contract
             COMMAND gameboy_snes_dsp_gaussian_tests)

    add_executable(gameboy_snes_dsp_sample_ring_tests
        tests/snes_dsp_sample_ring_tests.cpp)
    target_link_libraries(gameboy_snes_dsp_sample_ring_tests PRIVATE gameboy_core)
    add_test(NAME gameboy_snes_dsp_sample_ring_contract
             COMMAND gameboy_snes_dsp_sample_ring_tests)

    add_executable(gameboy_snes_dsp_key_on_sequence_tests
        tests/snes_dsp_key_on_sequence_tests.cpp)
    target_link_libraries(gameboy_snes_dsp_key_on_sequence_tests PRIVATE gameboy_core)
    add_test(NAME gameboy_snes_dsp_key_on_sequence_contract
             COMMAND gameboy_snes_dsp_key_on_sequence_tests)

    add_executable(gameboy_snes_dsp_key_control_tests
        tests/snes_dsp_key_control_tests.cpp)
    target_link_libraries(gameboy_snes_dsp_key_control_tests PRIVATE gameboy_core)
    add_test(NAME gameboy_snes_dsp_key_control_contract
             COMMAND gameboy_snes_dsp_key_control_tests)

    add_executable(gameboy_snes_dsp_end_state_tests
        tests/snes_dsp_end_state_tests.cpp)
    target_link_libraries(gameboy_snes_dsp_end_state_tests PRIVATE gameboy_core)
    add_test(NAME gameboy_snes_dsp_end_state_contract
             COMMAND gameboy_snes_dsp_end_state_tests)

    add_executable(gameboy_snes_dsp_pcm_renderer_tests
        tests/snes_dsp_pcm_renderer_tests.cpp)
    target_include_directories(gameboy_snes_dsp_pcm_renderer_tests PRIVATE tests/support)
    target_link_libraries(gameboy_snes_dsp_pcm_renderer_tests PRIVATE gameboy_core)
    add_test(NAME gameboy_snes_dsp_pcm_renderer_contract
             COMMAND gameboy_snes_dsp_pcm_renderer_tests)
    add_executable(gameboy_snes_dsp_audio_engine_tests
        tests/snes_dsp_audio_engine_tests.cpp)
    target_link_libraries(gameboy_snes_dsp_audio_engine_tests PRIVATE gameboy_core)
    add_test(NAME gameboy_snes_dsp_audio_engine_contract
             COMMAND gameboy_snes_dsp_audio_engine_tests)
    add_executable(gameboy_snes_apu_audio_engine_tests tests/snes_apu_audio_engine_tests.cpp)
    target_link_libraries(gameboy_snes_apu_audio_engine_tests PRIVATE gameboy_core)
    add_test(NAME gameboy_snes_apu_audio_engine_contract COMMAND gameboy_snes_apu_audio_engine_tests)

    add_executable(gameboy_snes_dsp_pcm_fixture_runner
        tests/support/snes_dsp_pcm_fixture_runner.cpp)
    target_include_directories(gameboy_snes_dsp_pcm_fixture_runner PRIVATE tests/support)
    target_link_libraries(gameboy_snes_dsp_pcm_fixture_runner PRIVATE gameboy_core)
    if(Python3_Interpreter_FOUND)
        add_test(NAME gameboy_snes_apu_audio_engine_pcm
                 COMMAND ${Python3_EXECUTABLE}
                         ${CMAKE_CURRENT_SOURCE_DIR}/tests/snes_apu_audio_engine_pcm_tests.py
                         $<TARGET_FILE:gameboy_snes_apu_audio_engine_tests>
                         $<TARGET_FILE:gameboy_snes_dsp_pcm_fixture_runner>)
    endif()
    add_executable(gameboy_snes_spc700_dsp_timeline_tests
        tests/snes_spc700_dsp_timeline_tests.cpp)
    target_link_libraries(gameboy_snes_spc700_dsp_timeline_tests PRIVATE gameboy_core)
    add_test(NAME gameboy_snes_spc700_dsp_timeline_contract
             COMMAND gameboy_snes_spc700_dsp_timeline_tests)
    add_executable(gameboy_snes_spc700_host_sound_tests
        tests/snes_spc700_host_sound_tests.cpp)
    target_link_libraries(gameboy_snes_spc700_host_sound_tests PRIVATE gameboy_core)
    add_test(NAME gameboy_snes_spc700_host_sound_contract
             COMMAND gameboy_snes_spc700_host_sound_tests)
    add_executable(gameboy_snes_spc700_ipl_probe
        tests/support/snes_spc700_ipl_probe.cpp)
    target_link_libraries(gameboy_snes_spc700_ipl_probe PRIVATE gameboy_core)
    add_executable(gameboy_sgb_score_controls_probe tests/sgb_score_controls_probe.cpp)
    target_link_libraries(gameboy_sgb_score_controls_probe PRIVATE gameboy_core)
    add_executable(gameboy_sgb_score_gate_probe tests/sgb_score_gate_probe.cpp)
    target_link_libraries(gameboy_sgb_score_gate_probe PRIVATE gameboy_core)
    add_executable(gameboy_sgb_score_render_probe tests/sgb_score_render_probe.cpp)
    target_link_libraries(gameboy_sgb_score_render_probe PRIVATE gameboy_core)
    add_executable(gameboy_sgb_score_track_probe tests/sgb_score_track_probe.cpp)
    target_link_libraries(gameboy_sgb_score_track_probe PRIVATE gameboy_core)
    add_executable(gameboy_sgb_score_pair_probe tests/sgb_score_track_probe.cpp)
    target_compile_definitions(gameboy_sgb_score_pair_probe PRIVATE GBB_SCORE_PAIR_PROBE)
    target_link_libraries(gameboy_sgb_score_pair_probe PRIVATE gameboy_core)
    add_executable(gameboy_sgb_score_clock_probe tests/sgb_score_clock_probe.cpp)
    target_link_libraries(gameboy_sgb_score_clock_probe PRIVATE gameboy_core)
    if(Python3_Interpreter_FOUND)
        add_test(NAME gameboy_sgb_native_score_controls
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/sgb_score_controls_tests.py
                --probe $<TARGET_FILE:gameboy_sgb_score_controls_probe>)
        add_test(NAME gameboy_sgb_native_score_pair
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/sgb_score_pair_tests.py
                --probe $<TARGET_FILE:gameboy_sgb_score_pair_probe>)
        add_test(NAME gameboy_sgb_expanded_native_score_gate
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/sgb_expanded_score_gate_tests.py
                --probe $<TARGET_FILE:gameboy_sgb_score_gate_probe>)
        add_test(NAME gameboy_sgb_native_score_gate
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/sgb_score_gate_tests.py
                --probe $<TARGET_FILE:gameboy_sgb_score_gate_probe>)
        add_test(NAME gameboy_sgb_native_score_render
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/sgb_score_render_tests.py
                --probe $<TARGET_FILE:gameboy_sgb_score_render_probe>)
        add_test(NAME gameboy_sgb_native_score_track
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/sgb_score_track_tests.py
                --probe $<TARGET_FILE:gameboy_sgb_score_track_probe>)
        add_test(NAME gameboy_sgb_native_score_clock
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/sgb_score_clock_tests.py
                --probe $<TARGET_FILE:gameboy_sgb_score_clock_probe>)
    endif()
    add_executable(gameboy_spc700_replacement_ipl_tests tests/spc700_replacement_ipl_tests.cpp)
    target_link_libraries(gameboy_spc700_replacement_ipl_tests PRIVATE gameboy_core)
    add_test(NAME gameboy_spc700_replacement_ipl_contract COMMAND gameboy_spc700_replacement_ipl_tests)
    if(Python3_Interpreter_FOUND)
        add_test(NAME gameboy_spc700_replacement_ipl_reproducible
                 COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/scripts/build_spc700_ipl.py --check)
        add_test(NAME gameboy_spc700_replacement_ipl_pcm
                 COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/spc700_replacement_ipl_pcm_tests.py
                         $<TARGET_FILE:gameboy_snes_spc700_ipl_probe>
                         $<TARGET_FILE:gameboy_snes_spc700_dsp_timeline_tests>
                         $<TARGET_FILE:gameboy_snes_dsp_pcm_fixture_runner>)
    endif()
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/roms/spc700.rom")
        add_test(NAME gameboy_spc700_replacement_local_reference_contract
                 COMMAND gameboy_spc700_replacement_ipl_tests ${CMAKE_CURRENT_SOURCE_DIR}/roms/spc700.rom)
    endif()
    add_executable(gameboy_snes_65c816_apu_trace
        tests/support/snes_65c816_apu_trace.cpp
        tests/support/snes_icd_gb_source.cpp
        tests/support/snes_65c816_trace_cpu.cpp)
    target_include_directories(gameboy_snes_65c816_apu_trace PRIVATE tests/support scripts)
    target_link_libraries(gameboy_snes_65c816_apu_trace PRIVATE gameboy_core)
    add_executable(gameboy_sgb_score_table_read_tests tests/sgb_score_table_read_tests.cpp)
    target_link_libraries(gameboy_sgb_score_table_read_tests PRIVATE gameboy_core)
    if(Python3_Interpreter_FOUND)
        add_test(NAME gameboy_sgb_score_table_reads
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/sgb_score_table_read_tests.py
                --runner $<TARGET_FILE:gameboy_sgb_score_table_read_tests>
                --trace $<TARGET_FILE:gameboy_snes_65c816_apu_trace>)
        if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/roms/sgb1.program.rom" AND
           EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/roms/sgb2.program.rom" AND
           EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/roms/spc700.rom")
            add_test(NAME gameboy_sgb_score_pair_local_reference
                COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/scripts/check_sgb_score_pair_reference.py
                    --probe $<TARGET_FILE:gameboy_sgb_score_pair_probe>
                    --trace $<TARGET_FILE:gameboy_snes_65c816_apu_trace>
                    --firmware-dir ${CMAKE_CURRENT_SOURCE_DIR}/roms)
            set_tests_properties(gameboy_sgb_score_pair_local_reference PROPERTIES
                LABELS "local;private-reference" TIMEOUT 1440)
            add_test(NAME gameboy_sgb_song_selection_local_reference
                COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/scripts/check_sgb_song_selection_reference.py
                    --trace $<TARGET_FILE:gameboy_snes_65c816_apu_trace>
                    --firmware-dir ${CMAKE_CURRENT_SOURCE_DIR}/roms --order 3,1,2,2,1,3)
            set_tests_properties(gameboy_sgb_song_selection_local_reference PROPERTIES
                LABELS "local;private-reference" TIMEOUT 420)
            add_test(NAME gameboy_sgb_pitch_local_reference
                COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/scripts/check_sgb_pitch_reference.py
                    --trace $<TARGET_FILE:gameboy_snes_65c816_apu_trace>
                    --firmware-dir ${CMAKE_CURRENT_SOURCE_DIR}/roms)
            set_tests_properties(gameboy_sgb_pitch_local_reference PROPERTIES
                LABELS "local;private-reference" TIMEOUT 840)
            add_test(NAME gameboy_sgb_gate_timing_local_reference
                COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/scripts/check_sgb_gate_timing_reference.py
                    --trace $<TARGET_FILE:gameboy_snes_65c816_apu_trace>
                    --firmware-dir ${CMAKE_CURRENT_SOURCE_DIR}/roms)
            set_tests_properties(gameboy_sgb_gate_timing_local_reference PROPERTIES
                LABELS "local;private-reference" TIMEOUT 3000)
            add_test(NAME gameboy_sgb_score_controls_local_reference
                COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/scripts/check_sgb_score_controls_reference.py
                    --probe $<TARGET_FILE:gameboy_sgb_score_controls_probe>
                    --trace $<TARGET_FILE:gameboy_snes_65c816_apu_trace>
                    --firmware-dir ${CMAKE_CURRENT_SOURCE_DIR}/roms)
            set_tests_properties(gameboy_sgb_score_controls_local_reference PROPERTIES
                LABELS "local;private-reference" TIMEOUT 1800)
            add_test(NAME gameboy_sgb_expanded_score_gate_local_reference
                COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/scripts/check_sgb_expanded_score_gate_reference.py
                    --probe $<TARGET_FILE:gameboy_sgb_score_gate_probe>
                    --trace $<TARGET_FILE:gameboy_snes_65c816_apu_trace>
                    --firmware-dir ${CMAKE_CURRENT_SOURCE_DIR}/roms)
            set_tests_properties(gameboy_sgb_expanded_score_gate_local_reference PROPERTIES
                LABELS "local;private-reference" TIMEOUT 4500)
            add_test(NAME gameboy_sgb_score_gate_local_reference
                COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/scripts/check_sgb_score_gate_reference.py
                    --probe $<TARGET_FILE:gameboy_sgb_score_gate_probe>
                    --trace $<TARGET_FILE:gameboy_snes_65c816_apu_trace>
                    --firmware-dir ${CMAKE_CURRENT_SOURCE_DIR}/roms)
            set_tests_properties(gameboy_sgb_score_gate_local_reference PROPERTIES
                LABELS "local;private-reference" TIMEOUT 1200)
            add_test(NAME gameboy_sgb_score_render_local_reference
                COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/scripts/check_sgb_score_render_reference.py
                    --probe $<TARGET_FILE:gameboy_sgb_score_render_probe>
                    --trace $<TARGET_FILE:gameboy_snes_65c816_apu_trace>
                    --firmware-dir ${CMAKE_CURRENT_SOURCE_DIR}/roms)
            set_tests_properties(gameboy_sgb_score_render_local_reference PROPERTIES
                LABELS "local;private-reference" TIMEOUT 1200)
            add_test(NAME gameboy_sgb_score_track_local_reference
                COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/scripts/check_sgb_score_track_reference.py
                    --probe $<TARGET_FILE:gameboy_sgb_score_track_probe>
                    --trace $<TARGET_FILE:gameboy_snes_65c816_apu_trace>
                    --firmware-dir ${CMAKE_CURRENT_SOURCE_DIR}/roms)
            set_tests_properties(gameboy_sgb_score_track_local_reference PROPERTIES
                LABELS "local;private-reference" TIMEOUT 1200)
            add_test(NAME gameboy_sgb_score_clock_local_reference
                COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/scripts/check_sgb_score_clock_reference.py
                    --probe $<TARGET_FILE:gameboy_sgb_score_clock_probe>
                    --trace $<TARGET_FILE:gameboy_snes_65c816_apu_trace>
                    --firmware-dir ${CMAKE_CURRENT_SOURCE_DIR}/roms)
            set_tests_properties(gameboy_sgb_score_clock_local_reference PROPERTIES
                LABELS "local;private-reference" TIMEOUT 1200)
            add_test(NAME gameboy_sgb_timing_local_reference
                COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/scripts/check_sgb_timing_reference.py
                    --trace $<TARGET_FILE:gameboy_snes_65c816_apu_trace>
                    --firmware-dir ${CMAKE_CURRENT_SOURCE_DIR}/roms)
            set_tests_properties(gameboy_sgb_timing_local_reference PROPERTIES
                LABELS "local;private-reference" TIMEOUT 1200)
            add_test(NAME gameboy_sgb_volume_pan_local_reference
                COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/scripts/check_sgb_volume_pan_reference.py
                    --trace $<TARGET_FILE:gameboy_snes_65c816_apu_trace>
                    --firmware-dir ${CMAKE_CURRENT_SOURCE_DIR}/roms)
            set_tests_properties(gameboy_sgb_volume_pan_local_reference PROPERTIES
                LABELS "local;private-reference" TIMEOUT 840)
            add_test(NAME gameboy_sgb_echo_local_reference
                COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/scripts/check_sgb_echo_reference.py
                    --trace $<TARGET_FILE:gameboy_snes_65c816_apu_trace>
                    --firmware-dir ${CMAKE_CURRENT_SOURCE_DIR}/roms)
            set_tests_properties(gameboy_sgb_echo_local_reference PROPERTIES
                LABELS "local;private-reference" TIMEOUT 840)
            add_test(NAME gameboy_sgb_phrase_local_reference
                COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/scripts/check_sgb_phrase_reference.py
                    --trace $<TARGET_FILE:gameboy_snes_65c816_apu_trace>
                    --firmware-dir ${CMAKE_CURRENT_SOURCE_DIR}/roms)
            set_tests_properties(gameboy_sgb_phrase_local_reference PROPERTIES
                LABELS "local;private-reference" TIMEOUT 1200)
            add_test(NAME gameboy_sgb_subroutine_local_reference
                COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/scripts/check_sgb_subroutine_reference.py
                    --trace $<TARGET_FILE:gameboy_snes_65c816_apu_trace>
                    --firmware-dir ${CMAKE_CURRENT_SOURCE_DIR}/roms)
            set_tests_properties(gameboy_sgb_subroutine_local_reference PROPERTIES
                LABELS "local;private-reference" TIMEOUT 1200)
            add_test(NAME gameboy_sgb_scheduler_local_reference
                COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/scripts/check_sgb_scheduler_reference.py
                    --trace $<TARGET_FILE:gameboy_snes_65c816_apu_trace>
                    --firmware-dir ${CMAKE_CURRENT_SOURCE_DIR}/roms)
            set_tests_properties(gameboy_sgb_scheduler_local_reference PROPERTIES
                LABELS "local;private-reference" TIMEOUT 2400)
        endif()
    endif()
    add_executable(gameboy_snes_icd_gb_title_probe
        tests/support/snes_icd_gb_title_probe.cpp
        tests/support/snes_icd_gb_source.cpp)
    target_include_directories(gameboy_snes_icd_gb_title_probe PRIVATE tests/support scripts)
    target_link_libraries(gameboy_snes_icd_gb_title_probe PRIVATE gameboy_core)
    add_executable(gameboy_sgb_frame_input_source_probe
        tests/sgb_frame_input_source_tests.cpp tests/support/snes_icd_gb_source.cpp)
    target_include_directories(gameboy_sgb_frame_input_source_probe PRIVATE tests/support scripts)
    target_link_libraries(gameboy_sgb_frame_input_source_probe PRIVATE gameboy_core)
    add_executable(gameboy_snes_65c816_trace_tests
        tests/snes_65c816_trace_tests.cpp
        tests/support/snes_65c816_trace_cpu.cpp)
    target_include_directories(gameboy_snes_65c816_trace_tests PRIVATE tests/support)
    target_link_libraries(gameboy_snes_65c816_trace_tests PRIVATE gameboy_core)
    add_test(NAME gameboy_snes_65c816_trace_contract
             COMMAND gameboy_snes_65c816_trace_tests)
    add_executable(gameboy_snes_65c816_sgb_sound_sync_tests
        tests/snes_65c816_sgb_sound_sync_tests.cpp
        tests/support/snes_65c816_trace_cpu.cpp)
    target_include_directories(gameboy_snes_65c816_sgb_sound_sync_tests PRIVATE tests/support)
    target_link_libraries(gameboy_snes_65c816_sgb_sound_sync_tests PRIVATE gameboy_core)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/roms/sgb1.program.rom")
        add_test(NAME gameboy_snes_65c816_local_sgb1_apu_trace
                 COMMAND gameboy_snes_65c816_trace_tests
                         --local-sgb1 ${CMAKE_CURRENT_SOURCE_DIR}/roms/sgb1.program.rom)
        if(Python3_Interpreter_FOUND AND
           EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/roms/spc700.rom")
            add_test(NAME gameboy_snes_65c816_local_sgb1_first_upload
                     COMMAND ${Python3_EXECUTABLE}
                             ${CMAKE_CURRENT_SOURCE_DIR}/tests/snes_65c816_local_upload_tests.py
                             $<TARGET_FILE:gameboy_snes_65c816_apu_trace>
                             ${CMAKE_CURRENT_SOURCE_DIR}/roms/sgb1.program.rom
                             ${CMAKE_CURRENT_SOURCE_DIR}/roms/spc700.rom)
            add_test(NAME gameboy_snes_65c816_local_sgb1_upload_boot
                     COMMAND ${Python3_EXECUTABLE}
                             ${CMAKE_CURRENT_SOURCE_DIR}/tests/snes_65c816_local_upload_tests.py
                             $<TARGET_FILE:gameboy_snes_65c816_apu_trace>
                             ${CMAKE_CURRENT_SOURCE_DIR}/roms/sgb1.program.rom
                             ${CMAKE_CURRENT_SOURCE_DIR}/roms/spc700.rom --boot)
            add_test(NAME gameboy_snes_65c816_local_sgb1_driver_dsp
                     COMMAND ${Python3_EXECUTABLE}
                             ${CMAKE_CURRENT_SOURCE_DIR}/tests/snes_65c816_local_upload_tests.py
                             $<TARGET_FILE:gameboy_snes_65c816_apu_trace>
                             ${CMAKE_CURRENT_SOURCE_DIR}/roms/sgb1.program.rom
                             ${CMAKE_CURRENT_SOURCE_DIR}/roms/spc700.rom --driver)
            add_test(NAME gameboy_snes_65c816_local_sgb1_sync_probe
                     COMMAND ${Python3_EXECUTABLE}
                             ${CMAKE_CURRENT_SOURCE_DIR}/tests/snes_65c816_local_sync_probe_tests.py
                             $<TARGET_FILE:gameboy_snes_65c816_apu_trace>
                             ${CMAKE_CURRENT_SOURCE_DIR}/roms/sgb1.program.rom
                             ${CMAKE_CURRENT_SOURCE_DIR}/roms/spc700.rom sgb1)
        endif()
    endif()
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/roms/sgb2.program.rom")
        add_test(NAME gameboy_snes_65c816_local_sgb2_apu_trace
                 COMMAND gameboy_snes_65c816_trace_tests
                         --local-sgb2 ${CMAKE_CURRENT_SOURCE_DIR}/roms/sgb2.program.rom)
        if(Python3_Interpreter_FOUND AND
           EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/roms/spc700.rom")
            add_test(NAME gameboy_snes_65c816_local_sgb2_first_upload
                     COMMAND ${Python3_EXECUTABLE}
                             ${CMAKE_CURRENT_SOURCE_DIR}/tests/snes_65c816_local_upload_tests.py
                             $<TARGET_FILE:gameboy_snes_65c816_apu_trace>
                             ${CMAKE_CURRENT_SOURCE_DIR}/roms/sgb2.program.rom
                             ${CMAKE_CURRENT_SOURCE_DIR}/roms/spc700.rom)
            add_test(NAME gameboy_snes_65c816_local_sgb2_upload_boot
                     COMMAND ${Python3_EXECUTABLE}
                             ${CMAKE_CURRENT_SOURCE_DIR}/tests/snes_65c816_local_upload_tests.py
                             $<TARGET_FILE:gameboy_snes_65c816_apu_trace>
                             ${CMAKE_CURRENT_SOURCE_DIR}/roms/sgb2.program.rom
                             ${CMAKE_CURRENT_SOURCE_DIR}/roms/spc700.rom --boot)
            add_test(NAME gameboy_snes_65c816_local_sgb2_driver_dsp
                     COMMAND ${Python3_EXECUTABLE}
                             ${CMAKE_CURRENT_SOURCE_DIR}/tests/snes_65c816_local_upload_tests.py
                             $<TARGET_FILE:gameboy_snes_65c816_apu_trace>
                             ${CMAKE_CURRENT_SOURCE_DIR}/roms/sgb2.program.rom
                             ${CMAKE_CURRENT_SOURCE_DIR}/roms/spc700.rom --driver)
            add_test(NAME gameboy_snes_65c816_local_sgb2_sync_probe
                     COMMAND ${Python3_EXECUTABLE}
                             ${CMAKE_CURRENT_SOURCE_DIR}/tests/snes_65c816_local_sync_probe_tests.py
                             $<TARGET_FILE:gameboy_snes_65c816_apu_trace>
                             ${CMAKE_CURRENT_SOURCE_DIR}/roms/sgb2.program.rom
                             ${CMAKE_CURRENT_SOURCE_DIR}/roms/spc700.rom sgb2)
            if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/roms/sgb2.boot.rom" AND
               EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/roms/Donkey Kong (JU) (V1.1) [S][!].gb")
                if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/roms/sgb1.program.rom" AND
                   EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/roms/sgb.boot.rom")
                    add_test(NAME gameboy_snes_apu_audio_engine_local_titles
                             COMMAND ${Python3_EXECUTABLE}
                                     ${CMAKE_CURRENT_SOURCE_DIR}/tests/snes_apu_audio_engine_local_title_tests.py
                                     $<TARGET_FILE:gameboy_snes_65c816_apu_trace>
                                     ${CMAKE_CURRENT_SOURCE_DIR}/roms
                                     ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/sgb/titles/donkey-kong-gameplay.script)
                    set_tests_properties(gameboy_snes_apu_audio_engine_local_titles PROPERTIES TIMEOUT 1000)
                endif()
                add_test(NAME gameboy_snes_icd_gb_local_donkey_audible_title
                         COMMAND ${Python3_EXECUTABLE}
                                 ${CMAKE_CURRENT_SOURCE_DIR}/tests/snes_icd_gb_local_title_tests.py
                                 $<TARGET_FILE:gameboy_snes_icd_gb_title_probe>
                                 "${CMAKE_CURRENT_SOURCE_DIR}/roms/Donkey Kong (JU) (V1.1) [S][!].gb"
                                 ${CMAKE_CURRENT_SOURCE_DIR}/roms/sgb2.boot.rom
                                 ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/sgb/titles/donkey-kong-gameplay.script)
                add_test(NAME gameboy_snes_65c816_local_icd_sound_handoff
                         COMMAND ${Python3_EXECUTABLE}
                                 ${CMAKE_CURRENT_SOURCE_DIR}/tests/snes_65c816_local_icd_sound_tests.py
                                 $<TARGET_FILE:gameboy_snes_65c816_apu_trace>
                                 ${CMAKE_CURRENT_SOURCE_DIR}/roms/sgb2.program.rom
                                 ${CMAKE_CURRENT_SOURCE_DIR}/roms/spc700.rom
                                 "${CMAKE_CURRENT_SOURCE_DIR}/roms/Donkey Kong (JU) (V1.1) [S][!].gb"
                                 ${CMAKE_CURRENT_SOURCE_DIR}/roms/sgb2.boot.rom)
                add_test(NAME gameboy_snes_65c816_local_title_sound_pcm
                         COMMAND ${Python3_EXECUTABLE}
                                 ${CMAKE_CURRENT_SOURCE_DIR}/tests/snes_65c816_local_title_sound_tests.py
                                 $<TARGET_FILE:gameboy_snes_65c816_apu_trace>
                                 ${CMAKE_CURRENT_SOURCE_DIR}/roms/sgb2.program.rom
                                 ${CMAKE_CURRENT_SOURCE_DIR}/roms/spc700.rom
                                 "${CMAKE_CURRENT_SOURCE_DIR}/roms/Donkey Kong (JU) (V1.1) [S][!].gb"
                                 ${CMAKE_CURRENT_SOURCE_DIR}/roms/sgb2.boot.rom
                                 ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/sgb/titles/donkey-kong-gameplay.script)
                add_test(NAME gameboy_snes_65c816_local_title_bus_clocked_pcm
                         COMMAND ${Python3_EXECUTABLE}
                                 ${CMAKE_CURRENT_SOURCE_DIR}/tests/snes_65c816_local_title_sound_tests.py
                                 $<TARGET_FILE:gameboy_snes_65c816_apu_trace>
                                 ${CMAKE_CURRENT_SOURCE_DIR}/roms/sgb2.program.rom
                                 ${CMAKE_CURRENT_SOURCE_DIR}/roms/spc700.rom
                                 "${CMAKE_CURRENT_SOURCE_DIR}/roms/Donkey Kong (JU) (V1.1) [S][!].gb"
                                 ${CMAKE_CURRENT_SOURCE_DIR}/roms/sgb2.boot.rom
                                 ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/sgb/titles/donkey-kong-gameplay.script
                                 --bus-clocked-dsp)
                add_test(NAME gameboy_snes_65c816_local_title_cycle_bus_pcm
                         COMMAND ${Python3_EXECUTABLE}
                                 ${CMAKE_CURRENT_SOURCE_DIR}/tests/snes_65c816_local_title_sound_tests.py
                                 $<TARGET_FILE:gameboy_snes_65c816_apu_trace>
                                 ${CMAKE_CURRENT_SOURCE_DIR}/roms/sgb2.program.rom
                                 ${CMAKE_CURRENT_SOURCE_DIR}/roms/spc700.rom
                                 "${CMAKE_CURRENT_SOURCE_DIR}/roms/Donkey Kong (JU) (V1.1) [S][!].gb"
                                 ${CMAKE_CURRENT_SOURCE_DIR}/roms/sgb2.boot.rom
                                 ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/sgb/titles/donkey-kong-gameplay.script
                                 --cycle-bus-dsp)
                add_test(NAME gameboy_snes_65c816_local_title_shared_bus_pcm
                         COMMAND ${Python3_EXECUTABLE}
                                 ${CMAKE_CURRENT_SOURCE_DIR}/tests/snes_65c816_local_title_sound_tests.py
                                 $<TARGET_FILE:gameboy_snes_65c816_apu_trace>
                                 ${CMAKE_CURRENT_SOURCE_DIR}/roms/sgb2.program.rom
                                 ${CMAKE_CURRENT_SOURCE_DIR}/roms/spc700.rom
                                 "${CMAKE_CURRENT_SOURCE_DIR}/roms/Donkey Kong (JU) (V1.1) [S][!].gb"
                                 ${CMAKE_CURRENT_SOURCE_DIR}/roms/sgb2.boot.rom
                                 ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/sgb/titles/donkey-kong-gameplay.script
                                 --shared-bus-dsp)
                add_test(NAME gameboy_snes_65c816_local_title_cycle_apu_pcm
                         COMMAND ${Python3_EXECUTABLE}
                                 ${CMAKE_CURRENT_SOURCE_DIR}/tests/snes_65c816_local_title_sound_tests.py
                                 $<TARGET_FILE:gameboy_snes_65c816_apu_trace>
                                 ${CMAKE_CURRENT_SOURCE_DIR}/roms/sgb2.program.rom
                                 ${CMAKE_CURRENT_SOURCE_DIR}/roms/spc700.rom
                                 "${CMAKE_CURRENT_SOURCE_DIR}/roms/Donkey Kong (JU) (V1.1) [S][!].gb"
                                 ${CMAKE_CURRENT_SOURCE_DIR}/roms/sgb2.boot.rom
                                 ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/sgb/titles/donkey-kong-gameplay.script
                                 --cycle-apu-sync)
                add_test(NAME gameboy_snes_65c816_local_title_fractional_apu_pcm
                         COMMAND ${Python3_EXECUTABLE}
                                 ${CMAKE_CURRENT_SOURCE_DIR}/tests/snes_65c816_local_title_sound_tests.py
                                 $<TARGET_FILE:gameboy_snes_65c816_apu_trace>
                                 ${CMAKE_CURRENT_SOURCE_DIR}/roms/sgb2.program.rom
                                 ${CMAKE_CURRENT_SOURCE_DIR}/roms/spc700.rom
                                 "${CMAKE_CURRENT_SOURCE_DIR}/roms/Donkey Kong (JU) (V1.1) [S][!].gb"
                                 ${CMAKE_CURRENT_SOURCE_DIR}/roms/sgb2.boot.rom
                                 ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/sgb/titles/donkey-kong-gameplay.script
                                 --fractional-apu-sync)
                add_test(NAME gameboy_sgb_local_native_frame_replay
                         COMMAND ${Python3_EXECUTABLE}
                                 ${CMAKE_CURRENT_SOURCE_DIR}/tests/sgb_native_frame_replay_local_tests.py
                                 $<TARGET_FILE:gameboy_snes_65c816_apu_trace>
                                 ${CMAKE_CURRENT_SOURCE_DIR}/roms/sgb2.program.rom
                                 ${CMAKE_CURRENT_SOURCE_DIR}/roms/spc700.rom
                                 "${CMAKE_CURRENT_SOURCE_DIR}/roms/Donkey Kong (JU) (V1.1) [S][!].gb"
                                 ${CMAKE_CURRENT_SOURCE_DIR}/roms/sgb2.boot.rom
                                 ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/sgb/titles/donkey-kong-gameplay.script)
                add_test(NAME gameboy_sgb_local_host_startup
                         COMMAND ${Python3_EXECUTABLE}
                                 ${CMAKE_CURRENT_SOURCE_DIR}/tests/sgb_host_startup_local_tests.py
                                 $<TARGET_FILE:gameboy_snes_65c816_apu_trace>
                                 ${CMAKE_CURRENT_SOURCE_DIR}/roms/sgb2.program.rom
                                 ${CMAKE_CURRENT_SOURCE_DIR}/roms/spc700.rom
                                 "${CMAKE_CURRENT_SOURCE_DIR}/roms/Donkey Kong (JU) (V1.1) [S][!].gb"
                                 ${CMAKE_CURRENT_SOURCE_DIR}/roms/sgb2.boot.rom)
                add_test(NAME gameboy_sgb_local_native_ppu_dma_replay
                         COMMAND ${Python3_EXECUTABLE}
                                 ${CMAKE_CURRENT_SOURCE_DIR}/tests/sgb_native_frame_replay_local_tests.py
                                 $<TARGET_FILE:gameboy_snes_65c816_apu_trace>
                                 ${CMAKE_CURRENT_SOURCE_DIR}/roms/sgb2.program.rom
                                 ${CMAKE_CURRENT_SOURCE_DIR}/roms/spc700.rom
                                 "${CMAKE_CURRENT_SOURCE_DIR}/roms/Donkey Kong (JU) (V1.1) [S][!].gb"
                                 ${CMAKE_CURRENT_SOURCE_DIR}/roms/sgb2.boot.rom
                                 ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/sgb/titles/donkey-kong-gameplay.script
                                 --ppu-dma-timing)
                foreach(profile IN ITEMS host_bus reference_clock)
                    set(sgb_clock_options)
                    if(profile STREQUAL "reference_clock")
                        set(sgb_clock_options --apu-clock-hz 1025280)
                    endif()
                    add_test(NAME gameboy_sgb_local_native_${profile}_replay
                             COMMAND ${Python3_EXECUTABLE}
                                     ${CMAKE_CURRENT_SOURCE_DIR}/tests/sgb_native_frame_replay_local_tests.py
                                     $<TARGET_FILE:gameboy_snes_65c816_apu_trace>
                                     ${CMAKE_CURRENT_SOURCE_DIR}/roms/sgb2.program.rom
                                     ${CMAKE_CURRENT_SOURCE_DIR}/roms/spc700.rom
                                     "${CMAKE_CURRENT_SOURCE_DIR}/roms/Donkey Kong (JU) (V1.1) [S][!].gb"
                                     ${CMAKE_CURRENT_SOURCE_DIR}/roms/sgb2.boot.rom
                                     ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/sgb/titles/donkey-kong-gameplay.script
                                     --ppu-dma-timing --host-bus-timing ${sgb_clock_options})
                endforeach()
            endif()
        endif()
    endif()
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/roms/spc700.rom")
        add_test(NAME gameboy_snes_spc700_local_ipl_upload
                 COMMAND gameboy_snes_spc700_ipl_probe
                         ${CMAKE_CURRENT_SOURCE_DIR}/roms/spc700.rom
                         --upload-smoke)
        if(Python3_Interpreter_FOUND)
            add_test(NAME gameboy_snes_spc700_local_ipl_pcm
                     COMMAND ${Python3_EXECUTABLE}
                             ${CMAKE_CURRENT_SOURCE_DIR}/tests/snes_spc700_real_ipl_pcm_tests.py
                             $<TARGET_FILE:gameboy_snes_spc700_ipl_probe>
                             ${CMAKE_CURRENT_SOURCE_DIR}/roms/spc700.rom
                             $<TARGET_FILE:gameboy_snes_spc700_dsp_timeline_tests>
                             $<TARGET_FILE:gameboy_snes_dsp_pcm_fixture_runner>)
        endif()
    endif()
    if(Python3_Interpreter_FOUND)
        add_test(NAME gameboy_snes_spc700_ipl_probe_contract
                 COMMAND ${Python3_EXECUTABLE}
                         ${CMAKE_CURRENT_SOURCE_DIR}/tests/snes_spc700_ipl_probe_tests.py
                         $<TARGET_FILE:gameboy_snes_spc700_ipl_probe>)
        add_test(NAME gameboy_snes_spc700_host_sound_pcm
                 COMMAND ${Python3_EXECUTABLE}
                         ${CMAKE_CURRENT_SOURCE_DIR}/tests/snes_spc700_host_sound_pcm_tests.py
                         $<TARGET_FILE:gameboy_snes_spc700_host_sound_tests>
                         $<TARGET_FILE:gameboy_snes_spc700_dsp_timeline_tests>
                         $<TARGET_FILE:gameboy_snes_dsp_pcm_fixture_runner>)
        add_test(NAME gameboy_snes_65c816_sgb_sound_sync_pcm
                 COMMAND ${Python3_EXECUTABLE}
                         ${CMAKE_CURRENT_SOURCE_DIR}/tests/snes_65c816_sgb_sound_sync_pcm_tests.py
                         $<TARGET_FILE:gameboy_snes_65c816_sgb_sound_sync_tests>
                         $<TARGET_FILE:gameboy_snes_spc700_dsp_timeline_tests>
                         $<TARGET_FILE:gameboy_snes_dsp_pcm_fixture_runner>)
        add_test(NAME gameboy_snes_dsp_pcm_compare_tool
                 COMMAND ${Python3_EXECUTABLE}
                         ${CMAKE_CURRENT_SOURCE_DIR}/tests/snes_dsp_pcm_compare_tool_tests.py
                         $<TARGET_FILE:gameboy_snes_dsp_pcm_fixture_runner>)
        add_test(NAME gameboy_snes_spc700_dsp_timeline_pcm
                 COMMAND ${Python3_EXECUTABLE}
                         ${CMAKE_CURRENT_SOURCE_DIR}/tests/snes_spc700_dsp_timeline_pcm_tests.py
                         $<TARGET_FILE:gameboy_snes_spc700_dsp_timeline_tests>
                         $<TARGET_FILE:gameboy_snes_dsp_pcm_fixture_runner>)
        add_test(NAME gameboy_snes_spc700_dsp_subsample_pcm
                 COMMAND ${Python3_EXECUTABLE}
                         ${CMAKE_CURRENT_SOURCE_DIR}/tests/snes_spc700_dsp_subsample_pcm_tests.py
                         $<TARGET_FILE:gameboy_snes_spc700_dsp_timeline_tests>
                         $<TARGET_FILE:gameboy_snes_dsp_pcm_fixture_runner>)
        add_test(NAME gameboy_snes_spc700_dsp_echo_subsample_pcm
                 COMMAND ${Python3_EXECUTABLE}
                         ${CMAKE_CURRENT_SOURCE_DIR}/tests/snes_spc700_dsp_echo_subsample_pcm_tests.py
                         $<TARGET_FILE:gameboy_snes_spc700_dsp_timeline_tests>
                         $<TARGET_FILE:gameboy_snes_dsp_pcm_fixture_runner>)
        add_test(NAME gameboy_snes_spc700_dsp_key_subsample_pcm
                 COMMAND ${Python3_EXECUTABLE}
                         ${CMAKE_CURRENT_SOURCE_DIR}/tests/snes_spc700_dsp_key_subsample_pcm_tests.py
                         $<TARGET_FILE:gameboy_snes_spc700_dsp_timeline_tests>
                         $<TARGET_FILE:gameboy_snes_dsp_pcm_fixture_runner>)
        add_test(NAME gameboy_snes_dsp_second_keyon_phase
                 COMMAND ${Python3_EXECUTABLE}
                         ${CMAKE_CURRENT_SOURCE_DIR}/tests/snes_dsp_second_keyon_phase_tests.py
                         $<TARGET_FILE:gameboy_snes_dsp_pcm_fixture_runner>)
        add_test(NAME gameboy_snes_dsp_shared_bus
                 COMMAND ${Python3_EXECUTABLE}
                         ${CMAKE_CURRENT_SOURCE_DIR}/tests/snes_dsp_shared_bus_tests.py
                         $<TARGET_FILE:gameboy_snes_dsp_pcm_fixture_runner>)
        add_test(NAME gameboy_snes_spc_write_cycle
                 COMMAND ${Python3_EXECUTABLE}
                         ${CMAKE_CURRENT_SOURCE_DIR}/tests/snes_spc_write_cycle_tests.py
                         $<TARGET_FILE:gameboy_snes_spc_write_fixture_runner>)
        add_test(NAME gameboy_snes_spc_bus_cycle
                 COMMAND ${Python3_EXECUTABLE}
                         ${CMAKE_CURRENT_SOURCE_DIR}/tests/snes_spc_bus_cycle_tests.py
                         $<TARGET_FILE:gameboy_snes_spc_write_fixture_runner>)
        add_test(NAME gameboy_snes_spc_resumable_cycle
                 COMMAND ${Python3_EXECUTABLE}
                         ${CMAKE_CURRENT_SOURCE_DIR}/tests/snes_spc_bus_cycle_tests.py
                         $<TARGET_FILE:gameboy_snes_spc_write_fixture_runner>
                         --resumable)
        add_test(NAME gameboy_snes_spc_half_cycle
                 COMMAND ${Python3_EXECUTABLE}
                         ${CMAKE_CURRENT_SOURCE_DIR}/tests/snes_spc_half_cycle_tests.py
                         $<TARGET_FILE:gameboy_snes_spc_write_fixture_runner>)
        add_test(NAME gameboy_sgb_apu_bus_comparison
                 COMMAND ${Python3_EXECUTABLE}
                         ${CMAKE_CURRENT_SOURCE_DIR}/tests/sgb_apu_bus_comparison_tests.py)
        add_test(NAME gameboy_sgb_timer_poll_comparison
                 COMMAND ${Python3_EXECUTABLE}
                         ${CMAKE_CURRENT_SOURCE_DIR}/tests/sgb_timer_poll_comparison_tests.py)
        add_test(NAME gameboy_sgb_phase_origin_comparison
                 COMMAND ${Python3_EXECUTABLE}
                         ${CMAKE_CURRENT_SOURCE_DIR}/tests/sgb_phase_origin_comparison_tests.py)
        add_test(NAME gameboy_sgb_boot_timeline_comparison
                 COMMAND ${Python3_EXECUTABLE}
                         ${CMAKE_CURRENT_SOURCE_DIR}/tests/sgb_boot_timeline_tests.py
                         ${CMAKE_CXX_COMPILER})
        add_test(NAME gameboy_sgb_host_startup_comparison
                 COMMAND ${Python3_EXECUTABLE}
                         ${CMAKE_CURRENT_SOURCE_DIR}/tests/sgb_host_startup_tests.py
                         ${CMAKE_CXX_COMPILER}
                         $<TARGET_FILE:gameboy_snes_65c816_apu_trace>)
        add_test(NAME gameboy_sgb_frame_input
                 COMMAND ${Python3_EXECUTABLE}
                         ${CMAKE_CURRENT_SOURCE_DIR}/tests/sgb_frame_input_tests.py
                         ${CMAKE_CXX_COMPILER}
                         $<TARGET_FILE:gameboy_sgb_frame_input_source_probe>)
        add_test(NAME gameboy_snes_spc_timer_phase
                 COMMAND ${Python3_EXECUTABLE}
                         ${CMAKE_CURRENT_SOURCE_DIR}/tests/snes_spc_timer_phase_tests.py
                         $<TARGET_FILE:gameboy_snes_spc_write_fixture_runner>)
        add_test(NAME gameboy_sgb_native_cycle_comparison
                 COMMAND ${Python3_EXECUTABLE}
                         ${CMAKE_CURRENT_SOURCE_DIR}/tests/sgb_native_cycle_comparison_tests.py)
        add_test(NAME gameboy_snes_dsp_native_volume_timing
                 COMMAND ${Python3_EXECUTABLE}
                         ${CMAKE_CURRENT_SOURCE_DIR}/tests/snes_dsp_native_volume_timing_tests.py
                         $<TARGET_FILE:gameboy_snes_dsp_pcm_fixture_runner>)
        add_test(NAME gameboy_snes_spc700_dsp_multi_key_pcm
                 COMMAND ${Python3_EXECUTABLE}
                         ${CMAKE_CURRENT_SOURCE_DIR}/tests/snes_spc700_dsp_multi_key_pcm_tests.py
                         $<TARGET_FILE:gameboy_snes_spc700_dsp_timeline_tests>
                         $<TARGET_FILE:gameboy_snes_dsp_pcm_fixture_runner>)
        add_test(NAME gameboy_snes_spc700_dsp_voice_register_pcm
                 COMMAND ${Python3_EXECUTABLE}
                         ${CMAKE_CURRENT_SOURCE_DIR}/tests/snes_spc700_dsp_voice_register_pcm_tests.py
                         $<TARGET_FILE:gameboy_snes_spc700_dsp_timeline_tests>
                         $<TARGET_FILE:gameboy_snes_dsp_pcm_fixture_runner>)
        add_test(NAME gameboy_snes_spc700_dsp_global_register_pcm
                 COMMAND ${Python3_EXECUTABLE}
                         ${CMAKE_CURRENT_SOURCE_DIR}/tests/snes_spc700_dsp_global_register_pcm_tests.py
                         $<TARGET_FILE:gameboy_snes_spc700_dsp_timeline_tests>
                         $<TARGET_FILE:gameboy_snes_dsp_pcm_fixture_runner>)
        add_test(NAME gameboy_snes_spc700_dsp_remaining_register_pcm
                 COMMAND ${Python3_EXECUTABLE}
                         ${CMAKE_CURRENT_SOURCE_DIR}/tests/snes_spc700_dsp_remaining_register_pcm_tests.py
                         $<TARGET_FILE:gameboy_snes_spc700_dsp_timeline_tests>
                         $<TARGET_FILE:gameboy_snes_dsp_pcm_fixture_runner>)
    endif()

    add_executable(gameboy_sgb_overlay_contract_tests
        tests/sgb_overlay_contract_tests.cpp)
    target_include_directories(gameboy_sgb_overlay_contract_tests
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/apps/sdl)
    target_link_libraries(gameboy_sgb_overlay_contract_tests PRIVATE gameboy_core)
    add_test(NAME gameboy_sgb_overlay_contract
             COMMAND gameboy_sgb_overlay_contract_tests)

    add_executable(gameboy_sgb_trace_contract_tests
        tests/sgb_trace_contract_tests.cpp)
    target_link_libraries(gameboy_sgb_trace_contract_tests PRIVATE gameboy_core)
    add_test(NAME gameboy_sgb_trace_contract
             COMMAND gameboy_sgb_trace_contract_tests)

    add_executable(gameboy_sgb_input_offset_tests
        tests/sgb_input_offset_tests.cpp)
    target_include_directories(gameboy_sgb_input_offset_tests PRIVATE scripts)
    add_test(NAME gameboy_sgb_input_offset
             COMMAND gameboy_sgb_input_offset_tests)

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
            apps/sdl/input_movie.cpp
            src/core_runtime.cpp)
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
            apps/link_harness/fault_channel.cpp
            apps/link_harness/harness_io.cpp
            apps/link_harness/options.cpp
            apps/link_harness/scenario_trace_writer.cpp
            apps/link_harness/scenario_trace.cpp
            apps/link_harness/pokemon_state.cpp
            apps/link_harness/scenario_state.cpp)
        target_include_directories(gameboy_link_harness_trace_tests
            PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/apps/link_harness
                    ${CMAKE_CURRENT_SOURCE_DIR}/include)
        target_link_libraries(gameboy_link_harness_trace_tests PRIVATE gbb_core_api)
        add_test(NAME gameboy_link_harness_trace
                 COMMAND gameboy_link_harness_trace_tests)

        # The process-level matrix is intentionally covered by normal desktop
        # and ASan/UBSan builds. Under TSan, eight instrumented harness runs
        # exceed the integration test timeout without adding useful coverage.
        if(Python3_Interpreter_FOUND AND TARGET gameboy_link_harness AND
           NOT GAMEBOY_ENABLE_THREAD_SANITIZER)
            add_test(NAME gameboy_real_link_harness_tcp
                     COMMAND ${Python3_EXECUTABLE}
                             ${CMAKE_CURRENT_SOURCE_DIR}/tests/real_link_harness_tcp_test.py
                             $<TARGET_FILE:gameboy_link_harness>)
            set_tests_properties(gameboy_real_link_harness_tcp
                                 PROPERTIES TIMEOUT 75 SKIP_RETURN_CODE 77)
        endif()

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
        add_executable(gameboy_settings_audio_contract_tests
            tests/settings_audio_contract_tests.cpp
            apps/sdl/settings_persistence.cpp)
        target_include_directories(gameboy_settings_audio_contract_tests
            PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/apps/sdl)
        target_compile_definitions(gameboy_settings_audio_contract_tests
            PRIVATE GBB_SETTINGS_PERSISTENCE_TEST_PATH)
        target_link_libraries(gameboy_settings_audio_contract_tests
            PRIVATE gbb_core_api SDL3::SDL3)
        add_test(NAME gameboy_settings_audio_contract
                 COMMAND gameboy_settings_audio_contract_tests)

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
        if(WIN32)
            target_link_libraries(gameboy_sdl_frame_pacer_tests PRIVATE winmm)
        endif()
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

        add_executable(gameboy_desktop_debugger_layout_contract_tests
            tests/desktop_debugger_layout_contract_tests.cpp)
        target_include_directories(gameboy_desktop_debugger_layout_contract_tests
            PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/apps/sdl)
        target_link_libraries(gameboy_desktop_debugger_layout_contract_tests
            PRIVATE SDL3::SDL3)
        add_test(NAME gameboy_desktop_debugger_layout_contract
                 COMMAND gameboy_desktop_debugger_layout_contract_tests)

        if(GAMEBOY_BUILD_PERFORMANCE_TESTS AND NOT EMSCRIPTEN AND
           NOT ANDROID AND NOT GAMEBOY_ENABLE_SANITIZERS AND
           NOT GAMEBOY_ENABLE_THREAD_SANITIZER)
            add_executable(gameboy_sdl_render_performance_tests
                tests/sdl_render_performance_tests.cpp
                apps/sdl/voxel_renderer.cpp)
            target_include_directories(gameboy_sdl_render_performance_tests
                PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/apps/sdl)
            target_link_libraries(gameboy_sdl_render_performance_tests
                PRIVATE gbb_core_api SDL3::SDL3)
            add_test(NAME gameboy_sdl_render_performance
                     COMMAND gameboy_sdl_render_performance_tests)
            set_tests_properties(gameboy_sdl_render_performance
                                 PROPERTIES LABELS "performance"
                                            TIMEOUT 90
                                            SKIP_RETURN_CODE 77
                                            ENVIRONMENT
                                            "SDL_AUDIODRIVER=dummy;SDL_VIDEODRIVER=dummy;SDL_RENDER_DRIVER=software;WAYLAND_DISPLAY=")
            add_test(NAME gameboy_sdl_visual_regression
                     COMMAND ${Python3_EXECUTABLE}
                             ${CMAKE_CURRENT_SOURCE_DIR}/scripts/visual_regression_gate.py
                             --backend sdl-software
                             --reference-dir
                             ${CMAKE_CURRENT_SOURCE_DIR}/tests/visual-baselines/sdl-software
                             --actual-dir
                             ${CMAKE_CURRENT_BINARY_DIR}/visual-captures/sdl-software
                             --report
                             ${CMAKE_CURRENT_BINARY_DIR}/visual-captures/sdl-software/report.json
                             --mode nearest
                             --mode voxel
                             --mode voxel_shape
                             --mode voxel_popup
                             --capture-command
                             $<TARGET_FILE:gameboy_sdl_render_performance_tests>)
            set_tests_properties(gameboy_sdl_visual_regression
                                 PROPERTIES LABELS "visual;performance"
                                            TIMEOUT 90
                                            SKIP_RETURN_CODE 77
                                            ENVIRONMENT
                                            "SDL_AUDIODRIVER=dummy;SDL_VIDEODRIVER=dummy;SDL_RENDER_DRIVER=software;WAYLAND_DISPLAY=")
        endif()
    endif()
    if(TARGET gameboy_sdl)
        add_test(NAME gameboy_sdl_version COMMAND gameboy_sdl --version)
        if(WIN32)
            # This launches the real native dashboard in an isolated smoke
            # mode. The test drives the Win32 controls and message boxes, so
            # regressions in modal lifecycle, owner-draw controls, layout
            # bounds, rendering, or missing-ROM diagnostics fail in CI.
            add_test(NAME gameboy_windows_dashboard_smoke
                     COMMAND gameboy_sdl --windows-dashboard-smoke)
            set_tests_properties(gameboy_windows_dashboard_smoke
                                 PROPERTIES TIMEOUT 45)
        endif()
    endif()

    if(GAMEBOY_TEST_ROM_DIR OR GAMEBOY_SAMESUITE_DIR OR
       GAMEBOY_ENABLE_EXTERNAL_SUITES)
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
