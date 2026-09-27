# Direct calls between recompiled functions that have no hook.
#
# Adapted from tools/llamadas_directas.py of StevensND/nfsmw-nx (GPL-3.0),
# https://github.com/StevensND/nfsmw-nx. See docs/switch-port/08-nfsmw-nx.md.
#
# With GCC every recompiled function sub_X is a weak alias of __imp__sub_X
# (DEFINE_REX_FUNC in the generated init header), so that a hook can replace it
# at link time. The generated code always calls sub_X, and a weak function
# cannot be inlined into another one (a hook could replace it): not within the
# same file and not with LTO. This step turns `sub_X(ctx, base);` into
# `__imp__sub_X(ctx, base);` when sub_X has no hook: the same function, which
# can now be inlined.
#
# Any 82xxxxxx address found in the given sources counts as hooked (hooks may
# be built with token pasting, so a plain REX_HOOK search is not enough). That
# is slightly more than needed, and safe.
#
# The init and register files are left alone: the dispatch table for indirect
# calls still points to sub_X and sees the hooks. Run it again after every
# code generation, which silently brings the weak calls back.
#
# Usage:
#   python tools/switch-codegen/direct_calls.py --gen <generated dir>
#       [--hooked-from <file or dir>]... [--prefix gta4_recomp] [--undo]
import argparse
import os
import re
import sys

SKIP_EXT = ('.a', '.o', '.obj', '.png', '.jpg', '.bin', '.dds', '.xtd')


def hooked_addresses(paths):
    names = set()
    for root in paths:
        if os.path.isfile(root):
            files = [root]
        else:
            files = [os.path.join(d, f) for d, _, fs in os.walk(root) for f in fs
                     if not f.endswith(SKIP_EXT)]
        for path in files:
            try:
                text = open(path, encoding='utf-8', errors='ignore').read()
            except OSError:
                continue
            for m in re.findall(r'(?<![0-9A-Fa-f])(82[0-9A-Fa-f]{6})(?![0-9A-Fa-f])', text):
                names.add('sub_' + m.upper())
    return names


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--gen', required=True, help='generated code directory')
    ap.add_argument('--hooked-from', action='append', default=[],
                    help='source file or directory whose 82xxxxxx addresses count as hooked')
    ap.add_argument('--prefix', default='gta4_recomp', help='generated file prefix')
    ap.add_argument('--undo', action='store_true', help='turn direct calls back into weak calls')
    args = ap.parse_args()

    hooked = hooked_addresses(args.hooked_from)
    weak_call = re.compile(r'(?<![\w])sub_([0-9A-F]{8})\(ctx, base\);')
    direct_call = re.compile(r'__imp__sub_([0-9A-F]{8})\(ctx, base\);')
    changed = files = 0
    weak_left = direct_total = 0
    for name in sorted(os.listdir(args.gen)):
        if not (name.startswith(args.prefix + '.') and name.endswith('.cpp')):
            continue
        path = os.path.join(args.gen, name)
        text = open(path, encoding='utf-8').read()
        if args.undo:
            new, n = direct_call.subn(lambda m: 'sub_%s(ctx, base);' % m.group(1), text)
        else:
            def swap(m):
                if 'sub_' + m.group(1) in hooked:
                    return m.group(0)
                return '__imp__sub_%s(ctx, base);' % m.group(1)
            new = weak_call.sub(swap, text)
            n = len(weak_call.findall(text)) - len(weak_call.findall(new))
        if new != text:
            open(path, 'w', encoding='utf-8', newline='\n').write(new)
            files += 1
        changed += n
        weak_left += len(weak_call.findall(new))
        direct_total += len(direct_call.findall(new))
    total = weak_left + direct_total
    print('%s: %d calls in %d files; %d hooked addresses kept weak' %
          ('undone' if args.undo else 'direct', changed, files, len(hooked)))
    if total:
        print('calls between recompiled functions: %d, direct %d (%.1f %%), weak %d' %
              (total, direct_total, 100.0 * direct_total / total, weak_left))
    return 0


if __name__ == '__main__':
    sys.exit(main())
