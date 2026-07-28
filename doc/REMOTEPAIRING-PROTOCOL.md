# RemotePairing protocol — implementable spec (from host-side symbol dump, 2026-07-27)

Source: `RemotePairing` (host macOS) export table. Addresses are file offsets in that binary — jump
straight to them in the disassembler. This is **Layer 1 (tunnel/pairing) of RE-MAP**, and it resolves
the make-or-break question for the CoreDevice-native proxy (ARCHITECTURE §0b).

## TL;DR — the make-or-break is answered, proxy is clean

- The **tunnel PSK is a fresh per-tunnel key the HOST generates** and hands the device over the
  already-authenticated+encrypted control channel (`requestTunnelBringup(usingKey:)` →
  `startListener(withKey:)`). It is NOT re-derived from the stored pairing record each time.
- What gates the tunnel is **control-channel authentication**: pair-verify (stored P-256) OR fresh
  manual pair-setup (PIN + consent) OR upgrade-lockdown. All three are non-attestation.
- Pairing keys are **generated in-process, no Secure-Enclave attestation**: `generatePairingKeyPair`
  (0xBA7C), `P256.Signing.PrivateKey.createKeyPair` (0x5110C), `CertificateUtilities.createTLSRawPublicKey`
  (0x50E74, RFC 7250 raw pubkey). Identity is self-asserted → **a proxy can generate its own identity
  and pair legitimately.**
- Crypto family = SRP + Curve25519/P256 + HKDF + ChaCha20-Poly1305 — same as our RCS/AirPlay work.

## Layer 2 — discovery (Bonjour)

`BonjourService` enum (nominal type desc 0x14DDD4) → the four+one service types:
| enum case | address | Bonjour service |
|---|---|---|
| `.default`      | 0x133068 | `_remotepairing._tcp` |
| `.pairSetup`    | 0x133064 | `_remotepairing-manual-pairing._tcp` |
| `.pairableHost` | 0x133074 | `_remotepairing-pairable-host._tcp` |
| `.tunnel`       | 0x13306C | `_rp-tunnel._tcp` |
| `.udpTunnel`    | 0x133070 | QUIC/UDP tunnel variant |

TXT-record keys = `NetworkPairingKeys` (0x128818): `identifier` (0x1284C4), `authTag` (0x1284E0),
`flags` (0x12851C), `model` (0x128540), `name` (0x128530), `wireProtocolVersion` (0x1284F8),
`minimumSupportedWireProtocolVersion` (0x128508).

**authTag = recognize-a-paired-device-without-connecting.** `DiscoveredBonjourAdvert.authTag` (0xDE55C)
is a keyed MAC over the peer identity. Host resolves it via
`PairingDataStorageProvider.resolve(authTag:for:type:)` → matches a stored `CUPairedPeer`. Device side
generates it: `CUPairingIdentity.authTag(for:type:)` (0x534F4); verify `CUPairedPeer.verify(authTag:data:type:)`
(0x3EF28). Enables anonymized adverts (UDID not in cleartext):
`supportsAnonymizedWirelessPairingAdverts`.

Browser: `BonjourBrowser` (0xE314) with `includePeerToPeer` + `mode` (onDemand/passive).
`ConnectableDeviceBrowser.startBrowsing()` (0x5CF00) is the higher-level API.

## Control channel

`ControlChannelConnection` (0x8DB40) over a pluggable `ControlChannelTransport`:
- `NWConnectionControlChannelTransport` (0xAEB14) — network (Bonjour endpoint)
- `RemoteXPCControlChannelTransport` (0xAB83C) — over remoted XPC (the USB/remoted path)
- `BluetoothLEConnectionControlChannelTransport` (0x121868) — BLE
- `JSONDataBasedControlChannelTransport` (0x2B43C) — JSON-encoded variant

### Wire framing
`ControlChannelMessageEnvelope` (0x10C708): `{ originatedBy, sequenceNumber, message }`.
`Message` is `.plain` (0x13524C) or `.streamEncrypted` (0x135248 → `StreamEncryptedMessage.encryptedData`
0x11641C). Pre-auth = plain; post-auth = streamEncrypted (ChaCha20-Poly1305 stream).
Body serialization: **OPACK** — `OPACKEncoderCreateData(from:)` (0xBBC8) / `OPACKDecode(data:)` (0xBC98)
— all the Codable structs (`encode(to:)`/`init(from:)`) marshal through OPACK (or JSON on that transport).

### Message set (`ControlChannelMessage`, 0x113800)
- **Request** (0x110274): `handshake`, `createListener`, `peerInfo`, `remoteUnlock`, `cancelPairSetup`,
  `createRemoteUnlockKey`
- **Response** (0x111DB4): `handshake`, `createListener`, `peerInfo`, `error`, `errorExtended`,
  `remoteUnlock`, `createRemoteUnlockKey`, `success`
- **Event** (0x10E41C): `pairingData`, `deviceInfoUpdated`, `pairingRejected`, `pairingRejectedWithError`,
  `pairVerifyFailed`, `awaitingUserConsent`, `ping`, `unpair`, `connectionRejected`

### State machine (`ControlChannelConnection.State`, 0xF8500)
`notStarted → transportStarting → handshakeInProgress → { preparingPairingSession,
setUpManualPairingInProgress, verifyManualPairingInProgress, upgradeLockdownPairingInProgress,
deviceRequestUserPairingConsentInProgress, deviceAwaitingPairVerify, hostRequestPeerInfoInProgress }
→ authenticated | unauthenticated | invalidated`

### Options
- `Options.Device` (0x8B398): `allowsPairSetup`, `allowsPinlessPairing`, `allowsIncomingTunnelConnections`,
  `allowsUpgradeOfLockdownPairings`, `allowsSharingSensitiveInfo`
- `Options.Host` (0x8AC18): `attemptPairVerify`

## Handshake → auth

1. **Handshake** — `HandshakeRequest` (0x114532) / `HandshakeResponse` (0x114C34). Negotiates wire
   protocol version (`ControlChannelConnectionWireProtocolVersion`, feature getters at 0x557xx–0x559xx:
   `supportsQUICUDPProtocol`, `supportsMigratableTunnels`, `supportsDeviceInitiatedNetworkPairing`,
   `supportsRemoteUnlock`, …).
2. **Auth, one of:**
   - **Pair-setup (manual):** states setUp→verify. `PairingData.Kind.setupManualPairing` (0x1353F0) /
     `.verifyManualPairing` (0x1353EC). SRP PIN via `AttemptPairingPinCommand.pinAttempt` (0x22B0C),
     `PairingChallengeEvent{lastAttemptIncorrect, throttleSeconds}` (0x22F70), fixed PIN
     `Defaults.deviceFixedNetworkPairingPin`. Consent: `deviceRequestUserPairingConsentInProgress`,
     event `awaitingUserConsent`, `PairingConsentCollectionOutcome` (0x131C90:
     rejected/promptingUserForConsent/challengeRequired/consentNotRequired/userConsented). On success both
     sides exchange long-term pubkeys → `PairingDataStorageProvider.save(peer:)` stores a `CUPairedPeer`.
   - **Pair-verify (already paired):** `Options.Host.attemptPairVerify`, state `deviceAwaitingPairVerify`.
     ECDH over stored P-256 keys → session keys → `authenticated`.
   - **Upgrade lockdown:** `PairingData.Kind.upgradeAutomationLockdownPairing` (0x1353F8) /
     `.upgradeNonAutomationLockdownPairing` (0x1353F4). Promote an existing USB lockdown record. Gated by
     `allowsUpgradeOfLockdownPairings`, `Defaults.hostUseLockdownForPairSetup`,
     `hostProactivelyUpgradeLockdownPairingRecords`.
   - **Promptless automation:** `initiatePromptlessPairing(forAutomationRecord:)` (0xA61E0) +
     `CUPairedPeer.isPairedForAutomation` (0x3E9B8) + `remotePairingAutomationRecordKey` (0x3D61C). No PIN
     prompt once an automation record exists → the CI/agent path.
3. Post-auth: `PeerDeviceInfo` (0x115A64) exchanged via `peerInfo` request/response
   (`{identifier,name,model,udid,ecid,deviceKVSData}`).

Keypair/identity primitives: `generatePairingKeyPair` (0xBA7C), `P256.Signing.PrivateKey.createKeyPair`
(0x5110C), `CertificateUtilities.createTLSRawPublicKey(with:)` (0x50E74) →
`RawTLSPublicKeyInfo{identity, publicKeyDERData}`, `InProcessIRKGenerator.generateIRKSync` (0xBB6C, BLE IRK).
Storage: `PairingDataStorageProvider` (proto desc 0x14BDB4): `copyOrCreateSelfIdentity`, `fetchPeer(matching:)`,
`generateAuthTag(for:)`, `resolve(authTag:for:type:)`, `save(peer:)`, `numPairedPeers`.

## Tunnel bringup (the payload)

Over the authenticated (encrypted) control channel:
```
host   ControlChannelConnection.requestTunnelBringup(usingKey:transportProtocolType:peerConnectionsInfo:onComplete:)  0xA6284
        → Request.createListener
device TunnelListenerCreator.startListener(peerToPeer:withKey:transportProtocolType:peerConnectionsInfo:…)            proto 0x14CD08
        → Response.createListener = ListenerStartedResponse{ port, deviceRawPublicKey, serviceName }                  0x115E64
host   connect tunnel to `port`:
        - QUICTunnelConnection (0x44C68)  when supportsQUICUDPProtocol / .udpTunnel
        - TCPTunnelConnection  (0x40E60)  otherwise
        TLS server identity pinned to deviceRawPublicKey (RFC 7250); session secured by `withKey`.
```
`transportProtocolType` = `TransportProtocolType` {quic, tcp, udp} (0x570EC).
`TunnelProtocolSecurityOptions` {quic, tcp} (0x14DF6C) carries the security params.

Tunnel handshake inside the connection: `TunnelMessage` (0x87F50) —
`clientHandshakeRequest` / `serverHandshakeResponse` / `additionalConnectionHello` /
`additionalConnectionResponse` (migratable/multipath tunnels). MTU etc. negotiated.

Then IP packets flow: `TunnelPacket` (0xFD20C, mostly IPv6 — `IPv6Header` 0x124960) over a utun-like
interface `VirtualInterface` (0x2D01C) / `SkywalkChannelVirtualInterface` (0x34684). Managed by
`NetworkTunnelManager` (0xE8AC8) / `TunnelEndpoint` (0xBCA34). Establish server side:
`establishServerTunnel(localParameters:remoteAddress:)` → `finishEstablishingServerTunnel(serverRSDPort:)`.

Inside the tunnel: **RSD on `serverRSDPort`**, then services at the tunnel IPv6 addr. Service names
(`RemoteServiceNames` 0xDC008): `tunnelService` (0xDBDD0), `lockdownService` (0xDBDE8),
`deviceComputeService` (0xDBE00). `RSDDeviceInfo{name, uuid}` (0xF5C18) is the identity presented via RSD.

## Proxy recipe (ARCHITECTURE §0b, now concrete)

1. **Advertise** `_remotepairing._tcp` (`BonjourService.default`) with TXT
   {identifier, authTag, model, name, wireProtocolVersion} on the client LAN.
2. **Accept** a `ControlChannelConnection` from the Mac's remoted, DEVICE role,
   `Options.Device{allowsPairSetup:true, allowsUpgradeOfLockdownPairings:true, allowsIncomingTunnelConnections:true}`.
3. **Authenticate** — first time: manual pair-setup (fixed PIN via `deviceFixedNetworkPairingPin`, or
   promptless once an automation record exists). Generate our own P-256 identity
   (`generatePairingKeyPair`) and store a self `CUPairedPeer`. Subsequent connects: pair-verify.
4. **On `requestTunnelBringup`** → `startListener(withKey:)`: return OUR `deviceRawPublicKey` + a port we
   listen on. Terminate the QUIC/TCP TLS-PSK tunnel locally.
5. **Relay** the inner IP/RSD/lockdown/deviceCompute traffic to the remote phone (proxy is separately,
   really paired with the phone over its own CoreDevice/USB link).
6. **Present the phone's RSD identity** inside the tunnel (RSDDeviceInfo, PeerDeviceInfo udid/ecid) so
   Xcode sees the phone, not the proxy.

Two independent pairings — Mac↔proxy (proxy's generated identity) and proxy↔phone (real) — bridged.
No entitlement, no USB emulation, no daemon interception, no attestation. SIP-on, root-free.

## Open items to confirm with live capture (cdcap2 harness ready)
- Exact SRP variant + HKDF salt/info strings for pair-setup (grep the `_CryptoHKDF` callers).
- The `withKey` length/type and how it maps to the tunnel TLS-PSK (raw 32B? wrapped?).
- OPACK schema of `HandshakeRequest`/`ListenerStartedResponse` on the wire (capture one bringup).
- Whether remoted cross-checks the RSD identity we present against anything beyond the pairing
  (i.e., does presenting the phone's udid/ecid from a proxy-paired peer get accepted downstream).
