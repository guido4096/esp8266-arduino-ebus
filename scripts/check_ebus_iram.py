#!/usr/bin/env python3
"""Check project eBUS ISR bodies and their direct calls in a linked ESP32-C3 ELF.

This catches flash-resident compiler helpers (including atomic<bool>::load).
It is a focused regression check, not a proof of all driver transitive paths
or of physical interrupt latency. ESP-IDF driver cache safety is configured
separately in sdkconfig.defaults.
"""
import argparse
import os
from pathlib import Path
import re
import shutil
import subprocess


METHODS = ("onFallingEdge()", "onBusIsrTimer()", "onSynGenTimer()")


def check(elf, objdump):
    table = subprocess.check_output([str(objdump), "-t", "-C", str(elf)], text=True)
    for method in METHODS:
        name = "ebus::detail::platform::BusEsp::" + method
        rows = [line.split(None, 5) for line in table.splitlines() if line.endswith(name)]
        if len(rows) != 1:
            raise RuntimeError(f"Missing or ambiguous ISR: {name}")
        address, _, kind, section, size, _ = rows[0]
        if kind != "F" or section != ".iram0.text":
            raise RuntimeError(f"ISR is not in IRAM: {name}: {section}")
        start = int(address, 16)
        disassembly = subprocess.check_output([
            str(objdump), "-d", "-C", f"--start-address={start}",
            f"--stop-address={start + int(size, 16)}", str(elf)], text=True)
        for line in disassembly.splitlines():
            # Direct jal and the resolved jalr/auipc pairs produced by this
            # toolchain both annotate their target with '<symbol>'.
            if not re.search(r"\b(?:jal|jalr|j)\b", line):
                continue
            target = re.search(r"\b([0-9a-f]{8})\s+<", line)
            if target and int(target[1], 16) >= 0x42000000:
                raise RuntimeError(f"Flash call from {name}: {line.strip()}")
        print(f"PASS IRAM body/direct calls: {name}")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("elf", type=Path)
    core = Path(os.environ.get("PLATFORMIO_CORE_DIR", Path.home() / ".platformio"))
    default = shutil.which("riscv32-esp-elf-objdump") or (
        core / "packages/toolchain-riscv32-esp/bin/riscv32-esp-elf-objdump")
    parser.add_argument("--objdump", type=Path, default=default)
    args = parser.parse_args()
    check(args.elf, args.objdump)
