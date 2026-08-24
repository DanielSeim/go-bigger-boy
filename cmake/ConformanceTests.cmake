function(gbb_add_conformance_test suite relative_path protocol cycle_limit)
    set(rom "${GAMEBOY_TEST_ROM_DIR}/${relative_path}")
    if(NOT EXISTS "${rom}")
        message(FATAL_ERROR "Missing conformance ROM: ${rom}")
    endif()

    string(MAKE_C_IDENTIFIER "${suite}_${relative_path}" test_id)
    set(test_name "conformance_${test_id}")
    set(test_command gameboy_test_runner "${rom}"
                     --max-cycles "${cycle_limit}"
                     --protocol "${protocol}")
    if(ARGC GREATER 4)
        list(APPEND test_command --model "${ARGV4}")
    endif()
    add_test(NAME "${test_name}" COMMAND ${test_command})
    set_tests_properties("${test_name}" PROPERTIES
                         LABELS "conformance;${suite}"
                         TIMEOUT 30)
endfunction()

find_package(Python3 3.8 REQUIRED COMPONENTS Interpreter)

function(gbb_add_visual_test name rom_path reference_path model frames
         dmg_compatibility_colors)
    set(rom "${GAMEBOY_TEST_ROM_DIR}/${rom_path}")
    set(reference "${GAMEBOY_TEST_ROM_DIR}/${reference_path}")
    if(NOT EXISTS "${rom}")
        message(FATAL_ERROR "Missing visual test ROM: ${rom}")
    endif()
    if(NOT EXISTS "${reference}")
        message(FATAL_ERROR "Missing visual test reference: ${reference}")
    endif()

    set(actual "${CMAKE_CURRENT_BINARY_DIR}/visual-results/${name}.ppm")
    set(command "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/compare_frame.py"
                --runner $<TARGET_FILE:gameboy_test_runner>
                --rom "${rom}"
                --reference "${reference}"
                --actual "${actual}"
                --model "${model}")
    if(frames STREQUAL "ld-bb")
        list(APPEND command --frame-on-ld-bb)
    else()
        list(APPEND command --frames "${frames}")
    endif()
    if(dmg_compatibility_colors)
        list(APPEND command --dmg-compatibility-colors)
    endif()
    add_test(NAME "visual_${name}" COMMAND ${command})
    set_tests_properties("visual_${name}" PROPERTIES
                         LABELS "conformance;visual"
                         TIMEOUT 30)
endfunction()

# Complete acceptance baseline from Mooneye Test Suite 8d742b9d55.
set(gbb_mooneye_tests
    acceptance/add_sp_e_timing.gb
    acceptance/bits/unused_hwio-GS.gb
    acceptance/bits/mem_oam.gb
    acceptance/bits/reg_f.gb
    acceptance/boot_regs-dmgABC.gb
    acceptance/call_cc_timing.gb
    acceptance/call_cc_timing2.gb
    acceptance/call_timing.gb
    acceptance/call_timing2.gb
    acceptance/di_timing-GS.gb
    acceptance/div_timing.gb
    acceptance/ei_sequence.gb
    acceptance/ei_timing.gb
    acceptance/halt_ime0_ei.gb
    acceptance/halt_ime0_nointr_timing.gb
    acceptance/halt_ime1_timing.gb
    acceptance/halt_ime1_timing2-GS.gb
    acceptance/if_ie_registers.gb
    acceptance/interrupts/ie_push.gb
    acceptance/instr/daa.gb
    acceptance/intr_timing.gb
    acceptance/jp_cc_timing.gb
    acceptance/jp_timing.gb
    acceptance/ld_hl_sp_e_timing.gb
    acceptance/oam_dma/basic.gb
    acceptance/oam_dma/reg_read.gb
    acceptance/oam_dma/sources-GS.gb
    acceptance/oam_dma_restart.gb
    acceptance/oam_dma_start.gb
    acceptance/oam_dma_timing.gb
    acceptance/pop_timing.gb
    acceptance/ppu/hblank_ly_scx_timing-GS.gb
    acceptance/ppu/intr_1_2_timing-GS.gb
    acceptance/ppu/intr_2_0_timing.gb
    acceptance/ppu/intr_2_mode0_timing.gb
    acceptance/ppu/intr_2_mode0_timing_sprites.gb
    acceptance/ppu/intr_2_mode3_timing.gb
    acceptance/ppu/intr_2_oam_ok_timing.gb
    acceptance/ppu/lcdon_timing-GS.gb
    acceptance/ppu/lcdon_write_timing-GS.gb
    acceptance/ppu/stat_irq_blocking.gb
    acceptance/ppu/stat_lyc_onoff.gb
    acceptance/ppu/vblank_stat_intr-GS.gb
    acceptance/push_timing.gb
    acceptance/rapid_di_ei.gb
    acceptance/ret_cc_timing.gb
    acceptance/ret_timing.gb
    acceptance/reti_intr_timing.gb
    acceptance/reti_timing.gb
    acceptance/rst_timing.gb
    acceptance/timer/div_write.gb
    acceptance/timer/tim00.gb
    acceptance/timer/tim00_div_trigger.gb
    acceptance/timer/tim01.gb
    acceptance/timer/tim01_div_trigger.gb
    acceptance/timer/tim10.gb
    acceptance/timer/tim10_div_trigger.gb
    acceptance/timer/tim11.gb
    acceptance/timer/tim11_div_trigger.gb
    acceptance/timer/tima_reload.gb
    acceptance/timer/rapid_toggle.gb
    acceptance/timer/tima_write_reloading.gb
    acceptance/timer/tma_write_reloading.gb
)

foreach(relative_path IN LISTS gbb_mooneye_tests)
    gbb_add_conformance_test(
        mooneye "mooneye-test-suite/${relative_path}" mooneye 20000000)
endforeach()

# Post-boot state is hardware-model-specific. These ROMs intentionally have
# mutually exclusive expectations and therefore run with an explicit model.
gbb_add_conformance_test(mooneye
    "mooneye-test-suite/acceptance/boot_div-dmg0.gb" mooneye 20000000 dmg0)
gbb_add_conformance_test(mooneye
    "mooneye-test-suite/acceptance/boot_div-dmgABCmgb.gb" mooneye 20000000 dmg)
gbb_add_conformance_test(mooneye
    "mooneye-test-suite/acceptance/boot_div-S.gb" mooneye 20000000 sgb)
gbb_add_conformance_test(mooneye
    "mooneye-test-suite/acceptance/boot_div2-S.gb" mooneye 20000000 sgb2)
gbb_add_conformance_test(mooneye
    "mooneye-test-suite/acceptance/boot_hwio-dmg0.gb" mooneye 20000000 dmg0)
gbb_add_conformance_test(mooneye
    "mooneye-test-suite/acceptance/boot_hwio-dmgABCmgb.gb" mooneye 20000000 dmg)
gbb_add_conformance_test(mooneye
    "mooneye-test-suite/acceptance/boot_hwio-S.gb" mooneye 20000000 sgb)
gbb_add_conformance_test(mooneye
    "mooneye-test-suite/acceptance/boot_regs-dmg0.gb" mooneye 20000000 dmg0)
gbb_add_conformance_test(mooneye
    "mooneye-test-suite/acceptance/boot_regs-mgb.gb" mooneye 20000000 mgb)
gbb_add_conformance_test(mooneye
    "mooneye-test-suite/acceptance/boot_regs-sgb.gb" mooneye 20000000 sgb)
gbb_add_conformance_test(mooneye
    "mooneye-test-suite/acceptance/boot_regs-sgb2.gb" mooneye 20000000 sgb2)
gbb_add_conformance_test(mooneye
    "mooneye-test-suite/acceptance/serial/boot_sclk_align-dmgABCmgb.gb"
    mooneye 20000000 dmg)

# CGB and CGB0 post-boot behavior that Mooneye keeps in misc because these
# ROMs are not applicable to every Game Boy hardware model. The two AGB-only
# misc ROMs are intentionally excluded because GBB does not emulate an AGB.
gbb_add_conformance_test(mooneye-cgb
    "mooneye-test-suite/misc/boot_div-cgb0.gb" mooneye 20000000 cgb0)
set(gbb_mooneye_cgb_tests
    misc/bits/unused_hwio-C.gb
    misc/boot_div-cgbABCDE.gb
    misc/boot_hwio-C.gb
    misc/boot_regs-cgb.gb
    misc/ppu/vblank_stat_intr-C.gb
)
foreach(relative_path IN LISTS gbb_mooneye_cgb_tests)
    gbb_add_conformance_test(
        mooneye-cgb "mooneye-test-suite/${relative_path}"
        mooneye 20000000 cgb)
endforeach()

# Mapper behavior is deterministic in an emulator and does not depend on a
# particular physical console revision.
set(gbb_mooneye_emulator_only_tests
    emulator-only/mbc1/bits_bank1.gb
    emulator-only/mbc1/bits_bank2.gb
    emulator-only/mbc1/bits_mode.gb
    emulator-only/mbc1/bits_ramg.gb
    emulator-only/mbc1/multicart_rom_8Mb.gb
    emulator-only/mbc1/ram_256kb.gb
    emulator-only/mbc1/ram_64kb.gb
    emulator-only/mbc1/rom_16Mb.gb
    emulator-only/mbc1/rom_1Mb.gb
    emulator-only/mbc1/rom_2Mb.gb
    emulator-only/mbc1/rom_4Mb.gb
    emulator-only/mbc1/rom_512kb.gb
    emulator-only/mbc1/rom_8Mb.gb
    emulator-only/mbc2/bits_ramg.gb
    emulator-only/mbc2/bits_romb.gb
    emulator-only/mbc2/bits_unused.gb
    emulator-only/mbc2/ram.gb
    emulator-only/mbc2/rom_1Mb.gb
    emulator-only/mbc2/rom_2Mb.gb
    emulator-only/mbc2/rom_512kb.gb
    emulator-only/mbc5/rom_16Mb.gb
    emulator-only/mbc5/rom_1Mb.gb
    emulator-only/mbc5/rom_2Mb.gb
    emulator-only/mbc5/rom_32Mb.gb
    emulator-only/mbc5/rom_4Mb.gb
    emulator-only/mbc5/rom_512kb.gb
    emulator-only/mbc5/rom_64Mb.gb
    emulator-only/mbc5/rom_8Mb.gb
)
foreach(relative_path IN LISTS gbb_mooneye_emulator_only_tests)
    gbb_add_conformance_test(
        mooneye-emulator-only "mooneye-test-suite/${relative_path}"
        mooneye 100000000)
endforeach()

# Blargg tests use automatic detection because some report over serial while
# others publish the equivalent result through their memory protocol.
set(gbb_blargg_tests
    "cpu_instrs/individual/01-special.gb"
    "cpu_instrs/individual/02-interrupts.gb"
    "cpu_instrs/individual/03-op sp,hl.gb"
    "cpu_instrs/individual/04-op r,imm.gb"
    "cpu_instrs/individual/05-op rp.gb"
    "cpu_instrs/individual/06-ld r,r.gb"
    "cpu_instrs/individual/07-jr,jp,call,ret,rst.gb"
    "cpu_instrs/individual/08-misc instrs.gb"
    "cpu_instrs/individual/09-op r,r.gb"
    "cpu_instrs/individual/10-bit ops.gb"
    "cpu_instrs/individual/11-op a,(hl).gb"
    instr_timing/instr_timing.gb
    mem_timing/mem_timing.gb
    mem_timing-2/mem_timing.gb
)

foreach(relative_path IN LISTS gbb_blargg_tests)
    gbb_add_conformance_test(
        blargg "blargg/${relative_path}" auto 100000000)
endforeach()

# Exact framebuffer comparisons against the reference images distributed with
# the pinned ROM bundle. Captures and magenta difference images are retained in
# the build tree when a comparison fails.
gbb_add_visual_test(dmg_acid2
    "dmg-acid2/dmg-acid2.gb" "dmg-acid2/dmg-acid2-dmg.png" dmg 60 OFF)
gbb_add_visual_test(cgb_acid2
    "cgb-acid2/cgb-acid2.gbc" "cgb-acid2/cgb-acid2.png" cgb 60 OFF)
gbb_add_visual_test(dmg_acid2_cgb
    "dmg-acid2/dmg-acid2.gb" "dmg-acid2/dmg-acid2-cgb.png" cgb 60 ON)
gbb_add_visual_test(scribbl_scxly_dmg
    "scribbltests/scxly/scxly.gb"
    "scribbltests/scxly/scxly-dmg.png" dmg 60 OFF)
gbb_add_visual_test(scribbl_lycscx_dmg
    "scribbltests/lycscx/lycscx.gb"
    "scribbltests/lycscx/lycscx-cgb-dmg.png" dmg 60 OFF)
gbb_add_visual_test(scribbl_lycscx_cgb
    "scribbltests/lycscx/lycscx.gb"
    "scribbltests/lycscx/lycscx-cgb-dmg.png" cgb 60 ON)
gbb_add_visual_test(scribbl_lycscy_dmg
    "scribbltests/lycscy/lycscy.gb"
    "scribbltests/lycscy/lycscy-cgb-dmg.png" dmg 60 OFF)
gbb_add_visual_test(scribbl_lycscy_cgb
    "scribbltests/lycscy/lycscy.gb"
    "scribbltests/lycscy/lycscy-cgb-dmg.png" cgb 60 ON)
gbb_add_visual_test(scribbl_palettely_dmg
    "scribbltests/palettely/palettely.gb"
    "scribbltests/palettely/palettely-dmg.png" dmg 60 OFF)
gbb_add_visual_test(scribbl_palettely_cgb
    "scribbltests/palettely/palettely.gb"
    "scribbltests/palettely/palettely-cgb.png" cgb 60 ON)
gbb_add_visual_test(scribbl_statcount_dmg
    "scribbltests/statcount/statcount-auto.gb"
    "scribbltests/statcount/statcount_auto-cgb-dmg.png" dmg 360 OFF)
gbb_add_visual_test(scribbl_statcount_cgb
    "scribbltests/statcount/statcount-auto.gb"
    "scribbltests/statcount/statcount_auto-cgb-dmg.png" cgb 360 ON)
gbb_add_visual_test(mealybug_m2_win_en_toggle_dmg
    "mealybug-tearoom-tests/ppu/m2_win_en_toggle.gb"
    "mealybug-tearoom-tests/ppu/m2_win_en_toggle_dmg_blob.png"
    dmg ld-bb OFF)
gbb_add_visual_test(gambatte_dmgpalette_m3_1
    "gambatte/dmgpalette_during_m3/dmgpalette_during_m3_1.gb"
    "gambatte/dmgpalette_during_m3/dmgpalette_during_m3_1_dmg08.png"
    dmg 60 OFF)
gbb_add_visual_test(gambatte_dmgpalette_m3_scx1_1
    "gambatte/dmgpalette_during_m3/dmgpalette_during_m3_scx1_1.gb"
    "gambatte/dmgpalette_during_m3/dmgpalette_during_m3_scx1_1_dmg08.png"
    dmg 60 OFF)
