# Releases and the official online service

Builds made from this repository cannot use the official matchmaking service. Only official releases
carry a secret, the **release credential**, and the service refuses clients without it. Forks can still
run their own server (`server/matchmaking.py`, with no key) and point their clients at it with
`TAKEDOWN_MATCHMAKING_HOST` and `TAKEDOWN_MATCHMAKING_PORT`.

## How it works

- **Client** (`src/net/release_credential.cpp`): if a file named `release.key` is found, the client adds
  `release_ts` (unix seconds) and `release_proof` to its `hello` message. The proof is HMAC-SHA256 keyed by
  the key over the same message layout as the account proof, with the ticket set to `release:<timestamp>`,
  so it also covers the version, wire version, mod identity and player name. With no key file nothing is
  sent; a source build has no key file.
- **Server** (`server/release_gate.py`, used by `server/matchmaking.py`): with a key configured, a `hello`
  without a valid proof is answered with `release_required` and the connection is closed. The timestamp
  must be within 15 minutes of the server's clock, and a proof can be used once inside that window.
- **Where the client looks** for the key, in order: the file named by `TAKEDOWN_RELEASE_KEY_FILE`,
  `release.key` beside the executable, in the working directory, and in the launcher's active runtime
  `bin/` folder (`<install>/.launcher/<runtime>/bin`, found through `.launcher/runtime.json`).

The key is never in the repository. `release.key` and `*.key` are gitignored.

## Setting it up

1. Generate the key once and keep it outside the repository (a password manager, or a private folder):

   ```sh
   python3 tools/release_key.py generate ~/.config/takedown-release/release.key
   ```

2. Add it to every official release, in the runtime staging folder right before the archive is made.
   This is the only place it gets into a package:

   ```sh
   python3 tools/release_key.py install <runtime-staging-dir> ~/.config/takedown-release/release.key
   ```

3. Give the same file to the matchmaking service, **after** a release that carries it is out:

   ```sh
   python3 server/matchmaking.py --release-key-file /etc/takedown/release.key ...
   # or set TAKEDOWN_RELEASE_KEY_FILE
   ```

   If the service is started by other code (the website's wrapper), setting `TAKEDOWN_RELEASE_KEY_FILE` in
   its environment is enough: `Matchmaking(...)` reads it when no gate is passed. A key file that cannot be
   read makes the service refuse every client rather than admit them all. The wrapper can also pass
   `release_gate=ReleaseGate(load_key(path))` itself.

   If your website has its own sign-in endpoints, have them verify the same proof with
   `release_gate.ReleaseGate.check` so a fork cannot sign in either.

`tools/release_key.py fingerprint <file>` prints a short hash, to confirm the server and a package use the
same key without showing it.

## Rolling out

Order matters, because a server that enforces the key refuses every client without it, including every
release that is already installed:

1. Ship a release whose game build contains `release_credential.cpp` and whose package contains the key.
2. Wait until the players you care about have updated.
3. Start the service with `--release-key-file`.

Until step 3 the service stays open, which is also how tests and local development servers behave.

## Limits

A key inside a client can be extracted by anyone who has a release, so this stops forks and casual
copies, not a determined attacker. Keep the usual server-side limits (rate limits, bans, version checks).
To invalidate a leaked key, generate a new one, ship a release with it and restart the service with the
new file; older releases stop working.
