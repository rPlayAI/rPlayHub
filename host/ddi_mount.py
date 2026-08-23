#!/usr/bin/env python3
"""Mount the personalized Developer Disk Image (iOS 17+) without Xcode or Device Hub.

Prototype of the flow the engine will own (app/DISTRIBUTION.md, TestFlight plan step 3):

    1. mobile_image_mounter: QueryPersonalizationIdentifiers, QueryNonce
    2. pick the BuildIdentity in the DDI's BuildManifest matching the phone's chip and board
    3. TSS request to Apple's signing server -> ApImg4Ticket bound to this ECID and nonce
    4. ReceiveBytes (the .dmg) then MountImage (ticket + trust cache)

Cribbed from pymobiledevice3 (tss.py, mobile_image_mounter.py) and libimobiledevice's libtatsu.
Benign up to step 3; `mount` changes the phone (it refuses if an image is already mounted).

Usage (daemon running, tunnel up):
    python3 host/ddi_mount.py status            # what is mounted
    python3 host/ddi_mount.py identity          # steps 1-2
    python3 host/ddi_mount.py ticket [out.plist] # steps 1-3, ask Apple, save the ticket
    python3 host/ddi_mount.py mount             # all four steps
    python3 host/ddi_mount.py unmount
Set RPLAY_DDI to the DDI directory (default /Library/Developer/DeveloperDiskImages/iOS_DDI).
"""
import os
import plistlib
import socket
import sys
import urllib.request
import uuid

sys.path.insert(0, __file__.rsplit("/", 1)[0])
import rsd
import diagnostics_relay as dr

SERVICE = "com.apple.mobile.mobile_image_mounter.shim.remote"
TSS_URL = "https://gs.apple.com/TSS/controller?action=2"
DDI_DIR = os.environ.get("RPLAY_DDI", "/Library/Developer/DeveloperDiskImages/iOS_DDI")


def open_mounter():
    info = dr.rpc("tunnel_info")["result"]
    addr = info["device_addr"]
    port = rsd.service_port(rsd.connect(addr, info["rsd_port"]), SERVICE)
    s = socket.create_connection((addr, port), timeout=60)
    dr.rsd_checkin(s)
    return s


def cmd(s, **req):
    r = dr.request(s, req)
    if r.get("Error"):
        raise RuntimeError(f"{req.get('Command')}: {r}")
    return r


def status(s):
    r = cmd(s, Command="LookupImage", ImageType="Personalized")
    return bool(r.get("ImageSignature"))


def identity(s):
    ids = cmd(s, Command="QueryPersonalizationIdentifiers", PersonalizedImageType="DeveloperDiskImage")
    ids = ids["PersonalizationIdentifiers"]
    nonce = cmd(s, Command="QueryNonce", PersonalizedImageType="DeveloperDiskImage")["PersonalizationNonce"]
    return ids, nonce


def load_manifest():
    path = os.path.join(DDI_DIR, "Restore", "BuildManifest.plist")
    with open(path, "rb") as f:
        return plistlib.load(f)


def pick_identity(manifest, ids):
    chip, board = int(ids["ChipID"]), int(ids["BoardId"])
    for bi in manifest["BuildIdentities"]:
        if int(bi["ApChipID"], 16) == chip and int(bi["ApBoardID"], 16) == board:
            return bi
    raise RuntimeError(f"no BuildIdentity for ChipID {chip:#x} BoardId {board:#x}")


def tss_request(bi, ids, nonce):
    """The signing request Apple's server answers with an ApImg4Ticket, built exactly as
    libimobiledevice's ideviceimagemounter + libtatsu build it (tss_request_add_ap_tags,
    _common_tags, _ap_img4_tags). The first attempt, written from memory, got STATUS=8 back;
    the differences that mattered: no @BBTicket, EPRO/ESEC on entries without
    RestoreRequestRules, SepNonce, PearlCertificationRootPub from the identity, UID_MODE false -- found by diffing against
    a request built with libtatsu itself (scratch tssdump.c)."""
    req = {
        "@HostPlatformInfo": "mac",
        "@VersionInfo": "libauthinstall-1049.100.23",
        "@UUID": str(uuid.uuid4()).upper(),
    }
    # Ap,* identifiers the device reports go in verbatim (none on the iPhone 12, some on newer).
    for k, v in ids.items():
        if k.startswith("Ap,"):
            req[k] = v
    production, security, img4 = True, True, True
    params = {"ApProductionMode": production, "ApSecurityMode": security, "ApSupportsImg4": img4,
              "ApInRomDFU": False}
    # tss_request_add_ap_tags: one entry per manifest component.
    for name, entry in bi["Manifest"].items():
        info = entry.get("Info")
        if info is None or info.get("IsFTAB"):
            continue
        trusted = bool(entry.get("Trusted"))
        rules = info.get("RestoreRequestRules")
        if img4 and not rules and not trusted:
            continue
        e = {k: v for k, v in entry.items() if k != "Info"}
        if rules:
            for rule in rules:
                ok = True
                for ck, cv in rule["Conditions"].items():
                    actual = {"ApRawProductionMode": production, "ApCurrentProductionMode": production,
                              "ApRawSecurityMode": security, "ApRequiresImage4": img4,
                              "ApInRomDFU": False}.get(ck)
                    if actual is None or actual != cv:
                        ok = False
                        break
                if ok:
                    for ak, av in rule["Actions"].items():
                        if isinstance(av, bool):
                            e[ak] = av
        elif img4:
            e["EPRO"] = production
            e["ESEC"] = security
        if trusted and "Digest" not in e:
            e["Digest"] = b""
        if e:
            req[name] = e
    # tss_request_add_common_tags
    req["ApECID"] = int(ids["UniqueChipID"])
    req["UniqueBuildID"] = bi["UniqueBuildID"]
    req["ApChipID"] = int(bi["ApChipID"], 16)
    req["ApBoardID"] = int(bi["ApBoardID"], 16)
    if "ApSecurityDomain" in bi:
        req["ApSecurityDomain"] = int(bi["ApSecurityDomain"], 16)
    # tss_request_add_ap_img4_tags
    for k in ("Ap,OSLongVersion", "Ap,OSReleaseType", "Ap,ProductMarketingVersion", "Ap,ProductType",
              "Ap,SDKPlatform", "Ap,Target", "Ap,TargetType", "Ap,Timestamp"):
        if k in bi:
            req[k] = bi[k]
    req["ApNonce"] = nonce
    req["@ApImg4Ticket"] = True
    req["ApSecurityMode"] = security
    req["ApProductionMode"] = production
    req["SepNonce"] = bytes(20)
    if "PearlCertificationRootPub" in bi:
        req["PearlCertificationRootPub"] = bi["PearlCertificationRootPub"]
    req["UID_MODE"] = False
    return req


def tss_ask(req):
    body = plistlib.dumps(req)
    hdrs = {"Content-Type": "text/xml; charset=\"utf-8\"",
            "User-Agent": "InetURL/1.0", "Cache-Control": "no-cache", "Expect": ""}
    with urllib.request.urlopen(urllib.request.Request(TSS_URL, body, hdrs), timeout=60) as resp:
        text = resp.read().decode("utf-8", "replace")
    fields = dict(kv.split("=", 1) for kv in text.split("&") if "=" in kv)
    if fields.get("STATUS") != "0":
        raise RuntimeError(f"TSS refused: {fields.get('STATUS')} {fields.get('MESSAGE')}")
    return plistlib.loads(fields["REQUEST_STRING"].encode())


def image_digest(bi):
    import hashlib
    path = os.path.join(DDI_DIR, "Restore", bi["Manifest"]["PersonalizedDMG"]["Info"]["Path"])
    with open(path, "rb") as f:
        return hashlib.sha384(f.read()).digest()


def get_ticket(s):
    """The phone keeps the manifest it last accepted for an image, keyed by the image's SHA-384;
    ideviceimagemounter asks for it first and only goes to Apple when there is none (seen in its
    debug log 2026-08-23). Same here: no network, no nonce dance, when the phone already knows
    this image."""
    ids, nonce = identity(s)
    bi = pick_identity(load_manifest(), ids)
    r = dr.request(s, {"Command": "QueryPersonalizationManifest", "PersonalizedImageType": "DeveloperDiskImage",
                       "ImageType": "DeveloperDiskImage", "ImageSignature": image_digest(bi)})
    if r.get("ImageSignature"):
        print("using the personalization manifest the phone already holds")
        return bi, r["ImageSignature"]
    reply = tss_ask(tss_request(bi, ids, nonce))
    return bi, reply["ApImg4Ticket"]


def mount(s):
    if status(s):
        print("a personalized image is already mounted; nothing to do")
        return
    bi, ticket = get_ticket(s)
    dmg_path = os.path.join(DDI_DIR, "Restore", bi["Manifest"]["PersonalizedDMG"]["Info"]["Path"])
    tc_path = os.path.join(DDI_DIR, "Restore", bi["Manifest"]["LoadableTrustCache"]["Info"]["Path"])
    with open(dmg_path, "rb") as f:
        dmg = f.read()
    with open(tc_path, "rb") as f:
        trust_cache = f.read()
    # A locked phone refuses the mount by closing this connection with no reply
    # ("Device is locked, can't mount" -- confirmed against ideviceimagemounter). Turn the raw
    # ConnectionError into the actionable message.
    try:
        r = cmd(s, Command="ReceiveBytes", ImageType="Personalized", ImageSize=len(dmg), ImageSignature=ticket)
    except ConnectionError:
        sys.exit("mount refused -- unlock the device (enter its passcode) and try again")
    if r.get("Status") != "ReceiveBytesAck":
        raise RuntimeError(f"ReceiveBytes: {r}")
    s.sendall(dmg)
    r = dr._recv_plist(s)
    if r.get("Status") != "Complete":
        raise RuntimeError(f"upload: {r}")
    # Field set and order as ideviceimagemounter sends it (its -d log, 2026-08-23); ImagePath is
    # the staging file the upload landed in.
    r = cmd(s, Command="MountImage", ImagePath="/private/var/mobile/Media/PublicStaging/staging.dimage",
            ImageSignature=ticket, ImageType="Personalized", ImageTrustCache=trust_cache)
    if r.get("Status") != "Complete":
        raise RuntimeError(f"MountImage did not complete: {r}")
    print("MountImage: Complete")


def main():
    action = sys.argv[1] if len(sys.argv) > 1 else "status"
    s = open_mounter()
    try:
        if action == "status":
            print("Personalized DDI mounted:", status(s))
        elif action == "identity":
            ids, nonce = identity(s)
            print({k: v for k, v in ids.items()}, "nonce", nonce.hex())
            bi = pick_identity(load_manifest(), ids)
            print("identity:", bi["Info"].get("DeviceClass"), bi["Manifest"]["PersonalizedDMG"]["Info"]["Path"])
        elif action == "ticket":
            bi, ticket = get_ticket(s)
            print(f"ApImg4Ticket: {len(ticket)} bytes for {bi['Info'].get('DeviceClass')}")
            if len(sys.argv) > 2:
                with open(sys.argv[2], "wb") as f:
                    plistlib.dump({"ApImg4Ticket": ticket}, f)
        elif action == "mount":
            mount(s)
        elif action == "unmount":
            print(cmd(s, Command="UnmountImage", MountPath="/System/Developer"))
        else:
            sys.exit(__doc__)
    finally:
        s.close()


if __name__ == "__main__":
    main()
