# Contributing

## Where to start

- [Roadmap to playable](https://github.com/Lucas-blip-png/dkb-pc-recomp/issues/10) lists what is done and what is open.
- Issues labelled [good first issue](https://github.com/Lucas-blip-png/dkb-pc-recomp/issues?q=is%3Aopen+label%3A%22good+first+issue%22) need no code: testing on your GPU and playing further.
- Issues labelled [help wanted](https://github.com/Lucas-blip-png/dkb-pc-recomp/issues?q=is%3Aopen+label%3A%22help+wanted%22) each name the files to start from.
- Say in the issue that you are working on it, before you start, so two people do not do the same job. A maintainer then assigns it to you and adds the `in progress` label. Issues that already carry that label are taken: ask in the issue first. The top of the [roadmap](https://github.com/Lucas-blip-png/dkb-pc-recomp/issues/10) lists what is being worked on now.
- Questions go to [Discussions](https://github.com/Lucas-blip-png/dkb-pc-recomp/discussions).
- [docs/architecture.md](docs/architecture.md) explains how the pieces fit, [docs/bananza-plan.md](docs/bananza-plan.md) has the plan and [docs/bananza-shaders.md](docs/bananza-shaders.md) the shader work.

## The one hard rule

Nothing that came out of the game goes into the repository: no game files, keys, extracted data, recompiled C, compiled binaries, shaders, shader caches, fonts, audio or movies. Not in commits, not in issues, not in pull requests. Only information about the game goes in: addresses, hashes, patch code we wrote, shader and bind-state identifiers.

Two checks enforce this:

- CI runs `tools/check_content.py` on every push and pull request.
- The same check runs before each local commit once you enable the hook:

  ```sh
  git config core.hooksPath .githooks
  ```

A failing check is never bypassed with `--no-verify`. If it flags a file by mistake, change the check in a separate pull request and say why.

Links to game files, keys or firmware in issues or pull requests get the issue closed and the content removed.

## Code from other projects

The rules are in [AGENTS.md](AGENTS.md), "Reusing code". In short: DKB Recomp is GPL-3.0-or-later as a whole, so code under MIT, BSD, ISC, zlib, Apache-2.0 and GPL-3.0-compatible licences (Eden, Eglantine, yuzu and the like) may be copied. Each copy keeps its original header and gets a row in [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) with the upstream commit. Files that came from TotK Recomp keep their MIT terms, and a fix meant for TotK Recomp itself must not use GPL code. Unicorn is used only as a separate test tool.

## Reporting bugs

Use the issue form. Include the commit you built (`git rev-parse --short HEAD`), your GPU, driver and Windows version, the log (`logs\dkb.log` next to `dkb.exe`; after a crash, copy it before you start the game again), and the steps to reproduce. Describe what you see instead of attaching screenshots of the game. [docs/dkb-testing.md](docs/dkb-testing.md) covers building, running and the useful settings.

## Pull requests

- One topic per pull request.
- Describe the problem, then what changed.
- Say how you tested it: which commit, which scene, how long, which GPU.
- A change to the lifter (`recomp/`) or the CPU headers passes the differential tests in [docs/building.md](docs/building.md), "Checks".
- Commit messages are plain sentences, without `feat:` or other prefixes.
- AI-assisted code is welcome. You are responsible for having read and run it.

## Code and docs

- Code, comments, commit messages and documentation are in English.
- Generated code is never edited by hand: change the generator.
