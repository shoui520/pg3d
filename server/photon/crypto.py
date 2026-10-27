"""
Photon's payload encryption: Diffie-Hellman over the 768-bit Oakley group
(generator 22), then AES-256-CBC with a zero IV and PKCS7 padding, keyed with
SHA-256 of the shared secret. PUN uses it for Authenticate only.

Pure Python: the standard library has no AES, and only a few small messages
per connection go through it.
"""
import hashlib
import secrets

OAKLEY_PRIME_768 = int(
    "FFFFFFFFFFFFFFFFC90FDAA22168C234C4C6628B80DC1CD129024E088A67CC74"
    "020BBEA63B139B22514A08798E3404DDEF9519B3CD3A431B302B0A6DF25F1437"
    "4FE1356D6D51C245E485B576625E7EC6F44C42E9A63A3620FFFFFFFFFFFFFFFF", 16)
GENERATOR = 22


def _to_bytes(n):
    """BigInteger.GetBytes(): big-endian, no leading zeros"""
    return n.to_bytes(max(1, (n.bit_length() + 7) // 8), "big")


class KeyExchange:
    def __init__(self):
        while True:
            self.secret = secrets.randbits(160)
            if 0 < self.secret < OAKLEY_PRIME_768 - 1:
                break
        self.public_key = _to_bytes(pow(GENERATOR, self.secret, OAKLEY_PRIME_768))

    def derive(self, client_public_key):
        shared = pow(int.from_bytes(client_public_key, "big"), self.secret, OAKLEY_PRIME_768)
        return Cipher(hashlib.sha256(_to_bytes(shared)).digest())


# ---------- AES ----------

def _xtime(a):
    a <<= 1
    return (a ^ 0x11B) & 0xFF if a & 0x100 else a


def _mul(a, b):
    r = 0
    while b:
        if b & 1:
            r ^= a
        a = _xtime(a)
        b >>= 1
    return r


def _make_sbox():
    sbox = [0] * 256
    inv = [0] * 256
    for x in range(256):
        # multiplicative inverse in GF(2^8), then the affine transform
        y = 0 if x == 0 else next(c for c in range(1, 256) if _mul(x, c) == 1)
        s = y
        for i in range(1, 5):
            s ^= ((y << i) | (y >> (8 - i))) & 0xFF
        s ^= 0x63
        sbox[x] = s
        inv[s] = x
    return sbox, inv


SBOX, INV_SBOX = _make_sbox()
_MUL = {c: [_mul(x, c) for x in range(256)] for c in (2, 3, 9, 11, 13, 14)}


def _expand_key(key):
    nk = len(key) // 4
    rounds = nk + 6
    w = [list(key[4 * i:4 * i + 4]) for i in range(nk)]
    rcon = 1
    for i in range(nk, 4 * (rounds + 1)):
        t = list(w[i - 1])
        if i % nk == 0:
            t = t[1:] + t[:1]
            t = [SBOX[b] for b in t]
            t[0] ^= rcon
            rcon = _xtime(rcon)
        elif nk > 6 and i % nk == 4:
            t = [SBOX[b] for b in t]
        w.append([a ^ b for a, b in zip(w[i - nk], t)])
    return [sum(w[4 * r:4 * r + 4], []) for r in range(rounds + 1)]


def _encrypt_block(rk, block):
    s = [b ^ k for b, k in zip(block, rk[0])]
    m2, m3 = _MUL[2], _MUL[3]
    for r in range(1, len(rk)):
        s = [SBOX[b] for b in s]
        s = [s[(i + 4 * (i % 4)) % 16] for i in range(16)]  # ShiftRows (column-major state)
        if r != len(rk) - 1:
            t = []
            for c in range(4):
                a0, a1, a2, a3 = s[4 * c:4 * c + 4]
                t += [m2[a0] ^ m3[a1] ^ a2 ^ a3, a0 ^ m2[a1] ^ m3[a2] ^ a3,
                      a0 ^ a1 ^ m2[a2] ^ m3[a3], m3[a0] ^ a1 ^ a2 ^ m2[a3]]
            s = t
        s = [b ^ k for b, k in zip(s, rk[r])]
    return bytes(s)


def _decrypt_block(rk, block):
    s = [b ^ k for b, k in zip(block, rk[-1])]
    m9, m11, m13, m14 = _MUL[9], _MUL[11], _MUL[13], _MUL[14]
    for r in range(len(rk) - 2, -1, -1):
        s = [s[(i - 4 * (i % 4)) % 16] for i in range(16)]  # InvShiftRows
        s = [INV_SBOX[b] for b in s]
        s = [b ^ k for b, k in zip(s, rk[r])]
        if r != 0:
            t = []
            for c in range(4):
                a0, a1, a2, a3 = s[4 * c:4 * c + 4]
                t += [m14[a0] ^ m11[a1] ^ m13[a2] ^ m9[a3], m9[a0] ^ m14[a1] ^ m11[a2] ^ m13[a3],
                      m13[a0] ^ m9[a1] ^ m14[a2] ^ m11[a3], m11[a0] ^ m13[a1] ^ m9[a2] ^ m14[a3]]
            s = t
    return bytes(s)


class Cipher:
    """AES-CBC, zero IV, PKCS7 (RijndaelManaged as the client sets it up)"""

    def __init__(self, key):
        self.rk = _expand_key(key)

    def encrypt(self, data):
        n = 16 - len(data) % 16
        data = bytes(data) + bytes([n]) * n
        prev = bytes(16)
        out = bytearray()
        for i in range(0, len(data), 16):
            prev = _encrypt_block(self.rk, bytes(a ^ b for a, b in zip(data[i:i + 16], prev)))
            out += prev
        return bytes(out)

    def decrypt(self, data):
        if not data or len(data) % 16:
            raise ValueError("ciphertext length %d" % len(data))
        prev = bytes(16)
        out = bytearray()
        for i in range(0, len(data), 16):
            block = bytes(data[i:i + 16])
            out += bytes(a ^ b for a, b in zip(_decrypt_block(self.rk, block), prev))
            prev = block
        n = out[-1]
        if not 1 <= n <= 16 or out[-n:] != bytes([n]) * n:
            raise ValueError("bad padding")
        return bytes(out[:-n])
