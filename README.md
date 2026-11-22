<div align="center">

  <img src="docs/smotherfish-icon.png" alt="Smotherfish" width="128">

  <h3>Smotherfish</h3>

  A Stockfish-strength UCI chess engine that plays for the smothered mate.
  <br>
  <strong>[Read the Smotherfish guide][guide-link]</strong>
  <br>
  <br>
  [Report bug][smf-issue-link]
  &middot;
  [Android builds][android-link]
  &middot;
  [Development log][handoff-link]
  &middot;
  [Upstream Stockfish][upstream-link]

  [![License][smf-license-badge]][smf-license-link]
  [![Based on][based-badge]][upstream-link]
  [![Protocol][uci-badge]][uci-link]
  <br>
  [![Last commit][commit-badge]][smf-commits-link]
  [![Android][android-badge]][android-link]

</div>

## Overview

Smotherfish is a free UCI chess engine derived from [Stockfish][upstream-link]
(the development version of 15 November 2026, after Stockfish 19).
It plays with Stockfish's full strength and adds one goal of its own: ending the
game with a smothered mate, where the enemy king is unable to move because its
own pieces occupy every square around it.

The engine proves forced smothered mates and prefers them to faster ordinary
mates. In brutal mode it also builds them: once a game is decided, it spends
moves boxing the enemy king in with its own pieces before delivering the mate.
Against an opponent who defends well the game is never decided, and
Smotherfish plays as ordinary Stockfish.

Like Stockfish, Smotherfish does not include a graphical user interface. Use it
with any UCI chess GUI, such as DroidFish on Android. It identifies itself as
"Smotherfish 1.2 (Stockfish dev-20261122-3e8fde75)".

## What it does

- **Proves smothered mates.** A dedicated search looks for forced sequences
  (checks, sacrifices and a few quiet moves) that end in a smothered mate.
  When it finds one, the engine plays it, even over a faster ordinary mate.
- **Brutal mode** (`SmotherBrutal`, on by default). Hunts for smothers on
  every move. Once the game is decided (at least 8 pawns up, or a forced
  mate), it spends up to 80 moves building one: it stops caring about extra
  material, keeps the enemy pieces next to their king, drives the king into a
  corner and brings the queen and knights close. It never lets its advantage
  drop below 6 pawns, and mates normally when the budget runs out.
- **Smother goal** (`SmotherGoal`): Hybrid (the default: true smothers
  first, near ones later in the game), Full (only true smothers), or Near
  (also "all but one" smothers, where one square next to the king is empty
  but covered). A true smother up to three moves longer is always preferred
  to a near one. Near ends about 46-73 of every 100 games against Elo
  1320-2500 opponents with a full or near smother, Hybrid about 38-53.
- **With brutal mode off** it plays Stockfish's moves except when a smother
  is proven.

## Measured results

Games at 5 seconds plus 0.05 seconds per move, 600 to 2000 games per row,
against Stockfish 19 limited by `UCI_Elo` and against the human-like Maia
networks. Smothered mates per 100 games:

| Opponent | Brutal mode off | Brutal mode | Score (brutal) |
| -------- | ------------ | ----------- | -------------- |
| Stockfish, Elo 1500 | 1.1 | 12.7 | 1.000 |
| Stockfish, Elo 2000 | - | 7.8 | 0.999 |
| Stockfish, Elo 2500 | - | 4.2 | 1.000 |
| Maia 1100 | - | 7.3 | 0.999 |
| Maia 1900 | - | 4.7 | 0.998 |
| Stockfish, Elo 3000 | 0.2 | 2.0 | 0.997 |

Keeping the opponent's pieces on the board while building a smother
(`SmotherBuildKeep`, 8 since November 2026) raised these further. Games ending
in a full or near ("all but one") smother, brutal mode, 1000 games per row:

| Opponent | Hybrid goal (default) | Near goal |
| -------- | --------------------- | --------- |
| Stockfish, Elo 1500 | 53 (61 with pondering) | 66 |
| Stockfish, Elo 2000 | 44 | 54 |
| Stockfish, Elo 2500 | 38 | 46 |
| Maia 1100 | 44 | 49 |
| Stockfish, Elo 3000 | 13 | - |

Losses are rare (none at Elo 1500 or 2500, 1 in 1000 games at Elo 2000: an
ordinary blitz loss before any smother hunting started), and a full-strength strength test
(SPRT, 6400 games) found no cost: +2 Elo against the default engine. Version 1.1 moved to the newer
Stockfish base: in direct games against version 1 it scored 0.517 (about
+12 Elo) over 4000 games, with the same smother rate. Brutal mode makes decided games longer
(a median of about 120 half-moves instead of about 53), because it delays the
mate to build the smother.

## Using it

Smother hunting is on out of the box (brutal mode with the Hybrid goal), so no
settings are needed. Set `SmotherGoal` to Near for the most smothers of either
kind, or Full for true smothers only; set `SmotherBrutal` to `false` for
ordinary Stockfish play that only takes smothers when they come up.
Everything else works like Stockfish.

**Android (DroidFish and similar apps).** Static arm64 binaries are built by
the `Android build` workflow (push to a branch named `android-build/<name>`).
Download `smotherfish-android-armv8-dotprod` (most phones since about 2019)
or `smotherfish-android-armv8` from the workflow run's artifacts, copy it to
the phone and add it as an engine. Do not rename it to `stockfish`, which
DroidFish treats specially.

**Building from source** works exactly as for Stockfish:

```
cd src
make -j profile-build
```

`./stockfish bench` must report 1210779 nodes, the same as the upstream
Stockfish commit it is based on: the bench runs with brutal mode off, and the
smother features do not change the normal search.

## Project files

- `src/smother.h`, `src/smother.cpp`: the smothered-mate prover and the
  construction score.
- `src/search.cpp`: how the prover, brutal mode and construction mode are
  wired into the search (search for "Smother").
- `docs/SMOTHERFISH.md`: how it works, all options, all measurements.
- `notes/HANDOFF.md`: the full development log, experiment by experiment.
- `notes/RESEARCH.md`: related engines and research.
- `docs/smotherfish-icon.png` (512 px), `docs/smotherfish-icon-full.png`
  (1254 px) and `docs/smotherfish-icon-sizes.png` (size preview): the
  project icon.
- `notes/scripts/`: the match runner, opening book generator, prover
  regression test (Lichess smothered-mate puzzles) and summary tools.
- `.github/workflows/smother-exp.yml`, `android-build.yml`: experiments on
  GitHub Actions and the Android builds.

## License and credits

Smotherfish is free software under the
[GNU General Public License version 3][smf-license-link], like Stockfish. All
credit for the engine itself goes to the Stockfish developers (see
[AUTHORS](AUTHORS)). If you distribute Smotherfish, you must include the
license and the full source code, including the changes in this fork.

## About Stockfish (upstream)

[Stockfish][website-link] is a **free and strong UCI chess engine** derived from
Glaurung 2.1 that analyzes chess positions and computes the optimal moves.

Stockfish **does not include a graphical user interface** (GUI) that is required
to display a chessboard and to make it easy to input moves. These GUIs are
developed independently from Stockfish and are available online. **Read the
documentation for your GUI** of choice for information about how to use
Stockfish with it.

See also the Stockfish [documentation][wiki-usage-link] for further usage help.

### Files

This distribution of Stockfish consists of the following files:

  * [README.md][readme-link], the file you are currently reading.

  * [Copying.txt][license-link], a text file containing the GNU General Public
    License version 3.

  * [AUTHORS][authors-link], a text file with the list of authors for the project.

  * [src][src-link], a subdirectory containing the full source code, including a
    Makefile that can be used to compile Stockfish on Unix-like systems.

  * a file with the .nnue extension, storing the neural network for the NNUE
    evaluation. Binary distributions will have this file embedded.

### Contributing

__See [Contributing Guide](CONTRIBUTING.md).__

#### Donating hardware

Improving Stockfish requires a massive amount of testing. You can donate your
hardware resources by installing the [Fishtest Worker][worker-link] and viewing
the current tests on [Fishtest][fishtest-link].

#### Improving the code

In the [chessprogramming wiki][programming-link], many techniques used in
Stockfish are explained with a lot of background information.
The [section on Stockfish][programmingsf-link] describes many features
and techniques used by Stockfish. However, it is generic rather than
focused on Stockfish's precise implementation.

The engine testing is done on [Fishtest][fishtest-link].
If you want to help improve Stockfish, please read this [guideline][guideline-link]
first, where the basics of Stockfish development are explained.

Discussions about Stockfish take place these days mainly in the Stockfish
[Discord server][discord-link]. This is also the best place to ask questions
about the codebase and how to improve it.

### Compiling Stockfish

Stockfish has support for 32 or 64-bit CPUs, certain hardware instructions,
big-endian machines such as Power PC, and other platforms.

On Unix-like systems, it should be easy to compile Stockfish directly from the
source code with the included Makefile in the folder `src`. In general, it is
recommended to run `make help` to see a list of make targets with corresponding
descriptions. An example suitable for most Intel and AMD chips:

```
cd src
make -j profile-build
```

Detailed compilation instructions for all platforms can be found in our
[documentation][wiki-compile-link]. Our wiki also has information about
the [UCI commands][wiki-uci-link] supported by Stockfish.

### Terms of use

Stockfish is free and distributed under the
[**GNU General Public License version 3**][license-link] (GPL v3). Essentially,
this means you are free to do almost exactly what you want with the program,
including distributing it among your friends, making it available for download
from your website, selling it (either by itself or as part of some bigger
software package), or using it as the starting point for a software project of
your own.

The only real limitation is that whenever you distribute Stockfish in some way,
you MUST always include the license and the full source code (or a pointer to
where the source code can be found) to generate the exact binary you are
distributing. If you make any changes to the source code, these changes must
also be made available under GPL v3.

### Acknowledgements

Stockfish uses neural networks trained on [data provided by the Leela Chess Zero
project][lc0-data-link], which is made available under the [Open Database License][odbl-link] (ODbL).


[authors-link]:       https://github.com/official-stockfish/Stockfish/blob/master/AUTHORS
[build-link]:         https://github.com/official-stockfish/Stockfish/actions/workflows/stockfish.yml
[commits-link]:       https://github.com/official-stockfish/Stockfish/commits/master
[discord-link]:       https://discord.gg/GWDRS3kU6R
[issue-link]:         https://github.com/official-stockfish/Stockfish/issues/new?assignees=&labels=&template=BUG-REPORT.yml
[discussions-link]:   https://github.com/official-stockfish/Stockfish/discussions/new
[fishtest-link]:      https://tests.stockfishchess.org/tests
[guideline-link]:     https://github.com/official-stockfish/fishtest/wiki/Creating-my-first-test
[license-link]:       https://github.com/official-stockfish/Stockfish/blob/master/Copying.txt
[programming-link]:   https://www.chessprogramming.org/Main_Page
[programmingsf-link]: https://www.chessprogramming.org/Stockfish
[readme-link]:        https://github.com/official-stockfish/Stockfish/blob/master/README.md
[release-link]:       https://github.com/official-stockfish/Stockfish/releases/latest
[src-link]:           https://github.com/official-stockfish/Stockfish/tree/master/src
[stockfish128-logo]:  https://stockfishchess.org/images/logo/icon_128x128.png
[uci-link]:           https://backscattering.de/chess/uci/
[website-link]:       https://stockfishchess.org
[website-blog-link]:  https://stockfishchess.org/blog/
[wiki-link]:          https://github.com/official-stockfish/Stockfish/wiki
[wiki-compile-link]:  https://github.com/official-stockfish/Stockfish/wiki/Compiling-from-source
[wiki-uci-link]:      https://github.com/official-stockfish/Stockfish/wiki/UCI-&-Commands
[wiki-usage-link]:    https://github.com/official-stockfish/Stockfish/wiki/Download-and-usage
[worker-link]:        https://github.com/official-stockfish/fishtest/wiki/Running-the-worker
[lc0-data-link]:      https://storage.lczero.org/files/training_data
[odbl-link]:          https://opendatacommons.org/licenses/odbl/odbl-10.txt

[build-badge]:        https://img.shields.io/github/actions/workflow/status/official-stockfish/Stockfish/stockfish.yml?branch=master&style=for-the-badge&label=stockfish&logo=github
[commits-badge]:      https://img.shields.io/github/commits-since/official-stockfish/Stockfish/latest?style=for-the-badge
[discord-badge]:      https://img.shields.io/discord/435943710472011776?style=for-the-badge&label=discord&logo=Discord
[fishtest-badge]:     https://img.shields.io/website?style=for-the-badge&down_color=red&down_message=Offline&label=Fishtest&up_color=success&up_message=Online&url=https%3A%2F%2Ftests.stockfishchess.org%2Ftests%2Ffinished
[license-badge]:      https://img.shields.io/github/license/official-stockfish/Stockfish?style=for-the-badge&label=license&color=success
[release-badge]:      https://img.shields.io/github/v/release/official-stockfish/Stockfish?style=for-the-badge&label=official%20release
[website-badge]:      https://img.shields.io/website?style=for-the-badge&down_color=red&down_message=Offline&label=website&up_color=success&up_message=Online&url=https%3A%2F%2Fstockfishchess.org

[guide-link]:    docs/SMOTHERFISH.md
[handoff-link]:  notes/HANDOFF.md
[upstream-link]: https://github.com/official-stockfish/Stockfish
[android-link]:  https://github.com/Demonic1594/Smotherfish/actions/workflows/android-build.yml
[based-badge]:   https://img.shields.io/badge/based%20on-Stockfish%20dev%202026--11--15-blue?style=for-the-badge
[uci-badge]:     https://img.shields.io/badge/protocol-UCI-informational?style=for-the-badge
[commit-badge]:  https://img.shields.io/github/last-commit/Demonic1594/Smotherfish/smotherfish?style=for-the-badge
[android-badge]: https://img.shields.io/badge/android-arm64%20builds-success?style=for-the-badge
[smf-issue-link]:    https://github.com/Demonic1594/Smotherfish/issues/new
[smf-license-link]:  Copying.txt
[smf-commits-link]:  https://github.com/Demonic1594/Smotherfish/commits/smotherfish
[smf-license-badge]: https://img.shields.io/badge/license-GPL--3.0-success?style=for-the-badge
