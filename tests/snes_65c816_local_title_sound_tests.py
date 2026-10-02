#!/usr/bin/env python3
"""Optional local-ROM test: unmodified Donkey Kong SOUND reaches host PCM."""

import argparse
import csv
import hashlib
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import wave

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
from compare_sgb_apu_bus import load as load_bus
from compare_sgb_timer_polls import observations

DONKEY_SHA256 = "b490c89efe718633b07381def66ce0ed58a5075aabe40c6e644baf2b408a76f4"
SCRIPT_SHA256 = "a5d37081cc52b8bfe72284f5cd836b0ccea9bfc3ddf25fcc1ac2769b3b2e802b"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("trace")
    parser.add_argument("program")
    parser.add_argument("ipl")
    parser.add_argument("gb_rom")
    parser.add_argument("gb_boot")
    parser.add_argument("input_script")
    parser.add_argument("--bus-clocked-dsp", action="store_true")
    parser.add_argument("--cycle-bus-dsp", action="store_true")
    parser.add_argument("--shared-bus-dsp", action="store_true")
    parser.add_argument("--cycle-apu-sync", action="store_true")
    parser.add_argument("--fractional-apu-sync", action="store_true")
    args = parser.parse_args()
    if args.fractional_apu_sync:
        args.cycle_apu_sync = True
    if args.cycle_apu_sync:
        args.shared_bus_dsp = True
    if hashlib.sha256(Path(args.gb_rom).read_bytes()).hexdigest() != DONKEY_SHA256:
        raise AssertionError("local Donkey Kong ROM does not match the pinned title")
    if hashlib.sha256(Path(args.input_script).read_bytes()).hexdigest() != SCRIPT_SHA256:
        raise AssertionError("Donkey Kong input script does not match the pinned sequence")
    with tempfile.TemporaryDirectory(prefix="gbb-sgb-title-pcm-") as directory:
        pcm_path = Path(directory) / "title.wav"
        event_path = Path(directory) / "sound-events.csv"
        bus_path = Path(directory) / "bus.json"
        result = subprocess.run(
            [args.trace, args.program, args.ipl, "--sync-gb-sgb2",
             args.gb_rom, args.gb_boot, "--input-script", args.input_script,
             "--instruction-limit", "40000000", "--pcm-output", str(pcm_path),
             "--sound-event-trace-output", str(event_path)] +
            (["--fractional-apu-sync"] if args.fractional_apu_sync else
             ["--cycle-apu-sync"] if args.cycle_apu_sync else
             ["--shared-bus-dsp"] if args.shared_bus_dsp else
             ["--cycle-bus-dsp"] if args.cycle_bus_dsp else
             ["--bus-clocked-dsp"] if args.bus_clocked_dsp else []) +
            (["--timer-poll-trace", "--apu-bus-output", str(bus_path)]
             if args.fractional_apu_sync else []),
            capture_output=True, text=True, timeout=180, check=False,
        )
        output = result.stdout + result.stderr
        anchor = re.search(r"first_audible_delivery_sample=(\d+)", output)
        if anchor is None or not pcm_path.exists():
            raise AssertionError("title trace did not export anchored PCM")
        with wave.open(str(pcm_path), "rb") as wav:
            if wav.getnchannels() != 2 or wav.getsampwidth() != 2 or \
                    wav.getframerate() != 32000 or \
                    not 0 < int(anchor.group(1)) < wav.getnframes():
                raise AssertionError("title WAV format or event anchor is invalid")
        with event_path.open(newline="", encoding="utf-8") as event_file:
            event_rows = list(csv.DictReader(event_file))
        packets = [event for event in event_rows if event["kind"] == "P"]
        hosts = [event for event in event_rows if event["kind"] == "H"]
        dsp = [event for event in event_rows if event["kind"] == "D"]
        if args.bus_clocked_dsp or args.cycle_bus_dsp or args.shared_bus_dsp:
            if "DSP clock from SPC reset; write-boundary observation enabled" not in output:
                raise AssertionError("bus-clocked DSP did not start at reset")
            checkpoints = [event for event in event_rows if event["kind"] == "V"]
            if not checkpoints or any(int(event["spc_cycle"]) == 0 for event in checkpoints):
                raise AssertionError("bus-clocked voice checkpoints lack cycle positions")
        if (args.cycle_bus_dsp or args.shared_bus_dsp) and \
                "SPC cycle-level reads, dummy accesses and timers enabled" not in output:
            raise AssertionError("title probe did not opt into cycle-level SPC execution")
        if args.shared_bus_dsp and \
                "Shared SPC/DSP APU RAM and live register readback enabled" not in output:
            raise AssertionError("title probe did not use the shared SPC/DSP bus")
        if args.cycle_bus_dsp or args.shared_bus_dsp:
            phases = [(int(event["address"]), int(event["value"]))
                      for event in event_rows if event["kind"] == "Q"]
            # Shared instruction-rendezvous baseline matches reference 37/41.
            # Exact rendezvous removes overshoot but produces 34/38: pin that
            # diagnostic result without claiming it matches hardware/reference.
            expected_phases = ([(34, 4), (38, 4)] if args.cycle_apu_sync and
                               not args.fractional_apu_sync else [(37, 4), (41, 4)])
            if phases[:2] != expected_phases:
                raise AssertionError("title KON bus phases changed for the selected diagnostic mode")
        if args.cycle_apu_sync:
            boundary = re.search(r"APU rendezvous target=(\d+) completed=(\d+) SPC_completed=(\d+)", output)
            if "Cycle-level SNES/SPC APU rendezvous enabled" not in output or \
                    boundary is None or len(set(boundary.groups())) != 1:
                raise AssertionError("title rendezvous did not stop at its exact SPC target")
        if args.fractional_apu_sync:
            boundary = re.search(r"APU fractional target_half=(\d+) completed_half=(\d+)", output)
            if boundary is None or boundary[1] != boundary[2] or \
                    "Fractional APU ports:" not in output:
                raise AssertionError("title fractional rendezvous did not stop at its exact half target")
            data = load_bus(bus_path, "gbb")
            if data.get("phase_writes_from_reset") is not True or not any(
                    e["kind"] == "w" and e["address"] == 0x43 and e["value"] == 0
                    and e["spc_half_clock"] == 3460 for e in data["events"]):
                raise AssertionError("sparse phase initialization was not captured from reset")
            state = observations(data)
            positive = [p for p in state["polls"] if p["ticks"]][:2]
            if "Timer polling and bounded driver-state trace enabled" not in output or \
                    [(p["half_clocks_after_kon"], p["ticks"], p.get("phase_before"),
                      p.get("phase_after")) for p in positive] != \
                    [(558, 2, 164, 252), (2876, 1, 252, 40)]:
                raise AssertionError("bounded timer/driver-state capture changed")
            if not any(e["kind"] == "W" and e["address"] == 0xfa and e["value"] == 16
                       for e in data["events"]):
                raise AssertionError("timer configuration was not captured")
            # Opt-in observation must not alter the established native PCM.
            if hashlib.sha256(pcm_path.read_bytes()).hexdigest() != \
                    "ed07e3f031130c20fd2f8eb5ac12599fbea4a3eb4501e90a1e955a97f89d8d10":
                raise AssertionError("timer observation changed title PCM")
        if len(packets) != 1 or len(hosts) < 4 or len(dsp) < 10 or \
                [(int(event["address"]), int(event["value"]))
                 for event in hosts[:4]] != [(0, 1), (1, 0), (2, 0), (3, 0)] or \
                (int(dsp[0]["address"]), int(dsp[0]["value"])) != (92, 0):
            raise AssertionError("post-SOUND host/DSP event trace is incomplete")
    if result.returncode != 4 or "SNES CPU trace reached instruction bound" not in output:
        raise AssertionError(f"unexpected bounded trace outcome: {output[-3000:]}")
    if "first audible SOUND frame=2472 packet= 41 00 00 00 01" not in output:
        raise AssertionError("title-authentic music-score packet was not captured")
    deliveries = re.findall(
        r"host SOUND delivery index=(\d+) GB_frame=(\d+) "
        r"GB_cycle=(\d+) SNES_master_clock=(\d+) "
        r"PCM_sample=(\d+) packet= ([0-9a-f ]+)", output)
    if [int(event[0]) for event in deliveries] != [0, 1, 2]:
        raise AssertionError("host SOUND delivery timeline is incomplete")
    first_music, second_music = deliveries[1:]
    if [int(first_music[1]), int(second_music[1])] != [2473, 2521] or \
            first_music[5] != second_music[5] or \
            not first_music[5].startswith("41 00 00 00 01") or \
            not 0.8 * 32000 < int(second_music[4]) - int(first_music[4]) \
            < 0.83 * 32000:
        raise AssertionError("repeated title SOUND packets differ or lack PCM anchors")
    gb_cycles = int(second_music[2]) - int(first_music[2])
    master_clocks = int(second_music[3]) - int(first_music[3])
    pcm_seconds = (int(second_music[4]) - int(first_music[4])) / 32000
    if abs(gb_cycles / master_clocks - 4194304 / 21477273) > 0.0001 or \
            abs(pcm_seconds - master_clocks / 21477273) > 0.002:
        raise AssertionError("SGB2 GB, SNES, and DSP clock spacing disagree")
    delivered = re.search(r"audible_SOUND_delivered=(\d+)", output)
    nonzero = re.search(r"post_audible_SOUND_nonzero=(\d+)", output)
    if delivered is None or int(delivered.group(1)) == 0:
        raise AssertionError("audible title packet did not reach SNES host")
    if nonzero is None or int(nonzero.group(1)) == 0:
        raise AssertionError("no nonzero PCM after audible packet delivery")
    print("unmodified Donkey Kong title SOUND delivered to SNES host; "
          f"{nonzero.group(1)} nonzero PCM samples after delivery")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (AssertionError, OSError, subprocess.TimeoutExpired) as error:
        print(f"local title SOUND test failed: {error}", file=sys.stderr)
        sys.exit(1)
