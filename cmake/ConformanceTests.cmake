function(gbb_add_conformance_test suite relative_path protocol cycle_limit)
    set(rom "${GAMEBOY_TEST_ROM_DIR}/${relative_path}")
    if(NOT EXISTS "${rom}")
        message(FATAL_ERROR "Missing conformance ROM: ${rom}")
    endif()

    string(MAKE_C_IDENTIFIER "${suite}_${relative_path}" test_id)
    set(test_name "conformance_${test_id}")
    add_test(NAME "${test_name}"
             COMMAND gameboy_test_runner "${rom}"
                     --max-cycles "${cycle_limit}"
                     --protocol "${protocol}")
    set_tests_properties("${test_name}" PROPERTIES
                         LABELS "conformance;${suite}"
                         TIMEOUT 30)
endfunction()

# Passing DMG-focused baseline from Mooneye Test Suite 8d742b9d55.
set(gbb_mooneye_tests
    acceptance/add_sp_e_timing.gb
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
)

foreach(relative_path IN LISTS gbb_mooneye_tests)
    gbb_add_conformance_test(
        mooneye "mooneye-test-suite/${relative_path}" mooneye 20000000)
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
