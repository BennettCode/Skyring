# Skyrim X Elden Ring

Play Skyrim with Elden Ring's combat: stamina, dodge rolls with i-frames, poise, weapon arts, flasks and runes, all inside Skyrim's world.
Skyrim draws the world while Elden Ring runs hidden in the background and supplies the combat rules. A pair of native plugins
link the two games over shared memory.

> **Status: pre-alpha / scaffolding.** Nothing is playable yet. See [`STATUS.md`](STATUS.md) and [`docs/ROADMAP.md`](docs/ROADMAP.md).

<!-- Screenshot / GIF goes here once something works. -->

## What works
- Nothing yet. The project structure and design docs are in place.

## What doesn't work yet
- Everything. See the roadmap.

## Requirements (planned; exact versions confirmed in Phase 0)
- **The Elder Scrolls V: Skyrim Special/Anniversary Edition** (Steam), version **TBD** (dev machine: 1.7.104)
- **SKSE64** matching your Skyrim version + **Address Library for SKSE Plugins**
- **ELDEN RING** (Steam), version **TBD** (dev machine: exe 2.7.1.0)
- **me3** mod loader. Elden Ring runs **offline only** with this mod.
- Windows 10/11, a PC strong enough to run both games at once

You need your own legal copy of both games. This repository contains **no game files**.

## Installation
TBD.

## How to play
TBD.

## How it works
See [`docs/DESIGN.md`](docs/DESIGN.md). In short: an SKSE plugin in Skyrim and an me3-loaded plugin in Elden Ring exchange input,
player state and combat events through a shared-memory protocol (`protocol/`), a design based on [SkyCraft](https://github.com/chasmlol/SkyCraft).

## Building from source
See [`CLAUDE.md`](CLAUDE.md) §6.

## Credits
See [`THIRD-PARTY-NOTICES.md`](THIRD-PARTY-NOTICES.md).

## Legal
Unofficial fan project. Not affiliated with or endorsed by Bethesda Softworks, ZeniMax, FromSoftware or Bandai Namco.
Offline/single-player only. Never use mods with Elden Ring's online features. This project was built with AI coding tools (Claude Code).
License: **GPL-3.0-or-later** (see [`LICENSE`](LICENSE)). The Skyrim plugin links CommonLibSSE-NG, which is GPL-3.0-or-later.
