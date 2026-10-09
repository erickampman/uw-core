# uw-core

Code shared by StageHand, Umpire and (`uw::ump` only) the plugins. Start
with README.md: the layers, how to build and test, and where the code came
from. The decisions about it are in Umpire's docs, not here: read
`~/dev/dev-midi/UmpireDAW/docs/decisions.md` ("Shared code", "DPF fork") and
`docs/roadmap.md` first. StageHand is `~/dev/dev-audio/StageHand` on the
Linux PC and `~/Dev-MIDI/StageHand` on the Mac.

**On the Mac:** uw-core hasn't been built on macOS yet. Follow
[docs/macos-check.md](docs/macos-check.md) first, and record the results
there. On the Mac, Umpire's docs are wherever its clone is, likely under
`~/Dev-MIDI`; ask the author if it isn't there.

## Working with the author

- Push uw-core before any pin that points at its commit. In every clone
  with submodules, on the Linux PC and the Mac, use
  `git push --recurse-submodules=check`, or set
  `git config push.recurseSubmodules check` once per clone, so git refuses
  a pin uw-core's remote doesn't have yet (README.md, "Pushing: uw-core
  first").

- Change uw-core only in this clone (`~/Dev-Common/uw-core` on the Mac,
  `~/dev/dev-common/uw-core` on the Linux PC), never inside a consumer's
  `external/uw-core` submodule. Commit, test and push here; then in the
  consumer, `git -C external/uw-core pull` and commit the pin. Each clone is
  its own repository, so a commit made in a submodule stays invisible here
  until it goes through GitHub.
- The author commits to `main` directly. Never open pull requests. Ask before
  pushing.
- One milestone per session, about a day of work each.
- Keep tests silent (no audible output).
- Record decisions in the docs, not only in conversation.
