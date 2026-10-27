"""
A stand-in for the Rilisoft web backend, just enough for the game to get an
account ID, keep its profile, read the time and not consider itself banned.

The Vita build sends the game's HTTPS requests here as plain HTTP, path
unchanged. action.php takes a form whose `action` says what to do; anything
this server doesn't know answers "fail" (or 404 for other paths), which the
game treats like a network error and retries later or does without.
"""
import asyncio
import json
import logging
import os
import secrets
import time
from urllib.parse import parse_qsl, urlsplit

log = logging.getLogger("http")

MAX_BODY = 4 << 20


class Players:
    """Accounts and the profile fields update_player sends, in a JSON file"""

    PROFILE = ("nick", "rank", "wins", "total_wins", "country", "continent", "lobby", "kill_cnt", "death_cnt")

    def __init__(self, path):
        self.path = path
        self.data = {"next_id": 1000001, "players": {}}
        if path and os.path.exists(path):
            with open(path, encoding="utf-8") as f:
                self.data = json.load(f)
        self.tokens = set()
        self._dirty = False

    def create(self, device, platform):
        pid = str(self.data["next_id"])
        self.data["next_id"] += 1
        self.data["players"][pid] = {"id": pid, "device": device, "platform": platform, "created": int(time.time())}
        self._dirty = True
        return pid

    def get(self, pid):
        return self.data["players"].get(str(pid))

    def update(self, pid, form):
        p = self.data["players"].setdefault(str(pid), {"id": str(pid), "created": int(time.time())})
        for k in self.PROFILE:
            if k in form:
                p[k] = form[k]
        if form.get("skin"):
            p["skin"] = form["skin"]
        p["seen"] = int(time.time())
        self._dirty = True

    def save(self):
        if not self._dirty or not self.path:
            return
        tmp = self.path + ".tmp"
        with open(tmp, "w", encoding="utf-8") as f:
            json.dump(self.data, f)
        os.replace(tmp, self.path)
        self._dirty = False


def _short(form):
    return {k: (v if len(v) <= 64 else "%s...(%d)" % (v[:24], len(v))) for k, v in form.items() if k != "auth"}


class Backend:
    def __init__(self, players):
        self.players = players

    # ---- action.php ----

    def action(self, form):
        a = form.get("action", "")
        handler = getattr(self, "do_" + a, None)
        if handler is None:
            return None
        return handler(form)

    def do_get_time(self, form):
        return str(int(time.time()))

    def do_create_player_intent(self, form):
        token = secrets.token_hex(16)
        self.players.tokens.add(token)
        return token

    def do_create_player(self, form):
        pid = self.players.create(form.get("device", ""), form.get("platform", ""))
        log.info("new account %s (%s)", pid, form.get("device_model", ""))
        return json.dumps({"id": pid})

    def do_start_check(self, form):
        pid = form.get("uniq_id", "")
        if not pid.isdigit():
            return "fail"
        if self.players.get(pid) is None:
            self.players.update(pid, {})  # an account from before this server: adopt it
        return pid

    def do_update_player(self, form):
        if form.get("id", "").isdigit():
            self.players.update(form["id"], form)
        return "ok"

    def do_update_player_online(self, form):
        return "ok"

    def do_update_game_time(self, form):
        return "ok"

    def do_update_accessories(self, form):
        return "ok"

    def _info(self, pid):
        p = self.players.get(pid)
        if p is None:
            return None
        player = {"id": p["id"], "nick": p.get("nick", "Player"), "rank": p.get("rank", "1"),
                  "skin": p.get("skin", ""), "wins": p.get("wins", "0"), "total_wins": p.get("total_wins", "0"),
                  "clan_name": "", "clan_logo": "", "clan_creator_id": ""}
        return {"player": player}

    def do_get_info_by_id(self, form):
        info = self._info(form.get("id", ""))
        return json.dumps(info) if info else "fail"

    def do_get_all_short_info_by_id(self, form):
        try:
            ids = json.loads(form.get("ids", "[]"))
        except ValueError:
            return "fail"
        out = {}
        for i in ids if isinstance(ids, list) else []:
            info = self._info(i)
            if info:
                out[str(i)] = info["player"]
        return json.dumps(out)

    # ---- other paths ----

    def get(self, path, form):
        if path.endswith("/getBanList.php"):
            return "0"
        if path.endswith("/get_time.php"):
            return str(int(time.time()))
        if path.endswith("/action.php"):
            return self.action(form)
        return None


class HttpServer:
    def __init__(self, backend):
        self.backend = backend

    async def handle(self, reader, writer):
        peer = writer.get_extra_info("peername") or ("?", 0)
        try:
            head = await asyncio.wait_for(reader.readuntil(b"\r\n\r\n"), 30)
            lines = head.decode("latin-1").split("\r\n")
            method, target, _ = (lines[0].split(" ", 2) + ["", ""])[:3]
            headers = {}
            for line in lines[1:]:
                if ":" in line:
                    k, v = line.split(":", 1)
                    headers[k.strip().lower()] = v.strip()
            n = int(headers.get("content-length") or 0)
            if n > MAX_BODY:
                raise ValueError("body of %d bytes" % n)
            body = await asyncio.wait_for(reader.readexactly(n), 30) if n else b""
            url = urlsplit(target)
            form = dict(parse_qsl(url.query, keep_blank_values=True))
            if "form-urlencoded" in headers.get("content-type", "") or (body and "content-type" not in headers):
                form.update(parse_qsl(body.decode("utf-8", "replace"), keep_blank_values=True))
            host = headers.get("host", "")
            try:
                reply = self.backend.get(url.path, form)
            except Exception:
                log.exception("%s %s", method, target)
                reply = "fail"
            if reply is None and url.path.endswith("/action.php"):
                reply = "fail"
            status = "200 OK" if reply is not None else "404 Not Found"
            log.info("%s %s%s %s -> %s %s", method, host, url.path, _short(form) if form else "",
                     status[:3], (reply or "")[:80])
            data = (reply or "").encode("utf-8")
            writer.write(("HTTP/1.1 %s\r\nContent-Type: text/plain; charset=utf-8\r\nContent-Length: %d\r\n"
                          "Connection: close\r\n\r\n" % (status, len(data))).encode("latin-1") + data)
            await writer.drain()
            self.backend.players.save()
        except (asyncio.IncompleteReadError, asyncio.TimeoutError, ConnectionError):
            pass
        except Exception:
            log.exception("http request from %s", peer[0])
        finally:
            writer.close()
