# Rain sensor circuit analysis

The WEATHERMAN 2.1 rain detector is a gold-plated interdigitated comb PCB plus
a small resistive heater. The heater is a *separate* circuit — a resistive
element on the same PCB as the comb but galvanically isolated from it, switched
low-side by a BJT under PWM on GPIO0. It warms the comb thermally, through the
shared substrate; there is no electrical path from the heater into the sense
traces. The comb sensing path is more interesting than it first appears, and
this document records what it actually does — as established by direct
measurement on the hardware, not by reading the schematic
alone.

## Circuit topology (verified)

Established from the WEATHERMAN 2.1 schematic and the WeMos D1 mini schematic,
cross-checked against continuity measurement on the populated PCB (the comb's two
electrodes, and D8→GND):

```
   GPIO13 / D7 ──┬── signal node N           ← sense pin
                 │
                 ├── C1 1nF ── +5V           ← storage cap, referenced to 5V
                 │
   rain comb ────┤                           ← one electrode on N
   (gold grid)   │
   rain comb ────┴──────────────────── GND   ← other electrode hard to GND
                 │  
                 │
                 └── R6 100kΩ ── GPIO15 / D8 ── R11 12kΩ ── GND
                                  (R11 is on the WeMos module)

   R10 (main-board pulldown footprint): X  NOT POPULATED — see note 2
```

Key facts:

1. **The comb is a direct conductance/capacitance element between GPIO13 and
   GND.** One electrode sits on the GPIO13 signal node N; the *other electrode is
   hard-grounded*. The comb is **not** a floating node bridged sideways to
   a bias network — an earlier version of this doc had it that way, which was
   wrong. C1 hangs off N and is referenced to the +5 V rail.
2. **GPIO15 (D8) carries a 12 kΩ pulldown (R11) on the WeMos module itself**
   (confirmed on the D1 mini schematic — R11, IO15→GND, alongside the R9/R10
   boot-strap 12 kΩ pulls on IO0/IO2). The WEATHERMAN main board crosses out its
   *own* R10/R11 footprints precisely because the module already provides the
   pulldown.
3. **R6 (100 kΩ) ties N to D8**, where it meets that 12 kΩ. R6 + R11 ≈ 112 kΩ is
   therefore a fixed, high-impedance secondary path from N toward GND, parallel
   to the comb. It is a bias/bleed leg, not the primary sense path.

So GPIO15's 12 kΩ does double duty: it satisfies the ESP8266 boot-strap
requirement (GPIO15 LOW at boot) *and*, through R6, gives N a fixed weak bleed in
parallel with the comb. GPIO13 has no strapping constraint and is free to be
driven — which is what the active-sense method below relies on.


## How wetness is sensed: active RC-discharge timing

Passive observation of either pin cannot work, and the firmware does not attempt
it. GPIO15 only reaches node N through R6 (100 kΩ) and is clamped near GND by its
12 kΩ pulldown, so it never meaningfully moves. GPIO13 sits on node N, but without
actively charging the node there is nothing to time — N just rests low, pulled
there by the comb (when wet) and the R6 + R11 bleed. The signal has to be
generated actively, by charging the node and watching how fast it falls.

The technique is charge-transfer timing on GPIO13:

```cpp
pinMode(13, OUTPUT);
digitalWrite(13, HIGH);          // charge C1 (1 nF) hard to Vcc
delayMicroseconds(120);
pinMode(13, INPUT);              // release; node bleeds and the level falls
uint32_t t0 = micros();
while (digitalRead(13) && (micros() - t0) < TIMEOUT_US) { }
uint32_t discharge_us = micros() - t0;   // time to fall below logic threshold
```

The firmware drives D7 **HIGH** to charge C1 and the comb's capacitance up to
Vcc — the pin's own high output is what charges the node — then releases the pin
to a high-impedance INPUT and times how long node N takes to fall back below the
input logic threshold (~1.4 V) as it discharges to GND through the comb (in
parallel with the R6 + R11 bleed). The 2 s sense interval in `rain_heater.yaml`
does exactly this, takes a **median of 7** samples (with an `InterruptLock`
around just the timing window) to reject ISR jitter, and publishes both the raw
time and a derived wetness %.

### The measured transfer function is inverted (and why)

The signal is monotonic but runs opposite to a naive "water shorts the comb to
ground" intuition:

| comb state    | measured discharge time |
|---------------|-------------------------|
| dry           | ~6 µs   (fast)          |
| saturated wet | ~67 µs  (slow)          |

**Longer discharge = wetter.** With the topology now verified — node N charged
HIGH, then released to discharge to GND through the comb in parallel with the
R6 + R11 ≈ 112 kΩ bleed — the two effects of a water film act in *opposition*,
and the measurement tells us which wins:

- **Resistance: would speed the fall.** A water film adds a conductive path from
  N to GND across the gold fingers, i.e. it *lowers* the discharge resistance.
  On its own that would make a wet comb discharge *faster*.
- **Capacitance: slows the fall, and dominates.** The interdigitated comb is a
  capacitance-maximising geometry — long, closely-spaced finger edges. Water's
  relative permittivity is ~80, so a film sharply *raises* the capacitance on
  node N. Since τ = R·C, more capacitance lengthens the fall.

These pull opposite ways, so the direction is not obvious a priori — but the
hardware settles it: **wet is slower (~67 µs) than dry (~6 µs)**, so the
capacitance increase outweighs the conductance increase in the RC product. This
is consistent with a thin, relatively high-resistance film that is nonetheless
dielectrically significant across the tightly-spaced fingers (large ΔC, modest
ΔG). It also matches the stall.biz product description, which states the sensor
responds to *both* the resistance and the capacitance change when a drop lands on
the grid (the gold plating being there for corrosion protection of the resistive
traces).

This firmware does not measure resistance and capacitance separately (as the
original firmware apparently did) — the single RC-discharge timing captures their
combined effect, which is all that's needed for a clean, monotonic wetness signal.

The absolute dry baseline of ~6 µs is *faster* than the ~95 µs
that the 112 kΩ bleed and a 1 nF cap alone would give. With the comb dry and
contributing almost no conductance, C1 should discharge slowly through the 112 kΩ
leg — yet it falls in microseconds. The most likely explanations are that the
effective discharge resistance dry is far below 112 kΩ (board/input leakage, or
the comb's residual dry conductance), or that the node capacitance actually in
play is well under 1 nF. This doesn't affect operation: the transfer function is
monotonic over a usable ~11× span, repeatable, and parametrised by the runtime
calibration below. A bench measurement of the node's actual RC dry (e.g. scope
the fall, or measure C1 and the dry R to GND directly) would close it; it is not
necessary to operate the sensor.

This also matches the original's quoted responsiveness: the product page cites a
1–5 s rain reaction time, consistent with our ~5–7 s detection target (see the
detection philosophy section).

### Why GPIO13 and not GPIO15 for sensing

Because the comb has to *dominate* the discharge. On GPIO13 the alternate path
to ground is the high-impedance 112 kΩ bleed, so the comb's conductance is what
moves the timing. If you instead charged and sensed on GPIO15, the 12 kΩ
pulldown would dominate and the comb (sitting behind R6's 100 kΩ) would shift
the discharge by only a few percent — swamped. The designer put the comb on the
clean high-impedance pin and the fixed reference behind R6 on the strapped pin
probably for exactly this reason.

GPIO15 should therefore stay **undeclared and unread** — driving or sensing it
fights its boot-strap role and adds nothing. GPIO13 must likewise not be claimed
by any other component, since the sense lambda flips its mode between OUTPUT and
INPUT itself.

### Heater PWM coupling

The heater is galvanically separate from the comb (above), but the two share the
+5 V/GND supply — the sensor connects through a single Regensensor/Heizung/+5V/
GND header — so the heater's switched current can ripple the shared rail that C1
references. The coupling is via the supply (and a little PCB-trace capacitance),
*not* a shared signal node; there is no DC interference path at all. The relevant
low-pass corner for what does couple in is

f_c = 1 / (2π · R6 · C1) = 1 / (2π · 10⁵ · 10⁻⁹) ≈ **1.6 kHz**

The 1 kHz PWM fundamental sits *just below* this corner (≈ 0.6 f_c), so it is
only lightly attenuated (~15 %) — it is **not** filtered away. Intermediate
heater duties can therefore perturb the discharge timing. Two things keep this
benign in practice:

- the median-of-7 absorbs most of the residual ripple, and
- the default rain-heating duty is **100 %**, which is constant DC — there is no
  1 kHz component at 0 % or 100 % duty (ripple is worst near 50 %). This is why
  cranking the heater to 100 % manually does not move the dry baseline.

If you ever lower the rain-heating duty below 100 %, re-verify that the comb
reading is not tracking heater duty; if it is, raise the duty back to 100 % or
median more samples.

## The published metric and its calibration

`rain_sensor_value` (the `wm_regensensor_wert` parity entity, 0–100 %, higher =
wetter) is a linear map of the measured discharge time between two
runtime-calibratable endpoints:

```
wet% = 100 · (discharge_us − dry_us) / (wet_us − dry_us)     clamped 0…100
```

`dry_us` and `wet_us` are exposed as the `Rain comb dry baseline` and `Rain comb
wet saturation` `number:` entities (per-unit, restore across reboot), because
C1 tolerance, the exact node network, and the input threshold vary between
builds. Calibrate `dry_us` from a confirmed-dry reading of the `Rain comb
discharge time` diagnostic **with the heater off**, and `wet_us` from a soaked
reading. The map is divide-by-zero guarded if `wet_us ≤ dry_us`.

`rain_sensor_value` has exactly one writer — the 2 s active-sense interval. The
old passive GPIO15 duty-cycle sampler must never be reintroduced: GPIO15 never
goes high through R6 + 12 kΩ, so it only stomps the value to 0 and breaks the
detector's `delayed_on` latch.

## Detection philosophy: two sensors, two jobs

The station has two independent rain signals, and they measure genuinely
different physical quantities:

- **Rain bucket (`wm_regenstaerke`, mm/h).** Integrates *volume over time* by
  counting tips. Quantitative, and necessarily slow — it needs ~0.5 mm of
  accumulation to tip. This is the **intensity** source.
- **Rain comb (`wm_regensensor_wert` → `wm_regenmelder`).** Measures surface
  *conductance* — is there a water film bridging the traces. Fast, but it
  **saturates** as soon as a film forms: a drizzle that wets the surface and a
  downpour read nearly the same. This is a **presence** signal, not an intensity
  one.

Consequently the comb is used only as a fast binary "is it raining" detector for
protective automations (retract awning, close roof windows); rain *strength* is
read from the bucket. Reading "strength" off the comb would really be reading
film thickness, dominated by droplet placement and drying rather than rainfall
rate. (The original firmware agrees: it named the bucket *regenstaerke* — rain
strength — and the comb *regensensor_wert* — sensor value — pointedly not a
strength.)

Because detection latency is the whole point of the comb, the `Rain detector`
binary sensor uses **asymmetric** on/off delays:

- `delayed_on: 5s` — fast onset. At the 2 s sense cadence that is ~2–3 confirmed
  median-of-7 samples: enough to reject a lone splash, bug, or dew drop, while
  flagging real rain in ~5–7 s.
- `delayed_off: 2min` — deliberately conservative. A missed onset (a wet awning)
  costs far more than a false trigger (a needless retract), so the flag stays
  "wet" until it is confident the comb is dry. The conservative clear is also
  what protects against the heater's early-dry-off effect (below).

## Heater philosophy: it dries, it does not fight rain

The heater is low-power and electrically separate from the comb. The build
manual's ohmmeter checks pin both facts down: the heating resistor measures
~35 Ω between connector points A and F, while A–B and A–D both read open —
i.e. the heater shares no circuit with the comb's sense traces (the D–B pair,
which drops to 0 Ω only when bridged by water). Heat reaches the comb by
thermal conduction through the shared PCB substrate, not by any current in the
comb.

The heater runs from 5 V, so ~35 Ω dissipates ~0.7 W. Evaporating
water costs ~2400 J/g, so even at 100 % efficiency that is only ~1 g/h — and a
palm-sized comb under light 1 mm/h rain already collects ~2.5 g/h. So the heater
**cannot keep the surface dry while it is actually raining**; even light rain
delivers more water than it can evaporate, and a downpour far more.
The heater's real job is to drive off the residual film *after* rain eases,
restoring a dry, responsive surface quickly for the next event.

The control loop (auto mode, evaluated every 60 s) applies the `std::max` of
three duty sources:

- **(a) Anti-dew** — ramps up as `(temperature − dewpoint)` approaches the
  configured margin. Pre-emptive: it keeps dew from forming a film the comb
  would misread as rain. It is **not** suppressed during rain — at 0.7 W there is
  nothing to suppress (the heater cannot dry the comb out from under active
  rain), so suppression added complexity for no effect.
- **(b) Frost protection** — full power below the frost-protect temperature,
  ramping over a configurable band above it. Temperature-driven and
  unconditional, so freezing precipitation gets the comb heated before ice can
  lock it. This is the one case where heating *during* precipitation is
  mandatory, and it is handled independently of the rain flag.
- **(c) Rain heating** — applies `cfg_rain_heat_pct` (default 100 %) while the
  rain detector is active, continuing through the `delayed_off` tail after rain
  stops. This is the dry-off/recovery phase. It runs *during* rain as well as
  after, which is intentional: the energy cost is small and the benefit is a
  comb that is dry and responsive sooner. Crucially it does **not**
  oscillate, precisely because 0.7 W cannot dry the comb out from under falling
  rain — so the detector will not chatter off while it is genuinely raining.

(a) and (b) require a valid outdoor temperature; if it is NaN they are skipped,
but (c) still applies — rain heating is independent of the temperature sensor
(rain is confirmed by the comb, and 0.7 W cannot corrupt a wet reading), so a dead
temperature sensor does not disable rain-drying. Every path sets an explicit
duty each cycle, so there is no stale-latch risk.

### Two design choices worth recording

- **No separate drying-boost timer, and no rain-bucket gating.** An earlier
  iteration tried to drive a post-rain drying pulse from the detector's release
  event, and a variant gated drying on bucket silence. Both were removed in
  favour of "heat while the detector is wet, plus its `delayed_off` tail," which
  produces the same recovery behaviour with far less machinery and no
  circular-dependency hazards. Simplicity-first; the risk of the simple version
  is only a little extra energy.
- **The early-dry-off coupling, and why `delayed_off` stays generous.** Because
  the heater dries the *comb* ahead of the surrounding awning/roof surface, the
  comb can read "dry" while the protected surface is still wet. The conservative
  `delayed_off: 2 min` (raise it in showery climates) is what holds the
  automation until the surroundings catch up. Do not shorten it to chase faster
  clearing.
- There is deliberately no maximum heat-on cap: rain heating lasts the entire
  detected-wet period. A comb that reads wet forever (a corrosion or leakage fault
  in the sensor or its connector) would therefore hold the heater on
  indefinitely. At ~6 W this is accepted rather than guarded — but if a stuck-wet
  fault is a concern for a given install, a timed script gating the rain-heating
  term is the clean place to add a cap.
