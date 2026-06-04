# Marlin — Two Trees BlueR fork (lostbean)

This is a **personal fork** of [MarlinFirmware/Marlin](https://github.com/MarlinFirmware/Marlin)
holding the 3D-printer configuration for a **Two Trees BlueR** running an **MKS
Robin Nano** board (with a laser module). The firmware code itself is upstream
Marlin; this fork's value lives in the `Marlin/Configuration.h`,
`Marlin/Configuration_adv.h`, and `platformio.ini` changes that calibrate it for
this specific machine.

> The branch `main` carries the config as a series of small, per-concern commits
> on top of a pinned upstream release. The branch `marlin-upstream` tracks the
> pristine upstream release used as the base. Keep config changes on `main`;
> advance `marlin-upstream` (and rebase/re-apply) when pulling newer Marlin.

## Repository layout

- `Marlin/Configuration.h`, `Marlin/Configuration_adv.h` — **the config that
  matters here.** Board, drivers (TMC2209 via the custom `BLUER_TMC2209` guard),
  PID, probe/leveling (`BLUER_Z_PROBE` inductive probe), motion, TFT UI, laser.
- `platformio.ini` — `default_envs = mks_robin_nano_v1v2` (the build target).
- `Marlin/src/` — upstream firmware source. **Do not edit** unless you mean to
  diverge from upstream.
- `buildroot/`, `ini/` — upstream build scripts and PlatformIO env definitions.
- `flake.nix`, `.envrc`, `lefthook.yml` — this fork's dev tooling (below).

## Tooling

Nix-native dev setup.

- **Dev shell** — `nix develop`, or let direnv load it automatically
  (`direnv allow` once). Provides `platformio`, `python3`, and `lefthook`.
- **Build** — inside the shell: `pio run` (builds the default
  `mks_robin_nano_v1v2` env). `pio run -t upload` to flash, `pio run -t clean`
  to clean. PlatformIO's core/cache lives in `.pio-core/` (gitignored) and it
  downloads toolchains on first build, so the first `pio run` needs network.
- **Formatting** — `nix fmt` runs treefmt **over Nix files only**. See the
  formatting rule below.
- **Commit gate** — a lefthook `pre-commit` hook formats staged files via
  `nix fmt` and re-stages them. Because treefmt is Nix-only, it can only ever
  touch staged `*.nix` files. Install with `lefthook install`.

## Formatting rule (important — this is a fork)

**Never reformat the Marlin C/C++/Python tree.** A whole-tree (or even
whole-file) reformat of upstream code creates large, painful merge conflicts on
every upstream sync. Therefore:

- `nix fmt` / treefmt is scoped to **Nix files only**. The firmware tree
  (`Marlin/**`, `buildroot/**`, `ini/**`, `config/**`, `*.h`, `*.cpp`, `*.py`,
  …) is explicitly excluded in `flake.nix`.
- For firmware code, the repo's upstream **`.editorconfig`** is the style source
  of truth (C/C++ = 2-space, Python = 4-space, LF endings, trim trailing
  whitespace). Let your editor honor it. That is the "format only the lines I
  touch" path — change a config value, match the surrounding 2-space style, done.
- Do **not** add a clang-format / prettier / ruff config or enable those in
  treefmt. If you ever genuinely need to format a *new* file you authored, do it
  by hand or with a one-off tool — don't wire a tree-wide formatter.

## Conventions

### Do

- Keep config edits **minimal and per-concern** — one logical change per commit
  (board, drivers, heating, probe, …), matching surrounding formatting only.
- Preserve the custom `BLUER_TMC2209` / `BLUER_Z_PROBE` `#if ENABLED(...)`
  guard blocks when editing `Configuration.h`.
- When syncing upstream, advance `marlin-upstream` to the new release and
  re-apply/rebase the config commits on top, rather than dragging the whole
  history forward. Watch for upstream **macro renames** (e.g. `DEFAULT_Kp` →
  `DEFAULT_KP`, env `mks_robin_nano35` → `mks_robin_nano_v1v2`).

### Don't

- **Do not add trailers, attribution, `Co-Authored-By`, or `Generated with`
  footers to commit messages.**
- Do not reformat upstream firmware files (see Formatting rule).
- Do not edit `Marlin/src/**` for config purposes — configuration belongs in the
  two `Configuration*.h` files.
