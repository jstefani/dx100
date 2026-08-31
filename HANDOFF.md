# Handoff: polyphonic distortion in dx100

Unresolved bug. Written by the previous model after several wrong turns,
so that the next attempt doesn't repeat them.

## The symptom (user's words)

- "when more than one note is sounded, there is either distortion or
  almost a digital ring mod type sound"
- "it never happens on single voices so it is some type of issue with
  more than one voice"
- "the ringmod might actually be distortion" — treat the two descriptions
  as possibly one artifact, not necessarily two.

Reproduces on hardware (monome norns). Not yet reproduced off-device.

## Current state

Repo: `git@github.com:jstefani/dx100.git`, branch `master`.

```
d4a95ca add "clear noise" to reset artifact-producing settings
edb6348 expose headroom + voice scaling as params; drop per-voice tanh
342d8f6 correct overstated claim about the SinOsc phase wrap
782cbfa fix: distortion and ring-mod artifacts on polyphonic playback
f2494c3 dx100: Yamaha 4-op FM engine for norns
```

Deployed to `we@norns.local:~/dust/code/dx100` via
`rsync -av --exclude '.git' --exclude 'test' ./ we@norns.local:~/dust/code/dx100/`.
**`Engine_DX100.sc` is a SuperCollider class — norns must be restarted
(SYSTEM > RESTART) for engine edits to take effect.** Lua edits only need
a script reload.

### Temporary debug params, still in the code

Under a `headroom (wip)` separator in PARAMS. **These were added to find
the bug and should be removed once it's fixed.**

- `headroom` (0–2, default 1.0) — flat output trim, `lib/Engine_DX100.sc:303`
- `voice scaling` / `scale_exp` (0–1.5, default 0.5) — exponent on the
  per-voice divisor. 0 = none, 0.5 = 1/sqrt(N), 1.0 = 1/N.
  Engine side: `rebalance` at `lib/Engine_DX100.sc:430`.

Also added: a `clear noise` trigger (`dx100.lua:719`) that zeroes
everything under "character" plus feedback, fixed-mode and detune.

## RULED OUT — do not re-investigate these

Each of these was tested, not just reasoned about.

**1. Output level / summing headroom. Conclusively ruled out.**
The user swept both debug params across their full range: "no combination
of voice scaling or headroom seems to affect the distorted nature of the
sound." Whatever this is, it is not voices summing too hot. Do not spend
time on gain staging.

**2. `SinOsc` phase-input overflow.** The previous model's first "fix"
(`782cbfa`) claimed the phase wrap at `lib/Engine_DX100.sc:177` fixed the
ring mod. **That claim was wrong** and is retracted in `342d8f6`.
Measured with sclang 3.14.1:

```
phase 7   : maxdiff = 2.1e-07     <- float noise
phase 20  : maxdiff = 6.6e-07     <- float noise
phase 30  : maxdiff = 1.30        <- real breakdown
phase 60  : maxdiff = 1.98
```

The bound is ~8pi (25.1 rad). This engine's worst case is
`m (7.0) * two modulators at full level` = **14 rad**; the default patch
reaches **5.25**. The bound is never approached. The `ph.wrap(-pi,pi)`
now in the code is a harmless no-op kept as insurance.

**3. `LocalIn`/`LocalOut` feedback bleeding between voices in the
ParGroup.** Tested directly: two synths in a `ParGroup`, each integrating
a different constant through a feedback loop. Ratio came out exactly
2.000, i.e. loops are correctly private per voice. Not the cause.

**4. Per-voice saturation.** `782cbfa` added `snd.tanh` per voice; this
was a mistake (same structural error as the `clip2` it replaced — it
cannot protect a sum, and `tanh` bends below 1.0 so it distorted every
voice unconditionally). Removed in `edb6348`. Removing it did not fix
the symptom either.

## NOT yet ruled out — suggested starting points

**Per-voice free-running LFOs.** `lib/Engine_DX100.sc:79-95`. Every voice
instantiates its own `SinOsc.kr`/`LFTri.kr` etc. with no phase reset, so
N voices modulate at the same *rate* but at random relative *phase*.
With `ams` or `pms` above zero this produces beating between voices that
would plausibly read as ring mod, and would be **inaudible on one voice**
— which matches the symptom precisely. Both default to 0, so first
establish whether the user's patch has them non-zero. If this is it, the
fix is a shared LFO on a control bus rather than one per voice.

**Fixed-frequency operators.** `lib/Engine_DX100.sc:105-110`. In fixed
mode an operator runs at a constant Hz *identical in every voice*, so
voices sum coherently and beat. Also inaudible on a single voice.
`rnd_voice` (`dx100.lua:176`) enables fixed mode with 8% probability per
operator, so a randomized patch can silently acquire this. Ask whether
the distortion follows particular patches.

**Voice stealing / re-trigger.** `noteOn` at `lib/Engine_DX100.sc:443`
re-uses a sounding synth for a repeated note id, and `steal`
(`lib/Engine_DX100.sc:502`) sets `killGate, 0` on the oldest. Worth
checking whether `FreeSelf.kr` (`lib/Engine_DX100.sc:158`) and the steal
path can interact to leave a voice half-freed or double-triggered.

**Aliasing.** `m = 7.0` is a high modulation index. High-frequency
sidebands fold back above Nyquist; with several voices at different
pitches the folded partials are inharmonic *relative to each other*,
which is a decent description of "digital ring mod". This would be
subtle on one note and obvious on a chord. Would need a spectrum capture
to confirm.

## How to test off-device

SuperCollider is at `/Applications/SuperCollider.app/Contents/MacOS/sclang`
(3.14.1). There is no `sclang` on PATH; use the full path.

**No `.sc` file may exist under `test/`.** norns compiles every `.sc`
under `~/dust`, so a duplicate class definition breaks the class library
build and the device will not start. A stubbed `CroneEngine` is worse
still — it breaks every engine on the device, not just this one. This
happened once; `test/**/*.sc` is now gitignored.

The scripts in `test/sc/` are standalone UGen tests needing no project
classes. Run from the repo root:

    /Applications/SuperCollider.app/Contents/MacOS/sclang \
      -l test/sc/conf.yaml test/sc/localin_isolation.scd

Compile-checking `Engine_DX100.sc` needs the real norns class library —
do that on the device.

**What works:** class-library compilation (catches syntax + unknown
UGens), and `s.waitForBoot` + `Synth(...)` + reading a `Bus.control` with
`getSynchronous`. The `LocalIn` isolation test above used this and was
reliable.

**What kept failing** — a real time sink, budget for it or avoid it:
`Score.recordNRT` hung repeatedly, and `Buffer.loadToFloatArray`'s
callback never fired under `sclang -l`, hanging until timeout. Five
separate attempts to measure chord-vs-single peak levels were lost this
way and **no audio measurement was ever obtained**. If you need waveform
data, prefer `Peak.ar` -> `Out.kr` -> `Bus.getSynchronous` polling, or
render with `scsynth -N` directly rather than through sclang's NRT.

## Method note

The previous model twice asserted a diagnosis from plausible-sounding
reasoning without measuring, and was wrong both times (the `SinOsc`
bound; then the summing-level theory that the user disproved by ear).
The `SinOsc` and `LocalIn` tests above only became useful once actually
run. Measure before claiming, and prefer asking the user for one
diagnostic listening test over shipping a speculative fix.

## Useful questions for the user

1. Does it happen on the **default patch**, or only after `K3` randomize
   / loading particular presets?
2. Are `pms` (pitch mod) or `ams` (amp mod) non-zero in a patch that
   distorts? (PARAMS > lfo)
3. Is any operator in **fixed** frequency mode? (PARAMS > op N > fixed)
4. Two notes or does it need more? A **held** dyad, or only on attack?
5. Does it survive `clear noise`?
