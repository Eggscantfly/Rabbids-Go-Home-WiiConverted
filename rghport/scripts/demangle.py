"""Demangler for the cfront / GNU-v2 style names CodeWarrior emits (Wii script module and Wii executable).

    OBJ_PosGet_C__FP3OBJP14MTH_tt_Vector3
      -> OBJ_PosGet_C(OBJ*, MTH_tt_Vector3*)
    p_Add__9TOOsarrayFUlPv
      -> TOOsarray::p_Add(unsigned long, void*)

Covers what actually occurs in these images: pointers, references, const, arrays, builtins, class names, T/N argument
back-references, function pointers and static/const member qualifiers.  Anything it cannot parse is returned unchanged
rather than guessed at.
"""
from __future__ import annotations

import re
import sys

BUILTIN = {
    'v': 'void', 'c': 'char', 's': 'short', 'i': 'int', 'l': 'long',
    'f': 'float', 'd': 'double', 'b': 'bool', 'w': 'wchar_t', 'x': 'long long',
    'r': 'long double', 'e': '...',
}
QUAL = {'U': 'unsigned', 'S': 'signed', 'C': 'const', 'V': 'volatile'}


class _P:
    def __init__(self, s):
        self.s, self.i = s, 0
        self.args = []

    def eof(self):
        return self.i >= len(self.s)

    def peek(self):
        return self.s[self.i] if self.i < len(self.s) else ''

    def take(self):
        c = self.s[self.i]
        self.i += 1
        return c

    def number(self):
        j = self.i
        while j < len(self.s) and self.s[j].isdigit():
            j += 1
        if j == self.i:
            return None
        n = int(self.s[self.i:j])
        self.i = j
        return n


def _type(p, depth=0):
    if p.eof() or depth > 20:
        return None
    c = p.peek()
    if c == 'P':
        p.take()
        t = _type(p, depth + 1)
        if t is None:
            return None
        # PF... is already spelled as a function *pointer* by the F case
        return t if '(*)' in t else t + '*'
    if c == 'R':
        p.take()
        t = _type(p, depth + 1)
        return None if t is None else t + '&'
    if c in ('C', 'V'):
        p.take()
        t = _type(p, depth + 1)
        return None if t is None else ('%s %s' % (QUAL[c], t))
    if c in ('U', 'S'):
        p.take()
        t = _type(p, depth + 1)
        return None if t is None else ('%s %s' % (QUAL[c], t))
    if c == 'A':                       # A<n>_<type>
        p.take()
        n = p.number()
        if p.peek() == '_':
            p.take()
        t = _type(p, depth + 1)
        return None if t is None else '%s[%s]' % (t, n if n is not None else '')
    if c == 'F':                       # function type: F<args>_<ret>
        p.take()
        args = []
        while not p.eof() and p.peek() != '_':
            t = _type(p, depth + 1)
            if t is None:
                return None
            args.append(t)
        ret = 'void'
        if p.peek() == '_':
            p.take()
            ret = _type(p, depth + 1) or 'void'
        return '%s(*)(%s)' % (ret, ', '.join(args) or 'void')
    if c == 'M':                       # pointer to member
        p.take()
        n = p.number()
        cls = p.s[p.i:p.i + n] if n else '?'
        p.i += n or 0
        t = _type(p, depth + 1)
        return '%s %s::*' % (t, cls)
    if c == 'T':                       # back-reference to arg n
        p.take()
        n = p.number()
        if n is None or n < 1 or n > len(p.args):
            return None
        return p.args[n - 1]
    if c == 'N':                       # N<count><argno>: repeat
        p.take()
        cnt = int(p.take())
        n = p.number()
        if n is None or n < 1 or n > len(p.args):
            return None
        p._repeat = (cnt - 1, p.args[n - 1])
        return p.args[n - 1]
    if c == 'Q':                       # qualified name Q<k><len><nm>...
        p.take()
        k = int(p.take())
        parts = []
        for _ in range(k):
            n = p.number()
            if n is None:
                return None
            parts.append(p.s[p.i:p.i + n])
            p.i += n
        return '::'.join(parts)
    if c.isdigit():
        n = p.number()
        nm = p.s[p.i:p.i + n]
        p.i += n
        return nm
    if c in BUILTIN:
        p.take()
        return BUILTIN[c]
    return None


def _args(mangled):
    p = _P(mangled)
    out = []
    while not p.eof():
        p._repeat = None
        t = _type(p)
        if t is None:
            return None
        out.append(t)
        rep = getattr(p, '_repeat', None)
        if rep:
            out.extend([rep[1]] * rep[0])
            p._repeat = None
        p.args = out
    return out


def split(sym):
    """(name, class, arg_mangling) or None."""
    i = sym.find('__')
    while i != -1:
        rest = sym[i + 2:]
        m = re.match(r'^(\d+)([A-Za-z_][A-Za-z0-9_]*)', rest)
        if m and len(m.group(2)) >= int(m.group(1)):
            n = int(m.group(1))
            cls = rest[len(m.group(1)):len(m.group(1)) + n]
            tail = rest[len(m.group(1)) + n:]
            if tail[:1] in ('F', 'C', 'S'):
                return sym[:i], cls, tail
        if rest[:1] == 'F':
            return sym[:i], None, rest
        if rest[:1] == 'Q':
            p = _P(rest)
            cls = _type(p)
            if cls and p.peek() in ('F', 'C', 'S'):
                return sym[:i], cls, rest[p.i:]
        i = sym.find('__', i + 1)
    return None


def demangle(sym):
    sp = split(sym)
    if not sp:
        return sym
    name, cls, tail = sp
    quals = []
    while tail[:1] in ('C', 'S', 'V'):
        quals.append({'C': 'const', 'S': 'static', 'V': 'volatile'}[tail[0]])
        tail = tail[1:]
    if tail[:1] != 'F':
        return sym
    args = _args(tail[1:])
    if args is None:
        return sym
    if args == ['void']:
        args = []
    full = ('%s::%s' % (cls, name)) if cls else name
    s = '%s(%s)' % (full, ', '.join(args))
    if 'const' in quals:
        s += ' const'
    if 'static' in quals:
        s = 'static ' + s
    return s


def arity(sym):
    """Number of declared arguments, or None if the name will not parse."""
    sp = split(sym)
    if not sp:
        return None
    tail = sp[2].lstrip('CSV')
    if tail[:1] != 'F':
        return None
    a = _args(tail[1:])
    if a is None:
        return None
    if a == ['void']:
        return 0
    return len(a)


if __name__ == '__main__':
    for t in sys.argv[1:] or ['OBJ_PosGet_C__FP3OBJP14MTH_tt_Vector3', 'p_Add__9TOOsarrayFUlPv',
                              'SCR_CreateSignal__FP3SCRPcPFPvR13ViD_tt_Stock__l', '__dt__8Cine_TRGFv']:
        print('%-64s -> %s' % (t, demangle(t)))
