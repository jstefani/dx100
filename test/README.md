# off-device test harness

SuperCollider lives at
`/Applications/SuperCollider.app/Contents/MacOS/sclang` (3.14.1); there is
no `sclang` on PATH.

`CroneStub.sc` stubs `CroneEngine` so `lib/Engine_DX100.sc` compiles off
the norns.

**Never deploy `test/` to the norns, and never leave a copy of
`Engine_DX100.sc` in here.** norns compiles every `.sc` file under
`dust/`, so a second copy of the class is a duplicate definition: the
class library fails to build and *the device will not start*. The copy
is gitignored for that reason.

To compile-check the engine, copy it in, run, then delete it:

    cp lib/Engine_DX100.sc test/sc/
    /Applications/SuperCollider.app/Contents/MacOS/sclang \
      -l test/sc/conf.yaml test/sc/<script>.scd
    rm test/sc/Engine_DX100.sc

Note `conf.yaml` uses a relative includePath, so run from the repo root.

Deploy with `test/` excluded:

    rsync -av --exclude '.git' --exclude 'test' \
      ./ we@norns.local:~/dust/code/dx100/

## scripts

- `sinosc_phase_bound.scd` — measures where SinOsc's phase input breaks
  down (~8pi). Establishes that the engine never reaches it.
- `localin_isolation.scd` — proves LocalIn/LocalOut feedback loops are
  private per synth inside a ParGroup.

Both write results to stdout and call `0.exit`.

## known-bad approaches

`Score.recordNRT` and `Buffer.loadToFloatArray` both hang under
`sclang -l` (callback never fires). Use `Peak.ar` -> `Out.kr` ->
`Bus.getSynchronous`, or drive `scsynth -N` directly.
