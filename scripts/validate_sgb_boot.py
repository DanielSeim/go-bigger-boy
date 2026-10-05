#!/usr/bin/env python3
"""Opt-in opaque SGB boot comparison; never exports firmware or artwork."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import tempfile


def packets(writes):
    result, bits = [], []
    receiving, armed = False, True
    for _, address, value in writes:
        if address != 0xff00:
            continue
        lines = value & 0x30
        if lines == 0x30:
            armed = True
        elif lines == 0 and armed:
            if receiving:
                raise AssertionError("new start before zero stop bit")
            receiving, armed, bits = True, False, []
        elif lines in (0x10, 0x20) and receiving and armed:
            armed = False
            if len(bits) == 128:
                if lines != 0x20:
                    raise AssertionError("invalid stop bit")
                result.append(bytes(sum(bits[i+b] << b for b in range(8))
                                    for i in range(0, 128, 8)))
                receiving = False
            else:
                bits.append(int(lines == 0x10))
    if receiving or len(result) != 6:
        raise AssertionError("incomplete bootstrap")
    return result


def compare(replacement, reference):
    a, b = replacement["handoff"], reference["handoff"]
    if a["cpu"] != b["cpu"]:
        raise AssertionError("CPU handoff differs")
    if a["wram"][:96] != b["wram"][:96]:
        raise AssertionError("header packet buffer differs")
    if packets(replacement["boot_writes"]) != packets(reference["boot_writes"]):
        raise AssertionError("transmitted header packets differ")
    # Presentation/stack scratch and exact LCD/DIV/APU phase are not claimed
    # equivalent. Report timing separately rather than hiding its differences.
    for index in (0, 1, 2, 5, 6, 7, *range(0x10, 0x40), 0x40, 0x42, 0x43,
                  0x45, 0x47, 0x48, 0x49, 0x4a, 0x4b, 0x50):
        if a["io"][index] != b["io"][index]:
            raise AssertionError(f"readable IO differs at {0xff00+index:04x}")
    return {"contract": "pass", "cycle_exact": False,
            "replacement": {k: a[k] for k in ("cycles", "divider_counter", "ppu_dot", "ppu_mode")},
            "reference": {k: b[k] for k in ("cycles", "divider_counter", "ppu_dot", "ppu_mode")}}


def capture(probe, model, rom, boot=None):
    command = [str(probe), str(rom), "--model", model, "--boot-trace", "--run-cycles", "0"]
    if boot:
        command += ["--boot-rom", str(boot)]
    return json.loads(subprocess.run(command, check=True, text=True, capture_output=True,
                                     timeout=30).stdout)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--probe", type=Path, required=True)
    parser.add_argument("--reference-dir", type=Path, required=True)
    parser.add_argument("--rom", type=Path, action="append", default=[])
    parser.add_argument("--report", type=Path, required=True)
    args = parser.parse_args()
    report = {"format": "gbb-sgb-boot-contract-v1", "cases": []}
    with tempfile.TemporaryDirectory(prefix="gbb-sgb-boot-contract-") as directory:
        cases = list(args.rom)
        for pattern in (0, 1, 0x55, 0xff):
            path = Path(directory)/f"synthetic-{pattern}.gb"
            image = bytearray(32768)
            image[0x100] = 0x76
            for address in range(0x104, 0x150):
                image[address] = (pattern*(address-0x104)) & 255
            for address in (0x143, 0x147, 0x148, 0x149):
                image[address] = 0
            image[0x146] = 3
            path.write_bytes(image)
            cases.append(path)
        for model in ("sgb", "sgb2"):
            boot = args.reference_dir/("sgb.boot.rom" if model == "sgb" else "sgb2.boot.rom")
            boot_hash = hashlib.sha256(boot.read_bytes()).hexdigest()
            for rom in cases:
                result = compare(capture(args.probe, model, rom), capture(args.probe, model, rom, boot))
                result.update(model=model, cartridge=rom.name,
                              cartridge_sha256=hashlib.sha256(rom.read_bytes()).hexdigest(),
                              reference_boot_sha256=boot_hash)
                report["cases"].append(result)
    args.report.write_text(json.dumps(report, indent=2)+"\n", encoding="utf-8")
    print(f"SGB/SGB2: {len(report['cases'])} packet/handoff contracts passed; cycle equivalence not claimed")


if __name__ == "__main__":
    main()
