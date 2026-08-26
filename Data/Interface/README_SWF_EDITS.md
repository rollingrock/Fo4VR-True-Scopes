# ScopeMenu SWF edits (v0.3.11, 2026-08-26)

Both files are the VANILLA Fallout 4 VR SWFs (extracted from the Interface BA2)
with ONE change each, made via JPEXS xml roundtrip (swf2xml -> edit -> xml2swf):

  The `PlaceObject2Tag` that puts `ButtonHintInstance` (the floating
  "GRAB Hold Breath" pill, character 10, depth 14) on the ScopeMenu stage now
  carries an explicit ZERO SCALE record (hasScale=true, scaleX=scaleY=0.0)
  instead of the identity matrix.

Why scale-0 instead of deleting the placement: the instance stays alive on the
stage, so the movie's own AS2 (`__setProp_ButtonHintInstance_...`) and the
native `SetUpButtonBar` path (`BSGFxObject::GetMember(root.ScopeMenuInstance,
"ButtonHintInstance")` - see TS_ScopeMenu_Ctor 0x140bc7d0a) find it in EVERY
knob combination - zero crash surface - while a zero-scaled clip rasterizes to
nothing.

Why a file edit at all: the pill is authored INTO this movie and composited
onto the scope widget surface (RT 0x62) outside the lens delivery footprint.
Eight runtime kills (scene-graph culls/scales/detach, the engine's own
hint-scale source, GFx property sets) all failed - the full ledger is
SESSION_2026-08-26_V03_CLOSEOUT.md Addendum 5 in the investigation repo.

world_ScopeMenu.swf is the variant the VR menu loads; ScopeMenu.swf (flat) is
shipped edited identically in case any path falls back to it.

Provenance: vanilla sources at C:\tools\ba2extract\fo4vr_interface\Interface\;
workspace with XML dumps in ../../swfwork (not shipped).
