# ESPHome Implementation Guidelines

A project-agnostic blueprint for how an ESPHome implementation should be built
and maintained. It encodes a philosophy and a set of standards, not a specific
device. Copy it into any ESPHome repository; the overarching principles are
meant to stay constant across projects.

It is written for two audiences: humans setting up a new project, and AI agents
asked to edit an existing one. Where guidance is specifically about *how an AI
should behave when editing*, it is called out as such. Nothing here is
project-specific — concrete device details belong in a project's own
`docs/development_notes.md`.

---

## 0. For AI agents: verify before you assert

ESPHome moves fast and its schema, defaults, and developer-facing APIs change on
a roughly monthly cadence. **Do not rely on training-data memory for anything
version-sensitive.** Before asserting that a key exists, is deprecated, was
removed, or behaves a certain way, confirm it against the current docs
(`esphome.io`), the changelog, or the source. The cost of one verification is
trivial; the cost of shipping config that fails to compile — or silently
misbehaves — on the user's actual ESPHome version is not.

Things that are *especially* worth checking rather than assuming:

- Whether a component option still exists or has moved (options migrate between
  components across releases).
- Default values that change over time (e.g. minimum auth modes, framework
  defaults).
- Frontend/web behaviors, which live in a separately versioned package and
  don't always match the docs.
- Whether a "modernization" you're about to suggest is already present — audit
  the actual current file, not a remembered older version of it.

When you finish an edit, **validate**: parse every file, simulate the package
merge if packages are used, and confirm that every cross-file `id()` reference,
`sorting_group_id`, and substitution actually resolves and that no IDs collide.
Preserve comments and lambda bodies verbatim when restructuring.

---

## 1. Core philosophy

**Separate what changes at flash time from what changes at runtime.** Anything a
user sets once per device (identity, addresses, bus parameters, fixed protocol
constants) belongs in substitutions. Anything a user tunes *while the device is
running* (calibration, thresholds, behavioral margins) belongs in a runtime
entity (`number`, `switch`, `select`, `datetime`) with `restore_value: true`,
tunable from the UI / Home Assistant / MQTT. If you find yourself telling a user
to edit YAML and reflash to change an operational value, that value is in the
wrong place.

**Separate stable per-device material from volatile project logic.** Identity,
secrets, and connectivity are specific to one physical device and essentially
never change because of a project update. Sensor definitions, math, and control
logic are the project and change with every release. Keep these in different
files (see Structure) so an upgrade never forces a user to merge edits into a
file they customized.

**Be honest about trade-offs; don't cargo-cult "best practices."** Some changes
that look modern are wrong in context, and some look like functional fixes but
are purely cosmetic. State the trade-off plainly and recommend; don't apply a
change just because it pattern-matches to "current style." (Anti-patterns
catalogues the concrete cases, including the ISR container-type trap.)

**Fail safe on dropouts.** A failed sensor read surfaces as `NaN`, and `NaN`
propagates silently — comparisons against it are always false, so a lambda that
reads a sensor and decides "if `temp > setpoint`, turn off the heater" will
leave the heater *on* when the sensor dies. Any lambda that drives a physical
output (heater, relay, motor, PWM) must explicitly test `std::isnan(x)` and
fall back to a defined safe state. Derived sensors that compute downstream
values can let `NaN` propagate freely (that's actually the right signal to HA);
output commands must short-circuit before they reach a relay.

---

## 2. Structure

For anything beyond a trivial single-sensor device, use a **package-based
architecture**: a thin per-device entry file plus a set of logic packages.

**The entry file is local and holds only:** substitutions (identity + the
values a user is expected to set), the `!secret`-bearing blocks (`wifi`, `api`,
`ota`, and `mqtt` if it carries credentials), the platform/board block, and the
pointers that pull in the logic (`packages:` and `external_components:`). It
should be short and stable.

**Packages hold the logic** and are organized by **domain cohesion** — each
package is a self-contained feature owning its own sensors, runtime entities,
intervals, and globals. Prefer a handful of meaningful domain files over either
one monolith or a swarm of tiny fragments. A useful split is: one infrastructure
package (logging, diagnostics, web UI, time, shared globals) plus one package
per functional subsystem.

**Pull packages remotely by git tag** for a published project, and pull any
custom C++ component via `external_components` from the *same* repository and
tag. Drive both from a single version substitution so that upgrading is one edit
(see Substitutions). For local development, swap the remote source to a
`type: local` path.

**Know the merge semantics**, because the whole architecture depends on them:
definitions from packages merge non-destructively with the entry file; lists of
components merge by `id` when present and concatenate otherwise; dictionaries
merge key-by-key; other scalars are last-wins; and **substitutions in the entry
file override same-named substitutions in packages.** That last rule is the
override channel — packages ship defaults, the entry file overrides.

**Make optional packages conditional the way the merge actually supports.** An
optional feature-package should be gated so it resolves to an empty dict `{}`
when disabled — an empty entry simply drops out of the merge. The reliable shape
is a *top-level `packages:` entry* wrapped in the guard, **not** a `${...}`
expression buried inside another entry's `files:` list (that does not resolve
the way it looks like it will, and is a recurring source of broken builds). For
a remote project the optional package is its own git entry; for local
development it is its own `!include`. The footgun worth a comment: substitutions
are strings, so a quoted `"false"` is *truthy* and will wrongly include the
package — feed the guard a bare boolean, or test the string value explicitly.
Confirm the conditional resolves both ways on your ESPHome version before
relying on it.

**Prefix IDs by their owning package.** Generic names like `outdoor_temp` or
`wind_speed` collide the moment a second package wants the same concept.
Domain prefixes (`tfa_temp`, `anemo_wind`) guarantee they don't, and they
double as a hint to where a given ID is defined when you're reading a lambda
from a different file. The same convention applies to globals and
`sorting_group_id`s; substitutions are usually scoped tightly enough that the
prefix matters less, but consistency doesn't hurt.

---

## 3. Substitutions

**Packages ship sensible defaults; the entry file overrides only what differs.**
Each package declares a `substitutions:` block with defaults for the knobs it
exposes, so it is self-documenting and so a new release can add a knob (with a
default) without breaking existing entry files.

**Never put `!secret` in a remote package** — remote packages cannot resolve
secret lookups. Secret-bearing blocks stay in the local entry file. If a package
genuinely needs a secret value, expose it as a substitution with a default and
let the local entry file fill it from `!secret`.

**A single version knob can drive everything.** Top-level substitutions are
resolved early enough to be used inside `packages:` and `external_components:`
URLs and refs, so one `..._ref` substitution can pin both the YAML packages and
the C++ component to the same tag. This is what makes "upgrade = bump one line"
real. (Keep that substitution top-level; deeply nested substitution resolution
is less reliable.)

**Quote substitution values that look like numbers** (`"12"`, `"833"`) —
substitutions are text, and unquoted numerics can surprise you.

**Don't use a substitution for something that should be a runtime entity.** A
substitution is flash-time; if the user should change it live, it's a `number`,
not a substitution (see Philosophy).

---

## 4. Runtime vs flash-time config, and flash wear

Runtime-tunable values are `number`/`switch`/`select` entities with
`restore_value: true` so they persist across reboots and survive firmware
upgrades untouched. Two caveats:

**Flash wear.** `restore_value: true` persists state across reboots, but the
writes are not unbounded: ESPHome coalesces them on a timer set by
`flash_write_interval` (default `1min`, and settable all the way to `never`), so
the wear arithmetic is driven by the *flush* interval, not the raw per-cycle
update rate. That makes it three tiers, not two: a value that changes a handful
of times in a device's life can restore freely; a value rewritten every cycle is
best left non-restored; and in between, lengthening `flash_write_interval`
trades persistence latency for flash lifespan. Either way, write the per-day
write-rate arithmetic into a comment so the next person doesn't naively flip
restore on. (On ESP8266, `restore_from_flash: true` is additionally required for
any state to reach flash at all.)

**Fixed-period timers can't track a runtime value directly.** An `interval:`
period is set at compile time. If a runtime entity is supposed to control "how
often X happens," implement it as a short fixed tick plus an elapsed-time
accumulator (`millis()` subtraction, which is rollover-safe with unsigned math)
that reads the runtime entity each tick — not by trying to parameterize the
interval period.

---

## 5. Web server and accessibility

Use **`web_server: version: 3`** and **`local: true`** so the standalone UI is
reachable on the LAN without exposing the device to the internet.

Organize entities with **`sorting_groups`** on the component plus a per-entity
`web_server.sorting_weight`. Lower weights sort first.

Two web-UI behaviors that are easy to get wrong:

- **Ungrouped entities float above grouped ones.** An entity with no
  `web_server` key sorts ahead of entities that have one. Assign groups
  deliberately rather than leaving entities ungrouped.
- **`entity_category` overrides grouping.** An entity tagged
  `entity_category: config` or `diagnostic` is forced into a frontend group by
  that category and **cannot be reordered** via `sorting_group_id` or group
  weight — those sections float to the top of the *web page* regardless. This is
  a known, long-standing frontend limitation. The trade-off: `entity_category`
  is what gives clean Configuration/Diagnostic sections in *Home Assistant*
  (the primary UI), so the usual right call is to keep `entity_category` and
  accept the web-page ordering, treating the device's own web page as a local
  fallback. Only drop `entity_category` (and use sorting groups instead) if the
  web-page order genuinely matters more than HA categorization. Don't add
  config that appears to fix this but doesn't — verify the rendered result.

Areas are **compile-time firmware metadata**, not runtime entities: an area
can't be set from the web UI or saved to flash. Empty-string area fields fail
validation. The normal way to place a device in an area is in Home Assistant
itself (which persists in HA's registry and survives reflashes); leave the
firmware `area:` out unless you specifically want it baked in.

---

## 6. Debugging and notification capability

Assume the device will eventually be in a sealed enclosure on a roof with no
serial access, and design for diagnosis over the network.

- **Enable the `debug:` component and a `reset_reason` text sensor.** This
  surfaces the last reset cause and logs device info at every boot. On
  ESP8266/ESP32 a crash backtrace is captured across warm reboots and replayed
  over the API on reconnect — so watchdog and exception crashes are diagnosable
  without a cable.
- **Gotcha — reset reason granularity.** Genuine faults read clearly
  ("Hardware Watchdog", "Exception"), but *every* software-initiated reboot
  (connectivity timeout, OTA, safe mode, manual restart) collapses into one
  generic "Software/System restart" string. You cannot distinguish them by the
  reset reason alone. To attribute a reboot to, say, a connectivity timeout,
  cross-reference an uptime sensor and an online/status binary sensor.
- **`logger: baud_rate: 0`** when the serial port is inaccessible, and rely on
  API / MQTT / web logs instead of UART.
- **Declare `safe_mode:` explicitly.** Under the modern `ota: - platform:
  esphome` structure it is **not** enabled implicitly. Safe mode is the recovery
  net for boot loops: after N consecutive failed boots the device comes up in a
  minimal Wi-Fi + OTA-only image so a fix can be pushed without opening the
  enclosure. Set the values explicitly even when they match defaults, to
  document intent.

---

## 7. Security and modern defaults

- Set **`min_auth_mode: WPA2`** explicitly rather than relying on the platform
  default (which has shifted over releases).
- Keep the web UI **`local`** and the API **encrypted**, and keep OTA
  password-protected. Treat the web UI's built-in auth as convenience, not a
  security boundary — it has had auth-bypass bugs (e.g. CVE-2025-57808), so the
  `local` flag is what actually protects it. The native API no longer takes a
  password at all (password auth was removed in 2026.1, since it only verified
  identity while leaving traffic in plaintext) — authenticate with the Noise
  encryption key, not `api: password:`. Secrets live only in the local entry
  file.
- For published projects, **pin remote sources to tags**, not moving branches,
  so a build is reproducible and a user isn't silently updated by an upstream
  commit. (With a fixed tag, the `refresh:` interval is moot.)

---

## 8. Future-proofing and standards

- **Track the deprecation calendar** and verify current behavior before relying
  on an API; prefer the currently-recommended structure over the one you
  remember (OTA-as-platform, web_server v3, etc.).
- **`project.name` must be `namespace.name`** — a dot-separated pair. A bare
  single token fails validation.
- **Entity, device, and area names have hard constraints — mind them up front.**
  Since ESPHome 2026.1 the `/` character is reserved as a URL path separator and
  is rejected in entity/device/area names, and names are capped at 120
  characters. The old `/<domain>/<name>` web URLs are deprecated and removed in
  2026.7.0. Renaming an entity later changes its URL and can break bookmarks (and
  any HA `entity_id` that wasn't pinned), so choose slash-free names early —
  and verify the current rules, since these cutovers land through mid-2026.
- **`suggested_display_precision`** decouples the precision Home Assistant
  *displays* from the precision sent over the API. Use it where a clean UI and a
  high-resolution feed both matter. Do **not** reach for `accuracy_decimals` as
  a way to make *on-device* math more precise — it's a display/format hint and
  doesn't change the float a lambda reads; on-device calculations already use
  full precision.
- **Match `state_class` and `device_class` to the measurement** or Home
  Assistant won't store the data for long-term statistics. Continuous readings
  get `state_class: measurement`; monotonically-accumulating counters (rain
  total, energy) get `state_class: total_increasing`; and `device_class` must
  agree with `unit_of_measurement` (`atmospheric_pressure` ↔ `hPa`/`inHg`,
  `temperature` ↔ `°C`/`°F`, etc.). A mismatch does *not* fail at compile — HA
  silently drops the data on its side, and you notice when the statistics tab
  is empty a week later. Verify against the HA device-class reference rather
  than guessing.
- **For new ESP32-family projects, default to `framework: type: esp-idf`.** The
  arduino-esp32 layer is a shrinking shim over the IDF — it lags on platform
  features and gets progressively less upstream attention, while the IDF itself
  receives the active development and is required for modern stacks (Thread,
  Matter, BLE on smaller variants). ESP8266 has no IDF option and stays on
  arduino. Don't migrate speculatively, though: an existing arduino-based ESP32
  config that compiles and runs is not a candidate for a framework swap — this
  default is for greenfield work.
- **HAL portability is opt-in.** Arduino-only calls (`digitalRead`, `micros`,
  `attachInterruptArg`, `IRAM_ATTR`, etc.) are fine for a platform that is
  locked to the Arduino framework. Only migrate to framework-agnostic HAL types
  (`InternalGPIOPin`, `ISRInternalGPIOPin`) if porting to an ESP-IDF-only target
  is a real goal.
- **Adoptable-product features are opt-in, ESP32-flavoured, and not for every
  project.** If you publish a device that *ships with ESPHome already on it* for
  strangers to adopt, two components come into play: `improv_serial` /
  `esp32_improv` let an end-user hand the device Wi-Fi credentials without
  touching YAML, and `dashboard_import` (a `package_import_url` in the published
  config) makes an ESPHome dashboard offer a one-click *adopt* that
  auto-generates a thin local entry file pulling your config as a remote
  package — i.e. it wires the §2 architecture into the dashboard. Both assume
  the device is discoverable on the network already running ESPHome and was
  flashed without baked-in secrets (the user supplies them via captive portal or
  improv); neither helps a sealed retrofit whose first flash is over USB with
  credentials in a local file, and improv is ESP32-only. Reach for them only if
  "adoptable product" is a genuine goal — otherwise the plain thin-entry-file
  workflow above is simpler and does the same job.

---

## 9. Maintainability

- **Upgrading should be one edit.** A single version substitution drives the
  package and component refs; runtime entities carry user tuning and are never
  touched by an upgrade.
- **Document per item, banner per section.** Keep the "why" next to the thing
  (especially in lambdas and any non-obvious constant), with a concise banner
  per package/section. Comments are part of the deliverable.
- **Right-size the modularity.** Domain cohesion beats both a monolith and an
  explosion of fragments. Combine data that is read or written together.
- **Validate every change automatically — and pin what you validate against.**
  For a published project, a broken `main` becomes broken OTA for every device
  pinned to the next tag the moment that tag is cut. There are two useful
  depths: `esphome config` does the fast schema + package-merge +
  substitution-resolution pass (it catches an optional-package or `${...}`
  mistake with no toolchain needed), and `esphome compile` additionally
  exercises lambdas and any C++ component. Run at least `config`, ideally
  `compile`, against the example entry file in a GitHub Action (or equivalent)
  on every pull request — it's a few lines of workflow and pays off the first
  time it catches a typo. Pin the ESPHome version CI uses and bump it
  deliberately, so "passes CI" means a known release; the entry file's
  `esphome: min_version:` is the matching runtime guardrail. If you use GitHub
  Actions, pin the actions to Node-24-compatible versions — GitHub forces Node
  24 from 2 June 2026 and removes Node 20 on 16 September 2026.

---

## 10. Anti-patterns to push back on

- **Modern-for-its-own-sake.** A change that aligns with current idiom but
  breaks correctness or costs complexity for no real benefit is not an
  improvement. The ISR ring-buffer case (raw `volatile` array is correct;
  `std::array` is not, because its accessors aren't `volatile`-qualified) is the
  canonical example.
- **Cosmetic dressed as functional.** Changing a display hint and claiming it
  improves computational accuracy; adding a sorting group that the frontend
  ignores. Verify the effect before claiming it.
- **Unverified additions.** Suggesting something already present, or "fixing" a
  non-problem, because the audit was done against a remembered older state
  instead of the actual current file.
- **Silent choices on real trade-offs.** When two reasonable options diverge
  (HA categorization vs web-page ordering; restore-value vs flash wear), surface
  the trade-off and recommend — don't pick silently.

---

## 11. Editing workflow checklist (for AI)

The verify-before-asserting and validation discipline from §0 applies
throughout; this is the ordered workflow that wraps it.

1. Read the whole relevant file(s) before partitioning or editing.
2. Verify any version-sensitive claim against current ESPHome docs/source (§0).
3. Make the change; preserve comments and lambda bodies verbatim.
4. Validate as in §0 — parse every file, simulate the merge, and confirm every
   cross-file `id()`, `sorting_group_id`, and `${substitution}` resolves with no
   collisions. Where a toolchain is available, prefer a real `esphome config`
   (and `compile`) over hand-simulation.
5. Surface any trade-off or assumption explicitly in the summary.
6. **Update the project store on agreed changes.** When the session uses AI
   project features (project knowledge bases, shared file mounts, persistent
   memory), the accepted file must be written back to the store. The AI
   typically cannot do this itself — the mounted project files are read-only —
   so flag it explicitly to the human collaborator at the end of the change.
   Skipping this step means the next session starts from a stale baseline and
   may re-propose the same modifications, contradict them, or — worst case —
   confidently cite the *old* version of a rule that was already revised.
