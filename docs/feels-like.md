# Thermal comfort and heat-stress indices

Multiple "feels like" formulas exist, calibrated for different
conditions and using different inputs. This document covers what's
implemented in the firmware, what's deliberately left out, and where
to look for more sophisticated indices.

## What this firmware provides

The YAML exposes four "feels like" / thermal-stress sensors that *can*
be computed from the available inputs:

| Sensor | Formula | Best for | Valid range |
|---|---|---|---|
| `Wind chill` | NOAA 2001 | Cold-weather perceived temp | T ≤ 10 °C and v ≥ 4.8 km/h |
| `Apparent temperature` | BoM / Steadman 1994 | All-purpose "feels like" | Full range, shade only |
| `Heat index` | NOAA Rothfusz | Hot-weather perceived temp | T ≥ 27 °C, RH ≥ 40 % |
| `Wet bulb temperature` | Stull 2011 closed-form | Hard physical heat-stress limit | T ∈ [−20, 50] °C, RH ∈ [5, 99] % |

The first three are "what does it feel like" indices. **Wet bulb is
different**: it's the temperature a parcel would reach if cooled by
evaporation, which is the hard thermodynamic limit on how cold
sweating can keep the body. At WBT ≈ 35 °C, even a fit person in
shade with unlimited water dies of hyperthermia within a few hours
because sweating can't lower body temperature below ambient. It's
worth watching as a climate-change indicator: WBT 35 °C has only
recently started to be observed at all and is expected to occur with
increasing frequency.

All four assume **shade conditions** (T_mrt ≈ T_air). In direct
sunlight, the true perceived temperature can be 5–10 °C higher than
the dry-air formulas predict. If your weather station is permanently
in sun, none of the formulas are appropriate without correction.

Wind Chill and Heat Index form a symmetric pair (cold+wind vs
hot+humid). Apparent Temperature smoothly covers the whole range and
is the right default for a "feels like" display. Wet Bulb is the
physical-limit canary.

## Why no UTCI

The most physically rigorous "feels like" index in current use is the
[Universal Thermal Climate Index (UTCI)](https://www.utci.org/). It
combines air temperature, mean radiant temperature, wind speed, and
humidity through a 6th-order polynomial regression (210 terms, per
Bröde et al. 2012) calibrated against a multi-node human thermal model.

For the WEATHERMAN 2.1, full UTCI **cannot be computed**. The blocker
is mean radiant temperature (T_mrt), which requires either a
black-globe thermometer or a complex estimate from solar radiation,
surface albedos, and view factors. We measure air temperature,
humidity, and wind — three of the four required inputs — but not
T_mrt.

## If you want closer-to-UTCI accuracy

Two options, neither implemented in this firmware:

### Option A: Estimate T_mrt from solar irradiance

If you have a calibrated pyranometer (W/m²) you can estimate T_mrt
under direct sun. Without one, BH1750 illuminance (lx) is too rough
to substitute — lx is a photopic-weighted measurement that drops the
infrared component that matters most for radiant heating.

A pragmatic but rough approximation when in sun (illuminance > 50,000 lx):

```
T_mrt ≈ T_air + 5°C
```

is the kind of bias a black-globe in summer sun would show in moderate
wind. This is rough enough that it would mostly cancel out compared
to the AT formula's existing optimism, so it's not worth implementing
on the ESP.

### Option B: Compute UTCI in Home Assistant

There's no official `utci` integration for HA, but a community template
sensor implementing the Bröde polynomial is available in various
forms. The polynomial is too large to embed cleanly in an ESPHome
lambda — it would be better as a HA Python or pyscript template
sensor that reads our `Outdoor temperature`, `Outdoor humidity`,
`Wind speed`, and a manually-set or solar-estimated `T_mrt`.

If anyone implements this cleanly, a PR adding an HA example
configuration here would be welcome.

## Recommendation

For practical use, `Apparent temperature` is the right sensor to put
on a dashboard. It's calibrated against extensive field data, has the
right qualitative behaviour across the temperature/wind/humidity
space, and is what's published as "feels like" by national weather
services in several countries.

Wind Chill is kept as a separate sensor because it has different
semantics (cold-only, single-purpose) and is more widely recognised
by users in some regions. Both can be shown side-by-side in HA, with
the user picking which they find more useful.
