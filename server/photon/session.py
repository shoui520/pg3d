"""
A Photon application on top of the transport: the Init handshake, the
key exchange for encrypted operations, and operation requests, responses
and events.
"""
import logging

from . import protocol16 as p16
from .crypto import KeyExchange

log = logging.getLogger("photon")

INIT_ENCRYPTION = 0  # internal operation
PARAM_CLIENT_KEY = PARAM_SERVER_KEY = 1


class Session:
    """Per-connection state; applications subclass it"""

    def __init__(self, app, peer):
        self.app, self.peer = app, peer
        self.cipher = None
        self.app_id = ""
        self.client_version = ""

    def __repr__(self):
        return repr(self.peer)

    @property
    def connected(self):
        return self.peer.connected

    def respond(self, code, return_code=0, params=None, debug=None, channel=0):
        r = p16.OperationResponse(code, return_code, debug, params or {})
        if return_code:
            log.info("%r: op %d -> %d %s", self, code, return_code, debug or "")
        self.peer.send(p16.encode_response(r), True, channel)

    def event(self, code, params, reliable=True, channel=0):
        self.peer.send(p16.encode_event(code, params), reliable, channel)

    def send_raw(self, message, reliable=True, channel=0):
        """An already encoded message (one event to many peers)"""
        self.peer.send(message, reliable, channel)

    def disconnect(self, reason=1):
        self.peer.transport.drop(self.peer, "disconnected by the server", reason=reason)


class Application:
    """Transport callbacks -> on_request(session, request, channel, reliable)"""

    name = "photon"
    session_class = Session

    def on_connect(self, peer):
        peer.session = self.session_class(self, peer)

    def on_disconnect(self, peer, why):
        if peer.session is not None:
            self.on_leave(peer.session, why)

    def on_leave(self, session, why):
        pass

    def on_message(self, peer, payload, channel, reliable):
        s = peer.session
        if len(payload) < 2 or payload[0] not in (0xF3, 0xFD):
            log.warning("%r: not a Photon message: %s", s, payload[:16].hex())
            return
        kind, encrypted = payload[1] & 0x7F, payload[1] & 0x80
        if kind == p16.MSG_INIT:
            self._init(s, payload, channel)
            return
        if kind not in (p16.MSG_OPERATION, p16.MSG_INTERNAL_OPERATION):
            log.warning("%r: message type %d ignored", s, kind)
            return
        body = payload[2:]
        if encrypted:
            if s.cipher is None:
                log.warning("%r: encrypted operation before the key exchange", s)
                return
            body = s.cipher.decrypt(body)
        req = p16.decode_request(body)
        if kind == p16.MSG_INTERNAL_OPERATION:
            self._internal(s, req, channel)
            return
        if log.isEnabledFor(logging.DEBUG):
            log.debug("%r: op %d %s%s", s, req.code, p16.lobby_text(req.params), " (encrypted)" if encrypted else "")
        self.on_request(s, req, channel, reliable)

    def _init(self, s, payload, channel):
        # 0xF3 0 | protocol 1.6 | sdk id | client version (4 bits+4 bits, b, b) | 0 | app id[32]
        if len(payload) >= 41:
            v = payload[5] & 0x7F
            s.client_version = "%d.%d.%d.%d" % (v >> 4, v & 15, payload[6], payload[7])
            s.app_id = payload[9:41].rstrip(b"\0").decode("ascii", "replace")
        log.info("%r: init, client %s, app %s", s, s.client_version, s.app_id or "-")
        s.peer.send(bytes([0xF3, p16.MSG_INIT_RESPONSE, 0]), True, channel)

    def _internal(self, s, req, channel):
        if req.code == INIT_ENCRYPTION:
            key = req.params.get(PARAM_CLIENT_KEY)
            if not isinstance(key, bytes):
                s.peer.send(p16.encode_response(p16.OperationResponse(req.code, -1, "no client key"), internal=True), True, channel)
                return
            kx = KeyExchange()
            s.cipher = kx.derive(key)
            r = p16.OperationResponse(req.code, 0, None, {PARAM_SERVER_KEY: kx.public_key})
            s.peer.send(p16.encode_response(r, internal=True), True, channel)
        else:
            log.warning("%r: internal operation %d ignored", s, req.code)
            s.peer.send(p16.encode_response(p16.OperationResponse(req.code, -2, "unknown"), internal=True), True, channel)

    def on_request(self, session, req, channel, reliable):
        session.respond(req.code, -2, debug="not supported", channel=channel)
