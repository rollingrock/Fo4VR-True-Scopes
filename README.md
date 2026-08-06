# FO4VR True Scopes

Real render-to-texture gun scopes for Fallout 4 VR: the world stays fully rendered while
the scope lens shows a live picture — without the vanilla scoped-mode frame redirect that
blacks out the main view.

**Status: phase 1** — the lens shows a live copy of the main frame (proof of the display
pipeline, validated live 2026-08-06). Phase 2 replaces the copy with a dedicated mono world
render from `PrimaryWeaponScopeCamera` at the weapon's zoom FOV.

## How it works

FO4VR already ships everything needed to *display* a scope image: the `world_scope.nif`
widget, a per-draw material special-case that binds render target `0x62` to the lens, and
an enable switch that shows the widget. What vanilla does to *fill* that target — redirect
the frame's single world render to the scope camera — is what blacks out the world.

This plugin:

1. **Defangs the redirect arm** (`BSGraphics::Renderer+3` setter → `ret`, RVA `0x1d947a0`).
   The vanilla enable switch keeps managing the widget; the frame redirect never engages.
2. **Fills the lens itself** from a hook inside `Main::DrawWorld_And_UI` (RVA `0xd87ca4`,
   the same slot the engine uses for the Pip-Boy local-map render):
   `ImageSpaceManager::Copy(mainFrame, 0x62)` every N frames.

Full reverse-engineering record: `fallout4-scope-in-scope-investigation` repo
(`ROUTE_B_STATIC_MAP_2026-08-06.md`, `SESSION_2026-08-06_ROUTE_B_LIVE.md`).

## Roadmap

- **Phase 1.5** — replace the enable switch (`FUN_140efaa60`) wholesale so eye-gated
  show/hide works with plugin-owned state (vanilla's uses `renderer+3` as state memory,
  which we keep at 0). Until then `forceAlwaysOn` is required.
- **Phase 2** — real scope image: LocalMapRenderer-pattern mono world render from
  `PrimaryWeaponScopeCamera` (zoom FOV from weapon zoomData) into a temp RT, then
  `Copy(temp, 0x62)`. Recipe fully mapped in the investigation repo.
- **Phase 3** — knobs: cadence, lens RT resolution (RenderTargetManager properties table),
  camera offset (`fScopeOffset*:VR`), per-scope-type tuning.

## Building

Same toolchain as [place-in-red-vr](https://github.com/rollingrock): CMake + vcpkg +
[rollingrock/CommonLibF4](https://github.com/rollingrock/CommonLibF4) (VR fork) as
`external/CommonLibF4` submodule (or set the `CommonLibF4Path` environment variable).

```
cmake --preset vs2022-windows-vcpkg-vr
cmake --build buildvr --config Release
```

## Settings — `Data/F4SE/Plugins/TrueScopesVR.toml`

```toml
[TrueScopesVR]
fillEnabled = true       # master switch for the lens fill
fillEveryNFrames = 1     # lens refresh cadence (RT persists between fills)
forceAlwaysOn = true     # required in phase 1, see roadmap
```

## Requirements

- Fallout 4 VR 1.2.72 + F4SEVR. The plugin byte-verifies both patch sites at startup and
  deactivates itself (log only, game untouched) on any mismatch.
