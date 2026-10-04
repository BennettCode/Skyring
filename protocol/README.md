# protocol/: shared-memory contract between the two plugins

```
protocol/
├── schema/messages.toml          SINGLE SOURCE OF TRUTH: version, constants, enums, structs, region map, event payloads
├── generated/
│   ├── skyrimxer_protocol.h      generated, used by skse/ (namespace sxer::proto)
│   └── skyrimxer_protocol.rs     generated, included by src/lib.rs as skyrimxer_protocol::proto
└── src/                          Rust crate `skyrimxer-protocol`: region, rings, seqlock slots, link state machine (used by er-plugin + tools/fake-peer)
```
- Regenerate with `cargo run -p protogen` (`tools/protogen/`). **Never hand-edit `generated/`.** `cargo test -p protogen` fails when it is stale.
- Region name `Local\SkyrimXER_v<version>`. Magic `'SXER'`. Bump `version` for **any** layout change.
- Fixed-size little-endian plain structs, explicit padding only. Both languages get size/offset asserts.
- The C++ link (`skse/src/bridge/Link.cpp`) mirrors `src/link.rs`, and `skse/src/bridge/Slot.h` mirrors `src/slot.rs`. Change both together.
- Game threads use the slots through `LinkShared` (region + `connected`), never the link's lock.
- `tests/run-tests.ps1` must pass before committing a schema or link change. Commit the schema + generated files together.
- See `docs/DESIGN.md` §4 for the layout and the link rules.
