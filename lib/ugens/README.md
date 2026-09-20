# DX100Voice UGen

One 4-op FM voice in C++. `Engine_DX100` allocates these; chorus/phaser stay in sclang.

## norns

```
ssh we@norns.local
cd ~/dust/code/dx100/lib/ugens
chmod +x build_norns.sh
./build_norns.sh
```

First run clones SuperCollider source to `~/supercollider` for headers. Then **SYSTEM > RESTART**.

Plugin lands in `~/.local/share/SuperCollider/Extensions/dx100/` as
`DX100Voice.so` and `DX100Voice_scsynth.so`. Must be an **scsynth**
plugin (`server_type` 0). Compiling with `SUPERNOVA` defined makes
scsynth ignore it (`UGen 'DX100Voice' not installed`).

The sclang class is `lib/ugens/DX100Voice.sc` (compiled from dust).

## laptop

```
export SC_PATH=/path/to/supercollider/source
./build_norns.sh
```

## editing the inner loop

The per-sample loop in `DX100Voice.cpp` must not call `sinf`, `powf`,
`expf` or `log2f`. On the norns CPU (Cortex-A53) those run in software at
50-150 cycles each. An earlier version made ~28 such calls per sample per
voice and was *slower* than the sclang graph it replaced — distortion past
four voices — because `SinOsc` is a table lookup and the `.kr` parts of
that graph ran once per 64 samples, not 48000 times a second.

Use the in-file replacements instead:

| instead of | use | error |
|---|---|---|
| `sinf(x)` (radians) | `sinRad(x)` | -123 dB |
| `sinf(2pi*p)` (turns) | `sinTab(p)` | -123 dB |
| `powf(2,x)` | `exp2f_fast(x)` | 0.0004 cents |
| `expf(x)` | `expf_fast(x)` | 2.4e-7 rel |
| `log2f(x)` | `log2f_fast(x)` | 5.7e-6 abs |

Anything derived only from scalar-rate `IN0` is constant for the whole
block — hoist it above the `for (n...)` loop. Key scaling and envelope
segment times are deliberately resolved once per block: they are scaling
coefficients, not audio, so a 64-sample staircase on them is inaudible.

Envelopes run in the dB domain: the state is an attenuation, decays add a
per-block constant, the attack multiplies by a per-block coefficient, and
one `exp2f_fast` per op per sample turns the total attenuation (envelope +
level + scaling + AM) into an amplitude. The rate -> time tables
(`decayTime`, `attackTime`, `releaseTime`) use `exp2f` but only at block
rate.

## hardware model

- level, D1L, key scaling, velocity, EG bias, AM: attenuations in dB.
  Level uses the 7-bit TL table (0.75 dB/step, wider below OL 20).
- modulation index: `kModIndex = 8 pi` for a level-99 operator.
- feedback 0-7: `4 pi * 2^(fb-7)` on the mean of the last two op4 samples.
- rates: 96 dB decay time halves every two rate steps, ~0.7 ms at 31;
  D1R/D2R 0 hold. RR 0 is ~23 s. Rate scaling (KRS 0-3) shortens times
  by `2^(-octaves above C1 * 2^(KRS-3))`.
- key-on resets all operator phases and the feedback history.
- the LFO lives in `Engine_DX100` (one `\dx100lfo` synth on a control
  bus); this UGen only applies delay, PMS (±800 cents at max) and AMS
  (96 dB at max) to the value it reads.
- `grit` = 10-bit sine phase without interpolation + 0.09375 dB envelope
  steps.

## the shadowing trap

scsynth scans **two** plugin directories:

- `~/.local/share/SuperCollider/Extensions/dx100/` — where `build_norns.sh` installs
- `/usr/local/lib/SuperCollider/plugins/` — the system dir

If a copy of `DX100Voice.so` exists in the system dir, **it wins**. Rebuilds
then appear to do nothing: the server maps the stale binary, CPU is
unchanged, and the new code never runs. `build_norns.sh` now warns when it
finds a mismatched copy there.

To check what is actually loaded:

```
PID=$(pgrep -x scsynth)
grep DX100 /proc/$PID/maps        # which files are mapped
nm -C <file> | grep gSineTable    # present only in the optimized build
```

Both paths must hold the same binary, and scsynth only picks up a new one
on **SYSTEM > RESTART** — swapping the file under a running server does
nothing, since the old one stays mapped.

## oversampling

FM generates sidebands above Nyquist that fold back as inharmonic energy.
Measured on the default patch at 48k (share of spectral energy that is
not near a harmonic of f0):

| note | off | 2x | 4x |
|---|---|---|---|
| A2 (110 Hz) | 1.4% | 0.3% | 0.1% |
| A4 (440 Hz) | 6.4% | 1.7% | 0.3% |
| A6 (1760 Hz) | 38.9% | 6.7% | 2.2% |

Feedback dominates it far more than `kModIndex` does: at A4, fb=0 gives
1.6% and fb=1.0 gives 70%.

`oversample` (PARAMS, 1/2/4) runs **only the oscillator core** at the
higher rate — envelopes, LFO and PEG stay at base rate, since they do not
fold. A 4th-order Butterworth lowpass at 0.45*sr runs at the oversampled
rate before decimation. Cost is therefore sublinear: roughly 1.3x for 2x
and 1.8x for 4x, not 2x/4x.

Default is **off**: this aliasing is characteristic of 4-op FM hardware,
and `os=1` is bit-identical to the build before oversampling existed
(verified across all 16 algorithms).
