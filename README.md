# FO4VR True Scopes

Real render-to-texture gun scopes for Fallout 4 VR: the world stays fully rendered
while the scope lens shows a live, magnified, correctly lit picture — no more
blacked-out world when you aim.

**Status: v0.3 test release.** All vanilla scopes work through the system (26/30 optics
field-verified; four known stragglers below), the picture is fitted per-scope to the
real eyepiece, the glass has a tuned optical look (rim shadow, sheen, parallax,
eye-box), scopes activate from your **pose** (raise the gun to your eye — pistol
scopes finally work at arm's length), and bullet-hole decals render in the lens.

## Requirements

- **Fallout 4 VR** (1.2.72 — the only VR binary; the plugin refuses anything else)
- **F4SEVR**
- ⚠️ **True Scopes REPLACES Better Scopes VR — they CANNOT coexist.** Both rework the
  same scope pipeline. If `BetterScopesVR.dll` is present the log names the conflict
  and True Scopes' hooks decline. Disable one.

## Install (MO2)

Install the archive as a normal MO2 mod. It contains:

```
F4SE/Plugins/truescopes_vr.dll
F4SE/Plugins/truescopes_vr.pdb   (keep it — it makes your crash dumps readable)
F4SE/Plugins/TrueScopesVR.toml
Interface/ScopeMenu.swf
Interface/world_ScopeMenu.swf
```

The two SWFs are the vanilla ScopeMenu movies with one change: the floating
"GRAB Hold Breath" pill is removed at the source (its placement is zero-scaled;
nothing else in the movie is touched). They will conflict with any other mod
that also edits ScopeMenu.swf — rare, but if you have one, load this mod after
it or the pill comes back.

No ESP. Uninstall = remove the mod.

## Using it

Raise a scoped weapon toward your eye — the widget appears and the lens fills with a
live magnified render. Lower it and the world is untouched. Workbench scope swaps are
picked up automatically (the system keys on the attached scope mod's model path).

Vanilla quirks that are **correct behavior**, not bugs:
- x2 (Fine) and x4 (Circle) reticles are hairline-thin **by design** (matches flat FO4).
- Night-vision and recon scopes switch to their screen-style looks.

## Settings

`TrueScopesVR.toml` ships with every field-tuned default and **re-loads on each
scope-raise** (no restart for most values). Reasonable knobs to touch:

| Knob | What |
|---|---|
| `poseMaxDistance` / `poseLookConeDegrees` (+ exit pairs) | how close/aligned the scope must be to your eye to activate |
| `accumClearScale`, `lensExposure`, `lensTintR/G/B` | lens brightness/tone |
| `vignette*`, `rimShadow*`, `sheen*`, `eyeBox*`, `parallax*`, `edgeBlur*`, `caStrength` | the glass look |
| `reticleAlpha/Scale/Offset` | reticle presentation |
| `fillEveryNFrames` | render cadence (1 = every frame, the default) |
| `[Scopes]` table | per-scope aperture/offset overrides — **this is how mod-added scopes get fitted**; copy the hunting-rifle example row |

Leave the rest alone unless asked — a misspelled or wrong-typed key is named in the
log instead of silently ignored.

## Reporting problems

Log: `Documents/My Games/Fallout4VR/F4SE/TrueScopesVR.log`. Include it in every
report — faults are self-diagnosing (step attribution + registers), the version is in
the header, and scope identification prints what it resolved. If the game crashed,
include the Buffout crash log too (the shipped .pdb makes it name our code).

If the render ever faults, the plugin disables itself for the session (up to 3
automatic retries on scope-raise) and the lens falls back to an unmagnified copy —
the game keeps running.

## Known issues (v0.3)

1. Four optics have placement/size defects: **Institute laser, harpoon gun, plasma,
   alien blaster** (deferred to a polish phase).
2. Bullet-hole decals: freshly fired holes render; some pre-existing/distant decal
   species may not. `decalStageEnabled=false` turns the stage off.
3. Heavy-scene performance: a scoped frame costs ~13.6 ms in dense areas (dominated by
   one shadowed spot light in the measured scene). Dense-city scoping may drop frames.
4. Modded scopes not in the built-in table fall back to a default aperture — use the
   `[Scopes]` override to fit them (worked example in the TOML).
5. Night scope image is serviceable but unpolished — a dedicated night pass is planned.

## Building

CMake + vcpkg + [rollingrock/CommonLibF4](https://github.com/rollingrock/CommonLibF4)
(VR fork) as the `external/CommonLibF4` submodule:

```
git clone --recurse-submodules https://github.com/rollingrock/Fo4VR-True-Scopes
cd Fo4VR-True-Scopes
cmake --preset vs2022-windows-vcpkg-vr
cmake --build buildvr --config Release
```

`CMakeUserPresets.json` (git-ignored) is where the local deploy path goes; copy
`CMakeUserPresets.json.template` and edit it.

The full reverse-engineering record — every address, mechanism, and dead end — lives
in a companion research repo (`fallout4-scope-in-scope-investigation`, currently
private). Ask if you want a look.
