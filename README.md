# Delta DQ9 Local Multiplayer PoC

An experimental cross-platform bridge that connects **Dragon Quest IX local wireless** between an iPhone build of Delta and a desktop melonDS LAN host.

On 2026-08-27, the iPhone client authenticated, associated as AID 1, entered the host's multiplayer world, and exchanged continuous melonDS MP command/reply/ack frames. Performance still needs tuning; this repository documents the working proof of concept rather than a finished consumer release.

On 2026-09-04, the same iPhone build also completed multiplayer communication with a Windows-hosted melonDS session. The proof of concept is therefore no longer limited to the original macOS test environment.

## What this adds

- An ENet-compatible melonDS 1.1 LAN client for `MelonDSDeltaCore`.
- Wiring for Delta's previously empty `Platform::MP_*` bridge functions.
- A LAN-only MAC translation so two emulators may start from the same firmware MAC without changing saves or firmware.
- Wi-Fi channel learning and active-player recovery for compatibility with the newer LAN protocol.
- Retries for authentication and association frames, compensating for melonDS 1.1's 16 ms stale-packet window.
- A small Wi-Fi receive-window backport for the older melonDS core embedded in Delta.
- Audio underrun handling that emits silence instead of stopping iOS RemoteIO during multiplayer synchronization stalls.

## Layout

- `src/MPLANClient.*` — the iOS-side ENet LAN implementation.
- `patches/melonds-delta-core.patch` — Xcode project and bridge integration.
- `patches/melonds-wifi.patch` — older melonDS Wi-Fi timing compatibility.
- `patches/delta-core-audio.patch` — iOS audio underrun recovery.
- `docs/architecture.md` — protocol flow and the debugging result.

## Reproduction baseline

The proof of concept was built against:

- Delta: `c1d3d068e019e6493eed45654569db3cc5beb86a`
- MelonDSDeltaCore: `eb9b07ec17307cfd4986cb607f8ee5f6492d73cb`
- Delta's embedded melonDS: `a53f9bab7b5c06efcbc6ecdf30ac0367555995aa`
- DeltaCore: `633dfa86967816315fe19b482511dab1ce517f28`
- ENet: `1.3.18`
- Original LAN host: official melonDS `1.1` for macOS
- Additional validated host platform: Windows

Copy `MPLANClient.cpp` and `MPLANClient.h` into `Cores/MelonDSDeltaCore/MelonDSDeltaCore/Bridge/`, place ENet 1.3.18 at `Cores/MelonDSDeltaCore/External/enet/`, then apply the three patches from their corresponding repository roots.

Signing identities, bundle identifiers, ROMs, firmware, BIOS files, and save data are intentionally excluded.

## Status

- Discovery: working
- ENet session join: working
- 802.11 authentication: working
- Association: working after bounded retry
- DQ9 multiplayer world entry: working
- Desktop host platforms: macOS and Windows validated
- Long-session stability and smooth audio/video: experimental

## Upstream and licensing

This is an independent experimental patch set, not an official Delta or melonDS feature. Delta, DeltaCore, MelonDSDeltaCore, melonDS, Dragon Quest IX, and ENet belong to their respective authors and rights holders.

The melonDS-derived bridge and this patch set are released under GPL-3.0-or-later. See `LICENSE` and `THIRD_PARTY.md`.
