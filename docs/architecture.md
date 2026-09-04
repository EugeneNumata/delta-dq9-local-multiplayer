# Architecture and observed handshake

## Data path

```text
DQ9 guest Wi-Fi
  -> old melonDS Wifi.cpp
  -> Delta Platform::MP_* bridge
  -> MPLANClient (ENet, UDP 7064)
  -> desktop melonDS LAN host (validated on macOS and Windows)
  -> host DQ9 guest Wi-Fi
```

Host discovery uses the melonDS LAN broadcast on UDP port 7063. Multiplayer frames use the two-channel ENet session on port 7064.

## Successful run

The captured working sequence was:

```text
beacon (0x0080)
authentication request (0x00B0, sequence 1)
authentication response (0x00B0, sequence 2, status 0)
association request (0x0000)
association response (0x0010, status 0, AID 1)
MP command -> reply -> acknowledgement
```

The association request required five retries in the successful run. melonDS 1.1 marks an incoming LAN packet stale after 16 ms; a small scheduling stall can therefore discard a valid management frame before the emulated game consumes it. Only authentication and association frames are retried, at 80 ms intervals with a hard limit of eight retries.

The original captured trace came from the macOS host. A later live test on 2026-09-04 confirmed that the iPhone client could also communicate with a Windows-hosted melonDS session, demonstrating that the bridge is not tied to macOS.

## MAC translation

Delta and desktop melonDS may use the same generated firmware MAC address. The bridge assigns the iPhone a distinct address only on the LAN wire and translates replies back before delivery to the guest. Firmware and save-related paths remain unchanged.

## Audio finding

During multiplayer synchronization, audio production may briefly fall behind. Delta's `AVAudioSourceNode` previously returned a data-unavailable error with zero bytes, which could stop RemoteIO and produce rhythmic stutter. The audio patch fills only those underrun gaps with silence so the audio engine stays alive.
