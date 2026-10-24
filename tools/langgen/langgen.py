#!/usr/bin/env python3
"""Novus OS - UI string tables.

`strings.toml` is the only place a translation lives. This script writes a
sorted pair table for C (the renderer looks up the Chinese source text at
draw time) and for Rust (the apps use the same lookup to measure English
labels before drawing them).

The output is committed; the build does not depend on Python.

Usage:
    python3 tools/langgen/langgen.py            # write the tables
    python3 tools/langgen/langgen.py --check    # fail if they are stale
"""

import glob
import os
import sys

try:
    import tomllib
except ModuleNotFoundError:  # Python < 3.11
    import tomli as tomllib

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
SRC_GLOB = os.path.join(HERE, "strings*.toml")
OUT_C = os.path.join(ROOT, "graphics", "nd_lang_dict.c")
OUT_RS = os.path.join(ROOT, "apps", "src", "lang_dict.rs")


def c_escape(s):
    return s.replace("\\", "\\\\").replace('"', '\\"')


def rs_escape(s):
    return s.replace("\\", "\\\\").replace('"', '\\"')


def load_pairs():
    pairs = []
    for path in sorted(glob.glob(SRC_GLOB)):
        with open(path, "rb") as fh:
            doc = tomllib.load(fh)
        pairs.extend((e["zh"], e["en"]) for e in doc.get("string", []))
    # Both consumers binary-search by UTF-8 bytes, so sort the same way.
    pairs.sort(key=lambda p: p[0].encode("utf-8"))
    seen = set()
    for zh, _ in pairs:
        if zh in seen:
            raise SystemExit("langgen: duplicate source string: " + zh)
        seen.add(zh)
    return pairs


def gen_c(pairs):
    out = []
    out.append("/**")
    out.append(" * Novus OS - Chinese to English UI string table (generated).")
    out.append(" *")
    out.append(" * Source: tools/langgen/strings.toml. Sorted by UTF-8 bytes so")
    out.append(" * the lookup is a binary search over the source text.")
    out.append(" */")
    out.append("")
    out.append('#include "nd_lang.h"')
    out.append("")
    out.append("typedef struct {")
    out.append("  const char *zh;")
    out.append("  const char *en;")
    out.append("} nd_lang_pair_t;")
    out.append("")
    out.append("static const nd_lang_pair_t g_pairs[] = {")
    for zh, en in pairs:
        out.append('  {"%s", "%s"},' % (c_escape(zh), c_escape(en)))
    out.append("};")
    out.append("")
    out.append("static int pair_cmp(const char *a, const char *b) {")
    out.append("  const unsigned char *pa = (const unsigned char *)a;")
    out.append("  const unsigned char *pb = (const unsigned char *)b;")
    out.append("")
    out.append("  while (*pa != 0 && *pa == *pb) {")
    out.append("    pa++;")
    out.append("    pb++;")
    out.append("  }")
    out.append("  return (int)*pa - (int)*pb;")
    out.append("}")
    out.append("")
    out.append("const char *nd_lang_lookup(const char *zh) {")
    out.append("  int lo = 0;")
    out.append("  int hi = (int)(sizeof(g_pairs) / sizeof(g_pairs[0])) - 1;")
    out.append("")
    out.append("  if (zh == 0) {")
    out.append("    return 0;")
    out.append("  }")
    out.append("  while (lo <= hi) {")
    out.append("    int mid = (lo + hi) / 2;")
    out.append("    int c = pair_cmp(zh, g_pairs[mid].zh);")
    out.append("")
    out.append("    if (c == 0) {")
    out.append("      return g_pairs[mid].en;")
    out.append("    }")
    out.append("    if (c < 0) {")
    out.append("      hi = mid - 1;")
    out.append("    } else {")
    out.append("      lo = mid + 1;")
    out.append("    }")
    out.append("  }")
    out.append("  return 0;")
    out.append("}")
    out.append("")
    return "\n".join(out)


def gen_rs(pairs):
    out = []
    out.append("//! Chinese to English UI string table (generated).")
    out.append("//!")
    out.append("//! Source: tools/langgen/strings.toml. Sorted by UTF-8 bytes;")
    out.append("//! `lookup` is a binary search over the Chinese source text.")
    out.append("")
    out.append("static PAIRS: &[(&str, &str)] = &[")
    for zh, en in pairs:
        out.append('    ("%s", "%s"),' % (rs_escape(zh), rs_escape(en)))
    out.append("];")
    out.append("")
    out.append("pub fn lookup(s: &str) -> Option<&'static str> {")
    out.append("    let key = s.as_bytes();")
    out.append("    let mut lo = 0usize;")
    out.append("    let mut hi = PAIRS.len();")
    out.append("")
    out.append("    while lo < hi {")
    out.append("        let mid = (lo + hi) / 2;")
    out.append("        let zh = PAIRS[mid].0.as_bytes();")
    out.append("        match zh.cmp(key) {")
    out.append("            core::cmp::Ordering::Equal => return Some(PAIRS[mid].1),")
    out.append("            core::cmp::Ordering::Less => lo = mid + 1,")
    out.append("            core::cmp::Ordering::Greater => hi = mid,")
    out.append("        }")
    out.append("    }")
    out.append("    None")
    out.append("}")
    out.append("")
    out.append("pub fn len() -> usize {")
    out.append("    PAIRS.len()")
    out.append("}")
    out.append("")
    return "\n".join(out)


def write(path, text, check):
    old = None
    if os.path.exists(path):
        with open(path, "r", encoding="utf-8") as fh:
            old = fh.read()
    if old == text:
        return True
    if check:
        print("langgen: stale: " + os.path.relpath(path, ROOT), file=sys.stderr)
        return False
    with open(path, "w", encoding="utf-8", newline="\n") as fh:
        fh.write(text)
    print("langgen: wrote " + os.path.relpath(path, ROOT))
    return True


def main():
    check = "--check" in sys.argv[1:]
    pairs = load_pairs()
    ok = write(OUT_C, gen_c(pairs), check)
    ok = write(OUT_RS, gen_rs(pairs), check) and ok
    if check and ok:
        print("langgen: tables are current (%d strings)" % len(pairs))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
