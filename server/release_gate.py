"""Release credential check for the official matchmaking service.

Official release builds ship a secret (`release.key`) that builds made from source do not have. The client
signs its `hello` message with it and the service refuses hellos without a valid signature, so only
official releases can use the official service. A fork can still run its own server (`matchmaking.py`
without a key) and point its clients at it with TAKEDOWN_MATCHMAKING_HOST.

The proof is HMAC-SHA256 keyed by the release key over the same message layout as the account proof
(`game_login_proof` in src/net/browser_auth.cpp), with the ticket field set to "release:<unix seconds>":

    B3-GAME-AUTH \\n release:<ts> \\n <version> \\n <wire> \\n <mods> \\n <name>

A key in a client can be extracted by someone who has a release, so this keeps casual forks out and does
not replace server-side limits. See docs/RELEASING.md.
"""
from __future__ import annotations

import hashlib
import hmac
import time
from pathlib import Path

LABEL = "B3-GAME-AUTH"
MAX_SKEW = 900          # seconds a hello's timestamp may differ from the server's clock
MIN_KEY_LENGTH = 32


class ReleaseRejected(Exception):
    """The hello carries no valid release proof."""


def load_key(path: Path | str) -> bytes:
    key = Path(path).read_text(encoding="ascii").strip().encode("ascii")
    if len(key) < MIN_KEY_LENGTH:
        raise ValueError("release key is too short")
    return key


def release_proof(key: bytes, timestamp: int, version: str, wire: int, mods: str, name: str) -> str:
    message = f"{LABEL}\nrelease:{timestamp}\n{version}\n{wire}\n{mods}\n{name}".encode("utf-8")
    return hmac.new(key, message, hashlib.sha256).hexdigest()


class ReleaseGate:
    def __init__(self, key: bytes | None, *, skew: int = MAX_SKEW, clock=time.time):
        # key None (an unreadable key file) refuses every client instead of admitting them all.
        self.key = key
        self.skew = skew
        self.clock = clock
        self.seen: dict[str, float] = {}

    def check(self, hello: dict) -> None:
        if self.key is None:
            raise ReleaseRejected("unconfigured")
        timestamp = hello.get("release_ts")
        proof = hello.get("release_proof")
        if type(timestamp) is not int or type(proof) is not str or len(proof) != 64:
            raise ReleaseRejected("missing")
        now = self.clock()
        if abs(now - timestamp) > self.skew:
            raise ReleaseRejected("stale")
        expected = release_proof(self.key, timestamp, str(hello.get("version", "")), int(hello.get("wire", 0)),
                                 str(hello.get("mods", "")), str(hello.get("name", "")))
        if not hmac.compare_digest(expected, proof.lower()):
            raise ReleaseRejected("invalid")
        # A captured hello cannot be replayed inside the clock window.
        self.seen = {p: until for p, until in self.seen.items() if until > now}
        if proof in self.seen:
            raise ReleaseRejected("replayed")
        self.seen[proof] = now + 2 * self.skew
