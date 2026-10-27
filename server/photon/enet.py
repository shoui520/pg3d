"""
Photon's reliable UDP ("eNet"), server side, as Photon3Unity3D 4.1 expects it.

Datagram: peerID i16 | crc flag u8 (0, or 0xCC with a CRC32 after the header)
          | command count u8 | sent time i32 | challenge i32 | commands
Command:  type u8 | channel u8 | flags u8 (1 reliable) | reserved u8
          | size i32 (header included) | reliable seq i32 | ...
          unreliable (7): + unreliable seq i32; fragment (8): + start seq,
          fragment count, fragment number, total length, fragment offset.

All numbers are big-endian. Control commands (connect, verify, disconnect,
ping, server time) travel on channel 255 and act at once; messages keep
per-channel order: reliable ones by sequence number, unreliable ones after
the reliable command they follow.

The client acks with the sent time from our header and derives the server
clock (PhotonNetwork.time) from it, so every transport stamps the same clock.
"""
import asyncio
import logging
import random
import struct
import time

CT_ACK, CT_CONNECT, CT_VERIFY_CONNECT, CT_DISCONNECT, CT_PING = 1, 2, 3, 4, 5
CT_RELIABLE, CT_UNRELIABLE, CT_FRAGMENT, CT_SERVER_TIME = 6, 7, 8, 12
CONTROL = 255
FLAG_RELIABLE = 1

HEADER = 12
DEFAULT_MTU = 1200
PEER_TIMEOUT = 30.0      # s without a datagram (a Vita can take 20 s to load a level)
RESEND_LIMIT = 30.0      # s a reliable command may stay unacknowledged
PING_INTERVAL = 1.0      # s of quiet before we ping
MAX_RTO = 2.0

log = logging.getLogger("enet")
_epoch = time.monotonic()


def now_ms():
    """The server clock, shared by every transport (int32, wraps)"""
    t = int((time.monotonic() - _epoch) * 1000) & 0xFFFFFFFF
    return t - (1 << 32) if t >= 1 << 31 else t


class Command:
    __slots__ = ("type", "channel", "flags", "reserved", "seq", "useq", "payload",
                 "start", "count", "number", "total", "offset",
                 "ack_seq", "ack_time", "sent", "first_sent", "rto")

    def __init__(self, type, channel, flags=FLAG_RELIABLE, payload=b"", seq=0):
        self.type, self.channel, self.flags, self.reserved = type, channel, flags, 4
        self.seq, self.useq, self.payload = seq, 0, payload
        self.start = self.count = self.number = self.total = self.offset = 0
        self.ack_seq = self.ack_time = 0
        self.sent = self.first_sent = 0.0
        self.rto = 0.0

    def encode(self):
        if self.type == CT_UNRELIABLE:
            extra = struct.pack(">i", self.useq)
        elif self.type == CT_FRAGMENT:
            extra = struct.pack(">5i", self.start, self.count, self.number, self.total, self.offset)
        elif self.type == CT_ACK:
            extra = struct.pack(">ii", self.ack_seq, self.ack_time)
        else:
            extra = b""
        size = 12 + len(extra) + len(self.payload)
        return struct.pack(">BBBBii", self.type, self.channel, self.flags, self.reserved, size, self.seq) + extra + self.payload

    def size(self):
        return 12 + len(self.payload) + (4 if self.type == CT_UNRELIABLE else 20 if self.type == CT_FRAGMENT
                                         else 8 if self.type == CT_ACK else 0)


def parse_commands(data, off, count):
    cmds = []
    for _ in range(count):
        if off + 12 > len(data):
            raise ValueError("command header past the datagram")
        t, ch, fl, rs, size, seq = struct.unpack_from(">BBBBii", data, off)
        if size < 12 or off + size > len(data):
            raise ValueError("command size %d at %d of %d" % (size, off, len(data)))
        c = Command(t, ch, fl, seq=seq)
        c.reserved = rs
        body = off + 12
        if t == CT_ACK:
            c.ack_seq, c.ack_time = struct.unpack_from(">ii", data, body)
        elif t == CT_UNRELIABLE:
            c.useq = struct.unpack_from(">i", data, body)[0]
            c.payload = bytes(data[body + 4:off + size])
        elif t == CT_FRAGMENT:
            c.start, c.count, c.number, c.total, c.offset = struct.unpack_from(">5i", data, body)
            c.payload = bytes(data[body + 20:off + size])
        else:
            c.payload = bytes(data[body:off + size])
        cmds.append(c)
        off += size
    return cmds


class Channel:
    __slots__ = ("out_rel", "out_unrel", "in_rel", "in_unrel", "rel", "unrel")

    def __init__(self):
        self.out_rel = self.out_unrel = 0
        self.in_rel = self.in_unrel = 0
        self.rel = {}     # seq -> Command waiting for its turn
        self.unrel = []   # unreliable commands waiting for a reliable one


class Peer:
    """One client connection"""

    def __init__(self, transport, addr, peer_id, challenge, mtu):
        self.transport, self.addr, self.id, self.challenge = transport, addr, peer_id, challenge
        self.mtu = max(576, min(mtu or DEFAULT_MTU, DEFAULT_MTU))
        self.channels = {}
        self.acks = []
        self.out = []          # commands ready to go, in order
        self.unacked = {}      # (channel, seq) -> Command
        self.rtt, self.rttvar = 0.2, 0.05
        self.last_recv = self.last_send = time.monotonic()
        self.connected = True
        self.session = None    # the application's per-connection state

    def __repr__(self):
        return "<peer %d %s:%d>" % (self.id, self.addr[0], self.addr[1])

    def channel(self, n):
        c = self.channels.get(n)
        if c is None:
            c = self.channels[n] = Channel()
        return c

    # ---- sending ----

    def _queue_reliable(self, cmd):
        ch = self.channel(cmd.channel)
        ch.out_rel += 1
        cmd.seq = ch.out_rel
        self.out.append(cmd)

    def send(self, payload, reliable=True, channel=0):
        frag = self.mtu - HEADER - 36
        if len(payload) > frag:
            ch = self.channel(channel)
            count = (len(payload) + frag - 1) // frag
            start = ch.out_rel + 1
            for i in range(count):
                c = Command(CT_FRAGMENT, channel, payload=payload[i * frag:(i + 1) * frag])
                c.start, c.count, c.number, c.total, c.offset = start, count, i, len(payload), i * frag
                self._queue_reliable(c)
        elif reliable:
            self._queue_reliable(Command(CT_RELIABLE, channel, payload=payload))
        else:
            ch = self.channel(channel)
            c = Command(CT_UNRELIABLE, channel, flags=0, payload=payload, seq=ch.out_rel)
            ch.out_unrel += 1
            c.useq = ch.out_unrel
            self.out.append(c)
        self.transport.dirty.add(self)

    def send_control(self, type, payload=b"", flags=FLAG_RELIABLE, reserved=4):
        c = Command(type, CONTROL, flags, payload)
        c.reserved = reserved
        if flags & FLAG_RELIABLE:
            self._queue_reliable(c)
        else:
            self.out.append(c)
        self.transport.dirty.add(self)

    def flush(self):
        now = time.monotonic()
        cmds = []
        for ch, seq, sent_time in self.acks:
            a = Command(CT_ACK, ch, flags=0)
            a.ack_seq, a.ack_time = seq, sent_time
            cmds.append(a)
        self.acks = []
        cmds += self.out
        self.out = []
        datagram, n = [], 0
        size = HEADER
        for c in cmds:
            s = c.size()
            if datagram and (size + s > self.mtu or n >= 64):
                self._emit(datagram)
                datagram, n, size = [], 0, HEADER
            datagram.append(c)
            n += 1
            size += s
            if c.flags & FLAG_RELIABLE and c.type != CT_ACK:
                if not c.first_sent:
                    c.first_sent = now
                    c.rto = min(max(self.rtt + 4 * self.rttvar, 0.05), MAX_RTO)
                c.sent = now
                self.unacked[(c.channel, c.seq)] = c
        if datagram:
            self._emit(datagram)

    def _emit(self, cmds):
        self.last_send = time.monotonic()
        body = b"".join(c.encode() for c in cmds)
        head = struct.pack(">hBBii", self.id, 0, len(cmds), now_ms(), self.challenge)
        self.transport.sendto(head + body, self.addr)

    def resend(self, now):
        """Reliable commands past their timeout go out again"""
        due = []
        for key, c in list(self.unacked.items()):
            if now - c.sent < c.rto:
                continue
            if now - c.first_sent > RESEND_LIMIT:
                return False
            c.rto = min(c.rto * 2, MAX_RTO)
            del self.unacked[key]
            due.append(c)
        if due:
            due.sort(key=lambda c: (c.channel, c.seq))
            self.out = due + self.out
            self.transport.dirty.add(self)
        return True

    # ---- receiving ----

    def _ack(self, c):
        u = self.unacked.pop((c.channel, c.ack_seq), None)
        if u is None:
            return
        sample = ((now_ms() - c.ack_time) & 0xFFFFFFFF) / 1000.0
        if sample < 10 and u.sent == u.first_sent:  # only the first send times a round trip
            self.rttvar = 0.75 * self.rttvar + 0.25 * abs(sample - self.rtt)
            self.rtt = 0.875 * self.rtt + 0.125 * sample

    def receive(self, cmds, sent_time):
        """Commands of one datagram; returns messages ready for the
        application as (payload, channel, reliable)"""
        self.last_recv = time.monotonic()
        ready = []
        for c in cmds:
            if c.flags & FLAG_RELIABLE and c.type != CT_ACK:
                self.acks.append((c.channel, c.seq, sent_time))
                self.transport.dirty.add(self)
            t = c.type
            if t == CT_ACK:
                self._ack(c)
            elif t == CT_DISCONNECT:
                self.transport.drop(self, "client disconnected", notify_client=False)
                return ready
            elif t in (CT_PING, CT_SERVER_TIME, CT_CONNECT, CT_VERIFY_CONNECT):
                pass  # the ack (stamped with our clock) is the answer
            elif t in (CT_RELIABLE, CT_FRAGMENT):
                self._queue_in(c, ready)
            elif t == CT_UNRELIABLE:
                ch = self.channel(c.channel)
                if c.useq <= ch.in_unrel:
                    continue
                if c.seq <= ch.in_rel:
                    ch.in_unrel = c.useq
                    ready.append((c.payload, c.channel, False))
                elif len(ch.unrel) < 256:
                    ch.unrel.append(c)
            else:
                log.debug("%r: command type %d ignored", self, t)
        return ready

    def _queue_in(self, c, ready):
        ch = self.channel(c.channel)
        if c.seq <= ch.in_rel or c.seq in ch.rel:
            return  # a resend of something we have
        ch.rel[c.seq] = c
        while True:
            nxt = ch.rel.get(ch.in_rel + 1)
            if nxt is None:
                break
            if nxt.type == CT_FRAGMENT:
                if nxt.seq != nxt.start:
                    log.warning("%r: fragment %d out of place", self, nxt.seq)
                    del ch.rel[nxt.seq]
                    ch.in_rel += 1
                    continue
                seqs = range(nxt.start, nxt.start + nxt.count)
                if not all(s in ch.rel for s in seqs):
                    break
                buf = bytearray(nxt.total)
                for s in seqs:
                    f = ch.rel.pop(s)
                    buf[f.offset:f.offset + len(f.payload)] = f.payload
                ch.in_rel = nxt.start + nxt.count - 1
                ready.append((bytes(buf), c.channel, True))
            else:
                del ch.rel[nxt.seq]
                ch.in_rel = nxt.seq
                ready.append((nxt.payload, c.channel, True))
            # unreliable commands that were waiting for this one
            if ch.unrel:
                waiting, ch.unrel = ch.unrel, []
                for u in sorted(waiting, key=lambda u: u.useq):
                    if u.seq <= ch.in_rel:
                        if u.useq > ch.in_unrel:
                            ch.in_unrel = u.useq
                            ready.append((u.payload, u.channel, False))
                    else:
                        ch.unrel.append(u)


class Transport(asyncio.DatagramProtocol):
    """A UDP port with its peers. `app` gets on_connect(peer),
    on_message(peer, payload, channel, reliable) and on_disconnect(peer, why)."""

    def __init__(self, app, name):
        self.app, self.name = app, name
        self.peers = {}
        self.dirty = set()
        self.sock = None
        self.next_id = random.randint(1, 20000)

    def connection_made(self, transport):
        self.sock = transport

    def sendto(self, data, addr):
        if self.sock:
            self.sock.sendto(data, addr)

    def error_received(self, exc):
        log.debug("%s: %s", self.name, exc)

    def datagram_received(self, data, addr):
        try:
            self._datagram(data, addr)
        except Exception:
            log.exception("%s: datagram from %s:%d (%d bytes): %s", self.name, addr[0], addr[1], len(data), data[:64].hex())
        self.flush()

    def _datagram(self, data, addr):
        if len(data) < HEADER:
            return
        peer_id, crc, count, sent_time, challenge = struct.unpack_from(">hBBii", data, 0)
        off = HEADER
        if crc == 0xCC:
            off += 4  # CRC not checked: UDP has its own
        elif crc == 1:
            log.warning("%s: %s:%d sent an encrypted datagram; not supported", self.name, *addr)
            return
        cmds = parse_commands(data, off, count)
        peer = self.peers.get(addr)
        if peer is not None and peer.challenge != challenge:
            if any(c.type == CT_CONNECT for c in cmds):
                self.drop(peer, "reconnected", notify_client=False)
                peer = None
            else:
                return
        if peer is None:
            connect = next((c for c in cmds if c.type == CT_CONNECT), None)
            if connect is None:
                return
            mtu = struct.unpack_from(">h", connect.payload, 2)[0] if len(connect.payload) >= 4 else DEFAULT_MTU
            self.next_id = self.next_id % 30000 + 1
            peer = Peer(self, addr, self.next_id, challenge, mtu)
            self.peers[addr] = peer
            # VerifyConnect: our peer ID, then 30 bytes the client skips
            peer.send_control(CT_VERIFY_CONNECT, struct.pack(">h", peer.id) + bytes(30))
            log.info("%s: %r connected (mtu %d)", self.name, peer, peer.mtu)
            self.app.on_connect(peer)
        for payload, channel, reliable in peer.receive(cmds, sent_time):
            if not peer.connected:
                break
            try:
                self.app.on_message(peer, payload, channel, reliable)
            except Exception:
                log.exception("%s: %r message on channel %d: %s", self.name, peer, channel, payload[:64].hex())

    def drop(self, peer, why, notify_client=True, reason=0):
        if not peer.connected:
            return
        if notify_client:
            peer.send_control(CT_DISCONNECT, reserved=reason)
            peer.flush()
        peer.connected = False
        self.peers.pop(peer.addr, None)
        self.dirty.discard(peer)
        log.info("%s: %r gone: %s", self.name, peer, why)
        try:
            self.app.on_disconnect(peer, why)
        except Exception:
            log.exception("%s: on_disconnect %r", self.name, peer)

    def flush(self):
        dirty, self.dirty = self.dirty, set()
        for p in dirty:
            if p.connected:
                p.flush()

    def tick(self):
        now = time.monotonic()
        for p in list(self.peers.values()):
            if now - p.last_recv > PEER_TIMEOUT:
                self.drop(p, "timed out", notify_client=False)
            elif not p.resend(now):
                self.drop(p, "reliable commands unacknowledged", notify_client=False)
            elif now - p.last_send > PING_INTERVAL and not p.out:
                # what a Photon server does: while PUN loads a level its main
                # loop is stalled, but a background thread still sends the acks
                # these ask for, so both sides know the other is there
                p.send_control(CT_PING)
        self.flush()
