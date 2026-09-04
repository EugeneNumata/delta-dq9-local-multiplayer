# NetroQuest 9 host PoC

## Goal

Prove that remote players can use Nintendo DS local multiplayer without sending its timing-sensitive wireless frames across the Internet.

The host PC runs every emulator instance. Nintendo DS local-wireless traffic stays on the host, while remote devices exchange only controller input and per-player audio/video streams.

## Architecture

```text
User-hosted PC
├── melonDS A ─┐
├── melonDS B ─┼── local DS multiplayer transport
├── capture    ├── per-player audio/video streams
└── input mux  └── per-player virtual controllers
         ⇅
    Internet transport
         ⇅
iPhone / Windows / macOS / browser player
```

This moves Internet latency outside the emulated DS wireless protocol. Network delay becomes ordinary remote-control latency instead of a missed DS response deadline.

## Phase 0: local baseline

Before adding Internet transport:

1. Run two isolated melonDS instances on one host PC.
2. Give each instance a distinct configuration, firmware identity, save directory, audio stream, and controller.
3. Complete a DQ9 local multiplayer session between the two instances.
4. Record CPU/GPU usage, frame pacing, audio behavior, and session stability for at least 30 minutes.

## Phase 1: one remote player

1. Capture only player B's two screens and audio.
2. Stream that output to one remote device with low latency.
3. Route the remote device's controls only to player B's virtual controller.
4. Keep player A local to the host PC.
5. Verify that DQ9 remains connected while the remote player moves, opens menus, and changes areas.

## Phase 2: reusable session host

- Add player slots and explicit input ownership.
- Add an invite code or URL.
- Encrypt signaling, input, audio, and video.
- Add reconnect without destroying the host session.
- Keep ROMs, BIOS files, firmware, and saves on the user's host PC.
- Expose game compatibility reports without distributing game assets.

## First success criteria

- Two emulator instances remain connected for 30 minutes.
- The remote player receives both DS screens and usable audio.
- Remote input controls only the assigned emulator instance.
- DQ9 remains playable with measured end-to-end input latency.
- Disconnecting the remote viewer does not terminate the emulated multiplayer session.

## Non-goals for the first PoC

- Public cloud hosting
- Matchmaking
- ROM distribution or upload
- Supporting every DS game
- Eliminating all visual or audio latency

The first PoC is deliberately user-hosted: the game processes and private game data stay on the owner's PC.
