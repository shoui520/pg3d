#!/usr/bin/env python3
"""
A private server for Pixel Gun 3D 12.5.0: Photon master and game server
(UDP) plus a stand-in for the account backend (HTTP), in one process.

    python pg3d_server.py --public-ip 192.168.1.10

--public-ip is the address clients use to reach this machine; the master
hands it out as the game server's address.
"""
import argparse
import asyncio
import logging
import os
import socket
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from backend.rilisoft import Backend, HttpServer, Players  # noqa: E402
from lb.loadbalancing import Game, Master, World  # noqa: E402
from photon.enet import Transport  # noqa: E402

log = logging.getLogger("server")


async def main(args):
    loop = asyncio.get_running_loop()
    world = World("%s:%d" % (args.public_ip, args.game_port))
    world.master, world.game = Master(world), Game(world)
    for app, port in ((world.master, args.master_port), (world.game, args.game_port)):
        app.transport = Transport(app, app.name)
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        sock.bind((args.bind, port))
        if hasattr(socket, "SIO_UDP_CONNRESET"):
            # Windows: a client's ICMP "port unreachable" would otherwise reset the socket
            sock.ioctl(socket.SIO_UDP_CONNRESET, False)
        await loop.create_datagram_endpoint(lambda t=app.transport: t, sock=sock)
        log.info("%s server on udp %s:%d", app.name, args.bind, port)

    os.makedirs(args.data, exist_ok=True)
    players = Players(os.path.join(args.data, "players.json"))
    http = await asyncio.start_server(HttpServer(Backend(players)).handle, args.bind, args.http_port)
    log.info("account backend on tcp %s:%d", args.bind, args.http_port)

    transports = (world.master.transport, world.game.transport)
    ticks = 0
    async with http:
        while True:
            await asyncio.sleep(0.05)
            ticks += 1
            for t in transports:
                t.tick()
            if ticks % 20 == 0:
                world.tick_lobbies()
            if ticks % 100 == 0:
                world.tick_stats()
            for t in transports:
                t.flush()


if __name__ == "__main__":
    ap = argparse.ArgumentParser(description=__doc__.strip().splitlines()[0])
    ap.add_argument("--public-ip", required=True, help="address clients reach this machine at")
    ap.add_argument("--bind", default="0.0.0.0")
    ap.add_argument("--master-port", type=int, default=5055)
    ap.add_argument("--game-port", type=int, default=5056)
    ap.add_argument("--http-port", type=int, default=8080)
    ap.add_argument("--data", default=os.path.join(os.path.dirname(os.path.abspath(__file__)), "data"),
                    help="where accounts are kept")
    ap.add_argument("-v", "--verbose", action="store_true", help="log every operation")
    args = ap.parse_args()
    logging.basicConfig(level=logging.DEBUG if args.verbose else logging.INFO,
                        format="%(asctime)s %(name)-6s %(message)s", datefmt="%H:%M:%S")
    try:
        asyncio.run(main(args))
    except KeyboardInterrupt:
        pass
