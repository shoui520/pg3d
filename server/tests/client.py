"""
A minimal Photon client for testing the server: connect, Init, optional key
exchange, operations, responses and events. Blocking, one socket, no resends
(fine on localhost).
"""
import random
import socket
import struct
import time
from collections import defaultdict, deque

from photon import enet, protocol16 as p16
from photon.crypto import KeyExchange
from photon.enet import Command, parse_commands

INIT_ENCRYPTION = 0


class Client:
    def __init__(self, addr, app_id="test-app", timeout=2.0):
        self.addr = addr
        self.app_id = app_id
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.settimeout(0.05)
        self.timeout = timeout
        self.peer_id = -1
        self.challenge = random.randint(1, 1 << 30)
        self.out_rel = defaultdict(int)
        self.out_unrel = defaultdict(int)
        self.in_rel = defaultdict(int)
        self.held = defaultdict(dict)
        self.inbox = deque()
        self.cipher = None
        self.connected = False
        self.disconnected = False
        self.server_time = None
        self.pings = 0

    # ---- transport ----

    def _send(self, cmds):
        head = struct.pack(">hBBii", self.peer_id, 0, len(cmds), int(time.monotonic() * 1000) & 0x7FFFFFFF, self.challenge)
        self.sock.sendto(head + b"".join(c.encode() for c in cmds), self.addr)

    def _reliable(self, channel, payload, type=enet.CT_RELIABLE):
        self.out_rel[channel] += 1
        return Command(type, channel, payload=payload, seq=self.out_rel[channel])

    def send_message(self, payload, channel=0, reliable=True):
        if reliable:
            self._send([self._reliable(channel, payload)])
        else:
            c = Command(enet.CT_UNRELIABLE, channel, flags=0, payload=payload, seq=self.out_rel[channel])
            self.out_unrel[channel] += 1
            c.useq = self.out_unrel[channel]
            self._send([c])

    def pump(self, duration=0.0):
        end = time.monotonic() + duration
        while True:
            try:
                data, _ = self.sock.recvfrom(65536)
            except socket.timeout:
                if time.monotonic() >= end:
                    return
                continue
            self._datagram(data)

    def _datagram(self, data):
        _, crc, count, sent_time, _ = struct.unpack_from(">hBBii", data, 0)
        self.server_time = sent_time
        acks = []
        for c in parse_commands(data, 12, count):
            if c.flags & 1 and c.type != enet.CT_ACK:
                a = Command(enet.CT_ACK, c.channel, flags=0)
                a.ack_seq, a.ack_time = c.seq, sent_time
                acks.append(a)
            if c.type == enet.CT_VERIFY_CONNECT:
                self.peer_id = struct.unpack_from(">h", c.payload, 0)[0]
                self.connected = True
            elif c.type == enet.CT_PING:
                self.pings += 1
            elif c.type == enet.CT_DISCONNECT:
                self.disconnected = True
            elif c.type in (enet.CT_RELIABLE, enet.CT_FRAGMENT):
                self.held[c.channel][c.seq] = c
                self._release(c.channel)
            elif c.type == enet.CT_UNRELIABLE:
                self._deliver(c.payload)
        if acks:
            self._send(acks)

    def _release(self, ch):
        held = self.held[ch]
        while True:
            nxt = held.get(self.in_rel[ch] + 1)
            if nxt is None:
                return
            if nxt.type == enet.CT_FRAGMENT:
                seqs = range(nxt.start, nxt.start + nxt.count)
                if not all(s in held for s in seqs):
                    return
                buf = bytearray(nxt.total)
                for s in seqs:
                    f = held.pop(s)
                    buf[f.offset:f.offset + len(f.payload)] = f.payload
                self.in_rel[ch] = nxt.start + nxt.count - 1
                self._deliver(bytes(buf))
            else:
                del held[nxt.seq]
                self.in_rel[ch] = nxt.seq
                self._deliver(nxt.payload)

    def _deliver(self, payload):
        kind = payload[1] & 0x7F
        r = p16.Reader(payload, 2)
        if kind == p16.MSG_INIT_RESPONSE:
            self.inbox.append(("init", None))
        elif kind in (p16.MSG_RESPONSE, p16.MSG_INTERNAL_RESPONSE):
            self.inbox.append(("response", r.op_response()))
        elif kind == p16.MSG_EVENT:
            code = r.u8()
            self.inbox.append(("event", p16.EventData(code, r.params())))

    def wait(self, pred, what):
        end = time.monotonic() + self.timeout
        while True:
            for i, m in enumerate(self.inbox):
                if pred(m):
                    del self.inbox[i]
                    return m[1]
            if time.monotonic() > end:
                raise TimeoutError("no %s; inbox: %r" % (what, list(self.inbox)))
            self.pump(0.02)

    def response(self, code):
        return self.wait(lambda m: m[0] == "response" and m[1].code == code, "response %d" % code)

    def event(self, code):
        return self.wait(lambda m: m[0] == "event" and m[1].code == code, "event %d" % code)

    def events(self, code):
        out = [m[1] for m in self.inbox if m[0] == "event" and m[1].code == code]
        self.inbox = deque(m for m in self.inbox if not (m[0] == "event" and m[1].code == code))
        return out

    # ---- Photon ----

    def connect(self):
        payload = bytearray(32)
        struct.pack_into(">h", payload, 2, 1200)
        self._send([self._reliable(enet.CONTROL, bytes(payload), enet.CT_CONNECT)])
        end = time.monotonic() + self.timeout
        while not self.connected:
            if time.monotonic() > end:
                raise TimeoutError("no VerifyConnect")
            self.pump(0.02)
        init = bytes([0xF3, 0, 1, 6, 1, 4, 1, 1, 0]) + self.app_id.encode().ljust(32, b"\0")
        self.send_message(init)
        self.wait(lambda m: m[0] == "init", "init response")

    def exchange_keys(self):
        kx = KeyExchange()
        body = bytearray([INIT_ENCRYPTION])
        p16.write_params(body, {1: kx.public_key})
        self.send_message(bytes([0xF3, p16.MSG_INTERNAL_OPERATION]) + bytes(body))
        r = self.response(INIT_ENCRYPTION)
        self.cipher = kx.derive(r.params[1])

    def op(self, code, params=None, encrypted=False, channel=0, reliable=True):
        body = bytearray([code])
        p16.write_params(body, params or {})
        if encrypted:
            self.send_message(bytes([0xF3, p16.MSG_OPERATION | 0x80]) + self.cipher.encrypt(bytes(body)), channel)
        else:
            self.send_message(bytes([0xF3, p16.MSG_OPERATION]) + bytes(body), channel, reliable)

    def call(self, code, params=None, encrypted=False):
        self.op(code, params, encrypted)
        return self.response(code)

    def disconnect(self):
        self._send([self._reliable(enet.CONTROL, b"", enet.CT_DISCONNECT)])
        self.sock.close()
