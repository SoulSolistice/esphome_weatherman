# esphome_weatherman

Native [ESPHome](https://esphome.io/) firmware for the [stall.biz WEATHERMAN 2.1](https://www.stall.biz/project/der-weatherman-2-1-die-ideale-wetterstation-fuer-die-hausautomation/)
weather station, replacing the closed-source `wm21_xx.bin` firmware with a fully
open, Home Assistant-native node.

Includes a custom ESPHome component (`tfa_tx141w`) that decodes the wired
data tap from the TFA Dostmann 30.3222.02 wind head — which is a rebranded
LaCrosse TX141W — by timing the baseband OOK pulses on a GPIO and validating
the rtl_433 protocol in software. No SDR, no RF receiver, no calls home.

## Features

- Full feature parity with the original firmware's `wm_*` variables
  (see [parity table](#wm_-variable-parity) below).
- All measurements published via the native ESPHome / Home Assistant API and
  optionally MQTT (for existing subscribers to the `wm/*` topic tree).
- **Built-in web UI** on port 80 for standalone access without
  Home Assistant. Useful for tuning thresholds, viewing live values,
  and as an OTA fallback.
- Rain heater control: manual 0–100 % slider, or automatic mode that
  combines anti-dew (dewpoint margin) and frost protection.
- Pulse-width histogram and per-stage failure counters in the decoder logs,
  so the TFA decoder can be tuned without a scope.

## Runtime configuration

Six calibration parameters are exposed as runtime `number:` entities and
can be tuned without re-flashing via Home Assistant, the on-device web UI,
or MQTT:

| Number entity                       | Original firmware param | Default |
|-------------------------------------|-------------------------|---------|
| Altitude                            | `altitude` (param 20)   | 513 m   |
| Rain bucket calibration             | `regen_koeff` (param 11)| 0.485 mm/tick |
| Sun lux threshold                   | `sonne_lux_schwelle` (param 10) | 8000 lx |
| Rain detector threshold             | `regenschwelle` (param 6) | 16 %  |
| Heater dewpoint margin              | `tau_schaltschwelle` (param 27) | 2.0 °C |
| Heater frost-protect temperature    | (new in v0.3)           | 2.0 °C |

Each value restores across reboots. Flash wear is negligible because
user-initiated config changes happen only a handful of times in a
device's lifetime.

Other parameters (TFA decoder timings, network identity, MQTT broker
address) remain as YAML substitutions because they're set at flash time
and changing them implies a re-flash anyway.

## Scope and non-goals

- **CCU interface is not implemented.** The original firmware optionally
  pushes values to a Homematic CCU. This project explicitly drops that
  path; everything goes through ESPHome's HA-native API and/or MQTT.
- **Sun position (elevation, azimuth, minutes-to-sunrise) is not
  computed on-device.** Delegate to Home Assistant's
  [`sun`](https://www.home-assistant.io/integrations/sun/) integration.
- **Rainfall accumulation periods** (`heute`, `gestern`, `letzte Stunde`)
  are not computed on-device. Use HA's `utility_meter` helper; the ESP
  publishes the lifetime total only.

## Repository layout

```
esphome_weatherman/
├── components/
│   └── tfa_tx141w/          # ESPHome external component (drop-in)
├── example/
│   ├── weatherman.yaml      # complete reference configuration
│   └── secrets.yaml.example
├── docs/
│   ├── tuning.md            # TFA decoder trial-and-error guide
│   ├── hardware-rain-sensor.md  # rain comb circuit analysis
│   └── feels-like.md        # UTCI / Apparent Temperature discussion
├── CHANGELOG.md
├── LICENSE                  # MIT
└── README.md
```

## Quick start

### 1. Get the config

```bash
git clone https://github.com/SoulSolistice/esphome_weatherman.git
cd esphome_weatherman/example
cp secrets.yaml.example secrets.yaml
# edit secrets.yaml with WiFi credentials
# edit weatherman.yaml: MQTT broker, altitude, static IP, etc. as desired
```

### 2. First flash

The WeMos D1 mini is sealed inside the polycarbonate box, and the original
proprietary firmware doesn't expose an ESPHome-compatible OTA partition,
so **the first flash must be done over USB**. Three options:

**Option A: ESPHome CLI (developer-friendly)**
```bash
esphome run weatherman.yaml --device /dev/ttyUSB0
```
If `/dev/ttyUSB0` doesn't appear, install the CH340 driver from
[wch.cn](https://www.wch-ic.com/products/CH340.html) or your distro's
package repository.

**Option B: ESPHome Web Tools (no install required)**

Compile a binary first (locally, in Home Assistant's ESPHome add-on, or
via the [ESPHome Builder](https://web.esphome.io/) web tool itself):
```bash
esphome compile weatherman.yaml
# binary appears at .esphome/build/weatherman/.pioenvs/weatherman/firmware.bin
```

Then visit [web.esphome.io](https://web.esphome.io/) in a Chromium-based
browser (Chrome / Edge / Brave), click **Connect**, select your serial
port from the prompt, and upload the firmware.bin. Requires Web Serial
API support, which Firefox and Safari don't have.

**Option C: Home Assistant Add-on**

If you run Home Assistant OS / Supervised, install the ESPHome add-on
from the add-on store. It provides a web UI for compiling and a USB
passthrough for first flash. After the initial flash, the device appears
in the add-on dashboard and all subsequent updates are OTA.

Once the first flash is successful and the device joins your WiFi,
**reseal the box** — all future updates and parameter tuning happen
over the air.

### 3. Tune the TFA decoder if needed

Watch the logs: `esphome logs weatherman.yaml`. You should see a
pulse-width histogram every 30 s; once `packets_valid` starts climbing,
the decoder is locked in. If not, follow the
[trial-and-error guide](docs/tuning.md).

### 4. Lock down

Once everything's stable:

- In `weatherman.yaml`: set `tfa_tx141w.debug: false` and
  `logger.level: INFO`.
- Apply via OTA: `esphome run weatherman.yaml`.

## Consuming this repo as an external component

Other ESPHome configurations can pull just the `tfa_tx141w` component:

```yaml
external_components:
  - source:
      type: git
      url: https://github.com/SoulSolistice/esphome_weatherman
      ref: main
    components: [ tfa_tx141w ]
```

Components live at the conventional `components/` path, so no `path:` is
needed. For local development against your own checkout:

```yaml
external_components:
  - source:
      type: local
      path: ../components       # relative to the YAML location
    components: [ tfa_tx141w ]
```

The component supports multiple instances (since v0.3.0); useful if
you ever want to read two TFA heads from different GPIOs on the same
board.

## Hardware (WEATHERMAN 2.1)

The original Bauanleitung is on
[stall.biz](https://www.stall.biz/project/der-weatherman-2-1-die-ideale-wetterstation-fuer-die-hausautomation/).
GPIO assignments for this firmware, verified against page 25 of
Bauanleitung ver. 10:

| Component                      | Interface          | GPIO        |
|--------------------------------|--------------------|-------------|
| TFA Dostmann 30.3222.02        | open-collector (T2 BC547C in head) + R1 4k7 pullup | GPIO12 (D6) |
| BMP280 (P/T)                   | I²C @ 0x76         | GPIO5 / 4   |
| BH1750 (illuminance)           | I²C @ 0x23         | GPIO5 / 4   |
| WH-SP-RG rain bucket (reed)    | digital pulse      | GPIO14 (D5) |
| Rain comb                      | passive (see [hardware-rain-sensor.md](docs/hardware-rain-sensor.md)) | GPIO15 (D8) |
| Rain comb heater               | PWM (low-side BJT) | GPIO0  (D3) |
| DS18B20 optional shaded probe  | 1-wire             | GPIO2  (D4) |

### Boot-pin notes

- **GPIO0 (heater)**: must be HIGH at boot. WeMos internal pullup handles
  this. Heater may pulse briefly during boot — harmless.
- **GPIO2 (DS18B20)**: must be HIGH at boot. The 4.7 kΩ pullup in the
  DS18B20 circuit handles this.
- **GPIO15 (rain comb)**: must be LOW at boot. The PCB's `R10` is
  unpopulated and the bias source is currently unverified — likely
  either the specific WeMos D1 mini variant in use, or leakage through
  the rain comb circuit. See [docs/hardware-rain-sensor.md](docs/hardware-rain-sensor.md)
  for the boot test to settle this. If you see boot loops, an external
  10 kΩ pulldown from D8 to GND is the safe fix.

## wm_* variable parity

The original firmware exposed values both as MQTT topics and as CCU
system variables. The CCU path is out of scope here; the table below
maps each `wm_*` variable to its ESPHome implementation.

| Original `wm_*` variable      | Type     | Unit  | Implementation                                     |
|-------------------------------|----------|-------|---------------------------------------------------|
| `wm_temperatur`               | number   | °C    | DS18B20 (preferred) → TFA NTC (fallback) → `Outdoor temperature` |
| `wm_feuchte_rel`              | number   | %     | TFA decoder → `Outdoor humidity`                  |
| `wm_feuchte_abs`              | number   | g/m³  | template → `Absolute humidity`                    |
| `wm_taupunkt`                 | number   | °C    | template (Magnus-Tetens) → `Dewpoint`             |
| `wm_windchill`                | number   | °C    | template (NOAA) → `Wind chill`                    |
| (new) Apparent Temperature    | number   | °C    | template (Steadman 1994) → `Apparent temperature`. See [feels-like.md](docs/feels-like.md). |
| (new) Heat Index              | number   | °C    | template (NOAA Rothfusz) → `Heat index`. Complement to wind chill for hot/humid conditions. |
| (new) Wet Bulb Temperature    | number   | °C    | template (Stull 2011) → `Wet bulb temperature`. Hard physical heat-stress limit. |
| `wm_himmeltemperatur`         | number   | °C    | **not implemented** (no IR sensor on this hardware) |
| `wm_wind_mittel`              | number   | m/s   | template → `Wind speed (m/s)`                     |
| `wm_wind_spitze`              | number   | m/s   | template + global → `Wind gust (m/s)`             |
| `wm_windstaerke`              | number   | Bft   | template → `Beaufort`                             |
| `wm_wind_dir`                 | number   | °     | TFA decoder → `Wind direction` (only with vane)   |
| `wm_windrichtung`             | string   | —     | template text → `Wind direction (cardinal)`      |
| `wm_lux`                      | number   | lux   | BH1750 → `Illuminance`                            |
| `wm_uv_index`                 | number   | —     | **not implemented** (no UV sensor on this hardware) |
| `wm_sonne_scheint`            | boolean  | —     | binary template → `Sun is shining`                |
| `wm_sonnenstunden_heute`      | number   | h     | delegate to HA `history_stats`                    |
| `wm_elevation` / `wm_azimut`  | number   | °     | delegate to HA `sun` integration                  |
| `wm_minuten_vor_sa/su`        | number   | min   | delegate to HA `sun` integration                  |
| `wm_regenstaerke`             | number   | mm/h  | pulse_counter → `Rain rate`                       |
| `wm_regen_letzte_h`           | number   | mm    | delegate to HA `utility_meter`                    |
| `wm_regen_mm_heute`           | number   | mm    | delegate to HA `utility_meter`                    |
| `wm_regen_mm_gestern`         | number   | mm    | delegate to HA `utility_meter`                    |
| `wm_regenstunden_heute`       | number   | h     | delegate to HA `history_stats`                    |
| `wm_regensensor_wert`         | number   | %     | template (rolling window) → `Rain sensor value`. See [hardware-rain-sensor.md](docs/hardware-rain-sensor.md) for caveats. |
| `wm_regenmelder`              | boolean  | —     | template (value ≥ `regenschwelle`) → `Rain detector` |
| `wm_barometer`                | number   | hPa   | template (sea-level corrected) → `Barometric pressure (sea level)` |
| `wm_barotrend`                | string   | —     | template (3 h snapshot) → `Barometric trend`. HA-side derivative offers finer granularity, see below. |
| `wm_ip`                       | string   | —     | `wifi_info` → `IP address`                        |

## Home Assistant helpers

### Rainfall periods

```yaml
utility_meter:
  rain_today:
    source: sensor.weatherman_rain_total
    cycle: daily
  rain_last_hour:
    source: sensor.weatherman_rain_total
    cycle: hourly
```

For "rain yesterday" the cleanest approach is a `statistics` sensor
combined with HA's long-term statistics retention, or read the
previous day's value from `utility_meter.rain_today` history.

### Daily sun/rain hours

```yaml
sensor:
  - platform: history_stats
    name: "Sun hours today"
    entity_id: binary_sensor.weatherman_sun_is_shining
    state: "on"
    type: time
    start: "{{ today_at('00:00') }}"
    end: "{{ now() }}"
  - platform: history_stats
    name: "Rain hours today"
    entity_id: binary_sensor.weatherman_rain_detector
    state: "on"
    type: time
    start: "{{ today_at('00:00') }}"
    end: "{{ now() }}"
```

### Refined barometric trend

The on-device `Barometric trend` is a simple steigend / stabil / fallend
classifier with a ±1 hPa hysteresis over 3 hours, matching the original
firmware. For meteorologist-style categorisation
(quickly / steadily / slowly), use HA's `derivative` integration:

```yaml
sensor:
  - platform: derivative
    source: sensor.weatherman_barometric_pressure_sea_level
    name: "Pressure trend (hPa/3h)"
    round: 2
    unit_time: h
    time_window: "03:00:00"
    # `derivative` reports hPa/hour because unit_time is h; multiply by 3
    # via a template sensor below for 3h reading, or set unit_time: d and
    # multiply differently. Easiest: use as-is and adjust thresholds.

  - platform: template
    sensors:
      pressure_trend_class:
        friendly_name: "Pressure trend"
        value_template: >-
          {% set v = states('sensor.pressure_trend_3h') | float(0) %}
          {% if v >  6.0 %}rising rapidly
          {% elif v >  3.5 %}rising quickly
          {% elif v >  1.5 %}rising steadily
          {% elif v >  0.1 %}rising slowly
          {% elif v > -0.1 %}steady
          {% elif v > -1.5 %}falling slowly
          {% elif v > -3.5 %}falling steadily
          {% elif v > -6.0 %}falling quickly
          {% else %}falling rapidly{% endif %}
```

Threshold scale (hPa per 3 h):

| Magnitude | Classification |
|---|---|
| 0.1 – 1.5  | slowly  |
| 1.6 – 3.5  | steadily |
| 3.6 – 6.0  | quickly |
| > 6.0      | rapidly |

### Sun position

```yaml
# Already provided by HA's built-in `sun` integration — no configuration needed.
# Use sensor.sun_next_dawn, sensor.sun_next_setting, sun.sun.attributes.elevation, etc.
```

### Feels-like / UTCI

The ESPHome side already exposes `Wind chill` (NOAA) and `Apparent
temperature` (BoM/Steadman). For closer-to-UTCI accuracy you'd need
mean radiant temperature, which this hardware doesn't measure. See
[docs/feels-like.md](docs/feels-like.md) for the discussion.

## License

MIT. See [LICENSE](LICENSE).
