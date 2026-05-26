# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

Development-phase history (the pre-publication build diary) has been moved to
`docs/development_notes.md`. This changelog starts fresh and tracks releases
from the first published tag onward.

## [Unreleased]

### Added
- Heater frost-protect transition band is now a runtime `number`
  (`cfg_frost_band_c`), matching the already-tunable dew margin; it was a
  hardcoded 2 °C. Falls back to a hard cutoff at the frost temperature if unset.
- WiFi link-health diagnostics for this weak-signal node (RSSI < −68 dBm): a
  restored **Boot count** and a per-session **WiFi disconnects** counter, plus
  on_connect/on_disconnect logging. A climbing boot count preceded by
  disconnects identifies a WiFi-driven reboot, which `reset_reason` alone cannot.

### Changed
- Standardised the GitHub owner to `SoulSolistice` across all files; the
  package-header comments and dev notes previously read `SoulSoliste`. The
  `weatherman_repo` URL that drives both `packages:` and `external_components:`
  now matches the real repository.
- `project.version` now follows `weatherman_ref` (the deployed git tag) instead
  of a separate hardcoded `version` substitution, so the reported firmware
  version can't drift from what's actually flashed.
- Wind gust now captures the true instantaneous peak (sampled before the
  5-sample smoothing average) rather than the peak of the smoothed value, which
  previously under-reported gusts.

### Fixed
- Removed a dead, unreachable branch in the TFA decoder's `ST_SYNCING` handling.
