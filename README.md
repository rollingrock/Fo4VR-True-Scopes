# FO4VR True Scopes

Real render-to-texture gun scopes for Fallout 4 VR: the world stays fully rendered while
the scope lens shows a live, magnified picture — without the vanilla scoped-mode frame
redirect that blacks out the main view.

**Status: phase 2 working.** The lens shows a genuine deferred-rendered world view from the
weapon's scope camera at its zoom FOV, lit and textured, fitted to the actual lens geometry
of the equipped scope. It is a development build, not a release: see
[Known issues](#known-issues).

## How it works

FO4VR already ships everything needed to *display* a scope image: the `world_scope.nif`
widget, a per-draw material special-case that binds render target `0x62` to the lens, and
an enable switch that shows the widget. What vanilla does to *fill* that target — redirect
the frame's single world render to the scope camera — is what blacks out the world.

This plugin:

1. **Defangs the redirect arm** (`BSGraphics::Renderer+3` setter → `ret`, RVA `0x1d947a0`).
   The vanilla enable switch keeps managing the widget; the frame redirect never engages.
2. **Runs its own deferred render** from a hook inside `Main::DrawWorld_And_UI` (RVA
   `0xd87ca4`, the same slot the engine uses for the Pip-Boy local-map render): a scope
   camera at the weapon's zoom FOV, its own G-buffer set and light accumulation, the
   engine's own resolve and composite, then delivery into the lens target `0x62`.
3. **Fits the image to the lens** using a per-scope aperture table measured from the
   shipped meshes, keyed by the attached scope OMOD's **model path** (not its root node
   name, which mods copy).

Full reverse-engineering record — every address, every dead end, and why each fix worked —
lives in the companion repo
[fallout4-scope-in-scope-investigation](https://github.com/rollingrock/fallout4-scope-in-scope-investigation).
Start with `STATUS_AND_KNOWN_ISSUES.md`.

## Known issues

- **Sun lighting.** Ambient and local lights reach the scope scene correctly; the
  directional sun does not, so the lens reads darker and flatter than the world.
- **Performance.** ~13.6 ms per scoped frame after the previs-culling fix. The remaining
  cost is render resolution (1024² for a disc of ~150 px) and shadowed light count, not
  geometry.
- Not a Nexus release. Expect to build it yourself and to read the investigation repo.

## Building

Same toolchain as [place-in-red-vr](https://github.com/rollingrock): CMake + vcpkg +
[rollingrock/CommonLibF4](https://github.com/rollingrock/CommonLibF4) (VR fork) as
`external/CommonLibF4` submodule (or set the `CommonLibF4Path` environment variable).

```
git clone --recurse-submodules https://github.com/rollingrock/Fo4VR-True-Scopes
cd Fo4VR-True-Scopes
cmake --preset vs2022-windows-vcpkg-vr
cmake --build buildvr --config Release
```

`CMakeUserPresets.json` (git-ignored) is where the local deploy path goes; copy
`CMakeUserPresets.json.template` and edit it.

## Settings — `Data/F4SE/Plugins/TrueScopesVR.toml`

Every calibration value is a documented setting rather than a constant, and the file
live-reloads each time a scope is raised, so tuning does not need a rebuild. The full list
is in `DEVBENCH_F4VR.md` in the investigation repo; the ones you are most likely to touch:

```toml
[TrueScopesVR]
fillEnabled = true          # master switch for the lens fill
fillEveryNFrames = 1        # lens refresh cadence (RT persists between fills)
cullToScopeFrustum = true   # bypass previs for the scope pass (the big perf win)
scopeFovDegrees = 0         # 0 = derive from the weapon's zoomData
```

## Development bench

The plugin carries an HTTP query server on `127.0.0.1:8930` (`GET /` lists the endpoints)
for reading live state, flipping settings and dumping render targets without a rebuild, and
it also registers a `scope` tool into [devbench](https://github.com/rollingrock/devbench)
when that plugin is present. devbench is never a load-order requirement — if it is absent,
the plugin logs one line and carries on.

## Requirements

- Fallout 4 VR 1.2.72 + F4SEVR. The plugin byte-verifies its patch sites at startup and
  deactivates itself (log only, game untouched) on any mismatch.

## License

GPL-3.0-or-later. See [LICENSE](LICENSE).

One exception, and it is deliberate: `src/DevBenchClient/DevBenchAPI.h` and
`DevBenchAPI.cpp` are vendored from devbench under the MIT license so that any plugin may
copy them, and they keep their own license header and
`src/DevBenchClient/DevBenchAPI.LICENSE.txt`. Do not relicense those two files.
