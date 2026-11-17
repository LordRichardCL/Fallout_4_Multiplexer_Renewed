#!/usr/bin/env python3
"""Generates src/core/esp/SubrecordOrder.inc (the order xEdit expects a record's subrecords in) from
xEdit's Fallout 4 record definitions.

For every record type, the signatures of its members are listed in definition order, with the
helpers they use (wbKeywords, wbObjectTemplate, wbDamageTypeArray(...), ...) expanded in place. A
signature is listed once, where it first appears. Signatures of record types (the allowed targets of
a FormID, such as [ARMO]) are not subrecords and are left out.

The order is used to place a field the written version lacks between the fields it has; it does not
reorder fields the written version already holds.

Run: python3 -I tools/gen_subrecord_order.py <xEdit>/Core/wbDefinitionsFO4.pas \
         <xEdit>/Core/wbDefinitionsCommon.pas src/core/esp/SubrecordOrder.inc
"""
import re
import sys

TOKEN = re.compile(r"""
    (?P<ws>\s+)
  | (?P<c1>\{[^}]*\})
  | (?P<c2>\(\*.*?\*\))
  | (?P<c3>//[^\n]*)
  | (?P<str>'(?:[^']|'')*')
  | (?P<hex>\$[0-9A-Fa-f]+)
  | (?P<num>\d+(?:\.\d+)?(?:[eE][-+]?\d+)?)
  | (?P<id>[A-Za-z_][A-Za-z0-9_]*)
  | (?P<op>:=|\.\.|<=|>=|<>|[()\[\],;.:+\-*/=<>@^\#])
""", re.S | re.X)

SIG = re.compile(r"^[A-Z][A-Z0-9_]{3}$")

DIRECTIVES = {"overload", "override", "final", "inline", "static", "virtual", "reintroduce", "forward"}

RECORD_CALLS = ("RegisterRecordDef", "RegisterRefRecordDef", "wbRecord", "wbRefRecord", "ReferenceRecord")


def tokenize(text):
    out = []
    for m in TOKEN.finditer(text):
        kind = m.lastgroup
        if kind in ("ws", "c1", "c2", "c3", "str", "hex", "num"):
            continue
        out.append((kind, m.group(kind)))
    return out


def definitions(toks):
    """name -> token list of its definition: `wbX := ... ;` and `function wbX ... begin ... end;`."""
    defs = {}
    i = 0
    n = len(toks)
    while i < n:
        kind, v = toks[i]
        if kind == "id" and v.lower() == "function" and i + 1 < n:
            # `function wbX(...)` or `function TwbGameDefFO4.wbX(...)`
            j = i + 1
            if j + 2 < n and toks[j + 1][1] == ".":
                j += 2
            name = toks[j][1]
            if not name.startswith("wb"):
                i += 1
                continue
            # end of the header: the `;` outside parentheses, then any directives
            depth = 0
            j += 1
            while j < n:
                w = toks[j][1]
                if w == "(":
                    depth += 1
                elif w == ")":
                    depth -= 1
                elif w == ";" and depth == 0:
                    break
                j += 1
            j += 1
            while j + 1 < n and toks[j][1].lower() in DIRECTIVES and toks[j + 1][1] == ";":
                j += 2
            if j < n and toks[j][1].lower() in ("begin", "var", "const", "type"):
                # an implementation: the body runs from its first `begin` to the matching `end`
                while j < n and toks[j][1].lower() != "begin":
                    j += 1
                depth = 0
                k = j
                while k < n:
                    w = toks[k][1].lower()
                    if w in ("begin", "case", "try"):
                        depth += 1
                    elif w == "end":
                        depth -= 1
                        if depth == 0:
                            break
                    k += 1
                defs.setdefault(name, toks[j:k])
                i = k
                continue
            i = j  # a declaration only
            continue
        if kind == "id" and v.startswith("wb") and i + 1 < n and toks[i + 1][1] == ":=":
            depth = 0
            k = i + 2
            while k < n:
                w = toks[k][1]
                if w in ("(", "["):
                    depth += 1
                elif w in (")", "]"):
                    depth -= 1
                elif w == ";" and depth == 0:
                    break
                k += 1
            defs[v] = toks[i + 2:k]  # the last assignment in the file wins
            i = k
            continue
        i += 1
    return defs


def records(toks):
    """(signature, member tokens) of every RegisterRecordDef / wbRecord call, in file order."""
    out = []
    i = 0
    n = len(toks)
    while i < n:
        v = toks[i][1]
        if v in RECORD_CALLS and i + 2 < n and toks[i + 1][1] == "(" and SIG.match(toks[i + 2][1]):
            depth = 0
            k = i + 1
            while k < n:
                w = toks[k][1]
                if w in ("(", "["):
                    depth += 1
                elif w in (")", "]"):
                    depth -= 1
                    if depth == 0:
                        break
                k += 1
            out.append((toks[i + 2][1], toks[i + 3:k]))
            i = k
            continue
        i += 1
    return out


def main():
    fo4, common, outPath = sys.argv[1], sys.argv[2], sys.argv[3]
    fo4Toks = tokenize(open(fo4, encoding="utf-8", errors="replace").read())
    commonToks = tokenize(open(common, encoding="utf-8", errors="replace").read())
    defs = definitions(commonToks)
    defs.update(definitions(fo4Toks))  # the game's own definitions win
    recs = records(fo4Toks)
    recordSigs = {s for s, _ in recs} | {"NULL", "PLYR"}

    def expand(toks, stack, out):
        for kind, v in toks:
            if kind != "id":
                continue
            if SIG.match(v):
                if v not in recordSigs and v not in out:
                    out.append(v)
            elif v.startswith("wb") and v in defs and v not in stack and len(stack) < 12:
                expand(defs[v], stack + [v], out)

    lines = [
        "// Generated by tools/gen_subrecord_order.py from xEdit's wbDefinitionsFO4.pas. Do not edit.",
        "// order(record, subrecords): the order xEdit expects a record's subrecords in.",
    ]
    seen = set()
    for sig, toks in recs:
        if sig in seen:
            continue
        seen.add(sig)
        out = []
        expand(toks, [], out)
        if out:
            lines.append('order("%s", "%s");' % (sig, " ".join(out)))
    open(outPath, "w", encoding="utf-8", newline="\n").write("\n".join(lines) + "\n")
    print("%d record types" % len(seen))


if __name__ == "__main__":
    main()
