#!/usr/bin/env python3
"""Execute donor enum-to-string methods; no Android services or kernel run.

Dependencies: pyelftools, unicorn. Supply the original donor shared library.
The hash guard prevents applying these findings to a different binary version.
"""
import argparse
import hashlib
import json
from pathlib import Path

from elftools.elf.elffile import ELFFile
from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM
from unicorn.arm64_const import UC_ARM64_REG_PC, UC_ARM64_REG_X0, UC_ARM64_REG_X30

EXPECTED_SHA = "068597b574985be80ff760973a4920e567a3cb21f14d108e0eb9401b634f1877"
CASES = {
    "_ZN3qc24v4l23StrENS0_11OutputOrderE": [
        "V4L2_DISPLAY_ORDER", "V4L2_DECODER_ORDER", "?", "?"
    ],
    "_ZN3qc24v4l23StrENS0_14LowLatencyModeE": [
        "LOWLATENCY_DISABLE", "LOWLATENCY_ENABLE", "?", "?"
    ],
}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("library", type=Path)
    args = parser.parse_args()
    actual_sha = hashlib.sha256(args.library.read_bytes()).hexdigest()
    if actual_sha != EXPECTED_SHA:
        parser.error("unrecognized donor library SHA256; refusing version-specific execution")

    with args.library.open("rb") as stream:
        elf = ELFFile(stream)
        segments = [(s["p_vaddr"], s.data()) for s in elf.iter_segments()
                    if s["p_type"] == "PT_LOAD"]
        symbols = {s.name: s["st_value"]
                   for s in elf.get_section_by_name(".dynsym").iter_symbols()
                   if s["st_shndx"] != "SHN_UNDEF"}

    findings = []
    for name, expected_values in CASES.items():
        for value, expected in enumerate(expected_values):
            emulator = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
            emulator.mem_map(0, 0x200000)
            emulator.mem_map(0xa00000, 0x1000)
            for address, data in segments:
                emulator.mem_write(address, data)
            emulator.reg_write(UC_ARM64_REG_X0, value)
            emulator.reg_write(UC_ARM64_REG_X30, 0xa00000)
            emulator.emu_start(symbols[name], 0xa00000, count=1000)
            if emulator.reg_read(UC_ARM64_REG_PC) != 0xa00000:
                raise RuntimeError("enum method did not return within the instruction budget")
            address = emulator.reg_read(UC_ARM64_REG_X0)
            observed = bytes(emulator.mem_read(address, 80)).split(b"\0", 1)[0].decode()
            if observed != expected:
                raise RuntimeError(f"{name}({value}): expected {expected}, got {observed}")
            findings.append({"symbol": name, "value": value, "string": observed})

    print(json.dumps({"passed": True, "sha256": actual_sha, "findings": findings,
                      "kernel_executed": False, "hardware_test": False}, indent=2))


if __name__ == "__main__":
    main()
