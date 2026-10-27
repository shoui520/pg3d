"""
Photon's "GpBinaryV16" serialization (Protocol16), as Photon3Unity3D 4.1 speaks it.

Values decode into Python objects that remember their wire type, so whatever
the server relays (event data, properties) encodes back to the same bytes:

    None 42 · bool 111 · Byte 98 · Short 107 · int 105 · Long 108
    Float 102 · float 100 (double) · str 115 · bytes 120 · Hashtable 104
    Dictionary 68 · Array 121 · IntArray 110 · list 122 (object[])
    Custom 99 · EventData 101 · OperationResponse 112 · OperationRequest 113

The integer and float wrappers compare and hash by type as well as value,
like keys in a C# Hashtable: Byte(1) and 1 are different keys.
"""
import struct

NULL, DICTIONARY, STRING_ARRAY, BYTE, CUSTOM, DOUBLE, EVENT_DATA, FLOAT = 42, 68, 97, 98, 99, 100, 101, 102
HASHTABLE, INTEGER, SHORT, LONG, INT_ARRAY, BOOLEAN = 104, 105, 107, 108, 110, 111
OP_RESPONSE, OP_REQUEST, STRING, BYTE_ARRAY, ARRAY, OBJECT_ARRAY = 112, 113, 115, 120, 121, 122


class _TypedInt(int):
    __slots__ = ()
    CODE = 0

    def __eq__(self, other):
        return type(other) is type(self) and int.__eq__(self, other)

    def __ne__(self, other):
        return not self.__eq__(other)

    def __hash__(self):
        return hash((self.CODE, int(self)))

    def __repr__(self):
        return "%s(%d)" % (type(self).__name__, int(self))


class Byte(_TypedInt):
    __slots__ = ()
    CODE = BYTE


class Short(_TypedInt):
    __slots__ = ()
    CODE = SHORT


class Long(_TypedInt):
    __slots__ = ()
    CODE = LONG


class Float(float):
    """A 32-bit float; a plain Python float is a double"""
    __slots__ = ()

    def __eq__(self, other):
        return type(other) is Float and float.__eq__(self, other)

    def __ne__(self, other):
        return not self.__eq__(other)

    def __hash__(self):
        return hash((FLOAT, float(self)))

    def __repr__(self):
        return "Float(%r)" % float(self)


class Hashtable(dict):
    """ExitGames.Client.Photon.Hashtable: any keys, any values"""
    __slots__ = ()


class Dictionary:
    """Dictionary<K, V>; a type code of 0 means each key (or value) carries its own"""
    __slots__ = ("ktype", "vtype", "items")

    def __init__(self, ktype, vtype, items=None):
        self.ktype, self.vtype = ktype, vtype
        self.items = items if items is not None else {}

    def __eq__(self, other):
        return isinstance(other, Dictionary) and (self.ktype, self.vtype, self.items) == (other.ktype, other.vtype, other.items)

    def __repr__(self):
        return "Dictionary(%d, %d, %r)" % (self.ktype, self.vtype, self.items)


class Array:
    """A typed array (int[], string[], bool[], ...). `items` hold plain values
    of the element type; `custom` is the type code for arrays of a custom
    type (items are then their serialized bytes)."""
    __slots__ = ("etype", "items", "custom")

    def __init__(self, etype, items, custom=0):
        self.etype, self.items, self.custom = etype, list(items), custom

    def __eq__(self, other):
        return isinstance(other, Array) and (self.etype, self.items, self.custom) == (other.etype, other.items, other.custom)

    def __hash__(self):
        return hash((ARRAY, self.etype, tuple(self.items)))

    def __len__(self):
        return len(self.items)

    def __iter__(self):
        return iter(self.items)

    def __repr__(self):
        return "Array(%d, %r)" % (self.etype, self.items)


class IntArray(list):
    """The int[] of type 110 (int length); PUN itself sends int[] as Array(105)"""
    __slots__ = ()


class Custom:
    """A registered custom type (PUN: Vector3 'V', Vector2 'W', Quaternion 'Q',
    PhotonPlayer 'P'), passed through as bytes"""
    __slots__ = ("code", "data")

    def __init__(self, code, data):
        self.code, self.data = code, bytes(data)

    def __eq__(self, other):
        return isinstance(other, Custom) and (self.code, self.data) == (other.code, other.data)

    def __hash__(self):
        return hash((CUSTOM, self.code, self.data))

    def __repr__(self):
        return "Custom(%r, %s)" % (chr(self.code), self.data.hex())


class OperationRequest:
    __slots__ = ("code", "params")

    def __init__(self, code, params=None):
        self.code, self.params = code, params if params is not None else {}

    def __repr__(self):
        return "OperationRequest(%d, %r)" % (self.code, self.params)


class OperationResponse:
    __slots__ = ("code", "return_code", "debug", "params")

    def __init__(self, code, return_code=0, debug=None, params=None):
        self.code, self.return_code, self.debug = code, return_code, debug
        self.params = params if params is not None else {}

    def __repr__(self):
        return "OperationResponse(%d, %d, %r, %r)" % (self.code, self.return_code, self.debug, self.params)


class EventData:
    __slots__ = ("code", "params")

    def __init__(self, code, params=None):
        self.code, self.params = code, params if params is not None else {}

    def __repr__(self):
        return "EventData(%d, %r)" % (self.code, self.params)


# ---------- reading ----------

class Reader:
    __slots__ = ("buf", "pos")

    def __init__(self, buf, pos=0):
        self.buf, self.pos = buf, pos

    def take(self, n):
        if self.pos + n > len(self.buf):
            raise ValueError("truncated: need %d bytes at %d of %d" % (n, self.pos, len(self.buf)))
        b = self.buf[self.pos:self.pos + n]
        self.pos += n
        return b

    def u8(self):
        return self.take(1)[0]

    def i16(self):
        return struct.unpack(">h", self.take(2))[0]

    def i32(self):
        return struct.unpack(">i", self.take(4))[0]

    def value(self, code=None):
        if code is None:
            code = self.u8()
        if code == INTEGER:
            return self.i32()
        if code == STRING:
            return self.take(self.i16() & 0xFFFF).decode("utf-8", "replace")
        if code == BOOLEAN:
            return self.u8() != 0
        if code == BYTE:
            return Byte(self.u8())
        if code == SHORT:
            return Short(self.i16())
        if code == LONG:
            return Long(struct.unpack(">q", self.take(8))[0])
        if code == FLOAT:
            return Float(struct.unpack(">f", self.take(4))[0])
        if code == DOUBLE:
            return struct.unpack(">d", self.take(8))[0]
        if code == HASHTABLE:
            n = self.i16()
            h = Hashtable()
            for _ in range(n):
                k = self.value()
                h[k] = self.value()
            return h
        if code == BYTE_ARRAY:
            return bytes(self.take(self.i32()))
        if code == ARRAY:
            return self.array()
        if code == OBJECT_ARRAY:
            return [self.value() for _ in range(self.i16())]
        if code == DICTIONARY:
            kt, vt = self.u8(), self.u8()
            n = self.i16()
            d = Dictionary(kt, vt)
            for _ in range(n):
                k = self.value(None if kt in (0, NULL) else kt)
                d.items[k] = self.value(None if vt in (0, NULL) else vt)
            return d
        if code == STRING_ARRAY:
            return Array(STRING, [self.value(STRING) for _ in range(self.i16())])
        if code == INT_ARRAY:
            return IntArray(self.i32() for _ in range(self.i32()))
        if code == CUSTOM:
            c = self.u8()
            return Custom(c, self.take(self.i16() & 0xFFFF))
        if code == EVENT_DATA:
            return EventData(self.u8(), self.params())
        if code == OP_REQUEST:
            return OperationRequest(self.u8(), self.params())
        if code == OP_RESPONSE:
            return self.op_response()
        if code in (0, NULL):
            return None
        raise ValueError("unknown type code %d at %d" % (code, self.pos - 1))

    def array(self):
        n = self.i16()
        et = self.u8()
        if et == ARRAY:
            return Array(ARRAY, [self.array() for _ in range(n)])
        if et == BYTE_ARRAY:
            return Array(BYTE_ARRAY, [bytes(self.take(self.i32())) for _ in range(n)])
        if et == CUSTOM:
            c = self.u8()
            return Array(CUSTOM, [bytes(self.take(self.i16() & 0xFFFF)) for _ in range(n)], c)
        if et == DICTIONARY:
            kt, vt = self.u8(), self.u8()
            items = []
            for _ in range(n):
                d = Dictionary(kt, vt)
                for _ in range(self.i16()):
                    k = self.value(None if kt == 0 else kt)
                    d.items[k] = self.value(None if vt == 0 else vt)
                items.append(d)
            a = Array(DICTIONARY, items)
            a.custom = (kt << 8) | vt
            return a
        return Array(et, [_plain(self.value(et)) for _ in range(n)])

    def params(self):
        n = self.i16()
        p = {}
        for _ in range(n):
            k = self.u8()
            p[k] = self.value()
        return p

    def op_response(self):
        code = self.u8()
        rc = self.i16()
        debug = self.value()
        return OperationResponse(code, rc, debug, self.params())


def _plain(v):
    """Array elements carry the array's type; keep them as plain values"""
    if isinstance(v, _TypedInt):
        return int(v)
    if isinstance(v, Float):
        return float(v)
    return v


# ---------- writing ----------

def _code_of(v):
    if v is None:
        return NULL
    t = type(v)
    if t is bool:
        return BOOLEAN
    if t is Byte:
        return BYTE
    if t is Short:
        return SHORT
    if t is Long:
        return LONG
    if t is int:
        return INTEGER
    if t is Float:
        return FLOAT
    if t is float:
        return DOUBLE
    if t is str:
        return STRING
    if t in (bytes, bytearray):
        return BYTE_ARRAY
    if isinstance(v, Hashtable):
        return HASHTABLE
    if t is Array:
        return ARRAY
    if t is IntArray:
        return INT_ARRAY
    if isinstance(v, (list, tuple)):
        return OBJECT_ARRAY
    if t is Dictionary:
        return DICTIONARY
    if t is Custom:
        return CUSTOM
    if t is EventData:
        return EVENT_DATA
    if t is OperationRequest:
        return OP_REQUEST
    if t is OperationResponse:
        return OP_RESPONSE
    if isinstance(v, dict):
        return HASHTABLE
    raise TypeError("cannot serialize %r" % (v,))


def _i16(n):
    return struct.pack(">h", n)


def _i32(n):
    return struct.pack(">i", n)


def write(out, v, set_type=True, code=None):
    if code is None:
        code = _code_of(v)
    if set_type:
        out.append(code)
    if code == NULL:
        return
    if code == INTEGER:
        out += _i32(int(v))
    elif code == STRING:
        b = v.encode("utf-8")
        out += _i16(len(b))
        out += b
    elif code == BOOLEAN:
        out.append(1 if v else 0)
    elif code == BYTE:
        out.append(int(v) & 0xFF)
    elif code == SHORT:
        out += _i16(int(v))
    elif code == LONG:
        out += struct.pack(">q", int(v))
    elif code == FLOAT:
        out += struct.pack(">f", float(v))
    elif code == DOUBLE:
        out += struct.pack(">d", float(v))
    elif code == HASHTABLE:
        out += _i16(len(v))
        for k, x in v.items():
            write(out, k)
            write(out, x)
    elif code == BYTE_ARRAY:
        out += _i32(len(v))
        out += v
    elif code == ARRAY:
        write_array(out, v)
    elif code == OBJECT_ARRAY:
        out += _i16(len(v))
        for x in v:
            write(out, x)
    elif code == DICTIONARY:
        out.append(v.ktype)
        out.append(v.vtype)
        _dict_items(out, v)
    elif code == INT_ARRAY:
        out += _i32(len(v))
        for x in v:
            out += _i32(x)
    elif code == CUSTOM:
        out.append(v.code)
        out += _i16(len(v.data))
        out += v.data
    elif code == EVENT_DATA:
        out.append(v.code)
        write_params(out, v.params)
    elif code == OP_REQUEST:
        out.append(v.code)
        write_params(out, v.params)
    elif code == OP_RESPONSE:
        write_op_response_body(out, v)
    else:
        raise TypeError("cannot serialize type %d" % code)


def _dict_items(out, d):
    out += _i16(len(d.items))
    for k, x in d.items.items():
        write(out, k, d.ktype in (0, NULL), None if d.ktype in (0, NULL) else d.ktype)
        write(out, x, d.vtype in (0, NULL), None if d.vtype in (0, NULL) else d.vtype)


def write_array(out, a):
    out += _i16(len(a.items))
    out.append(a.etype)
    if a.etype == ARRAY:
        for x in a.items:
            write_array(out, x)
    elif a.etype == CUSTOM:
        out.append(a.custom)
        for x in a.items:
            out += _i16(len(x))
            out += x
    elif a.etype == DICTIONARY:
        out.append(a.custom >> 8)
        out.append(a.custom & 0xFF)
        for d in a.items:
            _dict_items(out, d)
    else:
        for x in a.items:
            write(out, x, False, a.etype)


def write_params(out, params):
    out += _i16(len(params))
    for k, v in params.items():
        out.append(k)
        write(out, v)


def write_op_response_body(out, r):
    out.append(r.code)
    out += _i16(r.return_code)
    if r.debug:
        write(out, r.debug)
    else:
        out.append(NULL)
    write_params(out, r.params)


# ---------- messages ----------

MSG_INIT, MSG_INIT_RESPONSE, MSG_OPERATION, MSG_RESPONSE, MSG_EVENT = 0, 1, 2, 3, 4
MSG_INTERNAL_OPERATION, MSG_INTERNAL_RESPONSE = 6, 7


def encode_response(r, internal=False):
    out = bytearray([0xF3, MSG_INTERNAL_RESPONSE if internal else MSG_RESPONSE])
    write_op_response_body(out, r)
    return bytes(out)


def encode_event(code, params):
    out = bytearray([0xF3, MSG_EVENT, code])
    write_params(out, params)
    return bytes(out)


def decode_request(body):
    """body: the message after its two-byte header (decrypted if it was)"""
    r = Reader(body)
    code = r.u8()
    return OperationRequest(code, r.params())


def lobby_text(v):
    """A short, readable rendering for logs"""
    s = repr(v)
    return s if len(s) <= 400 else s[:400] + "..."
