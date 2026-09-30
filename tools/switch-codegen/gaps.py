#!/usr/bin/env python3
"""Code gaps with no function in the generated C++ (after nfsmw-nx's tools/huecos.py).

An indirect call (function pointer, vtable) to an address the analysis did not
mark as a function start stops the run with
    Call to invalid or unregistered function at guest address 0x...
(function_dispatcher.cpp), and the generator's validation does not see it: it
only checks direct branches. The gaps between the end of one generated
function and the start of the next are where such functions hide.

The generator writes one "\\t// <asm>" comment per PowerPC instruction, so
    end = start + 4 * comments
and every stretch between an end and the next start has no owner.

Two kinds, fixed differently in the config's [functions] (nfsmw-nx's
app/huecos.toml, and its notes):
  - a standalone function (thunk, callback): "0xGAP" = { }
  - the tail of the previous function, cut off too early (its code branches
    back into the parent): "0xPARENT" = { end = 0xEND }
A gap can hold several small functions (tables of 8/16-byte thunks): declare
only the gaps a run proves, and split those where a crash lands inside.

Usage:
  python tools/switch-codegen/gaps.py [--gen DIR] [--min 8] [--toml out.toml]
         [--check 0x82... ...]

Nothing is added to the config automatically: the output is candidates.
"""

import argparse
import bisect
import os
import re
import sys

RE_FUNC = re.compile(r"^DEFINE_REX_FUNC\(([A-Za-z_][A-Za-z0-9_]*)\)")
RE_MAPPING = re.compile(r"\{\s*0x([0-9A-Fa-f]{8}),\s*([A-Za-z_][A-Za-z0-9_]*)\s*\}")
DEFAULT_GEN = os.path.join(os.path.dirname(__file__), "..", "..", "glue", "rexglue-sdk-main",
                           "gta4-recomp", "generated", "eu-base")


def addresses(gen_dir):
    """{function name: guest address} from the mapping table of *_init.cpp
    (named functions such as xstart have no address in their name)."""
    table = {}
    for name in os.listdir(gen_dir):
        if name.endswith("_init.cpp"):
            with open(os.path.join(gen_dir, name), encoding="utf-8", errors="replace") as f:
                for m in RE_MAPPING.finditer(f.read()):
                    table.setdefault(m.group(2), int(m.group(1), 16))
    return table


def measure(gen_dir):
    """{start: instruction count} from the generated files."""
    table = addresses(gen_dir)
    sizes = {}
    files = sorted(f for f in os.listdir(gen_dir) if re.match(r".*_recomp\.\d+\.cpp$", f))
    for name in files:
        current, count = None, 0
        with open(os.path.join(gen_dir, name), encoding="utf-8", errors="replace") as f:
            for line in f:
                if line.startswith("DEFINE_REX_FUNC("):
                    m = RE_FUNC.match(line)
                    symbol = m.group(1) if m else ""
                    if symbol in table:
                        current, count = table[symbol], 0
                    elif symbol.startswith("sub_"):
                        current, count = int(symbol[4:], 16), 0
                elif current is not None:
                    if line.startswith("\t// "):
                        count += 1
                    elif line.startswith("}"):
                        sizes[current] = count
                        current = None
    return sizes, len(files)


def main():
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument("--gen", default=DEFAULT_GEN, help="folder of the generated code")
    p.add_argument("--min", type=int, default=4, help="smallest gap listed, in bytes")
    p.add_argument("--toml", help="write candidate [functions] entries here")
    p.add_argument("--check", nargs="*", default=[], help="addresses to locate")
    args = p.parse_args()

    sizes, n_files = measure(args.gen)
    starts = sorted(sizes)
    if not starts:
        sys.exit(f"no DEFINE_REX_FUNC in {args.gen}")
    gaps = []
    for a, b in zip(starts, starts[1:]):
        end = a + 4 * sizes[a]
        if end < b:
            gaps.append((end, b - end, a, b))
        elif end > b:
            print(f"overlap: {a:08X} ends at {end:08X} past {b:08X}")

    listed = [g for g in gaps if g[1] >= args.min]
    by_size = {}
    for g in gaps:
        by_size[g[1]] = by_size.get(g[1], 0) + 1
    print(f"{n_files} files, {len(starts)} functions, code {starts[0]:08X}-"
          f"{starts[-1] + 4 * sizes[starts[-1]]:08X}")
    print(f"gaps: {len(gaps)}, {sum(g[1] for g in gaps)} bytes; >= {args.min} bytes: {len(listed)}")
    print("most common sizes: " + ", ".join(f"{s} B x{c}" for s, c in
                                            sorted(by_size.items(), key=lambda x: -x[1])[:10]))
    for g in sorted(listed, key=lambda g: -g[1])[:20]:
        print(f"  {g[0]:08X} {g[1]:6} bytes, after {g[2]:08X}, before {g[3]:08X}")

    for text in args.check:
        a = int(text, 16)
        i = bisect.bisect_right(starts, a) - 1
        if i < 0:
            print(f"{a:08X}: before the first function")
            continue
        s = starts[i]
        end = s + 4 * sizes[s]
        if a < end:
            print(f"{a:08X}: inside {s:08X} (+0x{a - s:X}, function ends at {end:08X})")
        else:
            print(f"{a:08X}: in the gap {end:08X}-{starts[i + 1] if i + 1 < len(starts) else 0:08X}"
                  f" after {s:08X}: undeclared")

    if args.toml:
        with open(args.toml, "w", encoding="utf-8") as f:
            f.write("# Candidate gaps (tools/switch-codegen/gaps.py). Standalone functions\n"
                    "# as { }; a tail of the previous function needs { end = ... } on the\n"
                    "# parent instead. Review before copying into the config.\n[functions]\n")
            for g in listed:
                f.write(f'"0x{g[0]:08X}" = {{ }}   # {g[1]} bytes, after 0x{g[2]:08X}\n')
        print(f"wrote {len(listed)} candidates to {args.toml}")


if __name__ == "__main__":
    main()
