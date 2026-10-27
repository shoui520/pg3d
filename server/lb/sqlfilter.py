"""
The SQL lobby's matchmaking filter, e.g. from Pixel Gun 3D:

    ( C0 = 5 OR C0 = 7 ) AND C1 = "" AND C2 = 3 AND C3 = 0 AND C7 > 100

Comparisons (= == != <> < > <= >=), AND, OR, NOT, parentheses, IN (...),
numbers and quoted strings. A property the room doesn't have is NULL, and a
comparison with NULL is false, as in SQL.
"""
import re

_TOKEN = re.compile(r"""\s*(?:
    (?P<num>-?\d+(?:\.\d+)?)
  | "(?P<dq>(?:[^"\\]|\\.)*)"
  | '(?P<sq>(?:[^'\\]|\\.)*)'
  | (?P<op><=|>=|<>|!=|==|=|<|>|\(|\)|,)
  | (?P<word>[A-Za-z_][A-Za-z0-9_]*)
)""", re.X)


def _tokens(text):
    pos, out = 0, []
    text = text.strip()
    while pos < len(text):
        m = _TOKEN.match(text, pos)
        if not m or m.end() == pos:
            raise ValueError("unexpected %r at %d in %r" % (text[pos:pos + 10], pos, text))
        pos = m.end()
        if m.group("num") is not None:
            n = m.group("num")
            out.append(("val", float(n) if "." in n else int(n)))
        elif m.group("dq") is not None:
            out.append(("val", m.group("dq")))
        elif m.group("sq") is not None:
            out.append(("val", m.group("sq")))
        elif m.group("op") is not None:
            out.append(("op", m.group("op")))
        else:
            w = m.group("word")
            out.append(("kw", w.upper()) if w.upper() in ("AND", "OR", "NOT", "IN", "NULL", "IS") else ("name", w))
    return out


class _Parser:
    def __init__(self, toks):
        self.t, self.i = toks, 0

    def peek(self):
        return self.t[self.i] if self.i < len(self.t) else (None, None)

    def take(self, kind=None, value=None):
        tok = self.peek()
        if tok[0] is None or (kind and tok[0] != kind) or (value and tok[1] != value):
            raise ValueError("expected %s %s, got %r" % (kind, value, tok))
        self.i += 1
        return tok

    def expr(self):
        node = self.conj()
        while self.peek() == ("kw", "OR"):
            self.take()
            node = ("or", node, self.conj())
        return node

    def conj(self):
        node = self.neg()
        while self.peek() == ("kw", "AND"):
            self.take()
            node = ("and", node, self.neg())
        return node

    def neg(self):
        if self.peek() == ("kw", "NOT"):
            self.take()
            return ("not", self.neg())
        return self.atom()

    def operand(self):
        kind, v = self.take()
        if kind == "name":
            return ("prop", v)
        if kind == "val":
            return ("val", v)
        if (kind, v) == ("kw", "NULL"):
            return ("val", None)
        raise ValueError("expected an operand, got %r" % v)

    def atom(self):
        if self.peek() == ("op", "("):
            self.take()
            node = self.expr()
            self.take("op", ")")
            return node
        left = self.operand()
        kind, v = self.peek()
        if (kind, v) == ("kw", "IN"):
            self.take()
            self.take("op", "(")
            items = [self.operand()]
            while self.peek() == ("op", ","):
                self.take()
                items.append(self.operand())
            self.take("op", ")")
            return ("in", left, items)
        if (kind, v) == ("kw", "IS"):
            self.take()
            negate = self.peek() == ("kw", "NOT")
            if negate:
                self.take()
            self.take("kw", "NULL")
            return ("isnull", left, negate)
        op = self.take("op")[1]
        if op not in ("=", "==", "!=", "<>", "<", ">", "<=", ">="):
            raise ValueError("unexpected %r" % op)
        return ("cmp", op, left, self.operand())


def compile_filter(text):
    p = _Parser(_tokens(text))
    node = p.expr()
    if p.i != len(p.t):
        raise ValueError("trailing %r" % (p.t[p.i:],))
    return node


def _value(node, props):
    if node[0] == "val":
        return node[1]
    v = props.get(node[1])
    # plain numbers: the wire types (Byte, Short, Float...) compare by type
    if isinstance(v, int):
        return int(v)
    if isinstance(v, float):
        return float(v)
    return v


def _compare(op, a, b):
    if a is None or b is None:
        return False
    num = (int, float)
    if isinstance(a, num) != isinstance(b, num):
        # SQLite-style affinity: compare numbers with numeric text
        try:
            a, b = (float(a), float(b))
        except (TypeError, ValueError):
            a, b = str(a), str(b)
    try:
        return {"=": a == b, "==": a == b, "!=": a != b, "<>": a != b,
                "<": a < b, ">": a > b, "<=": a <= b, ">=": a >= b}[op]
    except TypeError:
        return False


def evaluate(node, props):
    """props: the room's custom properties by name"""
    kind = node[0]
    if kind == "or":
        return evaluate(node[1], props) or evaluate(node[2], props)
    if kind == "and":
        return evaluate(node[1], props) and evaluate(node[2], props)
    if kind == "not":
        return not evaluate(node[1], props)
    if kind == "cmp":
        return _compare(node[1], _value(node[2], props), _value(node[3], props))
    if kind == "in":
        a = _value(node[1], props)
        return any(_compare("=", a, _value(i, props)) for i in node[2])
    if kind == "isnull":
        isnull = _value(node[1], props) is None
        return not isnull if node[2] else isnull
    raise ValueError(kind)
