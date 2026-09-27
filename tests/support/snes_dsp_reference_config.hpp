#pragma once

// Development-only compatibility declaration for the locally installed
// bsnes SPC_DSP.cpp, which refers to its host's interpolation setting.
// The comparison uses the standard Gaussian path, never the cubic hack.
struct GbbExternalDspConfiguration {
    struct {
        struct { bool cubic; } dsp;
    } hacks;
};
inline constexpr GbbExternalDspConfiguration configuration{{{false}}};
