# Rain sensor circuit analysis

The WEATHERMAN 2.1 rain detector consists of a gold-plated interdigitated
comb PCB and a heater. The heater is independent and trivially modeled
(low-side BJT, PWM on GPIO0). The comb sensing path is more interesting
than it first appears.

## Circuit topology (verified)

```
Regensensor signal -----+---- R6 100kΩ -----+----- GPIO15 (D8)
                        |                   |
                       (bias rail)         C1 1nF
                                            |
                                           GND

R10 100kΩ:  X  NOT POPULATED on the WEATHERMAN PCB
```

The schematic's `R10 100k` is marked NC. For the ESP8266 to boot from
flash, GPIO15 must be LOW at boot, so *something* has to bias the line
low when the firmware starts. Possibilities, **all currently
unverified**:

1. **The specific WeMos D1 mini variant in use has an intrinsic 10 kΩ
   pulldown on D8.** Many clones do; the official LOLIN schematic
   doesn't clearly show one but bare ESP-12E/F modules sometimes
   include a pulldown for exactly this strapping reason.
2. **Leakage through R6 + the dry rain comb to GND** provides enough
   bias. The comb is gigohms when dry, so this is a marginal
   explanation but not impossible.
3. **Something else on the board** I'm not seeing in the schematic.

### Boot test to settle this

Easy experiment with the closed-source firmware still installed:

1. Remove or open-circuit the rain comb's bias rail temporarily
   (disconnect the Regensensor signal at the 8-pin header).
2. Power-cycle the WEATHERMAN.
3. If it still boots normally, the bias is coming from inside the
   WeMos module (possibility #1). The PCB really is robust without R10.
4. If it boot-loops or hangs, the rain comb path was carrying the
   bias (possibility #2). In that case you'd want to add an external
   10 kΩ pulldown from D8 to GND for reliability.

Worth running this once before sealing the box. The fix in either
case is trivial.

## What this means electrically

### R6 + C1 = 1.6 kHz low-pass filter

The series 100 kΩ and shunt 1 nF roll off any signal above

$$f_c = \frac{1}{2\pi R C} = \frac{1}{2\pi \cdot 10^5 \cdot 10^{-9}} \approx 1.6\ \text{kHz}$$

Time constant τ = 100 µs. Any perturbation faster than ~1.6 kHz is
attenuated before reaching the pin.

### The bias picture depends on whether a pulldown exists

**If a 10 kΩ pulldown exists** (possibility #1 from the topology
section), then the steady-state voltage divider is:

$$V_{GPIO15} = V_{bias} \cdot \frac{10\text{k}}{10\text{k} + 100\text{k} + R_{comb}}$$

With `V_bias = 5 V`:
- Dry comb (R = ∞): V_GPIO15 = 0 V ⟶ firmly LOW
- Shorted comb (R = 0): V_GPIO15 = 5 V × 10/110 ≈ 0.45 V ⟶ still LOW

For GPIO15 to exceed the digital threshold (~1.65 V) the divider math
requires `R_comb + 100k < 30 kΩ`, which is impossible (R6 alone is
100 kΩ). **So under DC analysis with a pulldown, GPIO15 should never
go high regardless of comb wetness.**

**If no pulldown exists**, GPIO15 sits wherever C1 leakage, comb
leakage, and ESP8266 input leakage happen to put it — likely floating
near the digital threshold where small perturbations cause toggling.
This *would* explain how the firmware could detect rain through static
sampling.

Either way, the DC story doesn't fully explain how the original
firmware extracts a wetness signal in clear-cut fashion. Several
mechanisms could be at play:

### Mechanism A: Higher bias rail

If the comb's other electrode is biased to something higher than 5 V
(e.g. a charge-pump or some clever circuit I'm not seeing on the
schematic), the math changes. I don't see any such circuit on page 25
of the Bauanleitung, but the schematic is dense and I may have missed
something.

### Mechanism B: Active capacitive sense

The original firmware may toggle GPIO15 between OUTPUT-HIGH and INPUT
modes. In OUTPUT-HIGH it drives the pin to 3.3 V, charging C1. When
switched to INPUT, C1 discharges through (R6 + R_comb) toward whichever
rail the comb is tied to. The discharge time is a direct function of
R_comb:

$$t_{discharge} = (R_6 + R_{comb}) \cdot C_1 \cdot \ln\left(\frac{V_{start}}{V_{threshold}}\right)$$

For R_comb = ∞: discharge time = ∞ (line stays high)
For R_comb = 100 kΩ: discharge time ≈ 100 µs
For R_comb = 10 kΩ: discharge time ≈ 10 µs

This would give a clean wetness signal with a wide dynamic range. The
original firmware's `regensensor_wert` value (0–100 scale) is
consistent with this technique: it could be the measured discharge
time normalised to some range. **This is the leading candidate for
what the firmware actually does.**

### Mechanism C: PWM-coupled rectification

The rain heater's 1 kHz PWM (when on) puts ripple on nearby traces.
With C1 already in place to integrate, partial rectification by ESD
diodes inside the ESP8266 could produce a small DC offset on GPIO15
that's a function of how conductive the comb is. Far less plausible
than B but worth mentioning.

## Implications for our YAML

ESPHome's `pulse_counter` with a passive INPUT pin (which is what we
currently have) can only observe whatever GPIO15 does on its own. If
the original firmware uses mechanism B (active capacitive sense), we'd
need to replicate that with our own driver code rather than passively
observing edges.

The current YAML configuration is set up for *empirical investigation*:
both `Rain sensor value` (rolling 60s duty cycle) and `Rain comb edge
rate` (Hz, no debounce) are exposed. The drip test below will tell us
which (if either) carries signal:

### Drip test

1. Wait for a confirmed dry comb (sun + low humidity for several hours).
2. Note the steady-state value of both `Rain sensor value` and
   `Rain comb edge rate`.
3. Drip 1–2 drops of water on the comb (between the gold traces).
4. Watch both values evolve over the next 10–15 minutes as the water
   spreads and evaporates.

**Predicted outcomes:**

- If `Rain sensor value` stays at 0 % throughout and `Rain comb edge
  rate` also stays at 0 Hz: passive observation isn't going to work,
  and we'll need to add a custom active-sense component (toggle output
  and time the discharge — mechanism B).
- If `Rain sensor value` jumps to non-zero values: there's a higher
  bias rail I missed, mechanism A holds, and the duty-cycle metric
  is the right signal.
- If `Rain comb edge rate` spikes to tens or hundreds of Hz: some kind
  of relaxation oscillation or supply-noise coupling is happening
  (mechanism C or a variant), and the edge-rate metric is the right
  signal.

Until the drip test is run, the `Rain detector` binary sensor defaults
to thresholding on `Rain sensor value ≥ regenschwelle` per the original
firmware's literal semantics. After the experiment, switch the lambda
to whichever metric actually responds.

## If we need active sensing

A minimal active-sense component would look like this in a future
component or in YAML lambda:

```cpp
// Charge phase
pinMode(15, OUTPUT);
digitalWrite(15, HIGH);
delayMicroseconds(50);              // settle C1 to Vcc

// Measure discharge
pinMode(15, INPUT);
uint32_t start = micros();
while (digitalRead(15) == HIGH && (micros() - start) < 10000) {
  // spin
}
uint32_t discharge_us = micros() - start;
// Use ESP's internal pulldown to provide a known discharge path;
// alternatively, the WeMos's 10k pulldown does this passively but
// faster than the 100k path through the comb.
```

This is straightforward to implement but is left out of the current
YAML until the empirical investigation determines it's actually
necessary.
