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
    trace, program, ipl, game, boot, script, *options = sys.argv[1:]
    corrected_options = ["--ppu-dma-timing", "--host-bus-timing"]
    reference_options = corrected_options + ["--apu-clock-hz", "1025280"]
    if options not in ([], ["--ppu-dma-timing"], corrected_options, reference_options):
        raise ValueError("unsupported local replay options")
    ppu_dma_timing = bool(options)
    host_bus_timing = "--host-bus-timing" in options
    reference_clock = "--apu-clock-hz" in options
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
                                 *options,
                                 "--timer-poll-trace", "--apu-bus-output", str(bus),
                                 "--boot-timeline-output", str(timeline), "--pcm-output", str(pcm)],
                                capture_output=True, text=True, timeout=240)
        if result.returncode != 4 or "SNES CPU trace reached instruction bound" not in result.stderr:
            raise AssertionError(result.stdout[-1000:] + result.stderr[-1000:])
        data = json.loads(timeline.read_text())
        rows = validate(data, "gbb")
        if data["input_mode"] != "gb-lcd-frame-held-v1":
            raise AssertionError("unmarked native replay")
        if data.get("ppu_dma_timing") is not ppu_dma_timing:
            raise AssertionError("unmarked PPU DMA timing mode")
        if data.get("host_bus_timing") is not host_bus_timing or data.get("external_boot_reset") is not True:
            raise AssertionError("unmarked host timing or external boot reset")
        if data["apu_half_hz"] != (2050560 if reference_clock else 2048000):
            raise AssertionError("unmarked diagnostic oscillator profile")
        actual = [(e["count"], e["value"]) for e in rows if e["kind"] == "N"]
        expected = [(f, native_button_mask(m)) for f, m in load_input_script(Path(script))]
        if actual != expected or len(actual) != 11:
            raise AssertionError("missing, duplicated or altered native input event")
        ready = [e for e in rows if e["kind"] == "R"]
        if [(e["value"], e["count"]) for e in ready] != [(0x2140aa, 0x8311), (0x2141bb, 0x8314)]:
            raise AssertionError("startup ready-read landmarks changed")
        booted = [e for e in rows if e["kind"] == "B"]
        if len(booted) != 1 or booted[0]["count"] != 1955920:
            raise AssertionError("external boot inherited post-boot state")
        if host_bus_timing:
            # Independently observed raw master-clock snapshots; no fitted offset.
            for kind, expected_clock in (("P", 1049973168), ("B", 135778620)):
                markers = [e for e in rows if e["kind"] == kind]
                if len(markers) != 1 or abs(markers[0]["master_clock_snapshot"] - expected_clock) > 2048:
                    raise AssertionError("cold-boot/music-start timing regressed beyond 0.1 ms")
            if [(e["value"], e["count"], e["digest_fnv64"]) for e in rows if e["kind"] == "U"] != [
                    (19504, 378, 6775648492224675512), (19472, 24, 11612272398183007014),
                    (1024, 9971, 11514011197385430042), (19200, 256, 14353112005375938616),
                    (19888, 41280, 3659709487927221914)]:
                raise AssertionError("independent upload fingerprints changed")
            if [e["value"] for e in rows if e["kind"] == "S"] != [9207936, 16777216, 16777216]:
                raise AssertionError("independent SOUND parameters changed")
        expected_pcm = ("26fa4738f2f99ac6b0c611a8417acea683667a1140269c60bd60d3748bbdac78" if reference_clock else
                        "d430fa49f199df87fc20b6465cba0fff0e89daf6b446a9a07159b90b188e4bc9" if host_bus_timing else
                        "15059e74817aeb5287062b903b71bccd58e4197f0c781129f907760fb5c518e0" if ppu_dma_timing else
                        "964da6c4780c053d5beb24d0eee75d2e9efd569c9f5aa729bd8715f1deabc847")
        if hashlib.sha256(pcm.read_bytes()).hexdigest() != expected_pcm:
            raise AssertionError("native input title PCM changed")
    print("All 11 LCD-frame input events and native PCM baseline match")


if __name__ == "__main__":
    main()
