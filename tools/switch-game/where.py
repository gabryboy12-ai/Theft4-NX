#!/usr/bin/env python3
"""Decode the module offsets in a switch-game log with addr2line.

Usage: where.py game.log [game-crash.log ...] [--elf out/build/switch-game/switch-game.elf]

Every "+0x..." in the thread snapshot lines ("  thread ..."), in TERMINATION
lines ("module base + 0x...") and every "pc - base"/"lr - base" of
game-crash.log is an ELF address of switch-game.elf. The script prints each
line followed by the function (and inlined chain) of each address. The ELF
must be the one of the build that ran.
"""

import argparse
import os
import re
import shutil
import subprocess
import sys

DEFAULT_ELF = os.path.join(os.path.dirname(__file__), "..", "..", "out", "build", "switch-game",
                           "switch-game.elf")
OFFSET = re.compile(r"(?:\+ ?0x|base \+ 0x|- base\s+0x)([0-9a-fA-F]+)")


def addr2line_tool():
    for name in ("aarch64-none-elf-addr2line",
                 "/opt/devkitpro/devkitA64/bin/aarch64-none-elf-addr2line",
                 "C:/devkitPro/devkitA64/bin/aarch64-none-elf-addr2line.exe"):
        if shutil.which(name) or os.path.exists(name):
            return name
    sys.exit("aarch64-none-elf-addr2line not found (devkitA64)")


def decode(tool, elf, addresses):
    if not addresses:
        return {}
    out = subprocess.run([tool, "-f", "-C", "-i", "-p", "-e", elf] +
                         [hex(a) for a in addresses], capture_output=True, text=True,
                         check=True).stdout
    # -p -i: one line per address, inlined frames on following lines starting with " (inlined by)"
    result, current = {}, None
    it = iter(addresses)
    for line in out.splitlines():
        if line.startswith(" (inlined by)") and current is not None:
            result[current].append(line.strip())
        else:
            current = next(it)
            result[current] = [line.strip()]
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("logs", nargs="+")
    parser.add_argument("--elf", default=DEFAULT_ELF)
    args = parser.parse_args()
    tool = addr2line_tool()
    for path in args.logs:
        with open(path, encoding="utf-8", errors="replace") as f:
            lines = f.read().splitlines()
        wanted = [line for line in lines
                  if line.lstrip("[game] ").startswith("thread ") or "  thread " in line
                  or "TERMINATION" in line or "- base" in line]
        addresses = sorted({int(m, 16) for line in wanted for m in OFFSET.findall(line)})
        names = decode(tool, args.elf, addresses)
        print(f"== {path}")
        for line in wanted:
            found = [int(m, 16) for m in OFFSET.findall(line)]
            if not found:
                continue
            print(line)
            for a in found:
                chain = names.get(a, ["?"])
                print(f"    +0x{a:x}  {chain[0]}")
                for inlined in chain[1:]:
                    print(f"             {inlined}")


if __name__ == "__main__":
    main()
