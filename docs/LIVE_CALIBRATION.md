# Live scope calibration

True Scopes exposes a versioned POD-only calibration API. FRIK is optional.

`TrueScopes_GetCalibrationAPI(2, sizeof(API))` returns read/set callbacks.
Call these on the game thread. Callers supply State storage with its byte size.
Epoch/probe and equipped-weapon/OMOD identity prevent a delayed preview from
editing a replacement weapon. No engine pointers, allocation ownership, STL or
CommonLib types cross the DLL boundary. See src/TrueScopes/CalibrationAPI.h.

Values cover weapon-relative right/forward/up, size, pitch and yaw. The default
values are a no-op. Translation is transformed into the widget parent's frame;
tilt composes with the unadjusted tracking rotation each frame, avoiding
accumulated tilt. Calibration changes the widget, not projectile direction,
scope camera/FOV, or activation logic. Existing upstream STS ownership/camera
paths are retained and decline normal widget fitting while STS owns it.

For fallback optics, the baseline is captured in weapon space so it can follow
weapon motion and reparenting. Anchors are stored in
`Mods_Config/FO4VRScopeCalibration/fallback-anchors-v1.json`. If the cache is
invalid, True Scopes preserves it and keeps new anchors in memory.

## Optional FRIK companion

The companion adds a True Scopes entry to FRIK's configuration menu. The
controller panel previews position, size, pitch and yaw. Save stores the fit
for the current weapon, optic and attachment configuration. Reset previews
the baseline and Cancel restores the last saved or opened values.
See [the companion build notes](../integrations/frik/README.md) to include it
in the True Scopes archive.
