# release/

Scripts that put together a player-facing zip in `dist/` (ignored by git).

A release zip may contain only: `SKSE/Plugins/SkyrimXER.dll`, `skyrimxer_er.dll`, an me3 profile, config templates, README, license, and
THIRD-PARTY-NOTICES. **No game files, and no SKSE/Address Library/me3 binaries** (link to them instead). No PDBs unless debugging.

## Release checklist
- [ ] No game files, extracted/converted assets, decompiled code, RE databases or `regulation.bin` (check `git ls-files` and the zip contents).
- [ ] The whitelist `.gitignore` is still in place, and new allow rules cover only source.
- [ ] `THIRD-PARTY-NOTICES.md` lists every project used. Their licenses are preserved.
- [ ] README: required games + **exact versions**, loaders (SKSE, Address Library, me3), what works, what doesn't, install steps.
- [ ] README says: unofficial fan project, not affiliated with Bethesda or FromSoftware/Bandai Namco, **offline only**, made with AI tools.
- [ ] GPL-3.0-or-later: the release links to the exact source commit, and `LICENSE` is included.
- [ ] Tested on a clean setup with a fresh test save.
