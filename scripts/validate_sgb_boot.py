#!/usr/bin/env python3
"""Opt-in opaque SGB boot comparison; never exports firmware or artwork."""
import argparse
import hashlib
import json
import random
import re
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
    if a["io"] != b["io"] or a["ie"] != b["ie"]:
        raise AssertionError("readable IO handoff differs")
    # All observable boot I/O writes, not merely packet values or pulse minima.
    if replacement["boot_writes"] != reference["boot_writes"]:
        raise AssertionError("boot IO timeline differs")
    phases = ("cycles", "divider_counter", "ppu_dot", "ppu_scanline", "ppu_mode", "serial_phase",
              "serial_bits", "apu_clocks")
    for field in phases:
        if a[field] != b[field]:
            raise AssertionError(f"handoff phase differs: {field}")
    # This is exact equivalence of the measured contract in the same core,
    # not a claim about all hardware behavior, VRAM artwork or analog output.
    return {"contract": "pass", "cycle_exact": True,
            "replacement": {k: a[k] for k in phases},
            "reference": {k: b[k] for k in phases}}


def capture(probe, model, rom, boot=None):
    command = [str(probe), str(rom), "--model", model, "--boot-trace", "--run-cycles", "0"]
    if boot:
        command += ["--boot-rom", str(boot)]
    return json.loads(subprocess.run(command, check=True, text=True, capture_output=True,
                                     timeout=30).stdout)


def compare_independent(replacement, reference):
    for field in ("af", "bc", "de", "hl", "sp", "pc", "ly", "div", "stat",
                  "divider_counter", "apu_phase", "ppu_phase"):
        if replacement[field] != reference[field]:
            raise AssertionError(f"independent handoff differs: {field}")
    def normalize(snapshot):
        # Anchor the reference core's execution clock to the first JOYP idle
        # write; unlike cores need not use the same execution time origin.
        origin=next(t for t,a,v in snapshot["writes"] if a==0xff00 and v==0x30)-32
        return ([[t-origin,a,v] for t,a,v in snapshot["writes"]],
                [[a-origin,b-origin,v,n] for a,b,v,n in snapshot["ly_reads"]])
    if normalize(replacement) != normalize(reference):
        raise AssertionError("independent IO/read timeline differs")
    return {"contract":"pass", "divider_counter":replacement["divider_counter"],
            "apu_phase":replacement["apu_phase"]}


def independent_capture(probe, model, rom, boot):
    return json.loads(subprocess.run([str(probe),model,str(rom),str(boot),"--trace"],
                     check=True,text=True,capture_output=True,timeout=60).stdout)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--probe", type=Path, required=True)
    parser.add_argument("--reference-dir", type=Path, required=True)
    parser.add_argument("--rom", type=Path, action="append", default=[])
    parser.add_argument("--report", type=Path, required=True)
    parser.add_argument("--independent-probe",type=Path,
                        help="optional execution-only SameBoy probe built from sgb_boot_handoff_reference.c")
    args = parser.parse_args()
    report = {"format": "gbb-sgb-boot-contract-v2", "cases": [],
              "gbb_probe_sha256":hashlib.sha256(args.probe.read_bytes()).hexdigest()}
    if args.independent_probe:
        report["independent_probe_sha256"]=hashlib.sha256(args.independent_probe.read_bytes()).hexdigest()
    with tempfile.TemporaryDirectory(prefix="gbb-sgb-boot-contract-") as directory:
        replacements={}
        if args.independent_probe:
            root=Path(__file__).resolve().parents[1]
            for model in ("sgb","sgb2"):
                header=(root/"firmware/gameboy"/f"{model}_boot_image.hpp").read_text()
                image=bytes(int(b,16) for b in re.findall(r"0x([0-9A-F]{2})",header))
                if len(image)!=256: raise AssertionError("invalid bundled image size")
                replacements[model]=Path(directory)/f"{model}.bin"
                replacements[model].write_bytes(image)
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
        # Repeatable high-entropy headers exercise checksum/popcount timing and
        # 32-clock VBlank poll boundaries, without proprietary cartridge data.
        for seed in range(16):
            rng=random.Random(seed)
            image=bytearray(32768); image[0x100]=0x76
            image[0x104:0x150]=bytes(rng.randrange(256) for _ in range(76))
            for address in (0x143,0x147,0x148,0x149): image[address]=0
            image[0x146]=3
            path=Path(directory)/f"synthetic-random-{seed}.gb"
            path.write_bytes(image); cases.append(path)
        for model in ("sgb", "sgb2"):
            boot = args.reference_dir/("sgb.boot.rom" if model == "sgb" else "sgb2.boot.rom")
            boot_hash = hashlib.sha256(boot.read_bytes()).hexdigest()
            for rom in cases:
                result = compare(capture(args.probe, model, rom), capture(args.probe, model, rom, boot))
                result.update(model=model, cartridge=rom.name,
                              cartridge_sha256=hashlib.sha256(rom.read_bytes()).hexdigest(),
                              reference_boot_sha256=boot_hash)
                if args.independent_probe:
                    result["independent"]=compare_independent(
                        independent_capture(args.independent_probe,model,rom,replacements[model]),
                        independent_capture(args.independent_probe,model,rom,boot))
                report["cases"].append(result)
    args.report.write_text(json.dumps(report, indent=2)+"\n", encoding="utf-8")
    print(f"SGB/SGB2: {len(report['cases'])} exact IO timeline/handoff phase contracts passed")


if __name__ == "__main__":
    main()
