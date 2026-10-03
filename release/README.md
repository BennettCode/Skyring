# release/

Scripts that put together a player-facing zip in `dist/` (ignored by git).

A release zip may contain only: `SKSE/Plugins/SkyrimXER.dll`, `skyrimxer_er.dll`, an me3 profile, config templates, README, license, and
THIRD-PARTY-NOTICES. **No game files, no SKSE/Address Library/me3 binaries** (link to them instead), no PDBs unless debugging.
Run the legal checklist in `CLAUDE.md` §12 before every release.
