# dx100

Yamaha 4-operator FM for monome norns — DX100 / DX21 / DX27 / TX81Z territory.

Four operators, the eight Yamaha algorithms, feedback on op 4, per-operator
rate/level envelopes, and the TX81Z operator waveform set. Mono or 16-voice poly.

The voice follows the DX100's YM2164 chip rather than a generic FM model:
levels, key scaling, velocity and amplitude modulation are all in dB, the
envelope runs in dB (linear decays, exponential-approach attack), a level-99
modulator swings the carrier by 8π, feedback averages the last two samples,
operator phases reset on key-on, and one LFO is shared by all voices with a
key-sync switch. The 64 frequency ratios are the ones printed in the manual.

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
fourth apart. Sustain pedal, mod wheel (CC 1), breath (CC 2) and volume (CC 7)
are wired up; CC 123 panics. Wheel and breath depth, breath pitch bias and
breath EG bias live under PARAMS > wheel / breath, as on the hardware's
performance page.

## keys

| control | action |
| --- | --- |
| E1 | algorithm (1–8 Yamaha, 9–16 extra) |
| E2 | feedback (0–7) |
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
frequency in Hz, plus detune (±3, about 0.9 cents a step). The ratio table is
the DX100's: 0.5 and the integers to 15, interleaved with √2, π/2 and √3
multiples up to 25.95. Integers give harmonic, instrument-like tones; the
irrational entries give bells and metal.

**Level** is in dB, 0.75 dB per step near the top of the range: 90 is −7 dB,
80 is −14 dB, 66 is −25 dB, 50 is −37 dB. A modulator around 65–75 is a gentle
index, 80–90 is brash. Numbers from a DX100/DX21/TX81Z patch sheet carry over
directly.

The envelope follows Yamaha's shape rather than ADSR:

- **AR** attack rate
- **D1R / D1L** first decay, falling to a level
- **D2R** second decay, running down from D1L while the key is held
- **RR** release rate

All rates are *rates*, so **higher is faster** — the inverse of a time control.
D1R and D2R at 0 hold their level (D2R 0 = sustain at D1L, as on the
hardware). The envelope moves in dB, so decays are exponential in amplitude,
and the attack is the chip's fast-start exponential approach.
An operator used as a modulator has its envelope shape the *timbre* over the
note, which is the whole trick of FM: percussive modulator envelopes give a
struck attack that decays to a purer tone.

**Key scale** rolls an operator off toward the top of the keyboard (real
instruments get less bright as they get higher). **Velocity** (0–7) sets how
much velocity opens that operator — put it high on modulators for a patch that
gets brighter as you play harder. **Rate scaling** (0–3) shortens the envelope
as pitch rises; 3 halves the times per octave. **Amp mod** picks which
operators the LFO's amplitude modulation reaches: carriers for tremolo,
modulators for a wah. **EG bias** (0–7) is how far breath EG bias opens the
operator.

## lfo and pitch eg

One LFO for all voices. **Key sync** restarts it at its peak on every key-on,
as the hardware does; off, it free-runs so chords shimmer against it. Pitch
mod at 99 is ±800 cents, amp mod at 99 is 96 dB. **Delay** holds, then fades
the LFO in over the same time (up to 10.7 s).

The pitch EG is the DX21/TX81Z three-stage one (the DX100 has none): on
key-on the pitch runs from where it rests (level 3) to level 1 at rate 1,
then to level 2 at rate 2 and holds; on key-off it returns to level 3 at
rate 3. 50 is no shift, 0/99 is ∓/± 4 octaves.

## character

**opp grit** (on by default) runs the oscillator with a 10-bit sine phase and
no interpolation, and steps the envelope in 0.09 dB, like the chip. The rest
of the section adds the DAC and its aliasing back on the mix: bit crush,
downsampling, drive, glitch and hiss. Those default to off.

## factory voices

`solid bass`, `e.piano`, `brass`, `glass bell`, `wood marimba`,
`lately bass`, `hollow pad`, `clav` — in PARAMS > presets, or K1+E1.
They are written in hardware units in `lib/presets.lua` (level 0–99, rates
0–31, D1L 0–15, RR 0–15), so a DX100/DX21/TX81Z patch sheet can be typed in
as-is. `e.piano` is the owner's manual tutorial voice. They are starting
points, not clones of the ROM patches.

Anything you build saves with the usual PARAMS > PSET menu.

## importing dx100 patches (.syx)

PARAMS > sysex > **import .syx** opens the file browser at `dust/`. Put
banks somewhere under it, e.g. `dust/data/dx100/syx/`. Both Yamaha 4-op
dump formats are read:

- **VCED** single voice (93 bytes, format 03): the edit-buffer dump the
  DX100 sends when you press a voice button.
- **VMEM** 32-voice bank (4096 bytes, format 04): the SYS INFO bulk dump.

Files may hold several messages, and headerless raw dumps are accepted.
DX21 and DX27 dumps are the same format. TX81Z banks load as their DX100
subset (waveforms and fixed frequency are ignored). Empty bank slots are
skipped.

Imported voices are appended after the eight factory voices in PARAMS >
presets > **voice** (and K1+E1), under the name stored in the dump. Each
carries its LFO, pitch EG, portamento, poly/mono, bend range, chorus and
wheel/breath settings, which are applied when the voice is loaded.
Levels, rates, ratios, detune, scaling, AME and EBS map one to one onto
the operator params. **clear imported** drops them again.

The import file is saved in psets, so a pset that points at an imported
voice gets its bank back on load.

**Live over MIDI:** a DX100 (or DX21/27/TX81Z) plugged into the norns can
dump straight in. A single-voice dump loads immediately as the current
voice, like the hardware's edit buffer; a 32-voice bulk dump is appended
to the list. On the DX100: FUNCTION > SYS INFO twice > "MIDI Transmit?"
> YES.
