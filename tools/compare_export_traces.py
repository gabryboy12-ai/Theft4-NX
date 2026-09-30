#!/usr/bin/env python3
"""Compare two export traces (PC reference vs Switch) and find the first divergence.

Both builds log, for every Xbox export, its first call and the value r3 holds
after that call (rex/hook.h, rex/diagnostics/export_stubs.h):

    export first call: NtCreateSemaphore (guest LR 829A21F8, thread 2242)
    export first result: NtCreateSemaphore -> 00000000 (thread 2242)

The PC (tools/pc-eu-base) and the Switch (tools/switch-game) run the same
generated code, so on the same path they call the same exports in the same
order from the same guest LR. This script walks the reference trace in order
and reports:
  1. the first export the other trace never calls (the divergence), with
     the exports around it;
  2. exports both call from a different guest LR;
  3. first results that differ, the NTSTATUS-looking ones (0x8xxxxxxx,
     0xCxxxxxxx) first: a status that differs is usually the cause; addresses
     and handles may differ legitimately;
  4. exports only the other trace calls.
Thread ids differ between platforms (Horizon ids, XThread handles), so each
trace's threads are named T0, T1, ... in the order they first call an export.

Usage:
  python tools/compare_export_traces.py <reference.log> <other.log>
      [--names PC Switch] [--context 5]
"""

import argparse
import re
import sys

CALL = re.compile(r"export first call: (\w+) \(guest LR ([0-9A-Fa-f]{8}), thread ([0-9A-Fa-f]+)\)")
RESULT = re.compile(r"export first result: (\w+) -> ([0-9A-Fa-f]{8})")


def load(path):
    calls, results, threads = [], {}, {}
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            m = CALL.search(line)
            if m:
                name, lr, tid = m.group(1), int(m.group(2), 16), m.group(3)
                label = threads.setdefault(tid, f"T{len(threads)}")
                calls.append((name, lr, label))
                continue
            m = RESULT.search(line)
            if m:
                results.setdefault(m.group(1), int(m.group(2), 16))
    return calls, results


def is_status(value):
    return (value & 0xF0000000) in (0x80000000, 0xC0000000)


def main():
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument("reference")
    p.add_argument("other")
    p.add_argument("--names", nargs=2, default=["PC", "Switch"])
    p.add_argument("--context", type=int, default=5)
    a = p.parse_args()
    ref_name, oth_name = a.names

    ref, ref_res = load(a.reference)
    oth, oth_res = load(a.other)
    if not ref or not oth:
        sys.exit(f"no 'export first call' lines ({ref_name} {len(ref)}, {oth_name} {len(oth)})")
    oth_by_name = {c[0]: (i, c) for i, c in enumerate(oth)}
    ref_names = {c[0] for c in ref}
    print(f"{ref_name}: {len(ref)} exports, {len(set(t for *_, t in ref))} threads; "
          f"{oth_name}: {len(oth)} exports, {len(set(t for *_, t in oth))} threads")

    # 1. Common prefix and first divergence, in the reference's order.
    prefix = 0
    while prefix < min(len(ref), len(oth)) and ref[prefix][0] == oth[prefix][0]:
        prefix += 1
    print(f"\nsame order for the first {prefix} exports")
    missing = next((i for i, c in enumerate(ref) if c[0] not in oth_by_name), None)
    if missing is None:
        print(f"{oth_name} calls every export {ref_name} calls")
    else:
        name, lr, thread = ref[missing]
        print(f"\nFIRST DIVERGENCE: {oth_name} never calls {name} "
              f"({ref_name}: export #{missing + 1}, guest LR {lr:08X}, {thread})")
        lo = max(0, missing - a.context)
        print(f"  {ref_name} around it:")
        for i in range(lo, min(len(ref), missing + a.context + 1)):
            n, l, t = ref[i]
            mark = ">>" if i == missing else ("  " if n in oth_by_name else "--")
            res = ref_res.get(n)
            print(f"  {mark} #{i + 1:<4} {n:<40} LR {l:08X} {t:<4} -> "
                  f"{'' if res is None else f'{res:08X}'}")
        last = [c for c in ref[:missing] if c[0] in oth_by_name]
        if last:
            n = last[-1][0]
            print(f"  last export both call before it: {n} "
                  f"(result {ref_name} {ref_res.get(n, 0):08X}, {oth_name} {oth_res.get(n, 0):08X})")

    # 2. Same export, different call site.
    lr_diff = [(n, l, oth_by_name[n][1][1]) for n, l, _ in ref
               if n in oth_by_name and oth_by_name[n][1][1] != l]
    print(f"\nfirst called from a different guest LR: {len(lr_diff)}")
    for n, l1, l2 in lr_diff[:10]:
        print(f"  {n:<40} {ref_name} {l1:08X}  {oth_name} {l2:08X}")

    # 3. Different first results.
    diffs = [(n, ref_res[n], oth_res[n]) for n, *_ in ref
             if n in ref_res and n in oth_res and ref_res[n] != oth_res[n]]
    diffs.sort(key=lambda d: not (is_status(d[1]) or is_status(d[2])))
    print(f"\ndifferent first results: {len(diffs)} (status-like first)")
    for n, r1, r2 in diffs[:15]:
        tag = "STATUS" if is_status(r1) or is_status(r2) else "value"
        print(f"  {tag:<6} {n:<40} {ref_name} {r1:08X}  {oth_name} {r2:08X}")

    # 4. Only in the other trace.
    extra = [c for c in oth if c[0] not in ref_names]
    print(f"\nonly {oth_name} calls: {len(extra)}")
    for n, l, t in extra[:15]:
        print(f"  {n:<40} LR {l:08X} {t}")


if __name__ == "__main__":
    main()
