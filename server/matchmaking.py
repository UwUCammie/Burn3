#!/usr/bin/env python3
"""Native Burnout 3 matchmaking and UDP rendezvous, Windows and POSIX.

The service owns lobby membership and version admission. Gameplay is sent
between peers; this process never runs a car/physics simulation.
"""
from __future__ import annotations
import argparse
import asyncio
from collections import deque
from dataclasses import dataclass, field
import json
import logging
import os
from pathlib import Path
import re
import secrets
import socket
import ssl
import struct
import time

from release_gate import ReleaseGate, ReleaseRejected, load_key

WIRE_VERSION = 1
# The version clients announce in their hello (kClientTag in src/net/matchmaking.cpp). It identifies the online
# protocol build and is not the release number in VERSION.
GAME_VERSION = "0.0.24"
MODES = {"race": 6, "road_rage": 6, "team_crush": 2, "double_impact": 2, "party_crash": 8}
MAX_LINE = 65536
MAX_LOBBY_PLAYERS = 256
UDP_REGISTER = struct.Struct("!4sI16s")
UDP_RELAY = struct.Struct("!4sI16sI")
LOG = logging.getLogger("takedown.matchmaking")

class ProtocolError(Exception):
    pass

@dataclass(frozen=True)
class AccountIdentity:
    name: str
    account_id: str
    epoch: int = 0
    session_id: str = ""

@dataclass
class Player:
    ident: int
    name: str
    writer: asyncio.StreamWriter
    address: str
    mod_identity: str
    token: bytes = field(default_factory=lambda: secrets.token_bytes(16))
    endpoint: tuple[str, int] | None = None
    room: int | None = None
    requests: deque = field(default_factory=deque)
    last_activity: float = field(default_factory=time.monotonic)
    relay_window: deque = field(default_factory=deque)
    relay_bytes: int = 0
    user_parameters: str = ""
    account_id: str = ""
    account_epoch: int = 0
    session_id: str = ""
    mod_manifest: list = field(default_factory=list)

@dataclass
class Room:
    ident: int
    host: int
    name: str
    mode: str
    track: str
    mod_identity: str
    members: list[int]
    key: str = field(default_factory=lambda: secrets.token_hex(16))
    started: bool = False
    ready: set[int] = field(default_factory=set)
    capacity: int = 0
    parameters: str = ""
    system_flags: int = 0
    password: str = ""
    mod_manifest: list = field(default_factory=list)
    # One bounded retry receipt, scoped to this room and its authenticated host.
    # Repeated original-game kick callbacks must not rotate the key repeatedly.
    last_kicked_member: int = 0

class Rendezvous(asyncio.DatagramProtocol):
    def __init__(self, service):
        self.service = service
        self.transport = None

    def connection_made(self, transport):
        self.transport = transport

    def datagram_received(self, data, address):
        # Fixed-size registration only: the server never reflects gameplay
        # packets or amplifies arbitrary datagrams to a third-party address.
        if data[:4] == b"B3RL":
            self.relay(data, address)
            return
        if len(data) != UDP_REGISTER.size:
            return
        magic, ident, token = UDP_REGISTER.unpack(data)
        player = self.service.players.get(ident)
        if magic != b"B3NR" or player is None or address[0] != player.address:
            return
        if not secrets.compare_digest(token, player.token):
            return
        endpoint = (address[0], address[1])
        changed = player.endpoint != endpoint
        player.endpoint = endpoint
        self.transport.sendto(UDP_REGISTER.pack(b"B3NA", ident, token), address)
        if changed and player.room in self.service.rooms:
            self.service.broadcast_room(self.service.rooms[player.room])

    def relay(self, data, address):
        # Transport fallback for restrictive NAT. Peers still own gameplay;
        # the server only forwards a bounded, session-authenticated datagram.
        if not UDP_RELAY.size < len(data) <= 1232:
            return
        _, ident, token, target = UDP_RELAY.unpack_from(data)
        player = self.service.players.get(ident)
        destination = self.service.players.get(target)
        if player is None or destination is None or player.ident == target:
            return
        if player.endpoint != address or not secrets.compare_digest(player.token, token):
            return
        room = self.service.rooms.get(player.room)
        if room is None or target not in room.members or not destination.endpoint:
            return
        now = time.monotonic()
        while player.relay_window and player.relay_window[0][0] < now - 1:
            _, size = player.relay_window.popleft()
            player.relay_bytes -= size
        if player.relay_bytes + len(data) > 256000:
            return
        player.relay_window.append((now, len(data)))
        player.relay_bytes += len(data)
        self.transport.sendto(data[UDP_RELAY.size:], destination.endpoint)

class Matchmaking:
    def __init__(self, version=GAME_VERSION, *, max_players=256, idle_timeout=60, authenticate=None, services=None,
                 release_gate=None):
        if type(max_players) is not int or not 1 <= max_players <= MAX_LOBBY_PLAYERS:
            raise ValueError("max_players must be between 1 and 256")
        self.version = version
        self.max_players = max_players
        self.idle_timeout = idle_timeout
        self.players: dict[int, Player] = {}
        self.rooms: dict[int, Room] = {}
        self.next_player = 1
        self.next_room = 1
        self.server = None
        self.udp = None
        self.udp_port = 0
        self.authenticate = authenticate
        self.services = services
        # None admits every client; with a ReleaseGate only official releases (docs/RELEASING.md) get in.
        self.release_gate = release_gate
        key_file = os.environ.get("TAKEDOWN_RELEASE_KEY_FILE")
        if release_gate is None and key_file:
            # Embedding code (the website) enables the gate through the environment alone.
            try:
                self.release_gate = ReleaseGate(load_key(key_file))
            except (OSError, ValueError) as exc:
                LOG.error("release key %s is unusable (%s): refusing every client until it is fixed", key_file, exc)
                self.release_gate = ReleaseGate(None)

    async def start(self, host="127.0.0.1", port=21800, udp_port=21801, ssl_context=None):
        loop = asyncio.get_running_loop()
        self.udp, _ = await loop.create_datagram_endpoint(lambda: Rendezvous(self),
            local_addr=(host, udp_port), family=socket.AF_INET)
        self.udp_port = self.udp.get_extra_info("sockname")[1]
        try:
            self.server = await asyncio.start_server(self.accept, host, port,
                family=socket.AF_INET, limit=MAX_LINE + 1, ssl=ssl_context,
                **({"ssl_handshake_timeout": 10} if ssl_context else {}))
        except BaseException:
            self.udp.close()
            raise
        return self.server.sockets[0].getsockname()[1]

    async def close(self):
        if self.server:
            self.server.close()
            await self.server.wait_closed()
        if self.udp:
            self.udp.close()
        writers = [p.writer for p in self.players.values()]
        for writer in writers:
            writer.close()
        for writer in writers:
            try:
                await writer.wait_closed()
            except (ConnectionError, OSError):
                pass

    @staticmethod
    def send(writer, message):
        if writer.is_closing():
            return
        # A stalled peer cannot accumulate unbounded lobby updates.
        if writer.transport.get_write_buffer_size() > 256 * 1024:
            writer.close()
            return
        writer.write((json.dumps(message, ensure_ascii=True, separators=(",", ":")) + "\n").encode("ascii"))

    async def read(self, reader, timeout):
        try:
            raw = await asyncio.wait_for(reader.readline(), timeout)
        except (ValueError, asyncio.LimitOverrunError) as exc:
            raise ProtocolError("message_too_large") from exc
        if not raw:
            raise EOFError
        if len(raw) > MAX_LINE or not raw.endswith(b"\n"):
            raise ProtocolError("message_too_large")
        try:
            value = json.loads(raw)
        except (ValueError, UnicodeDecodeError, RecursionError) as exc:
            raise ProtocolError("invalid_json") from exc
        if not isinstance(value, dict):
            raise ProtocolError("invalid_message")
        return value

    @staticmethod
    def text(message, key, maximum, *, required=True):
        value = message.get(key, "")
        if not isinstance(value, str) or len(value) > maximum or (required and not value):
            raise ProtocolError("invalid_" + key)
        if any(ord(c) < 32 or ord(c) == 127 for c in value):
            raise ProtocolError("invalid_" + key)
        return value

    def describe(self, room, *, peers=False):
        result = {"id": room.ident, "name": room.name, "mode": room.mode,
            "track": room.track, "host": room.host, "capacity": room.capacity or MODES[room.mode],
            "started": room.started, "count": len(room.members),
            "parameters": room.parameters, "system_flags": room.system_flags,
            "host_name": self.players[room.host].name,
            "locked": bool(room.password), "mods": room.mod_identity, "mod_manifest": room.mod_manifest}
        # The original browser's View Players page reads the advertised roster.
        # Keep connection credentials and network addresses member-only.
        result["players"] = [{"id": p.ident, "name": p.name, "parameters": p.user_parameters}
            for ident in room.members if (p := self.players.get(ident))]
        if peers:
            result["key"] = room.key
            result["players"] = [{"id": p.ident, "name": p.name,
                "ready": p.ident in room.ready, "parameters": p.user_parameters,
                "endpoint": list(p.endpoint) if p.endpoint else None}
                for ident in room.members if (p := self.players.get(ident))]
        return result

    def lobby_players(self):
        # The original public View Players menu lists everyone online, including
        # drivers outside a game. Its persona field holds 15 UTF-8 bytes. Keep
        # this projection separate from member rosters and account records.
        return [{"id": player.ident,
                 "name": player.name.encode("utf-8")[:15].decode("utf-8", errors="ignore") or "Driver",
                 "game": player.room if player.room in self.rooms else 0}
                for player in self.players.values()]

    def revoke_account(self, account_id, reason="account_signed_in_elsewhere"):
        self.disconnect(lambda player: player.account_id == account_id, reason)

    def revoke_session(self, session_id, reason="session_revoked"):
        # Only players admitted through this game session; an empty id matches no one.
        if session_id:
            self.disconnect(lambda player: player.session_id == session_id, reason)

    def disconnect(self, matches, reason):
        for player in list(self.players.values()):
            if matches(player):
                self.send(player.writer, {"type": "error", "error": reason})
                self.leave(player)
                self.players.pop(player.ident, None)
                player.writer.close()

    @staticmethod
    def parse_mods(message):
        identity = message.get("mods")
        if not isinstance(identity, str) or not re.fullmatch(r"[a-f0-9]{64}", identity):
            raise ProtocolError("invalid_mods")
        manifest = message.get("mod_manifest", [])
        if not isinstance(manifest, list) or len(manifest) > 32:
            raise ProtocolError("invalid_mod_manifest")
        result, ids, folders = [], set(), set()
        for item in manifest:
            if not isinstance(item, dict):
                raise ProtocolError("invalid_mod_manifest")
            ident, folder, revision = item.get("id"), item.get("folder"), item.get("revision", "")
            if (not isinstance(ident, str) or not re.fullmatch(r"[a-z0-9][a-z0-9._-]{0,63}", ident) or
                ".." in ident or ident.endswith(".") or ident in ids or not isinstance(folder, str) or
                not re.fullmatch(r"[a-zA-Z0-9][a-zA-Z0-9._-]{0,159}", folder) or ".." in folder or
                folder.endswith(".") or folder.casefold() in folders or type(item.get("enabled")) is not bool or
                not isinstance(revision, str) or (revision and not re.fullmatch(r"[a-f0-9]{64}", revision))):
                raise ProtocolError("invalid_mod_manifest")
            content = item.get("content_sha256", "")
            if not isinstance(content, str) or (content and not re.fullmatch(r"[a-f0-9]{64}", content)):
                raise ProtocolError("invalid_mod_manifest")
            ids.add(ident); folders.add(folder.casefold())
            result.append({"id": ident, "folder": folder, "revision": revision,
                           "enabled": item["enabled"], "content_sha256": content})
            if "name" in item:
                result[-1]["name"] = Matchmaking.text(item, "name", 96)
                if len(result[-1]["name"].encode("utf-8")) > 200:
                    raise ProtocolError("invalid_name")
        return identity, result

    def broadcast_room(self, room):
        event = {"type": "room", "room": self.describe(room, peers=True)}
        for ident in room.members:
            player = self.players.get(ident)
            if player:
                self.send(player.writer, event)

    def leave(self, player):
        room = self.rooms.get(player.room)
        player.room = None
        player.user_parameters = ""
        if room is None:
            return
        if player.ident == room.host:
            del self.rooms[room.ident]
            for ident in room.members:
                other = self.players.get(ident)
                if other:
                    other.room = None
                    self.send(other.writer, {"type": "room_closed", "reason": "host_left"})
            return
        room.members.remove(player.ident)
        room.ready.discard(player.ident)
        # Rotate membership key so a departed member cannot re-enter peer data.
        room.key = secrets.token_hex(16)
        self.broadcast_room(room)

    def command(self, player, message):
        kind = message.get("type")
        if kind == "mods":
            if player.room is not None:
                raise ProtocolError("already_in_room")
            player.mod_identity, player.mod_manifest = self.parse_mods(message)
            return {"type": "ok"}
        if kind == "ping":
            return {"type": "pong", "lobby_players": self.lobby_players()}
        if kind == "chat":
            text = self.text(message, "text", 256)
            target = self.text(message, "target", 32, required=False)
            attributes = self.text(message, "attributes", 32, required=False)
            if any(c not in "@ABCDEFGHIJKLMNOPQRSTUVWXYZ0123-" for c in attributes):
                raise ProtocolError("invalid_attributes")
            # Private routing and the SDK's self flag belong to the service.
            # In particular, the game's control-message decoder must be able
            # to distinguish its own echo from another driver's message.
            attributes = ''.join(c for c in attributes if c not in "PU")
            if target:
                destination = next((p for p in self.players.values() if p.name == target), None)
                if destination is None:
                    raise ProtocolError("player_not_found")
                recipients = {player.ident, destination.ident}
                attributes += "P" if "P" not in attributes else ""
            elif player.room in self.rooms:
                recipients = self.rooms[player.room].members
            else:
                recipients = [p.ident for p in self.players.values() if p.room is None]
            for ident in recipients:
                self.send(self.players[ident].writer, {"type": "chat", "name": player.name,
                    "text": text, "attributes": attributes + ("U" if ident == player.ident else "")})
            return {"type": "ok"}
        if kind == "list":
            mode = message.get("mode")
            after = message.get("after", 0)
            if type(after) is not int or after < 0:
                raise ProtocolError("invalid_after")
            rooms = [r for r in self.rooms.values() if r.ident > after and
                not r.started and (mode is None or r.mode == mode)]
            page = []
            for room in rooms[:12]:
                candidate = [*page, room]
                if page and len(json.dumps([self.describe(r) for r in candidate], ensure_ascii=True).encode()) > 12000:
                    break
                page = candidate
            return {"type": "rooms", "rooms": [self.describe(r) for r in page],
                "next": page[-1].ident if len(rooms) > len(page) else None}
        if kind == "leave":
            self.leave(player)
            return {"type": "left"}
        if kind in ("create", "join", "quick_match"):
            if player.room is not None:
                raise ProtocolError("already_in_room")
            mode = self.text(message, "mode", 32) if kind != "join" else None
            if mode is not None and mode not in (*MODES, "original"):
                raise ProtocolError("unsupported_mode")
            if kind == "create":
                if len(self.rooms) >= self.max_players:
                    raise ProtocolError("server_full")
                name = self.text(message, "name", 48)
                track = self.text(message, "track", 96, required=mode != "original")
                room = Room(self.next_room, player.ident, name, mode, track, player.mod_identity, [player.ident])
                room.mod_manifest = list(player.mod_manifest)
                if mode == "original":
                    capacity = message.get("capacity")
                    flags = message.get("system_flags", 0)
                    if type(capacity) is not int or not 2 <= capacity <= 8:
                        raise ProtocolError("invalid_capacity")
                    if type(flags) is not int or not 0 <= flags <= 0xffffffff:
                        raise ProtocolError("invalid_system_flags")
                    room.capacity = capacity
                    room.system_flags = flags
                    room.parameters = self.text(message, "parameters", 256)
                    room.password = self.text(message, "password", 32, required=False)
                self.next_room += 1
                self.rooms[room.ident] = room
            else:
                if kind == "quick_match":
                    room = next((r for r in self.rooms.values() if r.mode == mode and
                        r.mod_identity == player.mod_identity and not r.started and not r.password and
                        len(r.members) < (r.capacity or MODES[r.mode])), None)
                else:
                    ident = message.get("room")
                    if type(ident) is not int:
                        raise ProtocolError("invalid_room")
                    room = self.rooms.get(ident)
                if room is None:
                    raise ProtocolError("room_not_found")
                if room.mod_identity != player.mod_identity:
                    raise ProtocolError("mods_mismatch")
                if room.password and not secrets.compare_digest(
                        room.password.encode("utf-8"),
                        self.text(message, "password", 32, required=False).encode("utf-8")):
                    raise ProtocolError("wrong_password")
                if room.started or len(room.members) >= (room.capacity or MODES[room.mode]):
                    raise ProtocolError("room_unavailable")
                user_parameters = self.text(message, "user_parameters", 512, required=False)
                room.members.append(player.ident)
                player.user_parameters = user_parameters
            player.room = room.ident
            self.broadcast_room(room)
            return {"type": "joined", "room": self.describe(room, peers=True)}
        room = self.rooms.get(player.room)
        if room is None:
            raise ProtocolError("not_in_room")
        if kind == "kick":
            if player.ident != room.host:
                raise ProtocolError("host_only")
            member = message.get("member")
            if type(member) is not int or not 1 <= member <= 0xffffffff:
                raise ProtocolError("invalid_member")
            if member == room.host:
                raise ProtocolError("cannot_kick_host")
            if member not in room.members:
                target = self.players.get(member)
                if member == room.last_kicked_member and target is not None and target.room is None:
                    return {"type": "ok"}
                raise ProtocolError("member_not_found")
            target = self.players.get(member)
            if target is None or target.room != room.ident:
                raise ProtocolError("member_not_found")
            # Match ordinary leave semantics, including during a race timeout:
            # retain the target's authenticated connection and the host's room.
            self.leave(target)
            room.last_kicked_member = member
            self.send(target.writer, {"type": "room_closed", "reason": "kicked"})
            return {"type": "ok"}
        if kind == "update":
            # Original game settings remain opaque to matchmaking. The host
            # owns race parameters; each member owns only its own car/team data.
            user_parameters = (self.text(message, "user_parameters", 512, required=False)
                if "user_parameters" in message else player.user_parameters)
            parameters = None
            flags = room.system_flags
            if "parameters" in message:
                if player.ident != room.host:
                    raise ProtocolError("host_only")
                parameters = self.text(message, "parameters", 256)
            if "system_flags" in message:
                if player.ident != room.host:
                    raise ProtocolError("host_only")
                flags = message["system_flags"]
                if type(flags) is not int or not 0 <= flags <= 0xffffffff:
                    raise ProtocolError("invalid_system_flags")
            player.user_parameters = user_parameters
            room.system_flags = flags
            # Settings can have been composed before the start acknowledgement.
            # Only an explicit host finish request changes race admission.
            if parameters is not None:
                room.parameters = parameters
            self.broadcast_room(room)
            return {"type": "ok"}
        if kind == "ready":
            ready = message.get("ready")
            if type(ready) is not bool:
                raise ProtocolError("invalid_ready")
            if ready:
                room.ready.add(player.ident)
            else:
                room.ready.discard(player.ident)
            self.broadcast_room(room)
            return {"type": "ok"}
        if kind == "start":
            if player.ident != room.host:
                raise ProtocolError("host_only")
            if room.started:
                raise ProtocolError("already_started")
            if len(room.members) < 2 or not all(self.players[i].endpoint and
                    (room.mode == "original" or i in room.ready) for i in room.members):
                raise ProtocolError("players_not_ready")
            room.started = True
            self.broadcast_room(room)
            return {"type": "started"}
        if kind == "finish":
            if player.ident != room.host:
                raise ProtocolError("host_only")
            room.started = False
            room.ready.clear()
            self.broadcast_room(room)
            return {"type": "ok"}
        raise ProtocolError("unknown_command")

    async def accept(self, reader, writer):
        player = None
        try:
            hello = await self.read(reader, 10)
            if hello.get("type") != "hello":
                raise ProtocolError("hello_required")
            if type(hello.get("wire")) is not int or hello["wire"] != WIRE_VERSION:
                raise ProtocolError("protocol_mismatch")
            if hello.get("version") != self.version:
                self.send(writer, {"type": "error", "error": "version_mismatch", "required": self.version})
                await writer.drain()
                return
            name = self.text(hello, "name", 32)
            if self.release_gate is not None:
                try:
                    self.release_gate.check(hello)
                except ReleaseRejected as exc:
                    LOG.info("refused %s: no valid release credential (%s)", writer.get_extra_info("peername")[0], exc)
                    raise ProtocolError("release_required") from exc
            mod_identity, mod_manifest = self.parse_mods(hello)
            if self.authenticate:
                identity = await self.authenticate(hello, writer.get_extra_info("peername")[0])
                name = identity.name if isinstance(identity, AccountIdentity) else identity
                name = self.text({"name": name}, "name", 96)
                # The original frontend identifies its own roster row by name.
                # Two local instances may share a web account, but must receive
                # distinct names within its 15-byte native persona limit.
                base = name.encode("utf-8")[:15].decode("utf-8", errors="ignore") or "Driver"
                name = base
                serial = 2
                while any(p.name.casefold() == name.casefold() for p in self.players.values()):
                    suffix = f" ({serial})"
                    name = base.encode("utf-8")[:15 - len(suffix)].decode("utf-8", errors="ignore") + suffix
                    serial += 1
            if len(self.players) >= self.max_players:
                raise ProtocolError("server_full")
            player = Player(self.next_player, name, writer, writer.get_extra_info("peername")[0], mod_identity)
            player.mod_manifest = mod_manifest
            if self.authenticate and isinstance(identity, AccountIdentity):
                player.account_id = identity.account_id
                player.account_epoch = identity.epoch
                player.session_id = identity.session_id
            self.next_player += 1
            self.players[player.ident] = player
            self.send(writer, {"type": "welcome", "wire": WIRE_VERSION, "version": self.version,
                "id": player.ident, "token": player.token.hex(), "udp_port": self.udp_port,
                "name": name,
                "lobby": {"id": 1, "name": "Burnout 3", "limit": self.max_players,
                          "players": self.lobby_players()},
                "modes": MODES})
            await writer.drain()
            while True:
                message = await self.read(reader, self.idle_timeout)
                now = time.monotonic()
                while player.requests and player.requests[0] < now - 1:
                    player.requests.popleft()
                if len(player.requests) >= 30:
                    raise ProtocolError("rate_limit")
                player.requests.append(now)
                player.last_activity = now
                try:
                    request_id = message.get("request_id", 0)
                    if type(request_id) is not int or not 0 <= request_id <= 0xffffffff:
                        raise ProtocolError("invalid_request_id")
                    if message.get("type") == "service":
                        if self.services is None:
                            raise ProtocolError("service_unavailable")
                        reply = await self.services(player, message)
                    else:
                        reply = self.command(player, message)
                except ProtocolError as exc:
                    reply = {"type": "error", "error": str(exc)}
                if type(message.get("request_id")) is int and 0 < message["request_id"] <= 0xffffffff:
                    reply["request_id"] = message["request_id"]
                self.send(writer, reply)
                await asyncio.wait_for(writer.drain(), 10)
        except ProtocolError as exc:
            self.send(writer, {"type": "error", "error": str(exc)})
            try:
                await asyncio.wait_for(writer.drain(), 2)
            except (ConnectionError, OSError, asyncio.TimeoutError):
                pass
        except (EOFError, ConnectionError, OSError, asyncio.TimeoutError):
            pass
        finally:
            if player:
                self.leave(player)
                self.players.pop(player.ident, None)
            writer.close()
            try:
                await writer.wait_closed()
            except (ConnectionError, OSError):
                pass

async def main(args):
    context = None
    if bool(args.cert) != bool(args.key):
        raise ValueError("--cert and --key must be used together")
    if args.cert:
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.minimum_version = ssl.TLSVersion.TLSv1_2
        context.load_cert_chain(args.cert, args.key)
    gate = None
    if args.release_key_file:
        gate = ReleaseGate(load_key(args.release_key_file))
        LOG.info("only clients with the release credential are admitted")
    service = Matchmaking(max_players=args.max_players, release_gate=gate)
    port = await service.start(args.host, args.port, args.udp_port, context)
    LOG.info("Takedown v%s matchmaking TCP %s:%d, UDP rendezvous %d", GAME_VERSION, args.host, port, service.udp_port)
    try:
        await service.server.serve_forever()
    finally:
        await service.close()

if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--version", action="version", version="v" + GAME_VERSION)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=21800)
    parser.add_argument("--udp-port", type=int, default=21801)
    parser.add_argument("--max-players", type=int, default=256)
    parser.add_argument("--cert", type=Path)
    parser.add_argument("--key", type=Path)
    parser.add_argument("--release-key-file", type=Path, default=os.environ.get("TAKEDOWN_RELEASE_KEY_FILE"),
                        help="admit only clients holding this release credential (default: admit everyone)")
    args = parser.parse_args()
    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(message)s")
    try:
        asyncio.run(main(args))
    except KeyboardInterrupt:
        pass
