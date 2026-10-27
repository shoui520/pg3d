"""
End to end: the server in a subprocess, test clients going through what PUN
does (master, lobby, matchmaking, game server, events, properties, leaving)
and the account backend.

    python -m unittest discover -s tests -t .      (from server/)
"""
import json
import os
import random
import subprocess
import sys
import tempfile
import time
import unittest
import urllib.request
from urllib.parse import urlencode

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, ROOT)

from photon.protocol16 import Array, Byte, Hashtable, STRING  # noqa: E402
from tests.client import Client  # noqa: E402

LOBBY = {213: "PixelGun3D", 212: Byte(2)}
VERSION = "12.5.0Deathmatch"


class Flow(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        base = random.randint(20000, 40000)
        cls.master, cls.game, cls.http = base, base + 1, base + 2
        cls.data = tempfile.mkdtemp()
        cls.log = open(os.path.join(cls.data, "server.log"), "w+")
        cls.proc = subprocess.Popen(
            [sys.executable, os.path.join(ROOT, "pg3d_server.py"), "--public-ip", "127.0.0.1", "--bind", "127.0.0.1",
             "--master-port", str(cls.master), "--game-port", str(cls.game), "--http-port", str(cls.http),
             "--data", cls.data, "-v"], stdout=cls.log, stderr=subprocess.STDOUT)
        time.sleep(0.8)

    @classmethod
    def tearDownClass(cls):
        cls.proc.terminate()
        cls.proc.wait()
        if os.environ.get("SHOW_LOG"):
            cls.log.seek(0)
            print(cls.log.read())
        cls.log.close()

    def master_client(self, user):
        c = Client(("127.0.0.1", self.master))
        c.connect()
        c.exchange_keys()
        r = c.call(230, {220: VERSION, 224: "test-app", 225: user}, encrypted=True)
        self.assertEqual(r.return_code, 0, r.debug)
        self.assertEqual(r.params[225], user)
        c.token = r.params[221]
        c.event(226)  # app stats
        self.assertEqual(c.call(229, LOBBY).return_code, 0)
        return c

    def game_client(self, master, address):
        host, port = address.split(":")
        master.disconnect()
        c = Client((host, int(port)))
        c.connect()
        r = c.call(230, {221: master.token})
        self.assertEqual(r.return_code, 0, r.debug)
        return c

    def test_match(self):
        sql = "C0 = 5 AND C1 = \"\" AND C2 = 3"
        a = self.master_client("alice")
        r = a.call(225, {**LOBBY, 245: sql})
        self.assertEqual(r.return_code, 32760)
        r = a.call(227, {**LOBBY, 255: "room1"})
        self.assertEqual(r.return_code, 0, r.debug)
        a = self.game_client(a, r.params[230])
        props = Hashtable({Byte(253): True, Byte(254): True, Byte(255): Byte(10), Byte(249): True,
                           Byte(250): Array(STRING, ["C0", "C1", "C2"]), "C0": 5, "C1": "", "C2": 3, "C4": 4})
        r = a.call(227, {**LOBBY, 255: "room1", 248: props, 241: True, 249: Hashtable({Byte(255): "Alice"}), 250: True})
        self.assertEqual(r.return_code, 0, r.debug)
        self.assertEqual(r.params[254], 1)
        self.assertEqual(list(r.params[252]), [1])
        self.assertEqual(r.params[248][Byte(248)], 1)
        ev = a.event(255)
        self.assertEqual(ev.params[254], 1)

        b = self.master_client("bob")
        r = b.call(225, {**LOBBY, 245: "C0 = 4"})
        self.assertEqual(r.return_code, 32760)
        r = b.call(225, {**LOBBY, 245: sql, 248: Hashtable({Byte(255): Byte(5)})})
        self.assertEqual(r.return_code, 32760)  # max players still has to match
        r = b.call(225, {**LOBBY, 245: sql, 248: Hashtable({Byte(255): Byte(10), "C4": 3})})
        self.assertEqual(r.return_code, 0, r.debug)
        self.assertEqual(r.params[255], "room1")
        b = self.game_client(b, r.params[230])
        r = b.call(226, {255: "room1", 249: Hashtable({Byte(255): "Bob"}), 250: True})
        self.assertEqual(r.return_code, 0, r.debug)
        self.assertEqual(r.params[254], 2)
        self.assertEqual(r.params[249][1][Byte(255)], "Alice")
        self.assertEqual(r.params[248]["C4"], 4)
        self.assertEqual(b.event(255).params[254], 2)
        self.assertEqual(a.event(255).params[254], 2)

        # a cached event, a large one (fragmented), one to the master only
        a.op(253, {244: Byte(202), 245: Hashtable({Byte(7): 1001, Byte(0): "Player"}), 247: Byte(4)})
        ev = b.event(202)
        self.assertEqual(ev.params[254], 1)
        self.assertEqual(ev.params[245][Byte(7)], 1001)
        blob = os.urandom(5000)
        b.op(253, {244: Byte(10), 245: blob, 246: Byte(1)})
        self.assertEqual(a.event(10).params[245], blob)
        self.assertEqual(b.event(10).params[245], blob)
        b.op(253, {244: Byte(11), 245: 7, 246: Byte(2)})
        self.assertEqual(a.event(11).params[245], 7)

        # properties: a plain change, then compare-and-set
        r = b.call(252, {251: Hashtable({"C4": 6}), 250: True})
        self.assertEqual(r.return_code, 0)
        self.assertEqual(a.event(253).params[251]["C4"], 6)
        r = b.call(252, {251: Hashtable({"C4": 8}), 231: Hashtable({"C4": 5}), 250: True})
        self.assertNotEqual(r.return_code, 0)

        # a third player gets the cache; then the creator leaves and the master moves on
        c = self.master_client("carol")
        r = c.call(226, {**LOBBY, 255: "room1"})
        self.assertEqual(r.return_code, 0, r.debug)
        c = self.game_client(c, r.params[230])
        r = c.call(226, {255: "room1"})
        self.assertEqual(r.params[254], 3)
        self.assertEqual(c.event(202).params[254], 1)

        a.call(254)
        ev = b.event(254)
        self.assertEqual(ev.params[254], 1)
        self.assertEqual(ev.params[203], 2)
        c.event(254)
        c.op(253, {244: Byte(12), 246: Byte(2)})
        b.event(12)

        # the cached event of the actor who left is gone
        d = self.master_client("dave")
        r = d.call(226, {**LOBBY, 255: "room1"})
        d = self.game_client(d, r.params[230])
        r = d.call(226, {255: "room1"})
        self.assertEqual(r.return_code, 0)
        d.pump(0.3)
        self.assertEqual(d.events(202), [])

        # FindFriends sees who is in the room
        e = self.master_client("erin")
        r = e.call(222, {1: Array(STRING, ["bob", "nobody"])})
        self.assertEqual(list(r.params[1]), [True, False])
        self.assertEqual(list(r.params[2]), ["room1", ""])
        for x in (a, b, c, d, e):
            x.disconnect()

    def test_keepalive(self):
        # an idle client (PUN loading a level: acks only) is pinged and kept
        c = self.master_client("idle")
        c.pump(2.5)
        self.assertGreaterEqual(c.pings, 1)
        self.assertFalse(c.disconnected)
        self.assertEqual(c.call(229, LOBBY).return_code, 0)
        c.disconnect()

    def post(self, path, **form):
        req = urllib.request.Request("http://127.0.0.1:%d%s" % (self.http, path), urlencode(form).encode(),
                                     {"Content-Type": "application/x-www-form-urlencoded"})
        with urllib.request.urlopen(req, timeout=3) as f:
            return f.read().decode()

    def test_backend(self):
        self.assertEqual(self.post("/pixelgun3d-config/getBanList.php", id="1"), "0")
        self.assertTrue(self.post("/get_time.php").isdigit())
        token = self.post("/~rilisoft/action.php", action="create_player_intent", app_version="Android:12.5.0")
        self.assertTrue(token)
        pid = json.loads(self.post("/~rilisoft/action.php", action="create_player", token=token, device="d"))["id"]
        self.assertEqual(self.post("/~rilisoft/action.php", action="start_check", uniq_id=pid), pid)
        self.assertEqual(self.post("/~rilisoft/action.php", action="update_player", id=pid, nick="Vita", rank="12"), "ok")
        info = json.loads(self.post("/~rilisoft/action.php", action="get_info_by_id", id=pid))
        self.assertEqual(info["player"]["nick"], "Vita")
        self.assertEqual(self.post("/~rilisoft/action.php", action="no_such_action"), "fail")
        with open(os.path.join(self.data, "players.json")) as f:
            self.assertIn(pid, json.load(f)["players"])


if __name__ == "__main__":
    unittest.main()
