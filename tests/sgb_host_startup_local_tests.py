#!/usr/bin/env python3
"""Optional local firmware check of the experimental PPU DMA timing path."""
import json
from pathlib import Path
import subprocess
import sys
import tempfile


def main():
    trace, program, ipl, game, boot = sys.argv[1:]
    with tempfile.TemporaryDirectory(prefix="gbb-host-startup-local-") as directory:
        root = Path(directory)
        script, bus, timeline, host = (root / name for name in ("empty.script", "bus.json", "boot.json", "host.json"))
        script.write_text("GBB SGB input v1\n")
        result = subprocess.run([trace, program, ipl, "--sync-gb-sgb2", game, boot,
                                 "--input-script", str(script), "--native-gb-input",
                                 "--instruction-limit", "2000000", "--fractional-apu-sync",
                                 "--ppu-dma-timing", "--timer-poll-trace", "--apu-bus-output", str(bus),
                                 "--boot-timeline-output", str(timeline), "--host-startup-output", str(host)],
                                capture_output=True, text=True, timeout=60)
        if result.returncode != 4:
            raise AssertionError(result.stdout[-2000:] + result.stderr[-2000:])
        data = json.loads(timeline.read_text())
        if data.get("ppu_dma_timing") is not True:
            raise AssertionError("unmarked experimental DMA timing")
        uploads = [e for e in data["events"] if e["kind"] == "A"]
        # Pinned independent startup observation, not a hardware timing claim.
        if len(uploads) != 1 or abs(uploads[0]["master_clock_snapshot"] - 2135016) > 512:
            raise AssertionError("first upload still has a large startup timing gap")
        requests = json.loads(host.read_text())["dma_requests"]
        if [(row[3], row[4], row[6]) for row in requests[:3]] != [
                (9, 0x18, 65535), (0x80, 0x36, 24576), (0x80, 0x36, 24576)]:
            raise AssertionError("unexpected initial DMA configuration")
        if len([e for e in data["events"] if e["kind"] == "U"]) != 5:
            raise AssertionError("startup failed to complete its five uploads")
        alias = root / "alias"
        alias.mkdir()
        collided = root / "collided.json"
        invalid = subprocess.run([trace, program, ipl, "--sync-gb-sgb2", game, boot,
                                  "--fractional-apu-sync", "--apu-bus-output", str(collided),
                                  "--boot-timeline-output", str(root / "unused-boot.json"),
                                  "--host-startup-output", str(alias / ".." / "collided.json")],
                                 capture_output=True, text=True, timeout=10)
        if invalid.returncode == 0 or "output paths must differ" not in invalid.stderr or collided.exists():
            raise AssertionError("aliased host output was not rejected before writing")
        incomplete = root / "incomplete.json"
        short = subprocess.run([trace, program, ipl, "--sync-gb-sgb2", game, boot,
                                "--instruction-limit", "1", "--fractional-apu-sync",
                                "--apu-bus-output", str(root / "short-bus.json"),
                                "--boot-timeline-output", str(root / "short-boot.json"),
                                "--host-startup-output", str(incomplete)],
                               capture_output=True, text=True, timeout=10)
        if "incomplete host startup trace" not in short.stderr or incomplete.exists():
            raise AssertionError("partial startup capture was exported as complete")
    print("PPU DMA startup stalls reproduced; first upload agrees within 512 clocks; invalid outputs rejected")


if __name__ == "__main__":
    main()
