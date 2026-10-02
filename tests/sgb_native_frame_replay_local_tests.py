#!/usr/bin/env python3
"""Optional local-title contract for the opt-in held-state diagnostic path."""
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
from capture_sgb_libretro_audio import load_input_script, native_button_mask
from compare_sgb_boot_timeline import validate


def main():
    trace, program, ipl, game, boot, script = sys.argv[1:]
    if hashlib.sha256(Path(game).read_bytes()).hexdigest() != \
            "b490c89efe718633b07381def66ce0ed58a5075aabe40c6e644baf2b408a76f4":
        raise AssertionError("unexpected local Donkey Kong ROM")
    if hashlib.sha256(Path(script).read_bytes()).hexdigest() != \
            "a5d37081cc52b8bfe72284f5cd836b0ccea9bfc3ddf25fcc1ac2769b3b2e802b":
        raise AssertionError("unexpected input script")
    with tempfile.TemporaryDirectory(prefix="gbb-native-frame-title-") as directory:
        root = Path(directory)
        timeline, bus, pcm = root / "boot.json", root / "bus.json", root / "pcm.wav"
        result = subprocess.run([trace, program, ipl, "--sync-gb-sgb2", game, boot,
                                 "--input-script", script, "--native-gb-input",
                                 "--instruction-limit", "60000000", "--fractional-apu-sync",
                                 "--timer-poll-trace", "--apu-bus-output", str(bus),
                                 "--boot-timeline-output", str(timeline), "--pcm-output", str(pcm)],
                                capture_output=True, text=True, timeout=240)
        if result.returncode != 4 or "SNES CPU trace reached instruction bound" not in result.stderr:
            raise AssertionError(result.stdout[-1000:] + result.stderr[-1000:])
        data = json.loads(timeline.read_text())
        rows = validate(data, "gbb")
        if data["input_mode"] != "gb-lcd-frame-held-v1":
            raise AssertionError("unmarked native replay")
        actual = [(e["count"], e["value"]) for e in rows if e["kind"] == "N"]
        expected = [(f, native_button_mask(m)) for f, m in load_input_script(Path(script))]
        if actual != expected or len(actual) != 11:
            raise AssertionError("missing, duplicated or altered native input event")
        ready = [e for e in rows if e["kind"] == "R"]
        if [(e["value"], e["count"]) for e in ready] != [(0x2140aa, 0x8311), (0x2141bb, 0x8314)]:
            raise AssertionError("startup ready-read landmarks changed")
        if hashlib.sha256(pcm.read_bytes()).hexdigest() != \
                "c3890ffb50438ea2b28aa834a965137d7b425e927c92d24d45fb3ade0336156f":
            raise AssertionError("native input title PCM changed")
    print("All 11 LCD-frame input events and native PCM baseline match")


if __name__ == "__main__":
    main()
