# tidepool

A quiet reaction–diffusion garden, made entirely of terminal characters.

No sound. No network. No runtime libraries to install. No scores, goals, achievements, timers, or unsolicited notifications. Just something living gently in the corner of your attention.

At its heart is a **real, two-chemical Gray–Scott reaction–diffusion simulation**. Every letter is a soft touch in the place where that key would sit if you stretched your physical keyboard across the screen. Clicking or dragging also alters the chemistry. Spots, branching contours, rings, and curling paths emerge on their own.

## Run

On most Linux, macOS, and BSD terminals, with a C compiler:

```sh
make
./builds/tidepool
```

Build outputs, including the unit-test executable, go into `builds/`. Use
`make run` to build and start the garden, `make test` to run the unit and PTY
tests, and `make clean` to remove build outputs.

Or, without `make`:

```sh
mkdir -p builds
cc -O2 -std=c99 -Wall -Wextra tidepool.c -o builds/tidepool -lm
./builds/tidepool
```

The release ZIP also contains a **statically linked Linux x86-64 executable**. It has no shared-library dependencies, so it can run without a compiler:

```sh
./tidepool-linux-x86_64
```

This Linux executable is not portable to macOS, BSD, or other CPU architectures; use `make` there.

### Quiet modes and visual options

```sh
./builds/tidepool --palette moss --layout fi --renewal gentle
./builds/tidepool --renewal active --palette lagoon
./builds/tidepool --palette orchid --color truecolor --fps 10
./builds/tidepool --ascii --color 256
./builds/tidepool --mono --no-mouse
./builds/tidepool --no-hud
./builds/tidepool --help
```

Color detection chooses truecolor when advertised, otherwise 256 or 16 colors depending on `TERM` and `COLORTERM`. Most modern terminal emulators support `--color truecolor`; try that for the richest gradients. Supports UTF-8 half-block characters (`▀`) to display two simulation cells per terminal row. If UTF-8 isn't available, it uses ASCII automatically; `--ascii` forces this mode explicitly. `--mono` uses ASCII and no colors.

## Continuous renewal (optional)

Without intervention, the standard Gray–Scott model can approach a nearly
stationary pattern. That's not a bug: the chosen chemistry has stable states.
`--renewal` adds **local grazing** so the garden can slowly renew itself instead
of becoming a still wallpaper.

```sh
./builds/tidepool --renewal gentle  # recommended: a few unobtrusive grazers
./builds/tidepool --renewal active  # more moving clearings and recolonization
./builds/tidepool --renewal off     # original Gray–Scott, identical to no option
```

Grazers are **invisible**. They drift through established, V-rich formations,
subtly consume the V chemical and replenish U. The area can then regrow from
neighboring patterns. Grazers preferentially seek populated regions, wander
rather than follow perfect loops, and relocate when their patch is exhausted.
They respond to the garden's overall activity: sparse young gardens are left
to grow, and grazing slows down when little remains to consume. There are no
timed whole-screen resets, flashes, or forced new seeds.

The modes are deliberately different: **gentle** leaves much of the existing
pattern intact while small areas slowly change; **active** produces stronger,
more widespread renewal. The two modes do not guarantee that *every* area
remains in constant motion, but they substantially reduce late-stage
stagnation. Default **off** preserves the unmodified mathematical model.

Grazing runs in *simulation time*. The left/right speed keys also change how
fast the grazers move and eat. Pausing freezes everything, including grazers.
The same `--seed` and options reproduce the same evolution.

## Interaction

| Input | Effect |
| --- | --- |
| A–Z, 0–9, punctuation | A gentle reaction near that key's **spatial location** |
| Left mouse click or drag | Encourage a new pattern where you touch |
| Right mouse click or drag | Quiet a patch of active chemistry |
| Tab | Show/hide the transparent keyboard-position guide |
| Space | Pause/resume the physics |
| Left / right arrows | Slow down / speed up (four levels) |
| Up / down arrows | Cycle lagoon, orchid, moss, ember |
| Enter | Grow a fresh garden |
| `?` | Show/hide a terse help line |
| Esc or Ctrl-C | Exit and restore the terminal |

`--layout us` is the default. `--layout fi` maps Finnish/Swedish keys including **å, ä, ö**. The layout is a spatial map, not an on-screen text editor. You don't have to toggle the overlay to use it.

Mouse support requires an xterm-compatible emulator supporting SGR mouse reports. It is automatically enabled, and is harmless in terminals that don't support it. Some terminals require **Shift + drag** to select text while mouse reporting is enabled; disable mouse reporting with `--no-mouse` if you prefer ordinary mouse selection.

## How it works

- Two grids, `U` and `V`, follow Gray–Scott equations, updated using a nine-point discrete Laplacian and wrapping boundaries. In the default model, `Dᵤ = 0.16`, `Dᵥ = 0.08`, feed = `0.035`, kill = `0.060`. Grazing is an **optional, additional model rule**, not part of the classic Gray–Scott equations.
- A click or keystroke makes a small, softly tapered adjustment to local chemical concentrations, then the simulation takes over. A faint trace fades on its own.
- Colors are sampled from a smooth palette, with a dark foundation and warmer/lighter concentrations. Truecolor, 256-color, 16-color, and no-color rendering are supported.
- Each redraw captures one complete simulation state and sends its changes together. Input and simulation keep running while output drains. On xterm with palette-stack support, the garden caches its theme colors and sends short color references; the original terminal palette is restored on exit. Auto color mode detects this support. Output is limited to 1 MB/s on this path and 256 kB/s otherwise. Tiny shade changes accumulate before repainting, and cursor and color commands are compacted to keep dense gardens responsive while painting. This affects display precision, not the chemistry or renewal rate. `--fps` controls the simulation rate; large windows may repaint more slowly. To conserve more CPU, try `--fps 8`.
- Window resizing resamples the current garden rather than restarting it.
- Pausing really pauses: there's no animated spinner or breathing cue demanding attention.

## Terminal compatibility

Requires a POSIX-like OS, C99 compiler to build (or the supplied Linux x86-64 binary), TTY, ANSI cursor positioning, and ideally UTF-8. Tested with GCC on Linux via a pseudo-terminal. Standard emulators such as GNOME Terminal, Kitty, Konsole, Alacritty, iTerm2, Terminal.app, and xterm should work, but actual color/mouse features vary by terminal settings. Very basic terminals may prefer `--mono --no-mouse`.

Input is processed locally and not stored. The program enters the terminal's alternate screen and raw-input mode, then restores normal mode when leaving with Esc, Ctrl-C, or a handled termination signal. As with any full-screen terminal app, an uncatchable `SIGKILL`, power loss, or terminal crash can prevent cleanup. If your terminal ever appears garbled after an abnormal termination, type `reset`.

### Design choice

There is no productivity claim here. It is simply a small, unhurried visual object to interact with while thinking.
