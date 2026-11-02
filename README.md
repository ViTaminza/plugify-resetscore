# resetscore (Plugify)

Client command that resets the caller's score, MVPs and match stats ('mm_rs', 'sm_rs', 'rs' or '!rs'). A port of the Metamod:Source plugin 'ResetScore' by Pisex; resetscore.phrases.txt (the original translations) keeps the original layout.

Plugify port (fork) of **ResetScore** by Pisex (Metamod (C++)).
Original: https://github.com/Pisex/cs2-resetscore
The code was rewritten for Plugify / s2sdk.

## Requirements
- [Plugify](https://github.com/untrustedmodders/plugify) with [s2sdk](https://github.com/untrustedmodders/plugify-plugin-s2sdk)
- Plugin version: 1.2.0
- Plugify dependencies: s2sdk (>=2.18.2)
- Self-contained s2sdk integration: uses s2sdk directly, without the pisexcompat library.

## Installation
Download `resetscore-1.2.0.zip` from Releases and extract it into `game/csgo/`.

## Building
Requires Docker. `./build.sh` builds in the Steam Runtime 3 (sniper) SDK image; the package
is written to `build/package/resetscore`.

## License
GPL-3.0, see LICENSE.

