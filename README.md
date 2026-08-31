# dx100

Yamaha 4-operator FM for monome norns — DX100 / DX21 / DX27 / TX81Z territory.

Four operators, the eight Yamaha algorithms, feedback on op 4, per-operator
rate/level envelopes, and the TX81Z operator waveform set. Mono or 8-voice poly.

## install

Copy this folder to `~/dust/code/dx100` on the norns:

```
scp -r dx100 we@norns.local:~/dust/code/
```

The directory **must** be named `dx100` — the script does
`include("dx100/lib/presets")`, resolved relative to `dust/code/`:

```
git clone git@github.com:jstefani/dx100.git ~/dust/code/dx100
```

The engine is a new SuperCollider class, so **restart norns** after copying
(SYSTEM > RESTART, or `sudo systemctl restart norns-*`). Then pick `dx100`
from SELECT.

## play

Grid, or MIDI (any channel by default; set one in PARAMS > midi). Rows are a
fourth apart. Sustain pedal and CC 1 → pitch mod are wired up; CC 123 panics.

## keys

| control | action |
| --- | --- |
| E1 | algorithm (1–8) |
| E2 | feedback |
| E3 | level of the selected operator |
| K2 | select operator |
| K3 | randomize voice |
| K1 + E1 | factory voice |
| K1 + E2 | ratio of the selected operator |
| K1 + E3 | waveform of the selected operator |
| K1 + K3 | panic (all notes off) |

Arc: level, feedback, decay-1 rate, release — all for the selected operator.

The screen shows the algorithm as a graph. Carriers are filled boxes whose
fill tracks output level; modulators are hollow. The selected operator is
ringed, and op 4 grows a loop when feedback is up. To the right: that
operator's ratio, wave, level, and its rate/level envelope drawn as a curve.

## the FM part

Every operator has a **ratio** (multiple of the note frequency) or a **fixed**
frequency in Hz, plus detune in cents. Ratios that are small integers give
harmonic, instrument-like tones; non-integer ratios give bells and metal.

The envelope follows Yamaha's shape rather than ADSR:

- **AR** attack rate
- **D1R / D1L** first decay, falling to a level
- **D2R** second decay, running down from D1L while the key is held
- **RR** release rate

All rates are *rates*, so **higher is faster** — the inverse of a time control.
An operator used as a modulator has its envelope shape the *timbre* over the
note, which is the whole trick of FM: percussive modulator envelopes give a
struck attack that decays to a purer tone.

**Key scale** rolls an operator off toward the top of the keyboard (real
instruments get less bright as they get higher). **Velocity** sets how much
velocity opens that operator — put it high on modulators for a patch that
gets brighter as you play harder.

## character

The DX100's 12-bit DAC and its aliasing are half the sound. The character
section adds them back: bit crush, downsampling, drive, glitch and hiss.
All default to off.

## factory voices

`solid bass`, `e.piano`, `brass`, `glass bell`, `wood marimba`,
`lately bass`, `hollow pad`, `clav` — in PARAMS > presets, or K1+E1.
These are starting points, not clones of the ROM patches.

Anything you build saves with the usual PARAMS > PSET menu.
