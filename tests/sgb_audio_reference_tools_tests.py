#!/usr/bin/env python3
"""ROM-free contracts for scripted libretro audio capture and event comparison."""

from array import array
import hashlib
import json
from pathlib import Path
import sys
import tempfile
import wave

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))

from capture_sgb_libretro_audio import (BUTTON_IDS, decode_video_row, diagnostic_option,
                                        load_input_script, schedule_input)
from compare_sgb_audio_phase import rank_offsets, series_transitions
from compare_sgb_title_audio import (compare, correlation, read_stereo_wav,
                                     reference_frame_window, reference_sound_event,
                                     resample_mono, scene_agreement,
                                     waveform_alignment)
from compare_sgb_reference_stages import compare_stages
from compare_sgb_sound_event_trace import compare as compare_sound_trace
from compare_sgb_keyon_pcm import compare as compare_keyon_pcm
from compare_sgb_sound_ram import compare as compare_sound_ram


def write_wave(path: Path, rate: int, onset: float) -> None:
    values = array("h")
    for index in range(rate * 5):
        seconds = index / rate
        # A deliberately varied, deterministic envelope after the event.
        phase = seconds - onset
        amplitude = (0 if phase < 0 else
                     int((0.25 + (int(phase * 5) % 4) * 0.13) * 12000))
        value = amplitude if index % 2 == 0 else -amplitude
        values.extend((value, value))
    if sys.byteorder != "little":
        values.byteswap()
    with wave.open(str(path), "wb") as output:
        output.setnchannels(2)
        output.setsampwidth(2)
        output.setframerate(rate)
        output.writeframes(values.tobytes())


def write_scene(path: Path, shift: int = 0, blank: bool = False) -> None:
    pixels = bytearray()
    for y in range(224):
        for x in range(256):
            color = (0 if blank else (x // 8 + y // 8 + shift) % 4) * 64
            pixels.extend((color, color, color))
    path.write_bytes(b"P6\n256 224\n255\n" + pixels)


def main() -> None:
    native = lambda value, width: value.to_bytes(width, sys.byteorder)
    assert decode_video_row(native(0x7C00, 2) + native(0x03E0, 2), 0) == \
        bytes((255, 0, 0, 0, 255, 0))
    assert decode_video_row(native(0xF800, 2) + native(0x001F, 2), 2) == \
        bytes((255, 0, 0, 0, 0, 255))
    assert decode_video_row(native(0x00FF8040, 4), 1) == bytes((255, 128, 64))
    with tempfile.TemporaryDirectory(prefix="gbb-sgb-audio-tools-") as temporary:
        root = Path(temporary)
        script = root / "input.script"
        script.write_text("GBB SGB input v1\n1000 start\n1004 none\n"
                          "1600 a+right\n1604 none\n2800 b\n", encoding="utf-8")
        events = load_input_script(script)
        assert events == [
            (1000, 1 << BUTTON_IDS["start"]), (1004, 0),
            (1600, (1 << BUTTON_IDS["a"]) | (1 << BUTTON_IDS["right"])),
            (1604, 0), (2800, 1 << BUTTON_IDS["b"]),
        ]
        schedule = schedule_input(events, 60.098812, 228,
                                  [(1600, 253), (2800, 321)])
        assert schedule[0][0] < schedule[2][0] < schedule[4][0]
        assert schedule[2][0] == round(1600 * 60.098812 /
                                       (4194304 / 70224)) + 253
        assert schedule[4][0] == round(2800 * 60.098812 /
                                       (4194304 / 70224)) + 321
        for invalid in ("GBB SGB input v1\n1 a+a\n",
                        "GBB SGB input v1\n2 a\n2 none\n",
                        "wrong header\n1 start\n"):
            script.write_text(invalid, encoding="utf-8")
            try:
                load_input_script(script)
            except ValueError:
                pass
            else:
                raise AssertionError("invalid input script was accepted")
        assert abs(correlation([0, 1, 2], [0, 1, 2]) - 1) < 1e-12
        # A shifted, nonperiodic waveform must be found at single-sample
        # precision; an eight-sample lag grid misses this exact match.
        signal = [((index * 131 + 17) % 251) / 251 - 0.5
                  for index in range(512)]
        reference_signal = [0.0] * 100 + signal + [0.0] * 100
        lag, score = waveform_alignment(signal, reference_signal, 100)
        assert lag == 0 and abs(score - 1.0) < 1e-12
        shifted_signal = [0.0] * 103 + signal + [0.0] * 97
        lag, score = waveform_alignment(signal, shifted_signal, 100)
        assert lag == 3 and abs(score - 1.0) < 1e-12
        ramp = array("h", [value for index in range(12)
                           for value in (index * 1000, index * 1000)])
        assert resample_mono(ramp, 4, 0, 1, output_rate=4,
                             time_scale=0.5) == \
            [value / 32768 for value in (0, 500, 1000, 1500)]
        try:
            resample_mono(ramp, 4, 0, 1, time_scale=0)
        except ValueError:
            pass
        else:
            raise AssertionError("zero native clock ratio was accepted")
        gbb = root / "gbb.wav"
        reference = root / "reference.wav"
        gbb_scene = root / "gbb.ppm"
        reference_scene = root / "reference.ppm"
        write_scene(gbb_scene)
        write_scene(reference_scene)
        assert scene_agreement(gbb_scene, reference_scene) == 1.0
        write_wave(gbb, 32000, 1.15)
        write_wave(reference, 48000, 1.15)
        native = root / "native.wav"
        write_wave(native, 32040, 1.15)
        _, reference_pcm = read_stereo_wav(reference)
        _, native_pcm = read_stereo_wav(native)
        stages_timeline = root / "stages-timeline.json"
        stages = {
            "format": "gbb-libretro-audio-timeline-v1",
            "audio_source": "snes-only",
            "sample_rate": 48000, "sample_count": 240000,
            "pcm_sha256": hashlib.sha256(reference_pcm.tobytes()).hexdigest(),
            "native_dsp": {
                "sample_rate": 32040, "sample_count": 160200,
                "pcm_sha256": hashlib.sha256(native_pcm.tobytes()).hexdigest()},
            "run_samples": [[0, 48000], [48000, 48800], [48800, 240000]],
            "sgb_sound_events": [{"run_index": 1, "sample_start": 48000,
                                  "sample_end": 48800}]}
        stages_timeline.write_text(json.dumps(stages), encoding="utf-8")
        stage_report = compare_stages(gbb, reference, native, stages_timeline,
                                      32000, 0)
        assert "libretro: 25ms envelope=" in stage_report
        assert "native DSP: 25ms envelope=" in stage_report
        stages["native_dsp"]["pcm_sha256"] = "0" * 64
        stages_timeline.write_text(json.dumps(stages), encoding="utf-8")
        try:
            compare_stages(gbb, reference, native, stages_timeline, 32000, 0)
        except ValueError as error:
            assert "native PCM does not match" in str(error)
        else:
            raise AssertionError("mismatched native DSP capture was accepted")
        trace_csv = root / "sound-events.csv"
        trace_csv.write_text(
            "kind,master_clock,spc_cycle,pcm_sample,address,value\n"
            "P,1000000,0,32000,0,0\n"
            "D,0,100,32002,92,0\n"
            "H,1357942,0,32500,0,1\n",
            encoding="utf-8")
        trace_timeline = root / "trace-timeline.json"
        trace_reference = {
            "format": "gbb-libretro-audio-timeline-v1",
            "audio_source": "snes-only",
            "sgb_sound_events": [{"packet": "41000000010000000000000000000000",
                                  "run_index": 10, "cpu_vcounter": 21,
                                  "cpu_hcounter": 1118}],
            "post_audible_sound_writes": [
                {"kind": "dsp", "address": 92, "value": 0},
                {"kind": "host", "address": 0, "value": 1,
                 "run_index": 11, "cpu_vcounter": 21, "cpu_hcounter": 534}]}
        trace_timeline.write_text(json.dumps(trace_reference), encoding="utf-8")
        trace_report = compare_sound_trace(trace_csv, trace_timeline, 0)
        assert "GBB 357942 master clocks" in trace_report
        assert "reference 356780..356784 clocks" in trace_report
        assert "Host port/value prefix agrees for 1 writes" in trace_report
        trace_reference["post_audible_sound_writes"][1]["run_index"] = 12
        trace_timeline.write_text(json.dumps(trace_reference), encoding="utf-8")
        try:
            compare_sound_trace(trace_csv, trace_timeline, 0)
        except ValueError as error:
            assert "one known video frame" in str(error)
        else:
            raise AssertionError("unbounded CPU timing comparison was accepted")
        keyon_csv = root / "keyon-events.csv"
        keyon_csv.write_text(
            "kind,master_clock,spc_cycle,pcm_sample,address,value\n"
            "P,1000000,0,31990,0,0\n"
            "D,0,100,32000,76,4\n", encoding="utf-8")
        stages["native_dsp"]["pcm_sha256"] = \
            hashlib.sha256(native_pcm.tobytes()).hexdigest()
        stages["post_audible_sound_writes"] = [
            {"kind": "dsp", "address": 76, "value": 4,
             "dsp_sample": 32040}]
        stages_timeline.write_text(json.dumps(stages), encoding="utf-8")
        keyon_report = compare_keyon_pcm(gbb, native, keyon_csv,
                                          stages_timeline, (0.1, 0.2), 0.1)
        assert "First nonzero DSP KON" in keyon_report
        assert "Later windows do not re-optimize phase" in keyon_report
        assert "0.200s" in keyon_report
        matched_clock = root / "gbb-reference-clock.wav"
        write_wave(matched_clock, 32040, 1.15)
        assert "First nonzero DSP KON" in compare_keyon_pcm(
            matched_clock, native, keyon_csv, stages_timeline, (0.1, 0.2), 0.1)
        stages["post_audible_sound_writes"][0]["value"] = 8
        stages_timeline.write_text(json.dumps(stages), encoding="utf-8")
        try:
            compare_keyon_pcm(gbb, native, keyon_csv, stages_timeline)
        except ValueError as error:
            assert "KON register writes do not match" in str(error)
        else:
            raise AssertionError("mismatched key-on event was accepted")
        ram_csv = root / "ram-events.csv"
        ram_csv.write_text(
            "kind,master_clock,spc_cycle,pcm_sample,address,value\n"
            "P,1000000,0,31990,0,0\n"
            "D,0,100,32000,76,4\n"
            "Q,0,100,32000,2,4\n"
            "R,0,0,55000,4660,42\n"
            "D,0,101,57600,12,7\n" +
            "D,0,102,58360,76,4\n" +
            "Q,0,102,58360,62,4\n" +
            "".join(f"V,0,0,{sample},{address},0\n"
                    for sample in (56000, 58240, 59000)
                    for address in range(25)), encoding="utf-8")
        stages["post_audible_sound_writes"] = [
            {"kind": "dsp", "address": 76, "value": 4,
             "dsp_sample": 32040, "dsp_clock64": 37},
            {"kind": "ram", "address": 4660, "value": 42,
             "dsp_sample": 55068},
            {"kind": "dsp", "address": 12, "value": 7,
             "dsp_sample": 57672},
            {"kind": "dsp", "address": 76, "value": 4,
             "dsp_sample": 58450, "dsp_clock64": 41}]
        stages["post_audible_sound_writes"].extend(
            {"kind": "state", "address": address, "value": 0,
             "dsp_sample": sample}
            for sample in (56070, 58313, 59090) for address in range(25))
        stages_timeline.write_text(json.dumps(stages), encoding="utf-8")
        ram_report = compare_sound_ram(ram_csv, stages_timeline)
        assert "RAM address/value ordered prefix: 1 writes" in ram_report
        assert "+0.80s DSP write-derived registers: 2 common, 0 differ" in ram_report
        assert "Second-KON elapsed outputs: GBB=640, reference=640" in ram_report
        assert "GBB=62, reference=41; different write phase" in ram_report
        assert diagnostic_option(b"bsnes_dsp_fast", True) == b"OFF"
        assert diagnostic_option(b"bsnes_dsp_fast", False) is None
        assert diagnostic_option(b"unrelated", True) is None
        assert diagnostic_option(b"bsnes_entropy", True) is None
        assert diagnostic_option(b"bsnes_entropy", True, "None") == b"None"
        assert diagnostic_option(b"bsnes_entropy", False, "None") is None
        assert diagnostic_option(b"bsnes_dsp_fast", True, "None") == b"OFF"
        for row in stages["post_audible_sound_writes"]:
            if row["kind"] == "state" and row["dsp_sample"] == 59090:
                row["dsp_sample"] += 1
        stages_timeline.write_text(json.dumps(stages), encoding="utf-8")
        try:
            compare_sound_ram(ram_csv, stages_timeline)
        except ValueError as error:
            assert "exactly 640 native samples" in str(error)
        else:
            raise AssertionError("off-by-one checkpoint was accepted")
        for row in stages["post_audible_sound_writes"]:
            if row["kind"] == "state" and row["dsp_sample"] == 59091:
                row["dsp_sample"] -= 1
        stages_timeline.write_text(json.dumps(stages), encoding="utf-8")
        try:
            compare_sound_ram(ram_csv, stages_timeline, require_cycle_checkpoints=True)
        except ValueError as error:
            assert "immediately after DSP phase 27" in str(error)
        else:
            raise AssertionError("unclocked checkpoint was accepted")
        original_ram_csv = ram_csv.read_text(encoding="utf-8")
        ram_csv.write_text(original_ram_csv.replace("V,0,0,", "V,0,100,"),
                           encoding="utf-8")
        for row in stages["post_audible_sound_writes"]:
            if row["kind"] == "state":
                row["dsp_phase"] = 28
        stages_timeline.write_text(json.dumps(stages), encoding="utf-8")
        assert "GBB=640, reference=640" in compare_sound_ram(
            ram_csv, stages_timeline, require_cycle_checkpoints=True)
        stages["post_audible_sound_writes"][-1]["dsp_phase"] = 0
        stages_timeline.write_text(json.dumps(stages), encoding="utf-8")
        try:
            compare_sound_ram(ram_csv, stages_timeline, require_cycle_checkpoints=True)
        except ValueError as error:
            assert "immediately after DSP phase 27" in str(error)
        else:
            raise AssertionError("batched reference checkpoint was accepted")
        ram_csv.write_text(original_ram_csv, encoding="utf-8")
        stages["post_audible_sound_writes"][1]["value"] = 43
        stages_timeline.write_text(json.dumps(stages), encoding="utf-8")
        ram_report = compare_sound_ram(ram_csv, stages_timeline)
        assert "RAM address/value multisets differ" in ram_report
        gbb_ram = root / "gbb-ram.bin"
        ref_ram = root / "ref-ram.bin"
        gbb_ram.write_bytes(bytes(65536))
        ref_ram.write_bytes(bytes(65536))
        stages["apu_ram"] = {"size": 65536,
                             "sha256": hashlib.sha256(bytes(65536)).hexdigest()}
        stages_timeline.write_text(json.dumps(stages), encoding="utf-8")
        ram_report = compare_sound_ram(ram_csv, stages_timeline,
                                       gbb_ram, ref_ram)
        assert "byte-identical" in ram_report
        changed = bytearray(65536)
        changed[0x4db0] = 1
        gbb_ram.write_bytes(changed)
        ram_report = compare_sound_ram(ram_csv, stages_timeline,
                                       gbb_ram, ref_ram)
        assert "1 differing bytes" in ram_report
        stages["post_audible_sound_writes"][0]["value"] = 4
        stages["native_dsp"]["pcm_sha256"] = "0" * 64
        stages_timeline.write_text(json.dumps(stages), encoding="utf-8")
        try:
            compare_keyon_pcm(gbb, native, keyon_csv, stages_timeline)
        except ValueError as error:
            assert "does not match its timeline" in str(error)
        else:
            raise AssertionError("unbound native DSP capture was accepted")
        report = compare(gbb, reference, 32000, 60, 0.1, 2.0,
                         gbb_scene=gbb_scene, reference_scene=reference_scene)
        assert "Same-scene checkpoint: 100.0%" in report
        assert "25ms RMS-envelope correlation at nominal frame=" in report
        assert "Exploratory fixed 0.2s waveform windows:" in report
        assert "best within search range=" in report
        assert "does not establish waveform fidelity" in report
        try:
            compare(gbb, reference, 32000, 60, 0.1, 2.0,
                    gbb_scene=gbb_scene, reference_scene=reference_scene,
                    gbb_dsp_rate=32000)
        except ValueError as error:
            assert "both native DSP rates" in str(error)
        else:
            raise AssertionError("unpaired native DSP rate was accepted")
        report = compare(gbb, reference, 32000, 60, 0.1, 2.0,
                         gbb_scene=gbb_scene, reference_scene=reference_scene,
                         gbb_dsp_rate=32000, reference_dsp_rate=32040)
        assert "32000/32040=0.998752" in report
        phase = root / "phase"
        phase.mkdir()
        for number, shift in ((10, 0), (11, 1), (12, 1), (13, 0)):
            write_scene(phase / f"gbb-{number}.ppm", shift=shift)
        frame_range, transitions, threshold = series_transitions(
            str(phase / "gbb-*.ppm"), 0)
        assert frame_range == (10, 13) and set(transitions) == {11, 13}
        assert threshold > 0
        (phase / "gbb-12.ppm").unlink()
        try:
            series_transitions(str(phase / "gbb-*.ppm"), 0)
        except ValueError as error:
            assert "no missing frames" in str(error)
        else:
            raise AssertionError("gapped phase series accepted")
        ranked = rank_offsets((10, 14), {11: 500, 13: 500},
                              (110, 114), {111: 450, 113: 450}, 102, 3)
        assert ranked[0]["offset"] == 100
        assert ranked[0]["matched"] == 2 and ranked[0]["disagreements"] == 0
        try:
            rank_offsets((10, 14), {11: 500, 13: 500},
                         (110, 114), {111: 450, 113: 450}, 102, 121)
        except ValueError as error:
            assert "offset window" in str(error)
        else:
            raise AssertionError("unbounded phase search accepted")
        try:
            compare(gbb, reference, 32000, 60, 0.1, 2.0,
                    gbb_scene=gbb_scene, reference_scene=reference_scene,
                    reference_scene_frame=60)
        except ValueError as error:
            assert "requires a reference timeline" in str(error)
        else:
            raise AssertionError("unbound scene frame accepted")
        timeline = root / "reference-timeline.json"
        timeline.write_text(json.dumps({
            "format": "gbb-libretro-audio-timeline-v1",
            "sample_rate": 48000, "sample_count": 240000,
            "pcm_sha256": hashlib.sha256(reference_pcm.tobytes()).hexdigest(),
            "snapshot_sha256": {
                "60": hashlib.sha256(reference_scene.read_bytes()).hexdigest()},
            "run_samples": [[0, 47600], [47600, 48500], [48500, 240000]],
            "video_callbacks": [{"video_frame": 60, "run_index": 1,
                                 "sample_at_callback": 48000}]}), encoding="utf-8")
        assert reference_frame_window(timeline, 60, 48000, reference_pcm) == (47600, 48500)
        report = compare(gbb, reference, 32000, 60, 0.1, 2.0,
                         gbb_scene=gbb_scene, reference_scene=reference_scene,
                         reference_timeline=timeline, reference_scene_frame=60)
        assert "callback audio range [47600, 48500]" in report
        instrumented = json.loads(timeline.read_text(encoding="utf-8"))
        instrumented["audio_source"] = "snes-only"
        instrumented["sgb_sound_events"] = [{
            "run_index": 1, "video_frame_after_run": 60,
            "sample_start": 47600, "sample_end": 48500,
            "packet": "41000000010000000000000000000000"}]
        timeline.write_text(json.dumps(instrumented), encoding="utf-8")
        assert reference_sound_event(timeline, 0, bytes.fromhex("4100000001"),
                                     60, 48000, reference_pcm, reference_scene) == \
            (47600, 48500, "41000000010000000000000000000000")
        instrumented["sgb_sound_events"][0]["cpu_vcounter"] = 262
        instrumented["sgb_sound_events"][0]["cpu_hcounter"] = 20
        timeline.write_text(json.dumps(instrumented), encoding="utf-8")
        try:
            reference_sound_event(timeline, 0, bytes.fromhex("4100000001"),
                                  60, 48000, reference_pcm, reference_scene)
        except ValueError as error:
            assert "CPU position" in str(error)
        else:
            raise AssertionError("invalid reference CPU position was accepted")
        del instrumented["sgb_sound_events"][0]["cpu_vcounter"]
        del instrumented["sgb_sound_events"][0]["cpu_hcounter"]
        timeline.write_text(json.dumps(instrumented), encoding="utf-8")
        report = compare(gbb, reference, 32000, 60, 0.1, 2.0,
                         gbb_scene=gbb_scene, reference_scene=reference_scene,
                         reference_timeline=timeline, reference_scene_frame=60,
                         reference_sound_event_index=0,
                         reference_sound_packet_prefix=bytes.fromhex("4100000001"))
        assert "host-consumed SOUND event 0" in report
        assert "SNES-only reference" in report
        repeated_timeline = root / "repeated-timeline.json"
        repeated = json.loads(timeline.read_text(encoding="utf-8"))
        repeated["sgb_sound_events"][0].update(
            {"cpu_vcounter": 21, "cpu_hcounter": 1118})
        repeated["run_samples"] = [[0, 47600], [47600, 48500],
                                   [48500, 95000], [95000, 96000],
                                   [96000, 240000]]
        repeated["video_callbacks"].append({"video_frame": 61,
                                             "run_index": 3,
                                             "sample_at_callback": 95500})
        repeated["sgb_sound_events"].append({
            "run_index": 3, "video_frame_after_run": 61,
            "sample_start": 95000, "sample_end": 96000,
            "cpu_vcounter": 24, "cpu_hcounter": 894,
            "packet": "41000000010000000000000000000000"})
        repeated_timeline.write_text(json.dumps(repeated), encoding="utf-8")
        report = compare(gbb, reference, 32000, 60, 0.1, 2.0,
                         gbb_scene=gbb_scene, reference_scene=reference_scene,
                         reference_timeline=repeated_timeline,
                         reference_scene_frame=60,
                         reference_sound_event_index=0,
                         reference_sound_packet_prefix=bytes.fromhex("4100000001"),
                         gbb_dsp_rate=32000, reference_dsp_rate=32040,
                         gbb_repeat_event_sample=64000,
                         reference_repeat_sound_event_index=1)
        assert "Repeated SOUND packet: GBB sample 64000" in report
        assert "first V=21 H=1118, repeated V=24 H=894" in report
        assert "Independent 0.1-0.5s post-packet waveform windows:" in report
        try:
            compare(gbb, reference, 32000, 60, 0.1, 2.0,
                    gbb_scene=gbb_scene, reference_scene=reference_scene,
                    reference_timeline=repeated_timeline,
                    reference_scene_frame=60,
                    reference_sound_event_index=0,
                    reference_sound_packet_prefix=bytes.fromhex("4100000001"),
                    gbb_repeat_event_sample=64000)
        except ValueError as error:
            assert "both repeated SOUND anchors" in str(error)
        else:
            raise AssertionError("unpaired repeated SOUND anchor was accepted")
        repeated["sgb_sound_events"][1]["packet"] = \
            "41000000020000000000000000000000"
        repeated_timeline.write_text(json.dumps(repeated), encoding="utf-8")
        try:
            compare(gbb, reference, 32000, 60, 0.1, 2.0,
                    gbb_scene=gbb_scene, reference_scene=reference_scene,
                    reference_timeline=repeated_timeline,
                    reference_scene_frame=60,
                    reference_sound_event_index=0,
                    reference_sound_packet_prefix=bytes.fromhex("41"),
                    gbb_repeat_event_sample=64000,
                    reference_repeat_sound_event_index=1)
        except ValueError as error:
            assert "differs from first" in str(error)
        else:
            raise AssertionError("different repeated SOUND packet was accepted")
        try:
            reference_sound_event(timeline, 0, bytes.fromhex("418000"),
                                  60, 48000, reference_pcm, reference_scene)
        except ValueError as error:
            assert "differs from expected" in str(error)
        else:
            raise AssertionError("wrong SOUND packet accepted")
        instrumented["audio_source"] = "mixed"
        timeline.write_text(json.dumps(instrumented), encoding="utf-8")
        try:
            reference_sound_event(timeline, 0, bytes.fromhex("4100000001"),
                                  60, 48000, reference_pcm, reference_scene)
        except ValueError as error:
            assert "SNES-only" in str(error)
        else:
            raise AssertionError("mixed audio accepted as an isolated reference")
        instrumented["audio_source"] = "snes-only"
        timeline.write_text(json.dumps(instrumented), encoding="utf-8")
        wrong_scene = root / "wrong-reference.ppm"
        write_scene(wrong_scene, shift=0)
        wrong_scene.write_bytes(wrong_scene.read_bytes() + b"\n")
        try:
            reference_frame_window(timeline, 60, 48000, reference_pcm, wrong_scene)
        except ValueError as error:
            assert "scene does not match" in str(error)
        else:
            raise AssertionError("unbound reference scene accepted")
        for wrong_frame, wrong_rate in ((61, 48000), (60, 32000)):
            try:
                reference_frame_window(timeline, wrong_frame, wrong_rate, reference_pcm)
            except ValueError:
                pass
            else:
                raise AssertionError("invalid reference timeline accepted")
        try:
            compare(gbb, reference, 32000, 60, 0.1, 2.0,
                    gbb_scene=gbb_scene, reference_scene=reference_scene,
                    reference_timeline=timeline, reference_scene_frame=61)
        except ValueError as error:
            assert "differs from predicted" in str(error)
        else:
            raise AssertionError("mismatched reference scene frame accepted")
        broken = json.loads(timeline.read_text(encoding="utf-8"))
        broken["pcm_sha256"] = "0" * 64
        timeline.write_text(json.dumps(broken), encoding="utf-8")
        try:
            reference_frame_window(timeline, 60, 48000, reference_pcm)
        except ValueError as error:
            assert "PCM content" in str(error)
        else:
            raise AssertionError("wrong reference PCM accepted")
        broken["pcm_sha256"] = hashlib.sha256(reference_pcm.tobytes()).hexdigest()
        broken["run_samples"][1][0] += 1
        timeline.write_text(json.dumps(broken), encoding="utf-8")
        try:
            reference_frame_window(timeline, 60, 48000, reference_pcm)
        except ValueError as error:
            assert "noncontiguous" in str(error)
        else:
            raise AssertionError("noncontiguous reference audio accepted")
        write_scene(reference_scene, shift=1)
        assert scene_agreement(gbb_scene, reference_scene) < 0.65
        try:
            compare(gbb, reference, 32000, 60, 0.1, 2.0,
                    gbb_scene=gbb_scene, reference_scene=reference_scene)
        except ValueError as error:
            assert "scene checkpoint mismatch" in str(error)
        else:
            raise AssertionError("wrong scene was accepted")
        write_scene(reference_scene, blank=True)
        try:
            scene_agreement(gbb_scene, reference_scene)
        except ValueError as error:
            assert "too uniform" in str(error)
        else:
            raise AssertionError("blank scene was accepted")
        print("script mapping and guarded audio comparison pass")


if __name__ == "__main__":
    main()
