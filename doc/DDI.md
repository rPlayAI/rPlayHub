# The Developer Disk Image (DDI)

rPlayHub talks to the iPhone's developer services, and on iOS 17 and later those services only
answer once a **Developer Disk Image** is mounted on the phone. The phone forgets it on every
reboot, so it has to be mounted again each time.

**rPlayHub's engine mounts it for you** (`host-c/ddi.c`), with no Xcode and no Device Hub. What
it needs from you is the image files, and those are **Apple's software, which we cannot put in
this repository**. This page is how to get them.

## Do you need to do anything?

| Your setup | What to do |
|---|---|
| Mac with **Xcode or Device Hub** installed | Nothing. The engine uses Xcode's copy in `/Library/Developer/DeveloperDiskImages/iOS_DDI`. |
| **Linux**, or a Mac **without Xcode** | Download the files once, below. |
| Phone already connected to a Mac running Xcode or Device Hub **since its last reboot** | Nothing for now: the image is already mounted. You will need the files after the next reboot. |

## Download

From a clone of this repository:

```sh
./scripts/fetch-ddi.sh
```

That downloads three files (about 16 MB) into `~/.local/share/rplayhub/iOS_DDI/Restore/`, which
is where the engine looks. Then start the engine as usual; there is nothing to configure.

The files come from [doronz88/DeveloperDiskImage](https://github.com/doronz88/DeveloperDiskImage),
the public mirror that [pymobiledevice3](https://github.com/doronz88/pymobiledevice3) downloads
from. They are the personalized DDI that ships with Xcode, and are the same for every iPhone.

### Without the script

```sh
mkdir -p ~/.local/share/rplayhub/iOS_DDI/Restore
cd ~/.local/share/rplayhub/iOS_DDI/Restore
base=https://github.com/doronz88/DeveloperDiskImage/raw/main/PersonalizedImages/Xcode_iOS_DDI_Personalized
curl -fLO $base/BuildManifest.plist
curl -fLO $base/Image.dmg
curl -fLO $base/Image.dmg.trustcache
```

### From a Mac with Xcode

Copy the whole folder `/Library/Developer/DeveloperDiskImages/iOS_DDI` to the other machine, and
either put it at `~/.local/share/rplayhub/iOS_DDI` or point the engine at it:

```sh
RPLAY_DDI=/path/to/iOS_DDI ./host-c/cdhost
```

### Somewhere else

`./scripts/fetch-ddi.sh /some/dir` downloads into another folder; run the engine with
`RPLAY_DDI=/some/dir`. The folder must contain `Restore/BuildManifest.plist`.

## When the engine mounts it

Every time the engine starts, it checks whether an image is mounted, and mounts one if not. For
that the phone must be:

- **Unlocked.** The phone refuses the mount while locked. Unlock it and restart the engine.
- **In Developer Mode** (Settings → Privacy & Security → Developer Mode). Turning it on needs a
  reboot and a confirmation on the phone.
- **Paired** with this computer (the "Trust This Computer" prompt).

The computer needs **internet access** for the mount: the engine asks Apple's signing server
(`gs.apple.com`) for a ticket tied to your phone and this boot. No Apple ID is involved.

## What you will see

In the engine's output, under `Layer 3c: developer disk image`:

| Message | Meaning |
|---|---|
| `already mounted` | Nothing to do. |
| `mounted (no Xcode/Device Hub needed)` | The engine mounted it. |
| `the device is locked -- unlock it, then reconnect` | Unlock the phone and restart the engine. |
| `no DDI files: run scripts/fetch-ddi.sh ...` | The files were not found. See [Download](#download). |
| `could not mount the DDI (rc=-4)` | Apple's signing server could not be reached, or refused. Check the internet connection. |

## Newer iOS versions

A DDI covers a range of iOS versions, and a new iOS release can need a newer one. If mounting
fails after an iOS update, fetch again — the mirror tracks Xcode releases — or copy the folder
from an up-to-date Xcode.
