# tests/

- **Protocol layout tests** (both languages): every struct's size/alignment/field offsets match the schema. They must pass before any protocol commit.
- **Coordinate tests**: Skyrim↔ER axis, handedness and yaw conversion, written from the Phase 3 measurements (`docs/research/coordinates.md`).
- **Log checks**: small scripts that read collected telemetry and confirm things like "i-frame window was 13±1 frames" or "no damage taken during i-frames".

Rust tests live next to the code (`cargo test --workspace`). C++ tests are an xmake target `skyrimxer-tests`. This folder holds
shared fixtures and cross-language checks.
