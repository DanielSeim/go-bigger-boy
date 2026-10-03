#pragma once
#include "gameboy/snes_host_cpu.hpp"
namespace sgb_test {
using SnesIcdTraceSource = gameboy::SnesIcdSource;
using SnesTraceTiming = gameboy::SnesHostTiming;
using Snes65c816TraceCpu = gameboy::SnesHostCpu;
} // namespace sgb_test
