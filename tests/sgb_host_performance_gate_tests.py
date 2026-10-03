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


def report(model="sgb1", combined=True, ratio=2.0):
    return {"format": "gbb-sgb-host-performance-v1", "model": model, "combined": combined,
            "windows_complete": True, "restorations": 0, "output_hz": 48000,
            "windows": [{"master_clocks": gate.MASTER_HZ, "seconds": 1/ratio} for _ in range(60)]}


def main():
    assert gate.summarize(report())["p05"] == 2
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
        assert subprocess.run(command, capture_output=True).returncode == 0
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
