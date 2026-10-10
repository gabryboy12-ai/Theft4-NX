#!/usr/bin/env python3
"""Decode the module offsets in a switch-game log with addr2line.

Usage: where.py game.log [game-crash.log ...] [--elf out/build/switch-game/switch-game.elf]
       where.py --profile game.log [--elf ...] [--top 25]

Every "+0x..." in the thread snapshot lines ("  thread ..."), in TERMINATION
lines ("module base + 0x...") and every "pc - base"/"lr - base" of
game-crash.log is an ELF address of switch-game.elf. The script prints each
line followed by the function (and inlined chain) of each address. The ELF
must be the one of the build that ran.

--profile: the "profile id ..." lines of cvar nx_profile (nx_profiler.h), one
per sampled thread every 10 s, are added up over the whole log per thread and
per function: the share of the samples whose pc is in each function, then the
same for the lr (the caller, which names the wait when the pc is in a svc).
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


PROFILE_LINE = re.compile(r"profile (id \d+ \S+ '[^']*') \([\d.]+% cpu\) (\d+) samples:(.*)")
PROFILE_ITEM = re.compile(r"\+0x([0-9a-fA-F]+) x(\d+)")


def function_name(chain):
    # "name at file:line" -> name; inlined frames are not used for the totals
    return chain[0].split(" at ")[0] if chain else "?"


def profile(tool, elf, paths, top):
    threads = {}  # name -> [samples, {pc: n}, {lr: n}]
    for path in paths:
        with open(path, encoding="utf-8", errors="replace") as f:
            for line in f:
                m = PROFILE_LINE.search(line)
                if not m:
                    continue
                entry = threads.setdefault(m.group(1), [0, {}, {}])
                entry[0] += int(m.group(2))
                pcs, _, lrs = m.group(3).partition("; lr:")
                for part, table in ((pcs, entry[1]), (lrs, entry[2])):
                    for a, n in PROFILE_ITEM.findall(part):
                        table[int(a, 16)] = table.get(int(a, 16), 0) + int(n)
    addresses = sorted({a for e in threads.values() for t in e[1:] for a in t})
    names = decode(tool, elf, addresses)
    for thread, (samples, pcs, lrs) in sorted(threads.items(), key=lambda kv: -kv[1][0]):
        print(f"== {thread}: {samples} samples")
        for label, table in (("pc in", pcs), ("lr in", lrs)):
            per_function = {}
            for a, n in table.items():
                key = function_name(names.get(a, ["?"]))
                per_function[key] = per_function.get(key, 0) + n
            for key, n in sorted(per_function.items(), key=lambda kv: -kv[1])[:top]:
                print(f"  {label} {100.0 * n / max(samples, 1):5.1f}%  {n:6d}  {key}")
        print("  (only the top pcs of each 10 s window are logged: the shares are of all samples)")


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("logs", nargs="+")
    parser.add_argument("--elf", default=DEFAULT_ELF)
    parser.add_argument("--profile", action="store_true",
                        help="add up the nx_profile samples per thread and function")
    parser.add_argument("--top", type=int, default=25)
    args = parser.parse_args()
    tool = addr2line_tool()
    if args.profile:
        profile(tool, args.elf, args.logs, args.top)
        return
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
