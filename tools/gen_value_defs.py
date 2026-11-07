#!/usr/bin/env python3
"""Generates src/core/esp/ValueDefinitions.inc (named, typed field members) from xEdit's Fallout 4
record definitions (Core/wbDefinitionsFO4.pas).

Only layouts that are certain are emitted: fixed-size members (integers, floats, FormIDs, fixed byte
runs, nested fixed structs, fixed-count arrays). A struct stops at the first member whose size or
position is not fixed (strings, variable arrays, unions, version- or game-dependent members); the
bytes after it are shown as bytes by the program. A subrecord described differently in two places of
the same record is left out. The program applies a generated layout only where its own FormID
positions agree with it.

Run: python3 -I tools/gen_value_defs.py <xEdit>/Core/wbDefinitionsFO4.pas src/core/esp/ValueDefinitions.inc
"""
import re
import sys

# ---------------------------------------------------------------- tokens and expressions

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


def tokenize(text):
    out = []
    for m in TOKEN.finditer(text):
        kind = m.lastgroup
        if kind in ("ws", "c1", "c2", "c3"):
            continue
        v = m.group(kind)
        if kind == "str":
            v = v[1:-1].replace("''", "'")
        elif kind == "hex":
            kind, v = "num", str(int(v[1:], 16))
        out.append((kind, v))
    return out


class Call:
    def __init__(self, name, args):
        self.name, self.args = name, args

    def __repr__(self):
        return "%s(%s)" % (self.name, ", ".join(map(repr, self.args)))


class Ident:
    def __init__(self, name):
        self.name = name

    def __repr__(self):
        return self.name


class Lst:
    def __init__(self, items):
        self.items = items


class Str:
    def __init__(self, v):
        self.v = v


class Num:
    def __init__(self, v):
        self.v = v


class Other:
    pass


class Parser:
    def __init__(self, toks):
        self.t, self.i = toks, 0

    def peek(self, k=0):
        j = self.i + k
        return self.t[j] if j < len(self.t) else ("eof", "")

    def take(self):
        tok = self.peek()
        self.i += 1
        return tok

    def expect(self, v):
        tok = self.take()
        if tok[1] != v:
            raise SyntaxError("expected %r got %r at %d" % (v, tok, self.i))

    def args(self, close):
        items = []
        if self.peek()[1] == close:
            self.take()
            return items
        while True:
            items.append(self.expr())
            tok = self.take()
            if tok[1] == close:
                return items
            if tok[1] != ",":
                raise SyntaxError("expected , or %s got %r at %d" % (close, tok, self.i))

    def primary(self):
        kind, v = self.take()
        if kind == "str":
            node = Str(v)
            while self.peek()[0] == "str" or self.peek()[1] == "#":  # 'a'#13'b'
                self.take()
        elif kind == "num":
            node = Num(float(v) if "." in v else int(v))
        elif kind == "id":
            if v.lower() in ("not", "nil", "true", "false") and v.lower() == "not":
                self.expr_unary()
                return Other()
            node = Ident(v)
            if self.peek()[1] == "(":
                self.take()
                node = Call(v, self.args(")"))
        elif v == "[":
            node = Lst(self.args("]"))
        elif v == "(":
            node = self.expr()
            self.expect(")")
        elif v in ("-", "@", "+"):
            self.primary()
            return Other()
        else:
            raise SyntaxError("unexpected %r at %d" % ((kind, v), self.i))
        # method chains: .SetX(...); their arguments do not matter (and may hold anonymous functions)
        while self.peek()[1] == "." and self.peek(1)[0] == "id":
            self.take()
            self.take()
            if self.peek()[1] == "(":
                self.skip_parens()
        return node

    def skip_parens(self):
        depth = 0
        while True:
            kind, v = self.take()
            if kind == "eof":
                raise SyntaxError("unbalanced parentheses")
            if v == "(":
                depth += 1
            elif v == ")":
                depth -= 1
                if depth == 0:
                    return

    def expr_unary(self):
        return self.primary()

    def expr(self):
        node = self.primary()
        while self.peek()[1] in ("+", "-", "*", "/", "..", "=", "<>", "<", ">", "<=", ">=") or \
                self.peek()[1].lower() in ("or", "and", "in", "div", "mod", "shl", "shr", "xor"):
            self.take()
            self.primary()
            node = Other()
        return node


# ---------------------------------------------------------------- interpretation

SIG = re.compile(r"^[A-Z0-9_]{4}$")
INT_TYPES = {"itU8": ("U8", 1), "itS8": ("I8", 1), "itU16": ("U16", 2), "itS16": ("I16", 2),
             "itU32": ("U32", 4), "itS32": ("I32", 4), "itU6to30": None, "itU64": ("BYTES", 8), "itS64": ("BYTES", 8)}
FORMID_CALLS = {"wbFormID", "wbFormIDCk", "wbFormIDCK", "wbFormIDCkNoReach", "wbFormIDCKNoReach", "wbFormIDNoReach",
                "wbFormIDCkTarget"}


class Stop(Exception):
    pass


def labels_of(node):
    """Flag or enum names from wbFlags([...]) / wbEnum([...]) with a plain list of strings."""
    if isinstance(node, Call) and node.name in ("wbFlags", "wbEnum") and node.args and isinstance(node.args[0], Lst):
        names = []
        for it in node.args[0].items:
            if isinstance(it, Str):
                names.append(it.v)
            else:
                return None
        return names
    return None


def is_flags(node):
    return isinstance(node, Call) and node.name == "wbFlags"


def is_enum(node):
    return isinstance(node, Call) and node.name == "wbEnum" or isinstance(node, Ident) and node.name.endswith("Enum")


def name_of(args, i):
    return args[i].v if len(args) > i and isinstance(args[i], Str) else "Unknown"


def member_list(node, prefix, globals_, out, offset):
    """Appends fixed members of a value definition; returns the new offset. Raises Stop."""
    if isinstance(node, Ident) and node.name in globals_:
        return member_list(globals_[node.name], prefix, globals_, out, offset)
    if not isinstance(node, Call):
        raise Stop
    n, a = node.name, node.args
    nm = lambda i: (prefix + name_of(a, i)).strip()
    if n == "wbInteger":
        if len(a) < 2 or not isinstance(a[1], Ident) or a[1].name not in INT_TYPES or INT_TYPES[a[1].name] is None:
            raise Stop
        t, size = INT_TYPES[a[1].name]
        extra = a[2] if len(a) > 2 else None
        labels = labels_of(extra) if extra is not None else None
        if t == "BYTES":
            out.append((nm(0), "Bytes", offset, size, None))
        elif extra is not None and is_flags(extra):
            out.append((nm(0), {1: "Flags8", 2: "Flags16", 4: "Flags32"}[size], offset, 0, labels))
        elif extra is not None and is_enum(extra) and size in (1, 4) and labels:
            out.append((nm(0), {1: "Enum8", 4: "Enum32"}[size], offset, 0, labels))
        else:
            out.append((nm(0), t, offset, 0, None))
        return offset + size
    if n in ("wbFloat", "wbFloatAngle"):
        out.append((nm(0), "Float", offset, 0, None))
        return offset + 4
    if n in FORMID_CALLS:
        out.append((nm(0), "FormId", offset, 0, None))
        return offset + 4
    if n == "wbUnused":
        if len(a) == 1 and isinstance(a[0], Num) and a[0].v > 0:
            out.append((prefix + "Unused", "Bytes", offset, a[0].v, None))
            return offset + a[0].v
        raise Stop
    if n in ("wbByteArray", "wbUnknown"):
        if len(a) >= 2 and isinstance(a[1], Num) and a[1].v > 0:
            out.append((nm(0), "Bytes", offset, a[1].v, None))
            return offset + a[1].v
        raise Stop
    if n in ("wbVec3", "wbVec3Pos", "wbVec3Rot"):
        base = name_of(a, 0) if a else {"wbVec3": "Unknown", "wbVec3Pos": "Position", "wbVec3Rot": "Rotation"}[n]
        for i, c in enumerate("XYZ"):
            out.append((prefix + base + " " + c, "Float", offset + 4 * i, 0, None))
        return offset + 12
    if n == "wbVec3Int":
        base = name_of(a, 0)
        for i, c in enumerate("XYZ"):
            out.append((prefix + base + " " + c, "I16", offset + 2 * i, 0, None))
        return offset + 6
    if n == "wbVec3PosRot":
        for i, c in enumerate(["Position X", "Position Y", "Position Z", "Rotation X", "Rotation Y", "Rotation Z"]):
            out.append((prefix + c, "Float", offset + 4 * i, 0, None))
        return offset + 24
    if n in ("wbStruct", "wbStructSK", "wbStructExSK"):
        lst = next((x for x in a if isinstance(x, Lst)), None)
        if lst is None:
            raise Stop
        sname = next((x.v for x in a if isinstance(x, Str)), "")
        inner = prefix + (sname + " - " if sname else "")
        for m in lst.items:
            offset = member_list(m, inner, globals_, out, offset)
        return offset
    if n in ("wbArray", "wbArrayS") and len(a) >= 3 and isinstance(a[2], Num) and a[2].v > 0:
        for k in range(a[2].v):
            offset = member_list(a[1], prefix + name_of(a, 0) + " #" + str(k) + " - ", globals_, out, offset)
        return offset
    raise Stop


def struct_members(lst, globals_):
    """Members of a struct body up to the first one that is not fixed; and whether all were."""
    out, offset = [], 0
    for m in lst.items:
        try:
            tmp = []
            new = member_list(m, "", globals_, tmp, offset)
        except Stop:
            return out, offset, False
        out += tmp
        offset = new
    return out, offset, True


def subrecord_defs(node, globals_, out, depth=0):
    """Collects (sig, label, members, stride) for every subrecord-level definition in a record body."""
    if depth > 40:
        return
    if isinstance(node, Ident):
        if node.name in globals_:
            subrecord_defs(globals_[node.name], globals_, out, depth + 1)
        return
    if isinstance(node, Lst):
        for it in node.items:
            subrecord_defs(it, globals_, out, depth + 1)
        return
    if not isinstance(node, Call):
        return
    n, a = node.name, node.args
    sig = a[0].name if a and isinstance(a[0], Ident) and SIG.match(a[0].name) else None
    if sig:
        label = name_of(a, 1)
        try:
            if n in ("wbStruct", "wbStructSK", "wbStructExSK"):
                lst = next((x for x in a if isinstance(x, Lst)), None)
                if lst is not None:
                    members, size, complete = struct_members(lst, globals_)
                    if members:
                        out.append((sig, label, members, 0, complete))
                return
            if n == "wbInteger" or n in ("wbFloat", "wbFloatAngle"):
                tmp = []
                member_list(Call(n, a[1:]), "", globals_, tmp, 0)
                out.append((sig, label, tmp, 0, True))
                return
            if n in ("wbArray", "wbArrayS") and len(a) >= 3:
                elem = a[2]
                if isinstance(elem, Call) and elem.name in FORMID_CALLS:
                    return  # a FormID array: the program shows it by kind
                tmp = []
                stride = member_list(elem, "", globals_, tmp, 0)
                if tmp and stride > 0:
                    out.append((sig, label, tmp, stride, True))
                return
        except Stop:
            return
        return
    # containers: walk everything (wbRStruct, wbRArray, wbRUnion, ...)
    for x in a:
        subrecord_defs(x, globals_, out, depth + 1)


def main(src, dst):
    text = open(src, encoding="utf-8", errors="replace").read()
    toks = tokenize(text)
    globals_ = {}
    records = []
    i = 0
    while i < len(toks):
        kind, v = toks[i]
        if kind == "id" and i + 1 < len(toks) and toks[i + 1][1] == ":=" and v.startswith("wb"):
            p = Parser(toks)
            p.i = i + 2
            try:
                globals_[v] = p.expr()
                i = p.i
                continue
            except SyntaxError:
                pass
        if kind == "id" and v in ("RegisterRecordDef", "RegisterRefRecordDef") and toks[i + 1][1] == "(":
            p = Parser(toks)
            p.i = i + 2
            try:
                args = p.args(")")
                i = p.i
                if args and isinstance(args[0], Ident):
                    records.append((args[0].name, args))
                continue
            except SyntaxError as e:
                print("skipped a definition near token %d: %s" % (i, e), file=sys.stderr)
        i += 1

    lines = ["// Generated by tools/gen_value_defs.py from xEdit's wbDefinitionsFO4.pas. Do not edit.",
             "// gen(record, subrecord, label, stride, complete, members): named members of fixed layouts;",
             "// complete = every member of xEdit's layout is here (else the bytes after the last are not fixed)."]
    total_records = total_fields = 0
    for sig, args in sorted(records, key=lambda r: r[0]):
        if not SIG.match(sig):
            continue
        found = []
        for x in args[1:]:
            subrecord_defs(x, globals_, found)
        by = {}
        for s, label, members, stride, complete in found:
            key = (tuple((m[0], m[1], m[2], m[3]) for m in members), stride, complete)
            by.setdefault(s, []).append((label, members, stride, key, complete))
        emitted = 0
        for s in sorted(by):
            variants = by[s]
            if len({v[3] for v in variants}) != 1:
                continue  # described two ways in this record: ambiguous
            label, members, stride, _, complete = variants[0]
            parts = []
            for name, t, off, size, labels in members:
                lab = ""
                if labels:
                    lab = ", {" + ", ".join(cpp_str(x) for x in labels) + "}"
                if t == "Bytes":
                    parts.append("B(%s, %d, %d)" % (cpp_str(name), off, size))
                else:
                    parts.append("V(%s, T::%s, %d%s)" % (cpp_str(name), t, off, lab))
            lines.append('gen("%s", "%s", %s, %d, %s, {%s});' % (sig, s, cpp_str(label), stride,
                                                              "true" if complete else "false", ", ".join(parts)))
            emitted += 1
        if emitted:
            total_records += 1
            total_fields += emitted
    with open(dst, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(lines) + "\n")
    print("%d record definitions read, %d record types with %d described fields written" %
          (len(records), total_records, total_fields))


def cpp_str(s):
    return '"' + s.replace("\\", "\\\\").replace('"', '\\"') + '"'


if __name__ == "__main__":
    if len(sys.argv) != 3:
        raise SystemExit(__doc__)
    main(sys.argv[1], sys.argv[2])
