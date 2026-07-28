# Distribution — notarized DMG from our own site

`./scripts/package-dmg.sh` builds the app and produces `build/rPlayHub-<version>.dmg`.

```
# local testing (ad-hoc signed — Gatekeeper will block a downloaded copy)
./scripts/package-dmg.sh

# shippable
SIGN_ID="Developer ID Application: Your Name (TEAMID)" \
NOTARY_PROFILE=rplayhub \
./scripts/package-dmg.sh
```

One-time setup for the notary profile:

```
xcrun notarytool store-credentials rplayhub \
  --apple-id you@example.com --team-id TEAMID --password <app-specific-password>
```

The project already sets `ENABLE_HARDENED_RUNTIME = YES`, which notarization requires. The script
signs with `--timestamp --options=runtime`, verifies the signature, runs a Gatekeeper assessment,
builds the DMG, signs the DMG, submits it, waits, and staples the ticket.

**Do not skip notarization.** A downloaded, un-notarized app is blocked outright on current macOS —
not a "right-click to open" nag, a refusal. Stapling matters too, so the DMG validates even if the
user is offline when they first open it.

## Why not the Mac App Store

The App Store requires the sandbox, and the sandbox cannot do what this tool needs:

- **Creating the tunnel interface needs root.** The CoreDevice tunnel is raw IPv6 packets; to reach
  RSD and the services with ordinary sockets those packets have to be on a `utun` interface with a
  route. Creating one is privileged. A MAS app cannot run as root and cannot install a privileged
  helper — `SMJobBless` and `SMAppService` daemons are both disallowed for App Store apps.
- **Talking to `usbmuxd` means opening `/var/run/usbmuxd`, and the sandbox denies it.** Verified,
  not assumed: the same test binary in the same app bundle run by the same user gets
  `CONNECTED` unsandboxed and `EPERM` with `com.apple.security.app-sandbox` — the only difference
  being that entitlement. There is no public entitlement that restores it. (A first attempt at this
  test used a bare signed binary rather than a bundle; that is not a valid test — a non-bundled
  binary cannot initialize a sandbox container and dies with SIGTRAP whatever it does.)

So: **Developer ID + notarized DMG**, which allows both the helper and the direct socket access.

### The two changes, and exactly what each one buys

They are independent, they are both things we want for other reasons, and it is worth being precise
about which problem each one solves — because only one of them is needed to drop root, and only the
other one opens the App Store.

**1. A userspace TCP/IP stack over the tunnel** — *removes root, on every platform.*

Everything below the tunnel already runs unprivileged. Verified live against two phones: usbmux
enumeration, lockdown with a TLS session, reading the pair record, `StartService`, and the full
CDTunnel handshake all complete as a normal user with no entitlements. **The only privileged step in
the entire pipeline is putting the tunnel's IPv6 on a `utun` and routing to it.**

Replace that with a TCP/IP implementation over the tunnel socket and nothing needs root anywhere:
no interface, no route, no privileged helper, no driver. The tunnel already hands us raw IPv6
packets, so what is needed is UDP (trivial — the RTP video receive path) and TCP (the real work:
handshake, sequencing, retransmit, windows, teardown) for RSD and the per-service channels. The
16000-byte MTU makes this easier than it sounds. pymobiledevice3's `--userspace` tunnel is proof it
is tractable. This is also the answer for Windows, where the alternative is shipping a signed TUN
driver.

**2. The RemotePairing transport** — *removes the sandbox blocker, so it opens the App Store.*

Direct TCP to the phone over wifi with our own pairing handshake means no `/var/run/usbmuxd` to
open, which is the one thing the sandbox provably denies. A sandboxed app then needs only
`com.apple.security.network.client`, which we already declare. (Expect the macOS Local Network
privacy prompt for mDNS discovery — a user-visible consent dialog, not a blocker.)

So: **userspace stack alone → a DMG that never asks for a password.** Both together → a plausible
Mac App Store build. Neither is needed for what ships today.

## The process model this implies

```
rPlayHub.app          unprivileged, sandboxable, signed and notarized normally
   │  loopback: video :9877, control :9876
   ▼
engine                privileged — owns the tunnel interface and the CoreDevice stack
```

Keeping the privileged part out of the app is what makes the app signable and shippable without
special entitlements, and it is why the app is a *client* of the JSON-line API rather than a
reimplementation of it.

### DECIDED: one app, exactly like Device Hub

**One `.app` the user launches. The GUI runs unprivileged. The engine runs as root in a launchd
daemon embedded in the same bundle, registered with `SMAppService.daemon(plistName:)` and approved
once by the user in System Settings > Login Items.**

This is not a guess at Apple's design — it is measured. On this machine:

| process | user | role |
|---|---|---|
| `DeviceHub` | the logged-in user | the app; entitled `com.apple.private.coredevice.client` |
| `remoted` | **root** | launchd system daemon; creates the tunnel |
| `usbmuxd` | `_usbmuxd` | launchd system daemon; USB/wifi transport |

And `utun8` carries `fd12:5a0a:9360::2/64` at MTU 16000 — the same tunnel shape we negotiate,
created by the daemon, not by the app. Device Hub is one app to the user and two privilege domains
underneath. The only difference for us is that Apple's daemon ships with the OS while ours ships
inside our bundle.

What this means concretely:

- The localhost split we already have (video on :9877, control on :9876) is **kept, not undone** —
  it is what the app and daemon talk over. Merging is a packaging change, not a rewrite.
- The GUI never needs an entitlement we cannot get, and signs and notarizes normally.
- The daemon binary must live at `Contents/Library/LaunchDaemons/` and be signed with the same
  Team ID as the app.
- Running the GUI itself as root was rejected: it cannot ship as a double-clickable notarized app,
  and it would put a window server client in a root process for no benefit.

**Prerequisite, and it is the real work:** the daemon has to be the C engine, because Python is not
in the shipping path. `host-c/cdhost` currently reaches usbmux → lockdown → TLS → CDTunnel → utun →
pump → RSD-reachable. What it still lacks is everything above the tunnel: RemoteXPC over HTTP/2, the
XPC codec, RSD enumeration, the media-stream offer, and HID. Until that serves the API on those two
ports, registering a privileged helper would only get the user a System Settings prompt and still no
video — which is why the packaging step comes second, not first.

Today the engine is `host/mirror.py` started by `./scripts/live.sh`, which is the honest interim.

## Version bumping

`CFBundleShortVersionString` in `app/rPlayHub/Info.plist` is the version the DMG filename and volume
name are taken from. `MARKETING_VERSION` and `CURRENT_PROJECT_VERSION` in the project should be kept
in step with it.
