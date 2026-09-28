# Handoff — the RVRA bitrate experiment

Written 2026-08-22, closing out the 2026-08-02/03 session that built it. Everything below was
true at the end of that session; live state (which device is attached, whether a daemon is
running and to what) should be re-checked rather than assumed.

## State

Branch `rendering-resolution-switch`, 33 commits ahead of main, **nothing pushed**. Known-good
build in `builds/known-good-20260802-2349`. Mirroring is clean on the iPhone 13 Pro (iOS 27) over wifi; iOS 26 binds but cannot mirror.

## What got built

The instrument for the one RVRA lever never varied on its own — bitrate. Three pieces:

- **cdhost flags** (`host-c/cdhost.c`). `--rctl`, `--max-bitrate`, `--min-bitrate`, each with an
  env fallback (`RPLAY_RCTL` etc.). One table drives parsing, `--help` and the rebind re-exec
  together, so selecting a device in the app cannot silently drop an override — the failure mode
  where "the device ignored us" turns out to be "the flag never survived execv". Overrides are
  echoed at startup, so a daemon log is proof they applied.
- **`scripts/rvra-bitrate.py`** — the measurer. Reads the per-frame resolution trailer off the
  :9877 Annex-B broadcast; no decoder in the loop, since a decoder that keeps up is a confounder.
  Drives synthetic swipes through the control API (identical motion across runs) with still
  phases on both sides (a null result means nothing without knowing whether the phone was moving).
  `--self-test <capture>` proves the parser reports something before any null result is trusted:
  it finds 601 of 604 NALs carrying trailers on `build/devicehub-recording.h265`, independently
  reproducing the 601 figure quoted in `doc/RVRA-AND-PORTABILITY.md`.
- **`scripts/rvra-bitrate-run.sh` / `-all.sh`** — one condition / all five. The -all script kills
  and restarts cdhost five times (baseline, `--rctl 0`, `--rctl 800000`, `--rctl 20000000`,
  ceiling-high via streamConfig), refuses to run while rPlayHub or Device Hub is up (a second
  viewer splits the encoder budget), and prints a table plus the positive-control verdict.
  Results land in `build/rvra-bitrate/results.jsonl`, per-run daemon logs beside it.

## The command that has not been run

It restarts cdhost, and cdhost needs root for the utun, and sudo prompts for a password here —
so this is user-run. With the iPhone unlocked on the home screen:

```sh
sudo ./scripts/rvra-bitrate-all.sh
```

**Read the positive-control verdict before any other row.** Starving the target to 0.8 Mbit/s
should make downshifting *worse*. If rctl-low looks like baseline, the device is not acting on
RCTL at all and every other RCTL row is uninterpretable — not evidence bitrate is irrelevant,
evidence the knob never reached the encoder. In that case only `ceiling-high` carries
information, because it travels in streamConfig rather than RTCP. This ordering is the whole
design: it exists so the experiment cannot repeat the `RVRA1:0` mistake in a new variable
(we asked, nothing changed, we concluded the device ignores us — when actually our ask was
dropped before it ever arrived).

## Already known before it runs

Measured over three recorded captures (`scripts/rvra-bitrate.py --self-test`), from
`doc/RVRA-AND-PORTABILITY.md`:

| capture | kB/frame | below full tier | episodes |
|---|---|---|---|
| Device Hub's own session | 7.0 | **4.8%** | 2 |
| ours (`screen3.h265`) | 13.8 | **29.6%** | 6 |
| ours (`ours-ffmpeg.h265`) | 16.2 | **58.3%** | 19 |

Two consequences:

- The bitrate hypothesis starts weakened. Our stream already spends **2.3× Apple's bits per
  frame** and downshifts **twelve times as often**; rate starvation does not predict that shape.
  Run the experiment anyway — it is ten minutes and the answer is architectural — but expect it
  to fail.
- Option 2 (hide frames below full tier) stops being a safe default. At Apple's downshift rate it
  is two brief holds in twenty seconds — a product. At ours it is a picture frozen more than half
  the time — not one. So "accept the two-tier product" is contingent on closing this gap, not a
  fallback anyone can settle for today.

Confounder stated honestly: different sessions over different content, and motion provokes both
columns together. That is exactly why the instrument drives synthetic swipes. But the question
the table raises survives the confounder: **why does our negotiated session adapt more than
Device Hub's on the same phone?** Whatever the answer lives in what we negotiate, not in the
decoder afterwards — and unlike the bitrate ceiling it is a difference already measured rather
than a lever guessed at.

## After the result

- Positive control passed → read `rctl-high` and `ceiling-high` against baseline. If either holds
  full tier under identical motion, bitrate is the lever and the port stays portable without
  option 2. If neither does, adaptation is not rate-driven at these settings.
- Either way, the negotiation-difference question above becomes the next thread: diff our offer /
  streamConfig against what Device Hub sends (captures exist; `doc/DEVICEHUB-CAPTURE-FINDINGS.md`
  has the method). Making our session behave like Apple's makes option 2 acceptable even if the
  bitrate answer is negative.
- If the decision lands on option 2 anyway, its implementation notes are in
  `doc/RVRA-AND-PORTABILITY.md` §2 and `doc/RENDERING-HANDOFF.md`.

If Restart/Shutdown work happens instead (it is the cheapest real feature left, unblocked via
`com.apple.mobile.diagnostics_relay.shim.remote`), see `doc/RSD-SERVICES.md` first.

## Loose ends

- **Bug #7, stale tunnel** (AGENTS.md): the daemon keeps serving a dead session after its device
  leaves/rejoins wifi — `tunnel_info`/`stream_info` answer, displayservice connect times out,
  viewers get silence indistinguishable from an idle encoder. No liveness check exists;
  restarting cdhost is the fix. The measurer preflights exactly this and names it.
- **Corrupt scratch captures** in `build/` (untracked): `apple-notrailer.h265` is byte-identical
  to `devicehub-recording.h265` with all 601 trailers present; `ref-stripped.h265` stripped only
  at full tier, leaving untouched the exact below-full-tier frames stripping could matter for.
  Delete them rather than fix them.
- **sudo needs a password** on this machine, so anything restarting cdhost is user-run. Package
  restart-requiring steps as single commands.

## Kickstart prompt for the next session

Paste into a fresh session:

```
Read AGENTS.md first, then doc/BITRATE-EXPERIMENT-HANDOFF.md (handoff from the previous
session), doc/RVRA-AND-PORTABILITY.md before touching video, doc/RSD-SERVICES.md before adding
any device feature.

Project constraint: do not modify or add files outside ~/rPlayHub. Run recipe unchanged:
sudo ./host-c/cdhost (pick device in sidebar) + xcodebuild of app/rPlayHub.xcodeproj.

Facts:
1. The RVRA bitrate experiment is built and committed but has NOT been run -- it restarts cdhost
   five times and cdhost needs sudo, so it is user-run. iPhone unlocked on the home screen:
   sudo ./scripts/rvra-bitrate-all.sh
   Read the printed positive-control verdict before interpreting any other row; if it fails,
   only the ceiling-high row carries information. Results land in
   build/rvra-bitrate/results.jsonl with per-run daemon logs beside it.
2. Already measured from captures: our sessions spend 2.3x Apple's bits per frame and downshift
   twelve times as often (29.6%/58.3% of frames below full tier vs Device Hub's 4.8%). That
   weakens the bitrate hypothesis in advance AND makes option 2 (hide below-full-tier frames)
   contingent on closing the gap. Sharpest open question: why does our negotiated session adapt
   more than Device Hub's on the same phone? The difference lives in the negotiation.
3. Restart/Shutdown remain unblocked via com.apple.mobile.diagnostics_relay.shim.remote
   (doc/RSD-SERVICES.md) -- cheapest real feature left. iOS 26 binds but cannot mirror.
4. Known bug #7 (AGENTS.md): the daemon keeps serving a dead session after its device leaves
   wifi -- tunnel_info/stream_info answer, displayservice connect times out, viewers get
   silence. No liveness check; restarting cdhost is the fix.
5. Two corrupt scratch captures sit in build/: apple-notrailer.h265 is byte-identical to
   devicehub-recording.h265, and ref-stripped.h265 was stripped only at full tier. Delete them,
   don't fix them.

Method notes stand: faults present as "phone not sending video" and none were -- found by
reading logs for repetition; verify a tool reports something before trusting its silence
(python3 scripts/rvra-bitrate.py --self-test <capture> exists for exactly that).

Branch rendering-resolution-switch, 30+ commits, nothing pushed. Known-good build in
builds/known-good-20260802-2349. First task: if build/rvra-bitrate/results.jsonl exists,
analyze it against the decision tree in the handoff doc and update
doc/RVRA-AND-PORTABILITY.md with the outcome; otherwise ask me to run the experiment before
spending effort elsewhere.
```
