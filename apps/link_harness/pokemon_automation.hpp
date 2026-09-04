#pragma once

#include "gameboy/emulator.hpp"
#include "options.hpp"
#include "scenario_state.hpp"

#include <cstdint>
#include <iosfwd>

namespace gbb::link_harness {

// Drives the deterministic Pokémon Cable Club/table/trade/battle flow used
// by scripted harness scenarios. The state object is retained by the caller so
// each player can progress independently when one guest reaches a menu first.
void apply_auto_inputs(const Options& options,
                       std::uint64_t frame,
                       gameboy::Emulator& first,
                       gameboy::Emulator& second,
                       AutoInputState& input_state);

void append_auto_input_report(std::ostream& report,
                              const AutoInputState& input_state);

} // namespace gbb::link_harness
