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

Envelope segments are an exponential recursion (`ep *= epMul`), set up in
`envStartSeg` which runs only on segment boundaries. Do not move `expf`
back into `envTick`.

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
