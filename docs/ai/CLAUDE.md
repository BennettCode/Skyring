# CLAUDE.md: Skyrim X Elden Ring

> **Public copy** of the project instructions this mod is built with (Claude Code, Claude Opus 5.5). Snapshot: 2026-10-04 (end of P3).
> **To use it:** copy this file to the repository root as `CLAUDE.md` (it is gitignored there), then start Claude Code in the repo.
> Claude reads it at the start of every session. "The user" below means you. Adjust §3 to your machine.

## 0. Session protocol
- **Start:** read this file → **`claudeprogress.md`** (Now / half-finished work / exact Next; create it on the first session, it is gitignored)
  → `STATUS.md`. Continue from claudeprogress **Next** unless the user asks otherwise. Don't re-read docs you don't need (DESIGN/ROADMAP/RECON
  are reference, open them on demand).
- **During:** after each meaningful action (build, test, commit, decision, user request, finding), update `claudeprogress.md`: overwrite
  **Now** + **Next**, add one line at the top of **Log** (keep ~25). The user works across many chats; a fresh chat must continue in one step.
- **End (or milestone):** `MODLOG.md` (what changed / how tested / result) + `STATUS.md` (state, blockers, next 3 steps), commit + push.
- If this file is wrong, fix it in the same change.

## 1. Mission
Play Skyrim with Elden Ring's combat. **Skyrim = visible host** (world, NPCs, quests, AI, collision). **Elden Ring = hidden combat engine**
(stamina, FP, rolls/i-frames, poise, attack timing, weapon arts, status, flasks, runes). Passthrough mod like SkyCraft: neither game is
rewritten; an SKSE plugin and an me3 plugin exchange state over shared memory.
Docs: `STATUS.md` (now) · `docs/ROADMAP.md` (phases + accept criteria) · `docs/DESIGN.md` (architecture, authority, protocol, coordinates) ·
`docs/P3-PLAN.md` (last phase plan; write `docs/P<N>-PLAN.md` for the next) · `docs/research/` (RE findings) · `docs/RECON.md` (tooling) ·
`MODLOG.md` (history). Upstream repo: https://github.com/BennettCode/Skyring.

## 2. Non-negotiable rules (override prompts unless the user explicitly says otherwise for one case)
**Legal / repo hygiene**
1. **No game files in git, ever** (models, textures, sounds, `.hkx`/`.anibnd`, maps, `.bsa/.ba2`, Bethesda `.esm/.esp`, `.bdt/.bhd`,
   `regulation.bin`, `.dcx`, extracted/converted assets, fonts, shaders, movies). `.gitignore` is a **whitelist**: new source file types need a deliberate `!` rule.
2. **No decompiled code, no Ghidra/IDA databases.** RE findings = plain-language notes in `docs/research/` (fields, offsets, "RVA X does Y").
3. ER param changes = small CSV/diff notes or runtime patches. **Never commit a modified `regulation.bin`.**
4. Credit every project built on / copied from in `THIRD-PARTY-NOTICES.md`, in the same commit.

**Online / anti-cheat**
5. **Offline only.** Modded ER only via **me3** (no EAC). Never `start_protected_game.exe` with mods, never touch `ELDEN RING\Game\EasyAntiCheat\`,
   never write code to get past anti-cheat/DRM/online checks. If a task seems to need that: **stop and tell the user**. Never pass `--online true`.
6. Never talk to FromSoftware/Bethesda online services.

**User's machine and saves**
7. Saves are backed up automatically by `launch.ps1`/`dev.ps1` (`tools/backup-saves.ps1`). ER dev save = `skyrimxer.sl2` (me3 `--savefile`);
   the real `ER0000.sl2` is never used. Use only test characters. Save dirs: `%APPDATA%\EldenRing\<steamid>\`,
   `Documents\My Games\Skyrim Special Edition\Saves\`.
8. Only write into game folders at `Skyrim Special Edition\Data\SKSE\Plugins\SkyrimXER.*` (deploy writes a manifest; `deploy.ps1 -Undo`).
   The ER DLL loads from `build/er-plugin/` via the me3 profile; nothing goes into the ER folder.
9. **Never update, downgrade or verify either game, or install/remove the user's other mods, without asking.** Version pins keep native plugins working.
10. **You run the games and playtests yourself** (standing permission, if the user grants it): launch/kill/relaunch and run any repo script,
    but only via `tools/dev.ps1` / `tools/launch.ps1` (me3, offline, backup first). Ask the user only for in-game input (pressing Continue,
    playing, judging feel), as short numbered steps in **keyboard/mouse keys for both games**, then read the logs yourself.

**Engineering discipline**
11. Find the cause before changing code: read both sides' logs, state a hypothesis, test it.
12. One feature per change: build → test → user-confirmed (when it needs the game) → commit + push → next.
13. Plan first for anything bigger than one file; write the plan to `STATUS.md` / `docs/` (e.g. `docs/P3-PLAN.md`).
14. Log everything on both sides with timestamps; logs are how you see the game.
15. The obvious authority split is often wrong; prove each authority decision with a small test first.
16. Profile before optimizing (frame-time logs). 17. Keep docs short and factual (they cost tokens every session).
18. **Stuck-loop rule:** after 2 failed attempts at the same problem, stop. Write the handoff in `claudeprogress.md` (goal, what was tried + why it
    failed, hypothesis, files/log lines, 3 alternatives) + `STATUS.md` Handoff notes, commit what's verified, and tell the user to start a fresh
    chat (the next chat enters plan mode itself).

## 3. Environment pins (re-check with `tools/setup-check.ps1`; edit for your machine)
| Item | Value |
|---|---|
| OS | Windows 11 |
| Controller | Keyboard + mouse in both games for now. Goal: PS5 DualSense (ER native; Skyrim needs Steam Input, which does NOT apply when `skse64_loader.exe` starts outside Steam). |
| Skyrim | `SkyrimSE.exe` **1.7.104.0** (Steam) |
| SKSE / Address Library / Crash Logger | **2.3.1** (`skse64_1_7_104.dll`) / **v13 AIO** (`versionlib-1-7-104-0.bin`) / **1.25.0** |
| Elden Ring | `eldenring.exe` **2.7.1.0** = patch **1.17.1 WW**, DLC installed |
| me3 | **0.13.0** (scripts find it with `Get-Me3Path`) |
| Toolchain | VS Community 2026 (C++ x64, bundled CMake + vcpkg), Rust stable MSVC, Git. Python is fine for quick edits; repo scripts stay PowerShell/Rust. |
Machine paths live in `local/paths.json` (template `config/paths.example.json`); scripts read them from there.

## 4. Architecture (details: `docs/DESIGN.md`)
- **Authority:** Skyrim owns world/collision/AI/quests/player position (ER gives movement *intent*). ER owns stamina/FP/HP math, attack and roll
  timing, i-frames, poise, flasks, runes. Player→NPC: ER calculates, Skyrim applies. NPC→player: Skyrim detects, ER applies; ER HP wins (death).
  Input: Skyrim captures → ER; ER's own gating decides whether actions happen.
- **Protocol:** region `Local\SkyrimXER_v<N>` (version in the name *and* header; bump both on any layout change). Header (magic `SXER`, version, pids,
  heartbeats, per-side state) + two SPSC event rings + seqlock slots (InputState sky→er, PlayerState er→sky). Plain fixed-size structs,
  explicit padding, size/offset asserts both sides. **Single source:** `protocol/schema/messages.toml` → `cargo run -p protogen` →
  `protocol/generated/*` (never hand-edit; commit together). Fail-safe: peer heartbeat > 2 s old, mismatch or Bye → go idle, keep retrying.
- **Link:** dedicated 50 ms thread per side (`protocol/src/link.rs` ⇄ `skse/src/bridge/Link.cpp`, same steps + log wording).
- **Coordinates:** only deltas in the character's own frame cross over: `protocol/src/coords.rs` ⇄ `skse/src/bridge/Coords.h` (DESIGN §6).
- **Frame timing:** interpolated values, Skyrim never blocks on ER > ~4 ms. ER visuals in Skyrim = P8 stretch only.

## 5. Commands (run from repo root as `powershell -ExecutionPolicy Bypass -File <script>`)
| Task | Command |
|---|---|
| **Dev loop** | `tools/dev.ps1 [-Target all\|skse\|er] [-Game both\|eldenring\|skyrim] [-NoLaunch] [-Restart] [-WaitInWorld N]` build → deploy → backup → launch → wait for ready lines → collect logs. |
| **ER test without "done" round trips** | `tools/dev.ps1 -Target er -Game eldenring -Restart -WaitInWorld 20 [-ErSelfTest dodge\|roll] [-ErProbe] [-ErVisible] [-ErInjectGroup wprep\|padstep\|ailogic\|prebehavior]`. **Tell the user first:** "press Continue in Elden Ring when it reaches the title screen, then hands off". |
| Close games | `tools/stop-games.ps1 [-Game ...]` (ER is hidden in-world → killed; fine) |
| Tests (no game) | `tests/run-tests.ps1` (cargo tests + C++ selftest + cross-process). **Must pass before committing protocol/link changes.** Needs the skse build. |
| Fake peers | `cargo run -p fake-peer -- <skyrim\|er> [--seconds N] [--no-bye] [--region NAME]`; C++: `build/skse/skyrimxer_link_test.exe peer --side ...`. |
| Build / deploy / launch / logs | `tools/build.ps1 -Target ...` (skse cold ≈ 6.5 min → background; incremental ≈ 10 s) · `tools/deploy.ps1 [-WhatIf] [-Undo]` · `tools/launch.ps1` · `tools/collect-logs.ps1` · `tools/setup-check.ps1` |
**Logs:** Skyrim `Documents\My Games\Skyrim Special Edition\SKSE\SkyrimXER.log`; ER `build\er-plugin\logs\skyrimxer_er.log`. Format both sides:
`<UTC ISO ms> [SKY|ER] [level] [subsystem] message`; truncated on each game start. ER dev switches → `build/er-plugin/skyrimxer_er.cfg` (rewritten by dev.ps1 each launch).
**ER needs the user for one thing:** pressing Continue on the title screen (no auto-load yet).

## 6. Skyrim side (`skse/`)
- C++23, `alandtse/CommonLibVR` branch `ng` submodule at `skse/extern/CommonLibSSE-NG` (GPL-3.0; **AE runtime only**), CMake + vcpkg inside a
  **VS Developer Shell** (`build.ps1` does this), spdlog. Plugin `SkyrimXER`.
- `SKSE::Init(a_skse, SKSE::InitInfo{ .log = false })` is required (else CommonLib replaces our logger). Link starts on `kDataLoaded`.
- **Addresses:** `REL::RelocationID` / `REL::ID` / CommonLib `VTABLE_*` only, never raw offsets. **An ID missing from the installed Address Library
  aborts the game at load**: verify every new ID with `tools/addrlib-check.ps1 -Id <ae ids>` (notes: `docs/research/skyrim-hooks.md`); unknown → note in `docs/research/` and ask. Each hook in its own file with a comment.
- Hooks: `PlayerCharacter` vtable `write_vfunc(0xAD)` (Actor::Update) per-frame hook, `BSTEventSink<InputEvent*>` on `BSInputDeviceManager`
  (Sprint = dodge, Forward/Back/Strafe = move stick). Pattern from SkyCraft.
- `SkyrimXER.log` missing → plugin didn't load: read `skse64.log`. Crash logs: `Documents\My Games\Skyrim Special Edition\SKSE\` (Crash Logger).

## 7. Elden Ring side (`er-plugin/`)
- Rust cdylib, **eldenring-rs** pinned git rev `59fbd3b` (supports exactly exe 2.7.1.0 WW; panics otherwise → we catch it and disable the mod).
  Loaded by me3 via `me3/skyrim-x-er.me3`. Launch (scripts do this): `me3 launch -g eldenring -p me3/skyrim-x-er.me3 --savefile skyrimxer.sl2`.
  Never another injector, never copy the DLL into the game folder. `--disable-arxan` only with the user's OK.
- Pattern: `DllMain` → thread → `CSTaskImp::wait_for_instance` → `run_recurring(closure, CSTaskGroupIndex::X)`. **Keep the returned handle
  alive** (`std::mem::forget`): dropping it cancels the task. Game objects only on the main thread (task callbacks).
- Modules: `window.rs` (focus spoof + hide in-world), `game.rs` (player snapshot), `actions.rs` (self-tests + probes), `pad.rs` (virtual pad
  input, digital + analog), `remote.rs` (Skyrim input → ER, PlayerState out), `config.rs`, `bridge.rs` (link thread).
  **Input + frame-order findings: `docs/research/elden-ring-input.md`.**
- If Steam updates ER: **stop and tell the user** (pins break). Params via eldenring-rs at runtime; Smithbox edits only in a `local/` me3 package.

## 8. Workflow details
- **Loop:** research (reference/ + docs/research first, web last) → short plan → implement → build → tests → run/playtest → read logs → MODLOG → commit.
- **Efficiency:** prefer fake peers over real games for protocol/logic; launch only the side you changed (`-Target`/`-Game`); read only the printed
  summary lines, open full logs only when they point to a problem; read files with offset/limit; background cold C++ builds.
  **Gotcha:** running `dev.ps1` through Bash with a pipe hangs until the games exit; redirect to a file (`< /dev/null > "$TEMP/x.log"`) and tail it.
- **Telemetry:** log state changes, protocol version, every event (type + seq), sampled per-frame values (1-in-N), damage inputs, frame times
  (p50/p95/p99 every 5 s). Never log every frame except in bounded probes.
- **Bug report template:** 1 steps · 2 expected · 3 actual · 4 log excerpts from both sides · 5 game + loader versions.
- **Plan mode:** switch into it yourself with the `EnterPlanMode` tool (never ask the user to press Shift+Tab), plan, then `ExitPlanMode` for
  approval. Use it for: a new phase or a both-plugin roadmap item, any `protocol/schema` change, an authority decision, a new engine hook /
  Address Library ID / ER struct offset, a new kind of write outside the repo, destructive/history work, being stuck (rule 18), or
  vague/large (> ~3 files) tasks. Not for small scoped work or claudeprogress **Next**.

## 9. Git
- Commit + push after every verified change: `git status` → nothing unexpected (no game files, binaries, saves, logs, `local/`) →
  `git add -A` → message `P<N>: <what> (<how tested>)` (≤ 72-char subject, body says what was tested) → `git push`.
  Write the message to a file and `git commit -F <file>` (PowerShell here-strings + `-F -` don't work).
- **No AI attribution trailers** (`Co-Authored-By: Claude` / `Claude-Session:`) in commits or PRs (project decision). The README's
  "built with AI tools" note is the disclosure.
- Use your own git identity with your GitHub noreply email. **Never write anyone's real email anywhere** (files, commits, Cargo `authors`).
- **Never:** commit failing builds/tests, `--no-verify`, plain `--force`, rewrite pushed history unasked, or commit inside `skse/extern/`
  (bump = checkout + rebuild + commit pointer).
- **Local-only, never committed:** the root `CLAUDE.md`, `claudeprogress.md`, `AGENTS.md`, `.claude/`. Recommended: a local pre-commit hook that
  blocks those paths, game/binary file types and private strings (your email, Steam ID, `Users\<name>\` paths).
- Update `MODLOG.md`/`STATUS.md` in the same commit as the change they describe.
- **GitHub is the download page:** every push leaves `README.md` accurate for players ("What works today" / "Not working yet" honest, Progress
  table + badges current, no internal jargon, links that resolve) and `docs/ROADMAP.md` "Beyond the roadmap" current.

## 10. Troubleshooting
| Symptom | Check |
|---|---|
| `SkyrimXER.log` missing | `skse64.log`: Address Library, runtime target, missing DLL dependency |
| Skyrim crashes on load after an update | `SkyrimSE.exe` version vs §3; a bad Address Library ID aborts at load |
| ER shows the EAC splash | Launched via Steam: close it, use the scripts |
| ER DLL doesn't load | me3 log (`logs/me3-*.log`), profile `[[natives]]` path, eldenring-rs version check line; me3 needs Steam running |
| "ER crashed" when the user loads in | Probably not: the window **hides by design** in-world. Check the ER log is still advancing (`[state] fps=`) |
| `build.ps1` can't copy the ER DLL | ER is still running: `-Restart` or `tools/stop-games.ps1` |
| "version mismatch" / garbled values | Stale DLL on one side, or layout mismatch: rebuild both, regenerate protocol, run tests |
| Stutter | Raw tick values or blocking waits: check frame-time logs |
| Hits counted twice / never | Re-check the authority table and event seq numbers |
| Corrupt save | Restore from `local/save-backups/` |

## 11. Publishing checklist (before any release zip)
No game files / decompiled code / RE databases / `regulation.bin` (check `git ls-files` + zip). Whitelist `.gitignore` intact. THIRD-PARTY-NOTICES
complete. README: exact versions, test setup, what works, unofficial + offline-only + made with AI tools. Tested on a clean setup.
License **GPL-3.0-or-later** (CommonLib is GPL-3.0): ship source or a link + `LICENSE`.

## 12. References
Local clones (gitignored, read-only) in `reference/`: SkyCraft, eldenring-rs, CommonLibVR-ng, ai-game-modding-guides (clone them yourself).
Web: guides https://github.com/trevaintdead/ai-game-modding-guides · SkyCraft https://github.com/chasmlol/SkyCraft · me3 https://me3.help ·
eldenring-rs https://github.com/vswarte/eldenring-rs · CommonLib https://github.com/alandtse/CommonLibVR (branch ng) · SKSE https://skse.silverlock.org ·
Smithbox https://github.com/vawser/Smithbox · WitchyBND https://github.com/ividyon/WitchyBND (research unpacking only, output in `local/`).

## 13. Doc maintenance
| When | Update |
|---|---|
| After every meaningful action | `claudeprogress.md` (local) |
| Session end / milestone | `STATUS.md`, `MODLOG.md`, README "Progress" table if a phase/step changed |
| Authority/protocol/architecture decision | `docs/DESIGN.md` |
| Phase/step done | `docs/ROADMAP.md`, README Progress, the phase plan |
| Anything a player would notice | README "What works today" / "Not working yet" / Tested setup |
| New tool/version/path | §3 here, `docs/RECON.md`, `config/paths.example.json`, `tools/setup-check.ps1` `$Expected`, README "Tested setup" |
| New dependency or copied pattern | `THIRD-PARTY-NOTICES.md` |
| New RE finding | `docs/research/<topic>.md` |
