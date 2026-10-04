#!/usr/bin/env python3
import importlib.util
from pathlib import Path
import subprocess
import sys
import tempfile
import json

spec = importlib.util.spec_from_file_location("gate", Path(__file__).resolve().parents[1] / "scripts/check_sgb_host_performance.py")
gate = importlib.util.module_from_spec(spec)
spec.loader.exec_module(gate)
spec_diagnostics = importlib.util.spec_from_file_location("diagnostics", Path(gate.__file__).with_name("diagnose_sgb_host_performance.py"))
sys.path.insert(0, str(Path(gate.__file__).parent))
diagnostic = importlib.util.module_from_spec(spec_diagnostics)
spec_diagnostics.loader.exec_module(diagnostic)
spec_benchmark = importlib.util.spec_from_file_location("benchmark", Path(gate.__file__).with_name("benchmark_sgb_host.py"))
benchmark = importlib.util.module_from_spec(spec_benchmark)
spec_benchmark.loader.exec_module(benchmark)


def report(model="sgb1", combined=True, ratio=2.0):
    return {"format": "gbb-sgb-host-performance-v1", "model": model, "combined": combined,
            "windows_complete": True, "restorations": 0, "output_hz": 48000,
            "windows": [{"master_clocks": gate.MASTER_HZ, "seconds": 1/ratio} for _ in range(60)]}


def main():
    assert gate.summarize(report())["p05"] == 2
    measured = report()
    for window in measured["windows"]:
        window.update(cpu_seconds=0.25, voluntary_switches=1, involuntary_switches=2,
                      processor_before=0, processor_after=1)
    measured.update(calibration_before_seconds=0.01, calibration_after_seconds=0.02)
    explained = diagnostic.diagnostics(measured)
    assert explained["cpu_wall_ratio"] == 0.5
    assert explained["changed_processor_endpoints"] == 60
    assert explained["involuntary_switches"] == 120
    assert explained["calibration_after_before_ratio"] == 2
    assert explained["p05"] == 2  # Diagnostics never change wall-time acceptance.
    assert "cpu_wall_ratio" not in diagnostic.diagnostics(report())
    measured["windows"][0]["cpu_seconds"] = float("nan")
    assert "cpu_wall_ratio" not in diagnostic.diagnostics(measured)
    for bad in (dict(report(), windows_complete=False), dict(report(), windows_complete="false"),
                dict(report(), output_hz=0), dict(report(), restorations=1),
                dict(report(), windows=report()["windows"][:20]),
                dict(report(), windows=[{"master_clocks": gate.MASTER_HZ, "seconds": float("nan")}])):
        try:
            gate.summarize(bad)
        except ValueError:
            pass
        else:
            raise AssertionError("invalid benchmark accepted")
    with tempfile.TemporaryDirectory() as root:
        paths=[]
        for model in ("sgb1", "sgb2"):
            for combined in (False, True):
                path=Path(root)/f"{model}-{combined}.json"
                path.write_text(json.dumps(report(model, combined)))
                paths.append(str(path))
        command=[sys.executable, str(Path(gate.__file__)), *paths]
        original = json.loads(Path(paths[0]).read_text())
        for label in (None, "balanced", "silent"):
            benchmark.annotate_report(Path(paths[0]), label)
            annotated = json.loads(Path(paths[0]).read_text())
            context = annotated.pop("benchmark_context")
            assert context == {"power_profile_label": label,
                               "power_profile_source": "caller-declared" if label else "unspecified"}
            assert annotated == original
            assert gate.summarize(annotated) == gate.summarize(original)
        assert subprocess.run(command, capture_output=True).returncode == 0
        Path(paths[-1]).write_text(json.dumps(report("sgb2", True, 1.395)))
        marginal = subprocess.run(command, capture_output=True, text=True)
        assert marginal.returncode == 1 and "p05 1.395x" in marginal.stdout
        Path(paths[-1]).write_text(json.dumps(report("sgb2", True, 1.4)))
        assert subprocess.run(command, capture_output=True).returncode == 0
        Path(paths[-1]).write_text(json.dumps(report("sgb2", True, 1.495)))
        assert subprocess.run(command + ["--minimum-ratio", "1.5"],
                              capture_output=True).returncode == 1
        Path(paths[-1]).write_text(json.dumps(report("sgb2", True)))
        # Overall/median speed is fine, but sustained slow windows must fail.
        slow=report("sgb2", True)
        for window in slow["windows"][20:30]:
            window["seconds"]=0.9
        Path(paths[-1]).write_text(json.dumps(slow))
        assert subprocess.run(command, capture_output=True).returncode == 1
        assert subprocess.run(command[:-1], capture_output=True).returncode == 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
