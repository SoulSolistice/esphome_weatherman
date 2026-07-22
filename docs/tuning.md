# Tuning the TFA TX141W decoder

You don't need a scope. The decoder logs everything you'd otherwise read off
one: a 30 s pulse-width histogram, per-stage failure counters, and raw bytes
of every decoded (or attempted-decode) packet.

> **Enable the debug logs first.** The histogram, the stats line, and the
> raw-byte dumps used throughout this guide are gated by the decoder's *own*
> debug flag (not the logger level) and ship **off**. Set `tfa_debug: "true"`
> in the entry file's substitutions and re-apply before expecting any of this
> output — then set it back to `"false"` when the decoder is locked in.

## How the protocol works

The TFA Spring Breeze head talks to its base station with a 65-bit OOK
packet every ~31 s. The WEATHERMAN hardware mod taps the baseband
bitstream before the 433 MHz modulator: the BC547C inside the head
acts as an open-collector inverter on the data line, which the
controller PCB receives via R1 (4k7 pullup to 3.3 V) into GPIO12.
The decoder times the resulting edges in software.

- **Sync**: 4 cycles of 833 µs HIGH + 833 µs LOW (8 pulses)
- **Data**: 65 bits, each 625 µs long
  - `1` = 417 µs HIGH + 208 µs LOW
  - `0` = 208 µs HIGH + 417 µs LOW
- **Frame**: `PRE5 ID19 BAT1 TEST1 CH2 TYPE4 VAL_A12 VAL_B12 CRC8 stop1`
  - Preamble: `(b[0] >> 3) == 0x01`
  - CRC-8 poly `0x31` init `0x00` over bytes 0..6
  - `type=1`: `T = (VAL_A − 500) / 10 °C`, `H = VAL_B %`
  - `type=2`: `W = VAL_A / 10 km/h`, `dir = VAL_B°`

The decoder is **polarity-agnostic** — it tries both bit orientations and
keeps whichever passes CRC. This means it works regardless of whether
your specific hardware adds or removes inversions in the signal path.

## Stepwise trial-and-error

### Step 1: are edges arriving at GPIO12?

Watch the stats line that's logged every 10 s:

```
stats: edges=1234 syncs=12 valid=8 invalid=4 (preamble_fail=3 crc_fail=1 short=0) isr_overflows=0
```

- **`edges = 0` for 60+ seconds**: the wind head isn't transmitting,
  or the cable/transistor stage on the controller is broken. Re-check:
  - The 4-wire cable between head and controller.
  - T1 (BC337) on the controller PCB.
  - 3 V on the head's `+3V` line.
  - The BC547C + 1k mod inside the head (Bauanleitung §8).
- **`edges > 0` but `syncs = 0`**: continue to step 2.

### Step 2: are the pulse widths where they should be?

Every 30 s you get a histogram. A healthy signal shows three distinct
clusters at ~208, ~417, and ~833 µs, plus a tall ">=5k" bucket for the
inter-packet silence:

```
[I][tfa_tx141w]: Pulse-width histogram (µs, last 30s):
[I][tfa_tx141w]:     <150        0
[I][tfa_tx141w]:   150-200       3
[I][tfa_tx141w]:   200-250     156  <-- expect 'short' (~208µs)
[I][tfa_tx141w]:   250-300       2
[I][tfa_tx141w]:   300-350       0
[I][tfa_tx141w]:   350-400      18
[I][tfa_tx141w]:   400-450     154  <-- expect 'long' (~417µs)
[I][tfa_tx141w]:   450-500      12
[I][tfa_tx141w]:   500-700       1
[I][tfa_tx141w]:   700-900      24  <-- expect 'sync' (~833µs)
[I][tfa_tx141w]:    >=5k        24  <-- inter-packet gaps
```

Failure modes:

| Histogram pattern | Likely cause | Fix |
|---|---|---|
| Everything in `<150` and `>=5k` | Slow toggling, not real data. Transistor stage not switching cleanly, or head not transmitting. | Recheck wiring; replace cable. |
| Clusters at wrong widths (e.g. 100/200/400 instead of 208/417/833) | TFA variant with different timing or off-frequency MCU. | Override `t_short` / `t_long` / `t_sync` in YAML; widen `t_tolerance`. |
| Smeared clusters (200–500 evenly populated) | Noise on the data wire. | Shorten cable; add 10–22 nF between GPIO12 and GND (C1 already provides 1 nF). |
| No spike at `>=5k` | Continuous edges, no quiet gaps. 50/60 Hz mains pickup on a long unshielded run. | Shield the cable or shorten. |

### Step 3: sync OK but no valid packets

If `syncs > 0` and `valid = 0` after several minutes:

- **`preamble_fail >> crc_fail`**: bit alignment is wrong (off-by-one
  pulse). Usually means `min_sync_pulses` is too low and you're locking
  onto noise. Try 10 or 12.
- **`crc_fail >> preamble_fail`**: bit thresholding is wrong on individual
  bits. Tighten `t_tolerance` to 100 µs.
- **`short` counter climbs**: packets cut off before 128 pulses. Check
  `isr_overflows`; if non-zero, something else on the board is hogging
  IRQs (rare on ESP8266 with this code).

### Step 4: read raw frames

When the decoder fails CRC but got far enough to assemble bytes, it logs
them at DEBUG and pushes them to the `TFA last packet` text sensor:

```
RAW: 09 12 34 12 02 00 38 7A 80 (norm:crc inv:preamble)
```

`norm:crc` here means the un-inverted byte stream had a good preamble
(`09 >> 3 == 1`) but CRC didn't match. If specific bytes are consistently
close to plausible values (e.g. byte 4 always near 7C/7D for a 25 °C
reading) you can sometimes spot where bits are flipping.

### Step 5: once it works

Once `valid` is climbing steadily and the temperature / humidity / wind
sensors are publishing sane numbers:

- Set `tfa_tx141w.debug: false` in `weatherman.yaml`.
- Drop `logger.level` to `INFO`.
- Apply via OTA: `esphome run weatherman.yaml`.

The packet counters (`packets_valid`, `packets_invalid`, `sync_detections`)
stay exposed as diagnostic sensors, so you can see signal degradation
later in HA's history graph.

## Tuning parameters

| YAML key            | Default | What it does                                              |
|---------------------|---------|-----------------------------------------------------------|
| `t_sync`            | 833     | Expected sync half-pulse width (µs).                      |
| `t_short`           | 208     | Expected short data half-pulse width (µs).                |
| `t_long`            | 417     | Expected long data half-pulse width (µs).                 |
| `t_tolerance`       | 200     | ± window around each of the three above.                  |
| `t_reset`           | 2000    | Pulse wider than this resets the sync state.              |
| `min_sync_pulses`   | 8       | Sync pulses required before locking onto data (rtl_433 spec). |
