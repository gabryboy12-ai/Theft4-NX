# Lists, per recompiled function, the localized registers it reads before
# writing them.
#
# Adapted from tools/lee_antes.py of StevensND/nfsmw-nx (GPL-3.0),
# https://github.com/StevensND/nfsmw-nx. See docs/switch-port/08-nfsmw-nx.md.
#
# With registers as C++ locals (non_volatile_as_local, cr/ctr/xer_as_local) a
# function that reads r14-r31, f14-f31, v14-v31/v64-v127, a cr field, ctr or
# xer before writing it reads a local that holds zero. That is what a fragment
# split off its owner does: the owner jumps into it with those registers
# already set up. Such functions need share_registers = true in the codegen
# config (then they keep the registers in ctx and are no longer reported).
# Run it after every code generation.
#
# Usage:
#   python tools/switch-codegen/read_before_write.py <generated dir> [--prefix gta4_recomp]
#       [--toml]      print the result as [functions] entries with share_registers
import argparse
import glob
import re
import sys

FN_RE = re.compile(r'^DEFINE_REX_FUNC\((sub_[0-9A-F]+)\)')
TOK = re.compile(r'(?<![.\w])(r(?:1[4-9]|2[0-9]|3[01])|f(?:1[4-9]|2[0-9]|3[01])|'
                 r'v(?:1[4-9]|2[0-9]|3[01]|6[4-9]|[7-9][0-9]|1[01][0-9]|12[0-7])|cr[0-7]|ctr|xer)\b')
ASG = re.compile(r'^([A-Za-z0-9_]+)(?:\.[A-Za-z0-9_\[\]]+)*\s*=[^=]')


def scan(gen, prefix):
    res = {}
    for path in sorted(glob.glob(gen + '/' + prefix + '.*.cpp')):
        cur = None
        written = set()
        bad = {}
        ea_on_stack = False
        for ln in open(path, encoding='utf-8', errors='ignore'):
            m = FN_RE.match(ln)
            if m:
                cur = m.group(1)
                written = set()
                bad = {}
                continue
            if cur is None:
                continue
            if ln.startswith('}'):
                if bad:
                    res[cur] = bad
                cur = None
                continue
            s = ln.strip()
            if (not s or s.startswith('//') or s.startswith('PPC') or s.startswith('uint') or
                    s.startswith('REX_FUNC_PROLOGUE')):
                continue
            # prologue spill of a non-volatile to the stack
            if (s.startswith('REX_STORE') and re.search(r',\s*(r|f)\d+\.(u64|f64)\);$', s) and
                    'ctx.r1.' in s):
                continue
            # Theft4-NX: stvx128 spills of v64-v127 to the stack (ea from r1) are
            # prologue saves too, and the syncs emitted around share_registers
            # calls (`const auto s_X = ctx.X; ctx.X = X;`) only copy the locals.
            if s.startswith('ea = '):
                ea_on_stack = s.startswith('ea = (ctx.r1.u32') or s.startswith('ea = ctx.r1.u32')
            if s.startswith('const auto s_'):
                continue
            if ea_on_stack and s.startswith('simde_mm_store') and 'REX_RAW_ADDR(ea)' in s:
                continue
            s2 = re.sub(r'compare<[^>]*>\([^;]*,\s*xer\)', '', s)
            s2 = re.sub(r'cr\d\.so = xer\.so;', '', s2)
            # Theft4-NX: vcmp*. with Rc writes cr6 through setFromMask.
            mc = re.match(r'^(cr[0-7])\.(?:compare|setFromMask)', s)
            if mc:
                for r in TOK.findall(re.sub(r',\s*xer\)', ')', s[len(mc.group(1)):])):
                    if r not in written and r not in bad:
                        bad[r] = s[:100]
                written.add(mc.group(1))
                continue
            ms = re.match(r'^simde_mm_store\w*\(\(?(?:simde__m128i\*\))?(v\d+)\.\w+,(.*)$', s2)
            if ms:
                for r in TOK.findall(ms.group(2)):
                    if r not in written and r not in bad:
                        bad[r] = s[:100]
                written.add(ms.group(1))
                continue
            m2 = ASG.match(s2)
            lhs = None
            rhs = s2
            if m2:
                lhs = m2.group(1)
                rhs = s2[m2.end() - 1:]
            for r in TOK.findall(rhs):
                if r not in written and r not in bad:
                    bad[r] = s[:100]
            if lhs and TOK.fullmatch(lhs):
                written.add(lhs)
    return res


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('gen')
    ap.add_argument('--prefix', default='gta4_recomp')
    ap.add_argument('--toml', action='store_true')
    args = ap.parse_args()
    res = scan(args.gen, args.prefix)
    for name in sorted(res):
        regs = ' '.join(sorted(res[name]))
        if args.toml:
            print('"0x%s" = { share_registers = true }   # reads %s before writing' %
                  (name[4:], regs))
        else:
            print(name, regs, '|', list(res[name].values())[0])
    print('TOTAL', len(res), file=sys.stderr)
    return 0


if __name__ == '__main__':
    sys.exit(main())
