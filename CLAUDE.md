# uw-core

Code shared by StageHand, Umpire and (`uw::ump` only) the plugins. Start
with README.md: the layers, how to build and test, and where the code came
from. The decisions about it are in Umpire's docs, not here: read
`~/dev/dev-midi/UmpireDAW/docs/decisions.md` ("Shared code", "DPF fork") and
`docs/roadmap.md` first. StageHand is `~/dev/dev-audio/StageHand` on the
Linux PC and `~/Dev-MIDI/StageHand` on the Mac.

## Working with the author

- The author commits to `main` directly. Never open pull requests. Ask before
  pushing.
- One milestone per session, about a day of work each.
- Keep tests silent (no audible output).
- Record decisions in the docs, not only in conversation.
