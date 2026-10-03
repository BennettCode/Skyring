# protocol/: shared-memory contract between the two plugins

```
protocol/
├── schema/messages.toml          SINGLE SOURCE OF TRUTH: header, slots, events, version
└── generated/
    ├── skyrimxer_protocol.h      generated, used by skse/
    └── skyrimxer_protocol.rs     generated, used by er-plugin/
```
- Generate with `cargo run -p protogen` (`tools/protogen/`, created in Phase 2). **Never hand-edit `generated/`.**
- Region name `Local\SkyrimXER_v<version>`. Magic `'SXER'`. Bump `version` for **any** layout change.
- Fixed-size little-endian plain structs with explicit padding. The generator emits size/offset asserts for both languages.
- Layout tests (`tests/`) must pass on both sides before committing a schema change. Commit the schema + generated files together.
- See `docs/DESIGN.md` §4 for the slots and events.
