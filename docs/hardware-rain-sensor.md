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
capacitance increase wins. A dedicated bench experiment (next subsection)
sharpened this from "C beats R" into something stronger: the resistive
contribution is not merely outweighed, it is *below the measurement floor*. Even
soaking wet, the comb's conductance never pulls the node across a logic threshold
(see the `bridge` column in the results table). So the operative conclusion is
that on this hardware the wetness signal is **effectively all capacitance**. This
fits a thin, high-resistance film that is nonetheless dielectrically large across
the tightly-spaced fingers (large ΔC, *negligible measurable* ΔG), consistent
with gold-plated electrodes and relatively non-conductive (clean/soft) water.

The stall.biz product page describes the sensor as responding to *both* the
resistance and the capacitance change (the gold plating being there for corrosion
protection of the resistive traces). That dual-mode behaviour **does not
reproduce on this unit** — the experiment below tests for a separate resistive
signal directly and finds none across the full dry→deluge range. Whatever the
closed original firmware did with a resistive read, here it would be reading
noise. This firmware therefore takes the single RC-discharge timing — which
captures the entire usable signal as a clean, monotonic wetness measure — rather
than attempting a resistance/capacitance split.

The absolute dry baseline of ~6 µs is *faster* than the ~95 µs that the 112 kΩ
bleed and a full 1 nF cap would give — and the dual-read experiment closes the
gap. Its dry baseline read **cap = 6 µs** and **res = 2 µs**, and the resistive
read is the decisive one: it charges the node through R6's *known* 100 kΩ from
GPIO15, so its 2 µs rise pins the node capacitance independent of the (unknown,
dry) comb resistance — a full 1 nF charging through 100 kΩ would take ~100 µs, not
2 µs. Both reads land on an effective node capacitance of **a few tens of pF**
(~40–60 pF). So the second of the two candidates above is the answer: **the
capacitance actually in play is well under 1 nF.** Whatever C1's exact role, it is
not presenting its full 1 nF to the node — the node behaves as ~50 pF, and
112 kΩ · 50 pF · ln(3.3/1.4) ≈ 5 µs, matching the observed ~6 µs. The baseline is
therefore explained, not anomalous.

This also sets the scale of the inversion. With the conductance immaterial (so the
discharge resistance stays ~112 kΩ wet as well as dry), reaching the wet ~67 µs
requires the node capacitance to climb to ~700 pF — i.e. a water film adds on the
order of **~0.6 nF**. A large ΔC and an essentially unchanged R: precisely the
capacitance-dominated picture, now quantified rather than inferred.

This also matches the original's quoted responsiveness: the product page cites a
1–5 s rain reaction time, consistent with our ~5–7 s detection target (see the
detection philosophy section).

### Dual-read experiment: is there a separate resistive signal?

The stall.biz page claims the sensor evaluates *both* resistance and capacitance,
and the topology looked like it might provision for it: R6 returns node N to a
*controllable* GPIO (D8) rather than straight to GND, which is exactly what a
second driven electrode would need. To test whether a resistive read carries
information the capacitive read does not, the shipping single read was temporarily
replaced with a dual-read diagnostic.

One hypothesis is worth ruling out up front: the two pins are **not** used for
polarity alternation (driving the comb +/− on alternate cycles to average out
electrolytic corrosion). They *cannot* be — one comb electrode is hard-grounded,
so the comb's polarity cannot be reversed. Both excitations below drive the
signal side positive. The only thing a second pin can buy here is reading the node
in a *different regime*. Corrosion protection on this comb is the gold plating
alone.

The two regimes:

- **Capacitive (C-weighted)** — the normal read. Charge N from D7, release, time
  the fast fall to GND. On the microsecond timescale the node capacitance sets the
  fall, so this tracks ΔC.
- **Resistive (R-weighted)** — drive D8 HIGH through R6 and watch N rise. Once the
  node settles, its level is the pure resistive divider
  `3.3 V · R_comb/(R_comb + 100 kΩ)` — capacitance has dropped out — so a
  conductive film *lowers* it. Two things are logged: a continuous rise-time, and
  a settled digital level `bridge`, read after the node is given time to settle.
  `bridge` is the clean resistance bit: `no` = node still high (no conductive
  path), `YES` = a film is pulling it below threshold. With only digital I/O, that
  settled level is the one genuinely capacitance-free resistance measurement
  available.

The diagnostic lambda (it replaced the 2 s sense interval and was reverted
afterwards; the C channel kept driving `rain_comb_us` + `rain_sensor_value`, so
the detector stayed live during the run):

```cpp
const uint8_t SENSE = 13;          // D7 — comb sense node
const uint8_t EXC   = 15;          // D8 — far terminal via R6 100k
const uint32_t CAP_TIMEOUT = 600;  // us, fall window
const uint32_t RES_TIMEOUT = 2000; // us, rise window

// Channel C: capacitive discharge time (median of 7)
uint32_t cs[7];
for (int i = 0; i < 7; i++) {
  pinMode(EXC, INPUT);             // restore the R6 + 12k bleed leg
  pinMode(SENSE, OUTPUT);
  digitalWrite(SENSE, HIGH);       // charge the node
  delayMicroseconds(120);
  uint32_t dt;
  { InterruptLock lock;            // lock only the short timing window
    pinMode(SENSE, INPUT);
    uint32_t t0 = micros();
    while (digitalRead(SENSE) && (micros() - t0) < CAP_TIMEOUT) { }
    dt = micros() - t0; }
  cs[i] = dt;
  delay(3);
}
// ... insertion-sort cs[], cap_us = cs[3];

// Channel R: resistive rise-time + settled level (median of 5)
uint32_t rsa[5];
bool res_high = true;
for (int i = 0; i < 5; i++) {
  pinMode(EXC, INPUT);
  pinMode(SENSE, OUTPUT);
  digitalWrite(SENSE, LOW);        // drain node to ~0 V
  delayMicroseconds(300);
  pinMode(SENSE, INPUT);           // release sense
  pinMode(EXC, OUTPUT);
  digitalWrite(EXC, HIGH);         // excite far electrode through R6
  uint32_t t0 = micros();
  while (!digitalRead(SENSE) && (micros() - t0) < RES_TIMEOUT) { }
  rsa[i] = micros() - t0;          // rise time (RES_TIMEOUT if it never crosses)
  delayMicroseconds(1500);         // let the divider settle
  res_high = digitalRead(SENSE);   // settled level: HIGH = no conductive bridge
  pinMode(EXC, INPUT);             // restore D8 boot-strap pulldown
  delay(3);
}
// ... insertion-sort rsa[], res_us = rsa[2];

ESP_LOGI("rain_dual", "cap=%4u us  res=%4u us  bridge=%s",
         (unsigned) cap_us, (unsigned) res_us, res_high ? "no" : "YES");
```

The resistive rise loop is deliberately *not* inside an `InterruptLock` — a ~2 ms
window is too long to hold interrupts off — so its rise-time jitters with 433 MHz
TFA reception (the median tames it); the settled `bridge` read is unaffected and
is the value to trust. Note the resistive read leaves a brief DC excitation on the
comb each cycle; benign for a bench run, and another reason it is not in the
shipping firmware.

Wetting the comb from dry through a real heavy-rain event:

| comb state                   | cap (µs) | res (µs) | bridge |
|------------------------------|----------|----------|--------|
| bone dry                     | 6        | 1–2      | no     |
| dry, jitter ceiling          | 10       | 2        | no     |
| light mist                   | 14–51    | 21–47    | no     |
| splashing                    | ~63      | ~54      | no     |
| real heavy rain (saturated)  | 65       | 57–58    | no     |

Three findings, and they overturn the dual-mode hypothesis for this hardware:

1. **`bridge` never fired — not once, not even in saturated heavy rain.** The
   resistive divider never pulled the node below threshold, so the comb's
   conductance, even soaking wet, stays too low (resistance too high) to register
   on a digital read. The clean resistive channel carries **no usable signal
   across the entire dry→deluge range.**
2. **`res` and `cap` are the same signal.** The resistive *rise-time* is itself an
   RC measurement and shares the node capacitance, so it tracks `cap` (both climb
   with wetness) rather than measuring anything independent. There are not two
   channels here — there is one capacitance signal read two ways, plus a
   resistance bit that never trips.
3. **So a compound value buys nothing here.** Whatever the closed original
   firmware did with a resistive read, on this comb it would be reading noise; the
   single capacitive read captures the whole usable signal. The most likely reason
   the conductance is immaterial is the combination of gold-plated electrodes and
   relatively non-conductive (clean/soft) water — a wet-but-high-resistance film
   that is dielectrically large but barely conductive.

The experiment was reverted and the shipping firmware keeps the single capacitive
read. This subsection preserves the method and code so the question need not be
re-opened from scratch: a future build with a *different* comb (unplated, or a
more conductive environment) can drop the same diagnostic back in to re-check
whether a resistive channel has become worthwhile.

### Why GPIO13 and not GPIO15 for sensing

Because the comb has to *dominate* the discharge. On GPIO13 the alternate path
to ground is the high-impedance 112 kΩ bleed, so the comb — chiefly its
capacitance — is what moves the timing. If you instead charged and sensed on
GPIO15, the 12 kΩ
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
- **Rain comb (`wm_regensensor_wert` → `wm_regenmelder`).** Measures the surface
  *capacitance* — whether a water film is dielectrically loading the comb (the
  conductance turns out to be immaterial; see the experiment above). Fast, but it
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
  indefinitely. At ~0.7 W this is accepted rather than guarded — but if a stuck-wet
  fault is a concern for a given install, a timed script gating the rain-heating
  term is the clean place to add a cap.
