# tests/

**Run everything (no game needed):** `powershell -ExecutionPolicy Bypass -File tests/run-tests.ps1` (≈ 20 s; build the skse target first).
It must pass before any protocol or link commit.

| What | Where |
|---|---|
| Generated files up to date, schema validation | `cargo test -p protogen` |
| Rings (wrap-around, full, corrupt) and link state machine (handshake, heartbeat, timeout, Bye, restart, mismatch) | `cargo test -p skyrimxer-protocol` |
| C++ layout asserts (compile time) + C++ link selftest | `build/skse/skyrimxer_link_test.exe selftest` (source `skse/tests/link_test.cpp`, also `ctest --test-dir build/skse`) |
| Cross-language, two real processes: Rust ↔ C++ handshake, crash timeout, Bye | `run-tests.ps1` (fake peer: `cargo run -p fake-peer -- <skyrim\|er>`; C++ peer: `skyrimxer_link_test.exe peer`) |

Peer logs from the cross-language runs land in `logs/tests/`. The tests use a private region name, so they're safe while a game runs.

Planned: **coordinate tests** (Skyrim↔ER axes, handedness, yaw; from the Phase 3 measurements) and **log checks** (e.g. "no damage taken during i-frames").
