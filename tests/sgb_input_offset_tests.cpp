#include "sgb_input_script.h"

#include <iostream>

namespace {

int failures = 0;

void check(const bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

void test_piecewise_reference_frame_schedule() {
    gbb_sgb_input_script script{};
    script.count = 6;
    script.events[0] = {0, 0};
    script.events[1] = {824, 0};
    script.events[2] = {950, 0};
    script.events[3] = {8104, 0};
    script.events[4] = {8520, 0};
    script.events[5] = {8532, 0};

    gbb_sgb_input_offset_map map{};
    unsigned scheduled[GBB_SGB_INPUT_MAX_EVENTS]{};
    char error[128]{};
    check(gbb_sgb_input_schedule(&script, &map, scheduled, error,
                                 sizeof(error)), "identity schedule succeeds");
    check(scheduled[0] == 0 && scheduled[5] == 8532,
          "identity schedule preserves frames");

    map.base_offset = 133;
    map.count = 2;
    map.changes[0] = {900, 124};
    map.changes[1] = {8300, 155};
    check(gbb_sgb_input_schedule(&script, &map, scheduled, error,
                                 sizeof(error)), "piecewise schedule succeeds");
    check(scheduled[0] == 133 && scheduled[1] == 957 &&
              scheduled[2] == 1074 && scheduled[3] == 8228 &&
              scheduled[4] == 8675 && scheduled[5] == 8687,
          "offset changes apply at their inclusive script frames");

    map.changes[1].frame = 900;
    check(!gbb_sgb_input_schedule(&script, &map, scheduled, error,
                                  sizeof(error)),
          "duplicate change threshold is rejected");
    map.changes[1].frame = 8300;
    map.changes[0].frame = 825;
    map.changes[0].offset = 0;
    check(!gbb_sgb_input_schedule(&script, &map, scheduled, error,
                                  sizeof(error)),
          "mapping that reverses event order is rejected");
    map.changes[0] = {900, 124};
    script.events[5].frame = GBB_SGB_INPUT_MAX_FRAME;
    check(!gbb_sgb_input_schedule(&script, &map, scheduled, error,
                                  sizeof(error)),
          "mapped frame overflow is rejected");
}

} // namespace

int main() {
    test_piecewise_reference_frame_schedule();
    return failures == 0 ? 0 : 1;
}
