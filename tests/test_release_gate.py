#!/usr/bin/env python3
"""Release gate: proof format, rejection cases, and a real matchmaking handshake on localhost."""
import asyncio
import json
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "server"))
import matchmaking  # noqa: E402
import release_gate  # noqa: E402

KEY = ("0123456789abcdef" * 4).encode()
# The same vector is checked by tests/test_release_credential.cpp, so the client and the server agree.
VECTOR = "b5603a8a37224cb19235ad334ae277078ee30e6fce5201c41606425d1656ef49"


def expect(condition, message):
    if not condition:
        raise AssertionError(message)


MODS = "ab" * 32     # a mod identity is 64 hex digits


def hello(key, now, *, name="Driver", skew=0, version=None, mods="mods-abc"):
    version = version or matchmaking.GAME_VERSION
    message = {"type": "hello", "wire": matchmaking.WIRE_VERSION, "version": version, "name": name,
               "mods": mods, "mod_manifest": []}
    if key is not None:
        timestamp = int(now) + skew
        message["release_ts"] = timestamp
        message["release_proof"] = release_gate.release_proof(key, timestamp, version, matchmaking.WIRE_VERSION,
                                                              mods, name)
    return message


def test_unit():
    expect(release_gate.release_proof(KEY, 1700000000, "0.0.26", 1, "mods-abc", "Driver") == VECTOR,
           "proof differs from the shared vector")
    now = 1700000000.0
    gate = release_gate.ReleaseGate(KEY, clock=lambda: now)
    gate.check(hello(KEY, now))
    for label, message, reason in (
            ("no proof", hello(None, now), "missing"),
            ("wrong key", hello(b"x" * 64, now, name="Other"), "invalid"),
            ("old", hello(KEY, now, name="Old", skew=-5000), "stale"),
            ("future", hello(KEY, now, name="Future", skew=5000), "stale")):
        try:
            gate.check(message)
        except release_gate.ReleaseRejected as exc:
            expect(str(exc) == reason, f"{label}: {exc} != {reason}")
        else:
            raise AssertionError(f"{label}: accepted")
    try:
        gate.check(hello(KEY, now))     # the first hello again: a replay
    except release_gate.ReleaseRejected as exc:
        expect(str(exc) == "replayed", "replay reason")
    else:
        raise AssertionError("replay accepted")
    tampered = hello(KEY, now, name="Driver2")
    tampered["name"] = "Someone else"
    try:
        gate.check(tampered)
    except release_gate.ReleaseRejected as exc:
        expect(str(exc) == "invalid", "tampered name")
    else:
        raise AssertionError("tampered hello accepted")


def test_environment(tmp_dir):
    import os
    key_file = Path(tmp_dir) / "release.key"
    key_file.write_text(KEY.decode() + "\n")
    os.environ["TAKEDOWN_RELEASE_KEY_FILE"] = str(key_file)
    try:
        gated = matchmaking.Matchmaking()
        expect(isinstance(gated.release_gate, release_gate.ReleaseGate) and gated.release_gate.key == KEY,
               "the key file named by the environment enables the gate")
        os.environ["TAKEDOWN_RELEASE_KEY_FILE"] = str(Path(tmp_dir) / "missing.key")
        broken = matchmaking.Matchmaking()
        try:
            broken.release_gate.check(hello(KEY, 1700000000.0))
        except release_gate.ReleaseRejected as exc:
            expect(str(exc) == "unconfigured", "an unusable key file refuses every client")
        else:
            raise AssertionError("an unusable key file admitted a client")
    finally:
        os.environ.pop("TAKEDOWN_RELEASE_KEY_FILE", None)


async def exchange(port, message):
    reader, writer = await asyncio.open_connection("127.0.0.1", port)
    writer.write((json.dumps(message) + "\n").encode())
    await writer.drain()
    reply = json.loads(await asyncio.wait_for(reader.readline(), 5))
    writer.close()
    return reply


async def test_service():
    service = matchmaking.Matchmaking(release_gate=release_gate.ReleaseGate(KEY))
    port = await service.start("127.0.0.1", 0, 0)
    try:
        import time
        now = time.time()
        ok = await exchange(port, hello(KEY, now, name="Official", mods=MODS))
        expect(ok["type"] == "welcome", f"official client refused: {ok}")
        for label, message in (("no key", hello(None, now, name="Fork", mods=MODS)),
                               ("wrong key", hello(b"f" * 64, now, name="Fork2", mods=MODS))):
            reply = await exchange(port, message)
            expect(reply == {"type": "error", "error": "release_required"}, f"{label}: {reply}")
    finally:
        await service.close()
    # Without a gate (a fork's own server, or tests) everything is admitted.
    open_service = matchmaking.Matchmaking()
    port = await open_service.start("127.0.0.1", 0, 0)
    try:
        reply = await exchange(port, hello(None, 0, name="Anyone", mods=MODS))
        expect(reply["type"] == "welcome", f"ungated service refused a client: {reply}")
    finally:
        await open_service.close()


def main():
    import tempfile
    test_unit()
    with tempfile.TemporaryDirectory() as tmp_dir:
        test_environment(tmp_dir)
    asyncio.run(test_service())
    print("release gate tests passed")


if __name__ == "__main__":
    main()
