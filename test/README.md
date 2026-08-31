# off-device test harness

SuperCollider lives at
`/Applications/SuperCollider.app/Contents/MacOS/sclang` (3.14.1); there is
no `sclang` on PATH.

**No `.sc` files may live in this directory.** norns compiles every
`.sc` file under `~/dust` into its class library. A duplicate class
definition breaks the build and *the device will not start*:

    ERROR: duplicate Class found: 'CroneEngine'
    ERROR: duplicate Class found: 'Engine_DX100'

A stubbed `CroneEngine` is the worse of the two — it redefines the base
class every norns engine inherits from, breaking all of them. `test/**/*.sc`
is gitignored to prevent this.

The scripts here are standalone UGen tests that need no project classes,
so `conf.yaml` deliberately sets `includePaths: []`. Run from the repo
root:

    /Applications/SuperCollider.app/Contents/MacOS/sclang \
      -l test/sc/conf.yaml test/sc/<script>.scd

To compile-check `Engine_DX100.sc` itself you need the real norns class
library — do it on the device, not here.

Deploy with `test/` excluded:

    rsync -av --exclude '.git' --exclude 'test' \
      ./ we@norns.local:~/dust/code/dx100/

## scripts

- `sinosc_phase_bound.scd` — measures where SinOsc's phase input breaks
  down (~8pi). Establishes that the engine never reaches it.
- `localin_isolation.scd` — proves LocalIn/LocalOut feedback loops are
  private per synth inside a ParGroup.
- `quantize_poly.scd` — shows per-voice quantization produces 2.8x the
  error of quantizing the mix at 8 voices, and identical error at 1.
  This is why the character fx moved to a single post-mix synth.

Both write results to stdout and call `0.exit`.

## known-bad approaches

`Score.recordNRT` and `Buffer.loadToFloatArray` both hang under
`sclang -l` (callback never fires). Use `Peak.ar` -> `Out.kr` ->
`Bus.getSynchronous`, or drive `scsynth -N` directly.
