# Handoff: where Virtua Fighter 3 Recomp stands

For a new session (or a new person) picking the project up. As of v0.1.3-alpha, 20 Oct 2026.

## Releases

| Version | What |
|---|---|
| v0.1.0-alpha | First release: Mac, Windows, Steam Deck. |
| v0.1.1-alpha | Windows: a Radeon starts on Vulkan; the 60 fps hold after a slow first start. |
| v0.1.2-alpha | Tag only (its release was pulled and replaced by v0.1.3: local files had slipped into its commit, since rewritten out). |
| v0.1.3-alpha | Hold Start for the 2P outfit (#2); CRT colours; Distant textures (Sharp); the music as Supermodel's (timers, FM and pitch LFO depths: #1, #2); the test menu's exit crash; smooth motion's hitches and the swarms' flicker; the governor off for players; the site's 404 page; the menu's mouse clicks (#3). |

How to release: [RELEASE.md](RELEASE.md). The site's production branch is `claude/adoring-feynman-rsrxic` (not `main`).

## Rules learnt the hard way

- **Never commit with `git add -A` or `git add .`**: name the paths. The repo folder on the maintainer's Mac holds other
  projects' folders and the site tools' caches (`.wrangler/`, `functions/`, `.refhit-url`); `.gitignore` covers them
  now, but name the paths anyway.
- Commits as `mr-finster <340554150+mr-finster@users.noreply.github.com>` only (set per repo: `git config user.name`
  / `user.email`). Check `git log --format='%an <%ae>'` before any push.
- **Never re-spin a release**: a new version for any change to the downloads (Quiver and the like flag a changed file).
- Check each package carries the version (`git describe --tags`) and is a VF3 build (`VirtuaFighter3Recomp-…`, not
  VF1's or VF2's) before attaching it.
- Fetch tags (`git fetch --tags`) before building: the version comes from the tag.

## Open issues and backlog

- **#2** (open): the character select in 16:9 (drawn 4:3 now; VF2 fills the sides with painted art).
- **Ultrawide edge culling**: a player saw background 3D objects vanish at the edges in ultrawide. Not reproduced on
  6 stages headlessly (our cull test is widened; the game sends objects past the 4:3 sides). Needs the stage and a
  screenshot.
- **Aoi's river stage**: smooth motion's sky lead gives up for the whole fight (the viewport count changes ~28 times a
  fight; `SkyLead::turn` resets on it), so its 2D sky moves in the game's own lagging steps. Fix: let the lead survive
  viewport-count changes, as `pair_viewports` in r3d_scene.cpp does for the 3D.
- **Frame-rate limit setting** (Off / 30 / 60 / 120 / 144 / screen), for all three games (from vf2-recomp #19).
- **Native resolution for CRTs** (496x384 at 57.5 Hz, 1x, no filters), a Reddit request.
- Optional: crisper hair (cut-out RGBA4 hair cards drawn with an alpha test; Akira's alt costume looked soft), and the
  coin-settings screen's 67 addresses the recompiler's map missed (interpreted: correct, slower).
- The music: every song in the sound test matches Supermodel within 0.2 dB per band. The remaining 1-5 s stereo
  flickers (Aoi, Itou5) are 68000 timing noise (they swing as much against ourselves with the 68000 3% slower).

## Tools added in v0.1.2 / v0.1.3

- `tools/hitch/`: the smooth-motion finder (`VF3_HITCH` in vf3run): stops, lurches, unblended frames, real cuts
  blended, and per-kind flicker (`flicker.py`). `run.sh <rom> <outdir> [tag]` runs attract.txt and fight.txt.
- The governor runs only with `VF3_GOVERN` (tests).
- Distant textures: `video::Raster::arcade_lod` (the LOD base scaled by the lines drawn / 384).
- CRT colours: r3d.hlsl's `FS_COLOUR` through SDL's GPU render state (`colour_shader` in r3d_gpu.cpp); GPU renderer
  only. The same code went to VF1 and VF2 for their next releases.

## Sibling projects

VF1 and VF2 Recomp share most of the host (menu, governor, pacing, CRT colours, the menu's mouse fix). Changes made
here were passed to them; check their repos before porting anything back.
