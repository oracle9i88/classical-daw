# SWAM attack-response observations, 2026-09-24

**Observed threshold times, not a universal musical-offset calibration.**
SWAM Cello 3.12.2 continued reporting 20 ms, but some measured transients crossed
the threshold before 20 ms. Consequently the property report cannot simply be
subtracted and called “the expressive part of the attack”. The measurements do
not identify the reason for that difference inside the plugin, prove its
reported latency incorrect, or change the host's PDC calculation.

## Method and coverage

48 independent instances: 12 settings × MIDI pitches 48/60 (concert C3/C4) ×
two repetitions. Factory preset Cello; serialized transpose verified at zero;
CC11=100; velocity=96 except the named velocity cases. Room Simulator disabled
and Source Delay Mode=No Delay to avoid deliberately added room/distance effects.
Parameter writes use the inspected IDs/names/ranges and read back their values.
These normalized settings are not certified named sforzando/sul-tasto presets.

Each instance initializes independently, services the Cocoa run loop for five
seconds, then captures five seconds of stereo 48 kHz float32 audio, unnormalized
and unclipped. NoteOn is at sample 48000; NoteOff at 192000. MIDI is sent at the
exact within-block offset, not rounded to the 256-frame block boundary. No
output device is opened; no preset is saved. Plugin property latency is checked
on every render block, with notifications counted. All 48 captures retain the
same 0.020-second report and one initial notification, with nonzero peaks.

Analysis uses trailing **5 ms stereo RMS**, sampled every 1 ms. Reference A is
the largest envelope value during the first second after NoteOn. Threshold is
0.1 of that amplitude (**−20 dB**). The reported time is the **end** of the first
window of ten consecutive above-threshold windows; offline confirmation thus
uses nine additional milliseconds. There is no smoothing-delay subtraction.
The preceding 0.5-second RMS must be below the threshold; otherwise onset is
unresolved. References below 1e-7 amplitude are unresolved. The 1 ms hop is
sampling resolution, not a claim of 1 ms physical/perceptual accuracy.

Reference B is raw RMS 0.8–1.3 seconds after NoteOn, providing a late-window
comparison for bowed sounds. That interval is **not proven steady state**.
Plucked/struck sounds have no comparable sustained plateau, so B is not used to
interpret their attack. The raw analysis retains both computed measurements.

## Results

Ranges cover the four captures of each setting, not the full instrument range
or parameter space. Units are milliseconds after MIDI NoteOn.

| Setting (other parameters remain at Cello defaults) | Peak-relative t20 | Late-window-relative t20 |
|---|---:|---:|
| Bow, velocity 96 | 36–37 | 34–35 |
| Bow, velocity 32 | 38–43 | 38–41 |
| Bow, velocity 127 | 34–35 | 33–34 |
| Attack Ramp Speed = 0 | 39–45 | 38–45 |
| Attack Ramp Speed = 1 | 34–35 | 32–34 |
| Bow/Pizz Position = 0 | 37–42 | 34–36 |
| Bow/Pizz Position = 1 | 34–37 | 33–37 |
| Bow Pressure = 0.1 | 37–41 | 35–37 |
| Bow Pressure = 0.9 | 33–36 | 32–35 |
| Sordino enabled | 35–37 | 34–35 |
| Pizzicato | 9–11 | not interpreted as steady state |
| Col Legno | 9–12 | not interpreted as steady state |

Peak-relative bowed range is **33–45 ms**: minimum in Bow Pressure=0.9, maximum
in Attack Ramp Speed=0. The overall minimum is **9 ms**, shared by the tested
Pizzicato/Col Legno settings. These are the observed extrema, not guaranteed
global extrema. Two repetitions are not a statistical confidence interval.
They do not establish an orders-of-magnitude contrast between named techniques.

Arithmetic differences from the reported 20 ms are 13–25 ms for these bowed
cases and **−11 to −8 ms** for plucked/struck cases. Preserve the negative values
as a warning against interpreting that subtraction as a physical decomposition.
No value is automatically written to `track_delay_us`. For a future ensemble,
audition the actual preset/phrase and comparison instrument; keep its authored
track offset separate from PDC. Nonzero offset execution is still unsupported.

## Reproduction and retained failure

```sh
cmake --build build --target daw_swam_attack_probe
build/daw_swam_attack_probe out/NEW-attack-run
python3 scripts/analyze_swam_attack.py --self-test
python3 scripts/analyze_swam_attack.py out/NEW-attack-run
python3 scripts/analyze_swam_attack.py --verify out/NEW-attack-run
```

The local captures are in `out/swam-attack-20260924-state/`; the manifest and
analysis identify every raw float file with SHA256. No raw plugin audio/state
is committed. [Published measured rows and audio hashes](research/2026-09-24-swam-attack-analysis.json)
and [raw probe receipts](research/2026-09-24-swam-attack-evidence.txt) permit
checking the table and actual settings; rerendering requires the installed AU.

The first attempt stopped before rendering: a direct transpose parameter write
had not reached SWAM's serialized state. That FAIL is retained. The correction
uses the existing production strategy: correct the state before initialization,
restore it, then verify stored transpose after startup. No failed waveform was
replaced or silently included in the 48-case sample. Subsequent render instances
were not retried until they produced desired onset numbers.
