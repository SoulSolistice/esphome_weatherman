# Development notes

Project-specific learnings, gotchas, and the reasoning behind non-obvious
decisions in this firmware. The generic, reusable philosophy lives in
`ESPHOME_GUIDELINES.md` at the repo root; this file is the concrete companion to
it — the things that are true *for this device* and would otherwise have to be
rediscovered.

---

## Architecture: how the files fit together

The config is split into a thin per-device **entry file** (`example/weatherman.yaml`)
and five remote **packages** (`packages/*.yaml`) pulled from GitHub by tag, plus
the `tfa_tx141w` C++ component pulled via `external_components` from the same
repo and tag.

- **entry file** — identity, secrets (`wifi`/`api`/`ota`), MQTT, the platform
  block, and the `packages:` + `external_components:` pointers. Both pointers
  read a single `weatherman_ref` substitution, so upgrading the firmware is one
  edit: bump the tag.
- **core.yaml** — logger, debug hub, safe mode, captive portal, web server +
  sorting groups, time, shared globals, and generic device diagnostics.
- **sensors_tfa.yaml** — TFA decoder config, wind/humidity/temperature sensors,
  gust tracking, decoder diagnostics. Ships the TFA timing defaults as
  substitutions.
- **sensors_env.yaml** — I2C/1-Wire buses, BMP280, BH1750, DS18B20, canonical
  outdoor temperature, altitude/lux settings.
- **derived.yaml** — dewpoint, absolute humidity, wind chill, apparent temp,
  heat index, wet bulb, Beaufort, sea-level pressure, 3h change + trend.
- **rain_heater.yaml** — rain bucket/comb sensors, rain detector, PWM heater
  output, auto-mode switch, calibration/threshold settings, control loops.

Cross-package references (e.g. `derived.yaml`'s sea-level pressure reads
`pressure_hpa` from `sensors_env.yaml`, the heater loop in `rain_heater.yaml`
reads `dewpoint_c` from `derived.yaml`) resolve fine because ESPHome merges all
packages before resolving IDs.

---

## Gotchas discovered during development

**Web UI: config/diagnostic sections float to the top and can't be moved.**
Tried to push the Configuration and Diagnostics sections to the bottom
of the device's web page using named sorting groups with high weights. It does
not work: in web_server v3, `entity_category: config`/`diagnostic` forces the
frontend group and overrides any `sorting_group_id`/weight (ESPHome issue #6488,
still open). Keeping `entity_category` because it gives correct sectioning in
Home Assistant (the primary UI) and accepted the web-page float. The dead
sorting groups were removed. **Don't re-add them thinking it's a fix.**

**A runtime "window in minutes" can't parameterize an interval period.** The
gust-reset window was a substitution feeding `- interval: ${gust_window_min}min`.
Moving it to a runtime `number` meant the interval period would need to track a
live value, which `interval:` can't do (period is compile-time). The working
pattern: a fixed `30s` tick with a `millis()` elapsed-time accumulator that reads
`cfg_gust_window_min` each tick and resets the gust max when the window elapses.
The 30 s tick means the reset lands within ~30 s of the window — negligible for
5–15 min windows. The `millis()` subtraction is rollover-safe (unsigned).

**`area:` is compile-time and can't be empty.** Empty-string `area_name`/`area_id`
fail validation, and area can't be set from the web UI or saved to flash. It's
commented out; assign the device to an area in Home Assistant instead (persists
in HA, survives reflashes).

**`project.name` needs a namespace.** It must be `namespace.project`
(dot-separated). A bare token fails validation. Currently `SoulSolistice.esphome_weatherman`.

**`project.version` follows the git tag.** Rather than a separate hardcoded
`version`, the entry file sets `project.version: ${weatherman_ref}`, so the
reported firmware version is always the tag (or branch) the packages and C++
component were built from. Bump `weatherman_ref` and the version follows — one
source of truth, no drift.

**`safe_mode` is not implicit under the new OTA structure.** With
`ota: - platform: esphome`, safe mode must be declared explicitly. It is, in
`core.yaml`.

**Reset reason can't distinguish software reboots.** A Wi-Fi/API timeout, OTA,
safe-mode, and manual restart all report the generic "Software/System restart".
To spot a Wi-Fi-driven reboot, cross-reference the Uptime sensor and the Online
status sensor — and the dedicated **Boot count** + **WiFi disconnects** counters
(core.yaml), added because this node's weak RSSI makes WiFi reboots the common
case. Only hardware faults ("Hardware Watchdog", "Exception") are
self-identifying.

**Wind gust is the raw peak, captured before smoothing.** `wind_speed` carries a
5-sample moving average (`wm_wind_mittel`) for the published value used by wind
chill / apparent-temp / Beaufort, but the gust maximum (`wm_wind_spitze`) is
sampled in a lambda filter placed *before* that average — otherwise it would
report the peak of the mean and under-read true gusts. The order of the filters
is load-bearing; don't move the gust-capture lambda after the average.

**BH1750 `measurement_time` was removed from the schema.** MTREG is fixed at the
datasheet default. To extend low-light sensitivity, change sensors (e.g.
VEML7700) rather than trying to override MTREG.

**`internal_temperature` is ESP8266-unavailable.** The ESP8266 has no on-die
temperature sensor; the BMP280's box-internal temperature serves as the
controller-board temperature proxy instead.

---

## Hardware-specific decisions

**Wind direction is commented out by default.** The Spring Breeze head
(`w_station_type:2`) has no vane; its direction field is always 0, which would
publish a constant meaningless "N". Both the `wind_direction` sensor and the
cardinal text sensor are commented out together — uncomment both only on the
Anemometer variant (`type:3`).

**Canonical outdoor temperature prefers the DS18B20.** The TFA wind-head NTC
reads 1–2 °C high on calm sunny days (housing is vented but not aspirated). The
`outdoor_temp` template uses the shaded DS18B20 when available and falls back to
the wind-head NTC. Both sources are demoted to `diagnostic` so the main weather
group shows only the canonical value.

**Heater control logic lives inside the lambda, not in a YAML `condition:`.**
The auto-control interval always evaluates while auto mode is on so the heater
receives an explicit duty every cycle. Putting the rain check on a YAML
`condition:` would skip the whole `then:` and latch the PWM at its last value.
A NaN outdoor temperature commands the heater explicitly off (never latches at
the last value).

---

## restore_value decisions (flash-wear)

- `wind_gust_max_kmh` — **not restored.** Updates on every wind packet
  (~2,800 writes/day); restoring would wear flash out in a couple of years.
- `pressure_3h_ago` — **restored.** Changes every 3 hours (~8 writes/day);
  wear is negligible and preserving it keeps the barotrend accurate across OTA
  and brief power blips.
- Calibration/threshold numbers — restored. They change a handful of times in
  the device's life.
- `boot_count` — **restored.** One write per boot; negligible wear under normal
  operation, and if reboots ever get frequent enough to matter for flash, that
  frequency is the very signal the counter exists to expose.
- `wifi_disconnect_count` — **not restored.** Per-session only; a flapping link
  could otherwise write often. Lifetime reboot history is in `boot_count`.

The per-day write-rate arithmetic is in comments next to each global so the
reasoning is auditable and nobody naively flips the gust max to restored.

---

## Things considered and deliberately rejected

- **`std::array` for the ISR ring buffer.** Rejected: `std::array`'s `operator[]`
  is not `volatile`-qualified, so a `volatile std::array` for an ISR↔loop buffer
  forces `const_cast` gymnastics. The raw `volatile Edge[]` is the correct tool.
- **`accuracy_decimals: 4` to "smooth" the 3h pressure trend.** Rejected: the
  trend is computed on-device against `pressure_3h_ago`, not by HA's derivative
  integration, and `accuracy_decimals` is a display hint that doesn't change the
  float a lambda reads. It would change nothing. `suggested_display_precision`
  remains available as a genuine cosmetic option.
- **Pinning the component `ref` separately from the package `ref`.** Rejected as
  unnecessary: a hardware-protocol decoder rarely produces breaking changes, and
  a single `weatherman_ref` driving both is simpler. (Substitution-in-ref is
  supported for top-level substitutions in current ESPHome.)
