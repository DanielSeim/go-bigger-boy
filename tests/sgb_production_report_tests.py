"""ROM-free checks that private production reports fail closed."""
import copy
import unittest
from sgb_production_local_title_tests import PINS, validate


def reports():
    return [dict(format='gbb-sgb-production-v1', model=model, boot='bundled',
                 input_clock='presentation-frame', output_hz=48000, frames=3600,
                 restores=27, inputs=11, pin_profile='linux-gcc', **pins)
            for model, pins in PINS.items()]


class Reports(unittest.TestCase):
    def test_valid(self):
        validate(reports())

    def test_missing_duplicate_and_reordered_models(self):
        original = reports()
        for invalid in ([], original[:1], original[::-1], [original[0], original[0]]):
            with self.assertRaises(ValueError):
                validate(invalid)

    def test_incomplete_or_changed_output(self):
        for key, value in dict(boot='external', input_clock='native-lcd', output_hz=44100,
                              frames=3599, restores=0, inputs=0, nonzero=0, samples=0,
                              audio_hash=0, video_hash=0, gb_state_hash=0,
                              pin_profile='unknown').items():
            with self.subTest(key=key), self.assertRaises(ValueError):
                changed = copy.deepcopy(reports())
                changed[0][key] = value
                validate(changed)

    def test_other_compilers_require_complete_same_platform_comparison(self):
        other = reports()
        for report in other:
            report.update(pin_profile='same-platform', audio_hash=123)
        validate(other)  # The C++ runner has already asserted exact host parity.
        other[0]['inputs'] = 0
        with self.assertRaises(ValueError):
            validate(other)

    def test_invalid_counter_types_and_ranges(self):
        for value in (None, True, '123', -1, 2**64, 1.25):
            with self.subTest(value=value), self.assertRaises(ValueError):
                changed = reports()
                changed[0].update(pin_profile='same-platform', audio_hash=value)
                validate(changed)


if __name__ == '__main__':
    unittest.main()
