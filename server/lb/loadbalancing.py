"""
Photon LoadBalancing, as PUN 1.79 uses it: a master server that
authenticates, keeps lobbies and does matchmaking, and a game server that
hosts the rooms. One process serves both, so a room the master hands out is
the room the client then joins on the game server.

Master: Authenticate, JoinLobby/LeaveLobby, CreateGame/JoinGame/JoinRandomGame
(answering with the game server's address), FindFriends, GetGameList; game
lists for default lobbies, app stats.

Game server: Authenticate (with the master's token), CreateGame/JoinGame,
Leave, RaiseEvent (receivers, target actors, interest groups, the room's
event cache), SetProperties (with expected values), GetProperties,
ChangeGroups. The master client is kept on the server (game property 248).
"""
import logging
import random
import secrets
import time
import uuid

from photon import protocol16 as p16
from photon.protocol16 import Array, Byte, Hashtable, BOOLEAN, INTEGER, STRING
from photon.session import Application, Session
from .sqlfilter import compile_filter, evaluate

log = logging.getLogger("lb")

# operations
OP_AUTHENTICATE, OP_AUTHENTICATE_ONCE, OP_JOIN_LOBBY, OP_LEAVE_LOBBY = 230, 231, 229, 228
OP_CREATE_GAME, OP_JOIN_GAME, OP_JOIN_RANDOM = 227, 226, 225
OP_LEAVE, OP_RAISE_EVENT, OP_SET_PROPERTIES, OP_GET_PROPERTIES, OP_CHANGE_GROUPS = 254, 253, 252, 251, 248
OP_FIND_FRIENDS, OP_GET_LOBBY_STATS, OP_GET_REGIONS, OP_WEB_RPC, OP_SETTINGS, OP_GET_GAME_LIST = 222, 221, 220, 219, 218, 217

# parameters
P_ROOM_NAME, P_ACTOR_NR, P_TARGET_ACTOR, P_ACTOR_LIST, P_PROPERTIES = 255, 254, 253, 252, 251
P_BROADCAST, P_PLAYER_PROPS, P_GAME_PROPS, P_CACHE, P_RECEIVERS = 250, 249, 248, 247, 246
P_DATA, P_CODE, P_CLEANUP, P_GROUP, P_REMOVE, P_PUBLISH_USER_ID, P_ADD = 245, 244, 241, 240, 239, 239, 238
P_SUPPRESS, P_EXPECTED, P_ADDRESS = 237, 231, 230
P_PEER_COUNT, P_GAME_COUNT, P_MASTER_PEER_COUNT = 229, 228, 227
P_USER_ID, P_APP_ID, P_MATCHING, P_GAME_LIST, P_SECRET, P_APP_VERSION = 225, 224, 223, 222, 221, 220
P_JOIN_MODE, P_LOBBY_NAME, P_LOBBY_TYPE, P_MASTER_CLIENT_ID = 215, 213, 212, 203
P_FRIENDS = 1
P_FRIENDS_ONLINE, P_FRIENDS_ROOMS = 1, 2

# events
EV_GAME_LIST, EV_GAME_LIST_UPDATE, EV_APP_STATS = 230, 229, 226
EV_JOIN, EV_LEAVE, EV_PROPERTIES_CHANGED = 255, 254, 253

# game and actor properties
G_MAX_PLAYERS, G_VISIBLE, G_OPEN, G_PLAYER_COUNT = Byte(255), Byte(254), Byte(253), Byte(252)
G_REMOVED, G_LOBBY_PROPS, G_CLEANUP, G_MASTER = Byte(251), Byte(250), Byte(249), Byte(248)
A_USER_ID = Byte(253)

# return codes
E_NOT_ALLOWED, E_INVALID, E_TICKET_EXPIRED = -3, -2, 32753
E_GAME_EXISTS, E_GAME_FULL, E_GAME_CLOSED, E_NO_MATCH, E_NO_GAME = 32766, 32765, 32764, 32760, 32758

# EventCaching
CACHE_NONE, CACHE_ADD, CACHE_ADD_GLOBAL, CACHE_REMOVE, CACHE_REMOVE_LEFT = 0, 4, 5, 6, 7
# ReceiverGroup
TO_OTHERS, TO_ALL, TO_MASTER = 0, 1, 2
LOBBY_DEFAULT, LOBBY_SQL, LOBBY_ASYNC = 0, 2, 3
JOIN_CREATE_IF_NOT_EXISTS = 1

DEFAULT_LOBBY = ("", LOBBY_DEFAULT)
RESERVATION = 30.0  # s a room the master handed out waits for its creator


def _num(v):
    return not isinstance(v, bool) and isinstance(v, (int, float))


def same(a, b):
    """Equality for matchmaking: numbers by value whatever their wire type"""
    if _num(a) and _num(b):
        return float(a) == float(b)
    return a == b


def _lobby_of(params, fallback=DEFAULT_LOBBY):
    if P_LOBBY_NAME in params or P_LOBBY_TYPE in params:
        return (params.get(P_LOBBY_NAME) or "", int(params.get(P_LOBBY_TYPE) or 0))
    return fallback


class LbSession(Session):
    def __init__(self, app, peer):
        super().__init__(app, peer)
        self.authenticated = False
        self.user_id = ""
        self.app_version = ""
        self.lobby = None
        self.room = None
        self.actor = None


class CachedEvent:
    __slots__ = ("actor", "code", "data", "message", "global_")

    def __init__(self, actor, code, data, message, global_):
        self.actor, self.code, self.data, self.message, self.global_ = actor, code, data, message, global_


class Actor:
    def __init__(self, nr, session, props):
        self.nr, self.session, self.props = nr, session, props
        self.groups = set()
        self.all_groups = False

    def in_group(self, g):
        return self.all_groups or g in self.groups


class Room:
    def __init__(self, world, version, name, lobby):
        self.world, self.version, self.name, self.lobby = world, version, name, lobby
        self.props = Hashtable({G_OPEN: True, G_VISIBLE: True})
        self.actors = {}
        self.next_actor = 1
        self.master_id = 0
        self.cache = []
        self.cleanup = True
        self.suppress = False
        self.publish_user_id = False
        self.created = False
        self.since = time.monotonic()

    def __repr__(self):
        return "<room %r %d/%d>" % (self.name, len(self.actors), self.max_players)

    @property
    def max_players(self):
        return int(self.props.get(G_MAX_PLAYERS) or 0)

    @property
    def is_open(self):
        return self.props.get(G_OPEN, True) is not False

    @property
    def visible(self):
        return self.props.get(G_VISIBLE, True) is not False

    @property
    def full(self):
        return self.max_players > 0 and len(self.actors) >= self.max_players

    def actor_list(self):
        return Array(INTEGER, list(self.actors))

    def listing(self):
        """What a lobby shows of the room"""
        h = Hashtable()
        listed = self.props.get(G_LOBBY_PROPS)
        for k in (listed.items if isinstance(listed, Array) else ()):
            if k in self.props:
                h[k] = self.props[k]
        h[G_MAX_PLAYERS] = Byte(self.max_players)
        h[G_OPEN] = self.is_open
        h[G_PLAYER_COUNT] = Byte(len(self.actors))
        return h

    def broadcast(self, message, actors, reliable=True, channel=0):
        for a in actors:
            if a.session.connected:
                a.session.send_raw(message, reliable, channel)


class World:
    def __init__(self, game_address):
        self.game_address = game_address
        self.rooms = {}     # (app version, name) -> Room
        self.tokens = {}    # token -> (app id, app version, user id)
        self.master = self.game = None
        self._filters = {}
        self._listings = {}  # (app version, lobby) -> {room name: listing}

    def sql(self, text):
        f = self._filters.get(text)
        if f is None:
            f = compile_filter(text)
            if len(self._filters) > 500:
                self._filters.clear()
            self._filters[text] = f
        return f

    def remove_room(self, room):
        if self.rooms.get((room.version, room.name)) is room:
            del self.rooms[(room.version, room.name)]
            log.info("%r closed", room)

    def sessions(self):
        for app in (self.master, self.game):
            for p in list(app.transport.peers.values()):
                if p.session is not None:
                    yield app, p.session

    # ---- periodic ----

    def tick_lobbies(self):
        """Game lists for the default lobbies (SQL lobbies don't get any)"""
        members = {}
        for p in self.master.transport.peers.values():
            s = p.session
            if s and s.lobby and s.lobby[1] in (LOBBY_DEFAULT, LOBBY_ASYNC):
                members.setdefault((s.app_version, s.lobby), []).append(s)
        for key in set(members) | set(self._listings):
            now = self.listing(*key)
            before = self._listings.get(key, {})
            changes = Hashtable()
            for name, h in now.items():
                if before.get(name) != h:
                    changes[name] = h
            for name in before:
                if name not in now:
                    changes[name] = Hashtable({G_REMOVED: True})
            if key in members:
                self._listings[key] = now
            else:
                self._listings.pop(key, None)
            if changes and key in members:
                ev = p16.encode_event(EV_GAME_LIST_UPDATE, {P_GAME_LIST: changes})
                for s in members[key]:
                    s.send_raw(ev)

    def listing(self, version, lobby):
        return {r.name: r.listing() for r in self.rooms.values()
                if r.version == version and r.lobby == lobby and r.created and r.visible}

    def tick_stats(self):
        in_rooms = sum(len(r.actors) for r in self.rooms.values())
        on_master = len(self.master.transport.peers)
        games = sum(1 for r in self.rooms.values() if r.created)
        ev = p16.encode_event(EV_APP_STATS, {P_PEER_COUNT: in_rooms, P_MASTER_PEER_COUNT: on_master, P_GAME_COUNT: games})
        for p in list(self.master.transport.peers.values()):
            if p.session and p.session.authenticated:
                p.session.send_raw(ev)
        now = time.monotonic()
        for r in list(self.rooms.values()):
            if not r.created and not r.actors and now - r.since > RESERVATION:
                self.remove_room(r)


class _Lb(Application):
    session_class = LbSession

    def __init__(self, world):
        self.world = world
        self.transport = None

    def authenticate(self, s, req, channel):
        p = req.params
        token = p.get(P_SECRET)
        if isinstance(token, str) and token:
            info = self.world.tokens.get(token)
            if info is None:
                s.respond(req.code, E_TICKET_EXPIRED, debug="unknown token (server restarted?)", channel=channel)
                return
            s.app_id, s.app_version, s.user_id = info
        else:
            s.app_version = str(p.get(P_APP_VERSION) or "")
            s.app_id = str(p.get(P_APP_ID) or s.app_id)
            s.user_id = str(p.get(P_USER_ID) or "") or str(uuid.uuid4())
            token = secrets.token_hex(16)
            self.world.tokens[token] = (s.app_id, s.app_version, s.user_id)
        s.authenticated = True
        log.info("%s %r: authenticated as %s, version %r", self.name, s, s.user_id, s.app_version)
        s.respond(req.code, 0, {P_USER_ID: s.user_id, P_SECRET: token}, channel=channel)
        self.after_auth(s)

    def after_auth(self, s):
        pass

    def on_request(self, s, req, channel, reliable):
        if req.code in (OP_AUTHENTICATE, OP_AUTHENTICATE_ONCE):
            self.authenticate(s, req, channel)
            return
        if not s.authenticated:
            s.respond(req.code, E_NOT_ALLOWED, debug="not authenticated", channel=channel)
            return
        handler = self.ops.get(req.code)
        if handler is None:
            log.warning("%s %r: operation %d not supported: %s", self.name, s, req.code, p16.lobby_text(req.params))
            s.respond(req.code, E_INVALID, debug="operation not supported", channel=channel)
            return
        handler(self, s, req, channel, reliable)


class Master(_Lb):
    name = "master"

    def after_auth(self, s):
        in_rooms = sum(len(r.actors) for r in self.world.rooms.values())
        s.event(EV_APP_STATS, {P_PEER_COUNT: in_rooms, P_MASTER_PEER_COUNT: len(self.transport.peers),
                               P_GAME_COUNT: len(self.world.rooms)})

    def join_lobby(self, s, req, channel, reliable):
        s.lobby = _lobby_of(req.params)
        s.respond(req.code, channel=channel)
        if s.lobby[1] in (LOBBY_DEFAULT, LOBBY_ASYNC):
            key = (s.app_version, s.lobby)
            listing = self.world.listing(*key)
            self.world._listings[key] = listing
            s.event(EV_GAME_LIST, {P_GAME_LIST: Hashtable(listing)})

    def leave_lobby(self, s, req, channel, reliable):
        s.lobby = None
        s.respond(req.code, channel=channel)

    def _reserve(self, s, name, lobby):
        room = Room(self.world, s.app_version, name, lobby)
        self.world.rooms[(s.app_version, name)] = room
        return room

    def create_game(self, s, req, channel, reliable):
        name = req.params.get(P_ROOM_NAME) or str(uuid.uuid4())
        room = self.world.rooms.get((s.app_version, name))
        if room is not None and (room.actors or time.monotonic() - room.since < RESERVATION):
            s.respond(req.code, E_GAME_EXISTS, debug="a game with this name exists", channel=channel)
            return
        self._reserve(s, name, _lobby_of(req.params, s.lobby or DEFAULT_LOBBY))
        log.info("master %r: creating %r", s, name)
        s.respond(req.code, 0, {P_ROOM_NAME: name, P_ADDRESS: self.world.game_address}, channel=channel)

    def join_game(self, s, req, channel, reliable):
        name = req.params.get(P_ROOM_NAME) or ""
        room = self.world.rooms.get((s.app_version, name))
        if room is None:
            if req.params.get(P_JOIN_MODE) == JOIN_CREATE_IF_NOT_EXISTS:
                self._reserve(s, name, _lobby_of(req.params, s.lobby or DEFAULT_LOBBY))
                s.respond(req.code, 0, {P_ADDRESS: self.world.game_address}, channel=channel)
            else:
                s.respond(req.code, E_NO_GAME, debug="game does not exist", channel=channel)
            return
        if room.created and not room.is_open:
            s.respond(req.code, E_GAME_CLOSED, debug="game closed", channel=channel)
        elif room.full:
            s.respond(req.code, E_GAME_FULL, debug="game full", channel=channel)
        else:
            s.respond(req.code, 0, {P_ADDRESS: self.world.game_address}, channel=channel)

    def join_random(self, s, req, channel, reliable):
        p = req.params
        lobby = _lobby_of(p)
        expected = p.get(P_GAME_PROPS) or {}
        sql = p.get(P_DATA) if lobby[1] == LOBBY_SQL else None
        if lobby[1] == LOBBY_SQL:
            # SQL lobbies match by the filter; only the max players still counts.
            # (Pixel Gun 3D expects C4 = 3 but creates rooms with C4 = 4.)
            expected = {k: v for k, v in expected.items() if not isinstance(k, str)}
        try:
            flt = self.world.sql(sql) if isinstance(sql, str) and sql.strip() else None
        except ValueError as e:
            log.warning("master %r: bad SQL filter %r: %s", s, sql, e)
            s.respond(req.code, E_INVALID, debug="bad filter: %s" % e, channel=channel)
            return
        rooms = []
        for r in self.world.rooms.values():
            if r.version != s.app_version or r.lobby != lobby or not r.created:
                continue
            if not (r.is_open and r.visible) or r.full or not r.actors:
                continue
            if not all(same(r.props.get(k), v) for k, v in expected.items()):
                continue
            if flt is not None and not evaluate(flt, {k: v for k, v in r.props.items() if isinstance(k, str)}):
                continue
            rooms.append(r)
        if not rooms:
            log.info("master %r: no room for %r %s", s, sql, p16.lobby_text(expected))
            s.respond(req.code, E_NO_MATCH, debug="no match found", channel=channel)
            return
        mode = int(p.get(P_MATCHING) or 0)
        room = random.choice(rooms) if mode == 2 else min(rooms, key=lambda r: r.since)  # FillRoom: oldest first
        log.info("master %r: matched %r", s, room)
        s.respond(req.code, 0, {P_ROOM_NAME: room.name, P_ADDRESS: self.world.game_address}, channel=channel)

    def find_friends(self, s, req, channel, reliable):
        wanted = req.params.get(P_FRIENDS)
        ids = list(wanted.items if isinstance(wanted, Array) else wanted or [])
        where = {}
        for app, other in self.world.sessions():
            if other.user_id:
                room = other.room.name if app is self.world.game and other.room else ""
                if room or other.user_id not in where:
                    where[other.user_id] = room
        s.respond(req.code, 0, {P_FRIENDS_ONLINE: Array(BOOLEAN, [i in where for i in ids]),
                                P_FRIENDS_ROOMS: Array(STRING, [where.get(i, "") for i in ids])}, channel=channel)

    def game_list(self, s, req, channel, reliable):
        lobby = _lobby_of(req.params)
        sql = req.params.get(P_DATA)
        try:
            flt = self.world.sql(sql) if isinstance(sql, str) and sql.strip() else None
        except ValueError as e:
            s.respond(req.code, E_INVALID, debug="bad filter: %s" % e, channel=channel)
            return
        games = Hashtable()
        for r in self.world.rooms.values():
            if r.version == s.app_version and r.lobby == lobby and r.created and r.visible:
                if flt is None or evaluate(flt, {k: v for k, v in r.props.items() if isinstance(k, str)}):
                    games[r.name] = r.listing()
        s.respond(req.code, 0, {P_GAME_LIST: games}, channel=channel)

    def ok(self, s, req, channel, reliable):
        s.respond(req.code, channel=channel)

    ops = {OP_JOIN_LOBBY: join_lobby, OP_LEAVE_LOBBY: leave_lobby, OP_CREATE_GAME: create_game,
           OP_JOIN_GAME: join_game, OP_JOIN_RANDOM: join_random, OP_FIND_FRIENDS: find_friends,
           OP_GET_GAME_LIST: game_list, OP_SETTINGS: ok}


class Game(_Lb):
    name = "game"

    def _setup(self, room, p):
        props = p.get(P_GAME_PROPS)
        if isinstance(props, dict):
            room.props.update(props)
        room.props.pop(G_REMOVED, None)
        room.cleanup = p.get(P_CLEANUP, True) is not False
        room.suppress = p.get(P_SUPPRESS) is True
        room.publish_user_id = p.get(P_PUBLISH_USER_ID) is True
        if P_LOBBY_NAME in p or P_LOBBY_TYPE in p:
            room.lobby = _lobby_of(p)
        room.created = True
        room.since = time.monotonic()

    def create_game(self, s, req, channel, reliable):
        if s.room is not None:
            s.respond(req.code, E_NOT_ALLOWED, debug="already in a room", channel=channel)
            return
        p = req.params
        name = p.get(P_ROOM_NAME) or str(uuid.uuid4())
        key = (s.app_version, name)
        room = self.world.rooms.get(key)
        if room is not None and (room.created or room.actors):
            s.respond(req.code, E_GAME_EXISTS, debug="a game with this name exists", channel=channel)
            return
        if room is None:
            room = self.world.rooms[key] = Room(self.world, s.app_version, name, _lobby_of(p, DEFAULT_LOBBY))
        self._setup(room, p)
        log.info("game %r: created %r %s", s, room, p16.lobby_text(room.props))
        self._enter(s, room, p, req.code, channel)

    def join_game(self, s, req, channel, reliable):
        if s.room is not None:
            s.respond(req.code, E_NOT_ALLOWED, debug="already in a room", channel=channel)
            return
        p = req.params
        name = p.get(P_ROOM_NAME) or ""
        room = self.world.rooms.get((s.app_version, name))
        if room is None or not room.created:
            if p.get(P_JOIN_MODE) != JOIN_CREATE_IF_NOT_EXISTS:
                s.respond(req.code, E_NO_GAME, debug="game does not exist", channel=channel)
                return
            if room is None:
                room = self.world.rooms[(s.app_version, name)] = Room(self.world, s.app_version, name, _lobby_of(p, DEFAULT_LOBBY))
            self._setup(room, p)
            log.info("game %r: created %r on join", s, room)
        elif not room.is_open:
            s.respond(req.code, E_GAME_CLOSED, debug="game closed", channel=channel)
            return
        elif room.full:
            s.respond(req.code, E_GAME_FULL, debug="game full", channel=channel)
            return
        self._enter(s, room, p, req.code, channel)

    def _enter(self, s, room, p, code, channel):
        nr = room.next_actor
        room.next_actor += 1
        props = p.get(P_PLAYER_PROPS)
        actor = Actor(nr, s, Hashtable(props) if isinstance(props, dict) else Hashtable())
        if room.publish_user_id:
            actor.props[A_USER_ID] = s.user_id
        room.actors[nr] = actor
        if room.master_id not in room.actors:
            room.master_id = nr
        room.props[G_MASTER] = room.master_id
        s.room, s.actor = room, actor
        actors = room.actor_list()
        s.respond(code, 0, {P_ACTOR_NR: nr, P_ACTOR_LIST: actors, P_GAME_PROPS: Hashtable(room.props),
                            P_PLAYER_PROPS: Hashtable({n: a.props for n, a in room.actors.items()})}, channel=channel)
        join = p16.encode_event(EV_JOIN, {P_ACTOR_NR: nr, P_PLAYER_PROPS: actor.props, P_ACTOR_LIST: actors})
        room.broadcast(join, [actor] if room.suppress else list(room.actors.values()))
        for ce in room.cache:
            s.send_raw(ce.message)
        log.info("game %r: %s joined %r as actor %d (master %d)", s, s.user_id, room, nr, room.master_id)

    def leave(self, s, req, channel, reliable):
        s.respond(req.code, channel=channel)
        self._exit(s, "left")

    def on_leave(self, s, why):
        self._exit(s, why)

    def _exit(self, s, why):
        room, actor = s.room, s.actor
        s.room = s.actor = None
        if room is None or room.actors.get(actor.nr) is not actor:
            return
        del room.actors[actor.nr]
        if room.cleanup:
            room.cache = [ce for ce in room.cache if ce.global_ or ce.actor != actor.nr]
        params = {P_ACTOR_NR: actor.nr, P_ACTOR_LIST: room.actor_list()}
        if room.master_id == actor.nr:
            room.master_id = min(room.actors) if room.actors else 0
            room.props[G_MASTER] = room.master_id
            if room.actors:
                params[P_MASTER_CLIENT_ID] = room.master_id
        log.info("game %r: actor %d %s %r", s, actor.nr, why, room)
        if room.actors:
            if not room.suppress:
                room.broadcast(p16.encode_event(EV_LEAVE, params), room.actors.values())
        else:
            self.world.remove_room(room)

    def raise_event(self, s, req, channel, reliable):
        room, actor = s.room, s.actor
        if room is None:
            s.respond(req.code, E_NOT_ALLOWED, debug="not in a room", channel=channel)
            return
        p = req.params
        code = int(p.get(P_CODE) or 0)
        data = p.get(P_DATA)
        cache = int(p.get(P_CACHE) or 0)
        if cache == CACHE_REMOVE:
            self._uncache(room, code, p.get(P_ACTOR_LIST), data)
            return
        if cache == CACHE_REMOVE_LEFT:
            room.cache = [ce for ce in room.cache if ce.global_ or ce.actor in room.actors]
            return
        if cache > CACHE_REMOVE_LEFT:
            return  # cache slices: nothing to deliver
        params = {P_ACTOR_NR: actor.nr}
        if data is not None:
            params[P_DATA] = data
        message = p16.encode_event(code, params)
        if cache in (CACHE_ADD, CACHE_ADD_GLOBAL):
            room.cache.append(CachedEvent(actor.nr, code, data, message, cache == CACHE_ADD_GLOBAL))
        targets = p.get(P_ACTOR_LIST)
        if targets is not None:
            wanted = set(targets)
            to = [a for n, a in room.actors.items() if n in wanted]
        else:
            receivers = int(p.get(P_RECEIVERS) or 0)
            if receivers == TO_MASTER:
                to = [room.actors[room.master_id]] if room.master_id in room.actors else []
            elif receivers == TO_ALL:
                to = list(room.actors.values())
            else:
                to = [a for a in room.actors.values() if a is not actor]
            group = int(p.get(P_GROUP) or 0)
            if group:
                to = [a for a in to if a.in_group(group)]
        room.broadcast(message, to, reliable, channel)

    def _uncache(self, room, code, actors, data):
        who = set(actors) if actors is not None else None
        keep = []
        for ce in room.cache:
            hit = (not code or ce.code == code) and (who is None or ce.actor in who)
            if hit and isinstance(data, dict):
                hit = isinstance(ce.data, dict) and all(k in ce.data and ce.data[k] == v for k, v in data.items())
            if not hit:
                keep.append(ce)
        room.cache = keep

    def set_properties(self, s, req, channel, reliable):
        room = s.room
        if room is None:
            s.respond(req.code, E_NOT_ALLOWED, debug="not in a room", channel=channel)
            return
        p = req.params
        props = p.get(P_PROPERTIES)
        if not isinstance(props, dict):
            s.respond(req.code, E_INVALID, debug="no properties", channel=channel)
            return
        target = int(p.get(P_ACTOR_NR) or 0)
        if target:
            a = room.actors.get(target)
            if a is None:
                s.respond(req.code, E_INVALID, debug="no actor %d" % target, channel=channel)
                return
            current = a.props
        else:
            current = room.props
        expected = p.get(P_EXPECTED)
        if isinstance(expected, dict) and expected:
            if not all(same(current.get(k), v) for k, v in expected.items()):
                s.respond(req.code, E_INVALID, debug="expected values do not match", channel=channel)
                return
        for k, v in props.items():
            if v is None:
                current.pop(k, None)
            else:
                current[k] = v
        if not target and G_MASTER in props:
            new = int(props[G_MASTER] or 0)
            if new in room.actors:
                room.master_id = new
            room.props[G_MASTER] = room.master_id
        s.respond(req.code, channel=channel)
        if p.get(P_BROADCAST, True) is not False:
            ev = p16.encode_event(EV_PROPERTIES_CHANGED, {P_TARGET_ACTOR: target, P_PROPERTIES: props, P_ACTOR_NR: s.actor.nr})
            others = [a for a in room.actors.values() if expected or a is not s.actor]
            room.broadcast(ev, others)

    def get_properties(self, s, req, channel, reliable):
        room = s.room
        if room is None:
            s.respond(req.code, E_NOT_ALLOWED, debug="not in a room", channel=channel)
            return
        s.respond(req.code, 0, {P_GAME_PROPS: Hashtable(room.props),
                                P_PLAYER_PROPS: Hashtable({n: a.props for n, a in room.actors.items()})}, channel=channel)

    def change_groups(self, s, req, channel, reliable):
        actor = s.actor
        if actor is None:
            s.respond(req.code, E_NOT_ALLOWED, debug="not in a room", channel=channel)
            return
        remove, add = req.params.get(P_REMOVE), req.params.get(P_ADD)
        if remove is not None:
            if len(remove) == 0:
                actor.groups.clear()
                actor.all_groups = False
            else:
                actor.groups.difference_update(remove)
        if add is not None:
            if len(add) == 0:
                actor.all_groups = True
            else:
                actor.groups.update(add)

    ops = {OP_CREATE_GAME: create_game, OP_JOIN_GAME: join_game, OP_LEAVE: leave, OP_RAISE_EVENT: raise_event,
           OP_SET_PROPERTIES: set_properties, OP_GET_PROPERTIES: get_properties, OP_CHANGE_GROUPS: change_groups}
