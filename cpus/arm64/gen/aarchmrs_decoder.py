"""Generate the A64 decoder tables from Arm's AARCHMRS Instructions.json.

Usage: python3 -I aarchmrs_decoder.py <Instructions.json> <generated.rs>

Reads every Instruction leaf under the A64 InstructionSet, merges the encodesets
of the leaf and its ancestors into a fixed mask and value, folds the node
conditions, feature tests (IsFeatureImplemented) included, into per-leaf Rust
predicates over the 32-bit word and the CPU's features, and writes the tables. Any shape it does not understand
is a hard error. Source: Arm AARCHMRS, BSD-3-Clause, see NOTICE-AARCHMRS.
"""

import json
import re
import sys

LEAF = "Instruction.Instruction"
GROUP = "Instruction.InstructionGroup"
ALIAS = "Instruction.InstructionAlias"
ISET = "Instruction.InstructionSet"
# Features the Sail semantics test (so a template can branch on them) or a
# CPU description names, but no AARCHMRS encoding condition names.
EXTRA_FEATURES = [
    "FEAT_PMULL",
    "FEAT_FPAC",
    "FEAT_FPACCOMBINE",
    "FEAT_PAuth2",
    "FEAT_EPAC",
    "FEAT_CONSTPACFIELD",
    "FEAT_PACIMP",
    "FEAT_PACQARMA3",
    "FEAT_PACQARMA5",
    "FEAT_LSE2",
    "FEAT_DIT",
    "FEAT_SSBS",
    "FEAT_DPB",
    "FEAT_DPB2",
    "FEAT_ECV",
    "FEAT_CSV2",
    "FEAT_CSV3",
]
IDENT_RE = re.compile(r"^[A-Za-z][A-Za-z0-9_]*$")


class Unsupported(Exception):
    pass


def fail(what, node=None):
    detail = "" if node is None else " in " + json.dumps(node)[:300]
    raise Unsupported(what + detail)


def parse_bits(text, width, ctx):
    """'01x' -> (mask, value) with x as don't-care; only 0, 1, x are accepted."""
    if len(text) < 2 or text[0] != "'" or text[-1] != "'":
        fail("unquoted bit string %r" % text, ctx)
    s = text[1:-1]
    if len(s) != width:
        fail("bit string %r does not have width %d" % (text, width), ctx)
    mask = value = 0
    for i, ch in enumerate(s):
        bit = 1 << (width - 1 - i)
        if ch == "0":
            mask |= bit
        elif ch == "1":
            mask |= bit
            value |= bit
        elif ch != "x":
            fail("bit character %r in %r" % (ch, text), ctx)
    return mask, value


def encodeset(node):
    """Returns (mask, value, fields) of a node's own encodeset."""
    enc = node.get("encoding")
    if not enc:
        return 0, 0, []
    if enc["_type"] != "Instruction.Encodeset.Encodeset" or enc.get("width") != 32:
        fail("encodeset shape", enc)
    mask = value = 0
    fields = []
    for e in enc["values"]:
        t = e["_type"]
        if t not in ("Instruction.Encodeset.Field", "Instruction.Encodeset.Bits"):
            fail("encodeset entry type", e)
        r = e["range"]
        lo, width = r["start"], r["width"]
        if width < 1 or lo < 0 or lo + width > 32:
            fail("bad range", e)
        v = e["value"]
        if v["_type"] != "Values.Value":
            fail("encodeset value type", e)
        m, val = parse_bits(v["value"], width, e)
        if mask & (m << lo):
            fail("overlapping encodeset bits", e)
        mask |= m << lo
        value |= val << lo
        if t.endswith("Field"):
            fields.append((e["name"], lo, width))
    names = [f[0] for f in fields]
    if len(set(names)) != len(names):
        fail("duplicate field name in one encodeset %r" % names, enc)
    return mask, value, fields


def const_bool(b):
    return ("const", b)


def mk_not(x):
    if x[0] == "const":
        return const_bool(not x[1])
    return ("not", x)


def mk_nary(op, parts):
    """op is 'and' or 'or'; folds constants and duplicates."""
    absorb = op == "or"
    out = []
    for p in parts:
        if p[0] == "const":
            if p[1] == absorb:
                return p
            continue
        if p[0] == op:
            for q in p[1]:
                if q not in out:
                    out.append(q)
        elif p not in out:
            out.append(p)
    if not out:
        return const_bool(not absorb)
    if len(out) == 1:
        return out[0]
    return (op, tuple(out))


class Scope:
    """Field name resolution for one leaf: own fields first, then the nearest ancestor."""

    def __init__(self, chain, mask, value):
        self.chain = chain
        self.mask = mask
        self.value = value

    def lookup(self, name):
        for fields in reversed(self.chain):
            for f in fields:
                if f[0] == name:
                    return f
        fail("identifier %r does not resolve to an encodeset field" % name)


def compare(scope, field, pattern_mask, pattern_value, negate):
    """field == pattern (x bits free), partially evaluated against the known bits."""
    _name, lo, width = field
    pm = pattern_mask << lo
    pv = pattern_value << lo
    known = scope.mask & pm
    if (scope.value ^ pv) & known:
        eq = const_bool(False)
    else:
        dyn = pm & ~scope.mask
        eq = const_bool(True) if dyn == 0 else ("cmp", dyn, pv & dyn)
    return mk_not(eq) if negate else eq


def value_of(node):
    if node["_type"] != "Values.Value":
        fail("expected a Values.Value operand", node)
    return node["value"]


def lower(node, scope):
    t = node["_type"]
    if t == "AST.Bool":
        if not isinstance(node["value"], bool):
            fail("AST.Bool value", node)
        return const_bool(node["value"])
    if t == "AST.Function":
        if node["name"] != "IsFeatureImplemented":
            fail("function", node)
        args = node["arguments"]
        if len(args) != 1 or args[0]["_type"] != "AST.Identifier":
            fail("IsFeatureImplemented argument", node)
        name = args[0]["value"]
        if not name.startswith("FEAT_") or not IDENT_RE.match(name):
            fail("feature name %r" % name, node)
        return ("feat", name)
    if t == "AST.UnaryOp":
        if node["op"] != "!":
            fail("unary op", node)
        return mk_not(lower(node["expr"], scope))
    if t == "AST.BinaryOp":
        op = node["op"]
        if op in ("&&", "||"):
            return mk_nary(
                "and" if op == "&&" else "or",
                [lower(node["left"], scope), lower(node["right"], scope)],
            )
        left, right = node["left"], node["right"]
        if left["_type"] != "AST.Identifier":
            fail("left operand is not an identifier", node)
        field = scope.lookup(left["value"])
        width = field[2]
        if op in ("==", "!="):
            if right["_type"] == "AST.Identifier":
                other = scope.lookup(right["value"])
                if other[2] != width:
                    fail("field width mismatch", node)
                cmp = ("eqf", field[1], other[1], ((1 << width) - 1))
                return mk_not(cmp) if op == "!=" else cmp
            m, v = parse_bits(value_of(right), width, node)
            return compare(scope, field, m, v, op == "!=")
        if op == "IN":
            if right["_type"] != "AST.Set":
                fail("IN without a set", node)
            alts = []
            for item in right["values"]:
                m, v = parse_bits(value_of(item), width, node)
                alts.append(compare(scope, field, m, v, False))
            return mk_nary("or", alts)
        fail("binary op", node)
    fail("AST node type", node)


def mentions(c):
    """The features a condition tests."""
    if c[0] == "feat":
        return {c[1]}
    if c[0] == "not":
        return mentions(c[1])
    if c[0] in ("and", "or"):
        out = set()
        for y in c[1]:
            out |= mentions(y)
        return out
    return set()


def render(x):
    k = x[0]
    if k == "const":
        return "true" if x[1] else "false"
    if k == "cmp":
        return "w & 0x%08x == 0x%08x" % (x[1], x[2])
    if k == "feat":
        return "f.has(Feature::%s)" % x[1][len("FEAT_"):]
    if k == "eqf":
        return "(w >> %d) & 0x%x == (w >> %d) & 0x%x" % (x[1], x[3], x[2], x[3])
    if k == "not":
        inner = x[1]
        if inner[0] == "cmp":
            return "w & 0x%08x != 0x%08x" % (inner[1], inner[2])
        return "!(%s)" % render(inner)
    if k == "and":
        return " && ".join(render_paren(p, "or") for p in x[1])
    if k == "or":
        return " || ".join(render_paren(p, "and") for p in x[1])
    fail("render " + k)


def render_paren(p, inner):
    s = render(p)
    return "(%s)" % s if p[0] == inner else s


def mnemonic(leaf):
    asm = leaf.get("assembly")
    if not asm or not asm.get("symbols"):
        fail("leaf without assembly symbols", leaf)
    first = asm["symbols"][0]
    if first["_type"] != "Instruction.Symbols.Literal":
        fail("first assembly symbol is not a literal", first)
    return first["value"]


def collect(arm64):
    leaves = []

    def walk(node, mask, value, chain, conds, path):
        t = node["_type"]
        if t == ALIAS:
            return
        if t not in (GROUP, LEAF, ISET):
            fail("node type", node)
        m, v, fields = encodeset(node)
        if mask & m & (value ^ v):
            fail("conflicting fixed bits with ancestor", node["name"])
        mask2 = mask | m
        value2 = (value & ~m) | v
        chain2 = chain + [fields]
        conds2 = conds + [node["condition"]]
        if t == LEAF:
            leaves.append((node, mask2, value2, fields, chain2, conds2, path))
            return
        if node.get("children") is None:
            fail("group without children", node["name"])
        sub = path if t == ISET else path + [node["name"]]
        for c in node["children"]:
            walk(c, mask2, value2, chain2, conds2, sub)

    walk(arm64, 0, 0, [], [], [])
    return leaves


def intern(table, index, item):
    if item not in index:
        index[item] = len(table)
        table.append(item)
    return index[item]


def generate(src):
    with open(src, encoding="utf-8") as fh:
        doc = json.load(fh)
    meta = doc["_meta"]
    ver = meta["version"]
    lic = meta["license"]
    arm64 = [c for c in doc["instructions"] if c["name"] == "A64"]
    if len(arm64) != 1 or arm64[0]["_type"] != ISET:
        fail("A64 instruction set")
    leaves = collect(arm64[0])

    names = [l[0]["name"] for l in leaves]
    if len(set(names)) != len(names):
        fail("duplicate leaf names")
    for n in names:
        if not IDENT_RE.match(n):
            fail("leaf name is not a Rust identifier: %r" % n)

    groups, group_ix = [], {}
    paths, path_ix = [], {}
    mnems, mnem_ix = [], {}
    fieldtab, fieldlist_ix = [], {}
    rows = []
    conds = []
    features, feature_ix = [], {}
    needs = []
    for node, mask, value, fields, chain, cs, path in leaves:
        if not path:
            fail("leaf directly under A64", node["name"])
        scope = Scope(chain, mask, value)
        c = mk_nary("and", [lower(x, scope) for x in cs])
        feats = tuple(sorted(mentions(c)))
        for f in feats:
            intern(features, feature_ix, f)
        if c == const_bool(False):
            fail("leaf condition is never true", node["name"])
        key = tuple(fields)
        if key not in fieldlist_ix:
            fieldlist_ix[key] = (len(fieldtab), len(fields))
            fieldtab.extend(fields)
        fstart, flen = fieldlist_ix[key]
        rows.append(
            (
                mask,
                value,
                intern(groups, group_ix, path[0]),
                intern(paths, path_ix, tuple(path)),
                intern(mnems, mnem_ix, mnemonic(node)),
                fstart,
                flen,
            )
        )
        if c != const_bool(True):
            conds.append((node["name"], render(c)))
        needs.append(feats)

    for f in EXTRA_FEATURES:
        intern(features, feature_ix, f)
    features.sort()
    featsets, featset_ix = [], {}
    need_index = [intern(featsets, featset_ix, f) for f in needs]

    n = len(leaves)
    out = []
    w = out.append
    w("//! Generated from Arm AARCHMRS release %s (build %s, schema %s)." % (ver["ref"], ver["build"], ver["schema"]))
    w("//!")
    w("//! %s %s" % (lic["copyright"], lic["info"]))
    w("//! The BSD-3-Clause terms are in `NOTICE-AARCHMRS` next to this crate's manifest.")
    w("//!")
    w("//! Do not edit. Regenerate from the A64 `Instructions.json` with")
    w("//! `python3 -I cpus/arm64/gen/aarchmrs_decoder.py Instructions.json cpus/arm64/src/decode/generated.rs`")
    w("//! followed by `cargo fmt -p relift-cpu-arm64`.")
    w("")
    w("use super::{Features, Field};")
    w("")
    w("/// Every A64 encoding leaf of the specification, named as in AARCHMRS.")
    w("#[allow(non_camel_case_types)]")
    w("#[repr(u16)]")
    w("#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash, PartialOrd, Ord)]")
    w("pub enum Encoding {")
    for nm in names:
        w("    %s," % nm)
    w("}")
    w("")
    w("pub(super) const COUNT: usize = %d;" % n)
    w("")
    w("pub(super) static ALL: [Encoding; COUNT] = [")
    for nm in names:
        w("    Encoding::%s," % nm)
    w("];")
    w("")
    w("pub(super) static NAMES: [&str; COUNT] = [")
    for nm in names:
        w('    "%s",' % nm)
    w("];")
    w("")
    w("pub(super) static MASKS: [u32; COUNT] = [")
    for r in rows:
        w("    0x%08x," % r[0])
    w("];")
    w("")
    w("pub(super) static VALUES: [u32; COUNT] = [")
    for r in rows:
        w("    0x%08x," % r[1])
    w("];")
    w("")
    w("pub(super) static GROUP_INDEX: [u8; COUNT] = [")
    for r in rows:
        w("    %d," % r[2])
    w("];")
    w("")
    w("pub(super) static GROUPS: [&str; %d] = [" % len(groups))
    for g in groups:
        w('    "%s",' % g)
    w("];")
    w("")
    w("pub(super) static PATH_INDEX: [u16; COUNT] = [")
    for r in rows:
        w("    %d," % r[3])
    w("];")
    w("")
    w("pub(super) static PATHS: [&[&str]; %d] = [" % len(paths))
    for p in paths:
        w("    &[%s]," % ", ".join('"%s"' % x for x in p))
    w("];")
    w("")
    w("pub(super) static MNEMONIC_INDEX: [u16; COUNT] = [")
    for r in rows:
        w("    %d," % r[4])
    w("];")
    w("")
    w("pub(super) static MNEMONICS: [&str; %d] = [" % len(mnems))
    for m in mnems:
        w('    "%s",' % m)
    w("];")
    w("")
    w("pub(super) static FIELD_START: [u32; COUNT] = [")
    for r in rows:
        w("    %d," % r[5])
    w("];")
    w("")
    w("pub(super) static FIELD_LEN: [u8; COUNT] = [")
    for r in rows:
        w("    %d," % r[6])
    w("];")
    w("")
    w("const fn f(name: &'static str, lo: u8, width: u8) -> Field {")
    w("    Field { name, lo, width }")
    w("}")
    w("")
    w("pub(super) static FIELDS: [Field; %d] = [" % len(fieldtab))
    for nm, lo, width in fieldtab:
        w('    f("%s", %d, %d),' % (nm, lo, width))
    w("];")
    w("")
    w("/// An architecture feature some encoding needs, named as in AARCHMRS")
    w("/// without the `FEAT_` prefix.")
    w("#[allow(non_camel_case_types)]")
    w("#[repr(u8)]")
    w("#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash, PartialOrd, Ord)]")
    w("pub enum Feature {")
    for f in features:
        w("    %s," % f[len("FEAT_"):])
    w("}")
    w("")
    w("pub(super) const FEATURE_COUNT: usize = %d;" % len(features))
    w("")
    w("pub(super) static FEATURES: [Feature; FEATURE_COUNT] = [")
    for f in features:
        w("    Feature::%s," % f[len("FEAT_"):])
    w("];")
    w("")
    w("pub(super) static FEATURE_NAMES: [&str; FEATURE_COUNT] = [")
    for f in features:
        w('    "%s",' % f)
    w("];")
    w("")
    w("pub(super) static MENTIONS_INDEX: [u16; COUNT] = [")
    for i in need_index:
        w("    %d," % i)
    w("];")
    w("")
    w("pub(super) static MENTIONS: [&[Feature]; %d] = [" % len(featsets))
    for fs in featsets:
        w("    &[%s]," % ", ".join("Feature::%s" % f[len("FEAT_"):] for f in fs))
    w("];")
    w("")
    w("/// Whether the field and feature constraints of `e` hold for `w` on a CPU")
    w("/// with the features `f`; the fixed bits are checked elsewhere.")
    w("pub(super) fn condition(e: Encoding, w: u32, f: &Features) -> bool {")
    w("    match e {")
    for nm, expr in conds:
        w("        Encoding::%s => %s," % (nm, expr))
    w("        _ => true,")
    w("    }")
    w("}")
    w("")
    return "\n".join(out), n, len(conds), len(fieldtab), len(paths)


def main():
    if len(sys.argv) != 3:
        sys.exit("usage: aarchmrs_decoder.py <Instructions.json> <generated.rs>")
    try:
        text, n, nc, nf, npaths = generate(sys.argv[1])
    except Unsupported as e:
        sys.exit("error: unsupported input: %s" % e)
    with open(sys.argv[2], "w", encoding="ascii", newline="\n") as fh:
        fh.write(text)
    print("emitted %d leaves, %d conditions, %d field entries, %d paths" % (n, nc, nf, npaths))


main()
