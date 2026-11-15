#!/usr/bin/env python3
"""The runtime charter: runtime/core names no guest OS and no guest CPU.

The C twin of relift/tests/workspace_charter.rs. The core of the C runtime
must build for any guest (other consoles, PowerPC, MIPS, x86, big-endian,
32-bit), so:

- core_includes_nothing_above_it: every #include in runtime/core resolves
  inside runtime/core (or is a system header), and none names a header that
  lives under runtime/cpus or runtime/guests.
- core_spells_no_guest_word: no file under runtime/core matches a rule of
  DENYLIST, except the (file, rule) pairs in allow.txt, each with a reason.
- every_allowance_is_used: each allow.txt entry still matches, so the list
  only shrinks.

Each absence rule has a presence control, so an empty walk or a broken
pattern cannot pass: the walk must find the core's sources, headers and
tests; the include rule must reject at least one include of the CPU and
guest layers; every rule must match its own sample; and the denylist must hit
at least one file under runtime/guests/switch or runtime/cpus/arm64.

Run from anywhere: python3 runtime/charter/check.py. Exit status 1 on any
failure. Standard library only.
"""

import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
RUNTIME = os.path.dirname(HERE)
CORE = os.path.join(RUNTIME, "core")
ABOVE = [os.path.join(RUNTIME, "cpus"), os.path.join(RUNTIME, "guests")]
CONTROLS = [os.path.join(RUNTIME, "guests", "switch"), os.path.join(RUNTIME, "cpus", "arm64")]
ALLOW = os.path.join(HERE, "allow.txt")

# (rule, pattern, sample the pattern must match, what it guards against)
DENYLIST = [
    ("horizon", r"(?i)horizon", "the Horizon kernel", "the Switch OS"),
    ("svc", r"(?i)\bsvc|_svc\b|svc_", "trace_svc", "Horizon supervisor calls"),
    ("npdm", r"(?i)npdm", "the NPDM's width", "Switch program metadata"),
    ("nn-sdk", r"\bnn::|\bnnSdk\b", "nn::os::Mutex", "the Switch SDK"),
    ("switch-console", r"\bSwitch\b|(?i:nintendo)", "the Switch layer", "the console by name"),
    ("horizon-kernel", r"\b(?:hos|HOS|kobj|KOBJ)_|\bKR_[A-Z]|\b(?:HIPC|CMIF|TIPC)\b|\bsm:", "hos_lock()",
     "Horizon kernel and IPC names"),
    ("a64-system-registers", r"(?i)tpidr|fpcr|fpsr|nzcv|cnt(?:pct|vct|frq)|dczid|ctr_el|_el[0-3]\b",
     "tpidrro_el0", "ARM64 system registers"),
    ("a64-isa", r"(?i)aarch64|arm64|armv8|\ba64\b|cortex|aapcs|\b(?:ldx[rp]|stx[rp]|ldaxr|stlxr|clrex|wfe|sevl?|udf|brk|hlt)\b",
     "AArch64 LDXR", "ARM64 architecture and instruction names"),
    ("register-file", r"->(?:x|w|v)\[|\bx\[3[01]\]|\bx30\b|\blr\b", "c->x[30]", "reads of a guest register file"),
    ("page-4k", r"(?i)4 ?KiB|\b0x1000ull\b|RL_GUEST_PAGE", "the 4 KiB map", "a fixed guest page size"),
    ("horizon-clock", r"(?i)19[.,]2 ?MHz|19200000|\b12u?\) ?/ ?625", "19.2 MHz", "the Switch counter rate"),
    ("address-widths", r"\b3[69]-bit\b|\b2\^3[69]\b|\(36 or 39\)", "the 36-bit layout", "Switch address space widths"),
]

INCLUDE = re.compile(r'^\s*#\s*include\s*([<"])([^>"]+)[>"]', re.M)


def walk(root):
    """Every regular file under root, recursively, as paths relative to RUNTIME."""
    out = []
    for d, dirs, files in os.walk(root):
        dirs.sort()
        for f in sorted(files):
            p = os.path.join(d, f)
            if os.path.isfile(p):
                out.append(os.path.relpath(p, RUNTIME))
    return out


def read(rel):
    with open(os.path.join(RUNTIME, rel), encoding="utf-8", errors="replace") as f:
        return f.read()


def headers_above():
    """Basenames and relift/ paths of every header under cpus/ and guests/."""
    names, paths = set(), set()
    for root in ABOVE:
        for rel in walk(root):
            if rel.endswith(".h"):
                names.add(os.path.basename(rel))
                i = rel.find("include/")
                if i >= 0:
                    paths.add(rel[i + len("include/"):])
    return names, paths


def bad_includes(rel, text, above_names, above_paths, search):
    """Includes of rel that resolve outside search, or that name a header from above."""
    bad, seen = [], 0
    here = os.path.dirname(os.path.join(RUNTIME, rel))
    for m in INCLUDE.finditer(text):
        seen += 1
        kind, name = m.group(1), m.group(2)
        line = text.count("\n", 0, m.start()) + 1
        lowered = name.lower()
        if "cpus/" in lowered or "guests/" in lowered or name in above_paths or os.path.basename(name) in above_names:
            bad.append((line, name, "names a header of a CPU or guest layer"))
            continue
        dirs = ([here] if kind == '"' else []) + search
        found = any(os.path.isfile(os.path.join(d, name)) for d in dirs)
        if not found and (kind == '"' or name.startswith("relift/")):
            bad.append((line, name, "does not resolve inside core"))
    return bad, seen


def load_allow():
    """allow.txt: `path<spaces>rule<spaces>reason`, # comments; returns {(path, rule): reason}."""
    allow, errors = {}, []
    with open(ALLOW, encoding="utf-8") as f:
        for n, raw in enumerate(f, 1):
            line = raw.strip()
            if not line or line.startswith("#"):
                continue
            parts = line.split(None, 2)
            if len(parts) < 3 or not parts[2].strip():
                errors.append(f"allow.txt:{n}: needs a path, a rule and a reason")
                continue
            if parts[1] not in {r[0] for r in DENYLIST}:
                errors.append(f"allow.txt:{n}: unknown rule `{parts[1]}`")
                continue
            allow[(parts[0], parts[1])] = parts[2]
    return allow, errors


def hits(rel, text, rules):
    out = []
    for name, pattern, _, _ in rules:
        for m in re.finditer(pattern, text):
            line = text.count("\n", 0, m.start()) + 1
            out.append((name, line, m.group(0)))
    return out


def main():
    failures = {}
    checks = []

    def check(name, problems):
        checks.append(name)
        if problems:
            failures[name] = problems

    core_files = walk(CORE)
    core_c = [f for f in core_files if f.endswith(".c")]
    core_h = [f for f in core_files if f.endswith(".h")]
    walk_problems = []
    if len(core_c) < 5 or len(core_h) < 2:
        walk_problems.append(f"walked only {len(core_c)} .c and {len(core_h)} .h files under core/")
    if "core/core.h" not in core_files or "core/include/relift/abi_core.h" not in core_files:
        walk_problems.append("core/core.h or core/include/relift/abi_core.h not found")
    if not any(f.startswith("core/tests/") for f in core_files):
        walk_problems.append("the walk did not recurse into core/tests/")
    check("the_walk_reads_the_core", walk_problems)

    above_names, above_paths = headers_above()
    search = [CORE, os.path.join(CORE, "include"), os.path.join(CORE, "tests")]
    inc_problems, seen = [], 0
    for rel in core_files:
        if rel.endswith((".c", ".h")):
            bad, n = bad_includes(rel, read(rel), above_names, above_paths, search)
            seen += n
            inc_problems += [f"{rel}:{line}: #include {name}: {why}" for line, name, why in bad]
    if seen == 0:
        inc_problems.append("presence control: found no #include in core at all")
    if not above_names:
        inc_problems.append("presence control: found no header under cpus/ or guests/")
    rejected = 0
    for root in CONTROLS:
        for rel in walk(root):
            if rel.endswith((".c", ".h")):
                rejected += len(bad_includes(rel, read(rel), above_names, above_paths, search)[0])
    if rejected == 0:
        inc_problems.append("presence control: the include rule rejects nothing in guests/switch or cpus/arm64")
    check("core_includes_nothing_above_it", inc_problems)

    rule_problems = []
    for name, pattern, sample, _ in DENYLIST:
        if not re.search(pattern, sample):
            rule_problems.append(f"rule `{name}` does not match its own sample {sample!r}")
    check("every_rule_matches_its_sample", rule_problems)

    allow, allow_errors = load_allow()
    used = set()
    word_problems = list(allow_errors)
    for rel in core_files:
        for name, line, word in hits(rel, read(rel), DENYLIST):
            if (rel, name) in allow:
                used.add((rel, name))
                continue
            word_problems.append(f"{rel}:{line}: rule `{name}` matched {word!r}")
    control_hits = 0
    for root in CONTROLS:
        for rel in walk(root):
            if hits(rel, read(rel), DENYLIST):
                control_hits += 1
    if control_hits == 0:
        word_problems.append("presence control: the denylist hits no file under guests/switch or cpus/arm64")
    check("core_spells_no_guest_word", word_problems)

    stale = [f"allow.txt: `{p} {r}` matches nothing any more; remove it" for (p, r) in allow if (p, r) not in used]
    check("every_allowance_is_used", stale)

    for name in checks:
        print(f"{'FAIL' if name in failures else 'ok  '} {name}")
        for p in failures.get(name, []):
            print(f"    {p}")
    print(f"{len(core_files)} core files, {seen} includes, {len(DENYLIST)} rules, "
          f"{control_hits} control files hit, {len(allow)} allowances; {len(failures)} of {len(checks)} checks failed")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
