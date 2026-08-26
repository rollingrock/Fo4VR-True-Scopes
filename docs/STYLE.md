# Code style

This repo follows the same conventions as FRIK and Better Scopes. The short
version: write like an engineer leaving notes for himself, not like someone
writing an article. When in doubt, look at `Fallout4VR_Body/src/GameHooks.cpp`
or `Fo4VR-Better-Scopes/BetterScopes.cpp` and match that.

## Comments

- Sparse. Most code needs none. A section gets a one-line header at most
  (`// renderer stuff`), a tricky spot gets one or two plain sentences.
- Sentence case, no ALL-CAPS emphasis, no exclamation marks, no emoji.
- Say what and why, not the story of how it was found. History lives in git
  log and the investigation repo session docs, not in the source.
- No version tags, dates, or session references in comments. `git log` and
  `git blame` carry the ledger; every version bump has a detailed commit
  message.
- Keep the hard-won facts, drop the narrative around them. Addresses, RVAs,
  struct offsets, and the one-phrase reason they matter stay; how many
  attempts it took to find them does not.

  Bad:

      // v0.3.4 - WHY v0.3.3 NEVER FOUND IT, and the engine ground truth. The
      // walk read the NiNode children fields off EVERY child; on a BSTriShape
      // leaf those offsets are garbage, the read faulted, and the function-level
      // __except silently aborted the WHOLE search - every frame, with no log
      // (the only field signal was the found-line's ABSENCE). Ground truth from
      // NiNode::GetObjectByName (0x141c18500) itself: ...

  Good:

      // children array is base +0x168, count u16 +0x172 (null holes are legal,
      // the engine null-skips them). +0x174 is the non-null element count and
      // undercounts across holes - don't use it as a loop bound.

- Inline trailing comments are fine for a non-obvious instruction or constant:
  `and_(edi, 0xffff); // edi is an int but should be treated as a short`
- Warnings that stop someone from breaking things are worth keeping, kept
  short: what will break and why, one or two lines.
- Removed code that is still useful reference can stay commented out under a
  one-line marker (`// removed code, left for reference`).

## Settings comments

Comments in `Settings.h` and the shipped TOML double as user documentation for
the knobs. One or two lines per setting: what it does, units or range if not
obvious. Same voice as everywhere else.

## Code

- CommonLibF4 conventions: tabs, `a_` parameter prefix, `g_` file-scope
  globals, PascalCase functions, RE addresses through `REL::Offset` with the
  constant named in `Addresses.h`.
- New engine offsets and RVAs get a named constant with a one-line derivation
  note (which function or call site it was read from), not a magic number at
  the use site.
- Prefer the existing patterns in the file over inventing new ones.

## For AI-assisted sessions

Any tooling or assistant working on this repo must match this file. Comment
diffs that add narrative blocks, version arcs, or caps-for-emphasis get
rejected. The long-form reasoning belongs in the investigation repo's session
docs and in commit messages, where it is welcome and encouraged.
