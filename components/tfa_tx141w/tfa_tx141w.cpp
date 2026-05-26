#include "tfa_tx141w.h"
#include "esphome/core/log.h"
#include <Arduino.h>
#include <cstdio>
#include <cstring>

namespace esphome::tfa_tx141w {

static const char *const TAG = "tfa_tx141w";

// ============================================================
// ISR
// ============================================================
void IRAM_ATTR TFATX141W::isr_trampoline(void *arg) {
  static_cast<TFATX141W *>(arg)->on_edge();
}

void IRAM_ATTR TFATX141W::on_edge() {
  uint32_t now = micros();
  uint8_t lvl = (uint8_t)(digitalRead(this->pin_) & 1);
  uint32_t w = this->isr_w_;
  uint32_t next = (w + 1) & EDGE_BUF_MASK;
  // v2 fix: drop-NEWEST on overflow. The previous version advanced isr_w_
  // unconditionally, which made the ring buffer indistinguishable from empty
  // and silently lost the entire backlog. The dropped edge will manifest in
  // the main loop as one oversized pulse, which the decoder treats as a reset.
  if (next == this->isr_r_) {
    this->isr_overflows_++;
    return;
  }
  this->edge_buf_[w].ts = now;
  this->edge_buf_[w].level = lvl;
  this->isr_w_ = next;
}

// ============================================================
// Lifecycle
// ============================================================
void TFATX141W::setup() {
  ESP_LOGCONFIG(TAG, "Setting up TFA TX141W decoder on GPIO%u", this->pin_);
  pinMode(this->pin_, INPUT);
  // attachInterruptArg eliminates the need for a singleton trampoline; this
  // also means multiple instances on different GPIOs would be possible (none
  // exist on the WEATHERMAN 2.1 hardware, but the code no longer enforces a
  // single instance).
  attachInterruptArg(digitalPinToInterrupt(this->pin_), &TFATX141W::isr_trampoline, this, CHANGE);
  this->reset_decoder_();
  this->last_stats_ms_ = millis();
  this->last_hist_ms_ = millis();
  this->last_any_packet_ms_ = millis();
  this->publish_status_("setup complete, waiting for sync...");
}

void TFATX141W::dump_config() {
  ESP_LOGCONFIG(TAG, "TFA TX141W Decoder:");
  ESP_LOGCONFIG(TAG, "  Pin: GPIO%u", this->pin_);
  ESP_LOGCONFIG(TAG, "  Debug logging: %s", YESNO(this->debug_));
  ESP_LOGCONFIG(TAG, "  Timing: sync=%uµs short=%uµs long=%uµs tol=±%uµs reset=%uµs", this->t_sync_, this->t_short_,
                this->t_long_, this->t_tol_, this->t_reset_);
  ESP_LOGCONFIG(TAG, "  Min sync pulses to lock: %u", this->min_sync_pulses_);
  LOG_SENSOR("  ", "Temperature", this->sensor_temp_);
  LOG_SENSOR("  ", "Humidity", this->sensor_hum_);
  LOG_SENSOR("  ", "Wind speed", this->sensor_wind_);
  LOG_SENSOR("  ", "Wind direction", this->sensor_wind_dir_);
  LOG_SENSOR("  ", "Battery level", this->sensor_battery_);
  LOG_SENSOR("  ", "Sensor ID", this->sensor_id_);
  LOG_SENSOR("  ", "Packets valid", this->sensor_pkts_valid_);
  LOG_SENSOR("  ", "Packets invalid", this->sensor_pkts_invalid_);
  LOG_SENSOR("  ", "Sync detections", this->sensor_syncs_);
}

// ============================================================
// Main loop
// ============================================================
void TFATX141W::loop() {
  // Snapshot the ISR write pointer at top of loop.
  uint32_t w = this->isr_w_;

  while (this->isr_r_ != w) {
    uint32_t ts = this->edge_buf_[this->isr_r_].ts;
    uint8_t lvl = this->edge_buf_[this->isr_r_].level;
    this->isr_r_ = (this->isr_r_ + 1) & EDGE_BUF_MASK;

    this->stat_total_edges_++;

    if (!this->have_prev_edge_) {
      this->last_edge_ts_ = ts;
      this->have_prev_edge_ = true;
      continue;
    }

    uint32_t width = ts - this->last_edge_ts_;
    this->last_edge_ts_ = ts;

    // Level that JUST ENDED is the complement of the current (post-edge) level
    uint8_t ended_level = lvl ^ 1;

    this->hist_add_(width);
    this->process_pulse_(width, ended_level);
  }

  // Periodic debug output (every 10s)
  uint32_t now_ms = millis();
  if (this->debug_) {
    if (now_ms - this->last_stats_ms_ >= 10000) {
      this->log_stats_();
      this->last_stats_ms_ = now_ms;
    }
    if (now_ms - this->last_hist_ms_ >= 30000) {
      this->dump_histogram_();
      this->last_hist_ms_ = now_ms;
    }
  }

  // Stale-data warning every 90s if no valid packet
  if (this->stat_valid_ > 0 && now_ms - this->last_any_packet_ms_ > 90000) {
    ESP_LOGW(TAG, "No valid packet for %u ms; total edges=%u syncs=%u", (unsigned)(now_ms - this->last_any_packet_ms_),
             this->stat_total_edges_, this->stat_syncs_);
    this->last_any_packet_ms_ = now_ms;  // throttle
  }
}

// ============================================================
// Decoder state machine
// ============================================================
void TFATX141W::reset_decoder_() {
  this->state_ = ST_IDLE;
  this->sync_count_ = 0;
  this->pulse_count_ = 0;
}

void TFATX141W::process_pulse_(uint32_t width, uint8_t /*ended_level*/) {
  // A very long pulse means line was idle for a while → start fresh.
  if (width >= this->t_reset_) {
    if (this->state_ == ST_DATA && this->pulse_count_ >= 130) {
      // We had a full-ish packet; try to decode before resetting.
      this->try_finish_packet_();
    }
    this->reset_decoder_();
    return;
  }

  const uint32_t sync_lo = this->t_sync_ > this->t_tol_ ? this->t_sync_ - this->t_tol_ : 0;
  const uint32_t sync_hi = this->t_sync_ + this->t_tol_;
  const uint32_t short_lo = this->t_short_ > this->t_tol_ ? this->t_short_ - this->t_tol_ : 0;
  const uint32_t short_hi = this->t_short_ + this->t_tol_;
  const uint32_t long_lo = this->t_long_ > this->t_tol_ ? this->t_long_ - this->t_tol_ : 0;
  const uint32_t long_hi = this->t_long_ + this->t_tol_;

  bool in_sync_range = (width >= sync_lo && width <= sync_hi);
  bool in_data_range = (width >= short_lo && width <= long_hi);

  switch (this->state_) {
    case ST_IDLE:
      if (in_sync_range) {
        this->sync_count_ = 1;
        this->state_ = ST_SYNCING;
      }
      break;

    case ST_SYNCING:
      if (in_sync_range) {
        this->sync_count_++;
      } else if (this->sync_count_ >= this->min_sync_pulses_) {
        // Sync is over; this pulse is the first data half.
        this->stat_syncs_++;
        if (this->sensor_syncs_ != nullptr) this->sensor_syncs_->publish_state(this->stat_syncs_);
        this->state_ = ST_DATA;
        this->pulse_count_ = 0;
        if (in_data_range) {
          this->pulse_buf_[this->pulse_count_++] = width;
        } else {
          // First post-sync pulse out of range — abandon
          if (this->debug_) {
            ESP_LOGD(TAG, "post-sync pulse out of range: %uµs", width);
          }
          this->reset_decoder_();
        }
      } else {
        // False start: too few sync pulses, and this pulse is not itself a
        // sync pulse (that case is handled by the `if (in_sync_range)` above).
        // Drop back to IDLE; a genuine sync pulse will re-arm via ST_IDLE.
        this->reset_decoder_();
      }
      break;

    case ST_DATA:
      if (in_data_range) {
        if (this->pulse_count_ < PULSE_BUF_SIZE) {
          this->pulse_buf_[this->pulse_count_++] = width;
        }
        if (this->pulse_count_ >= 130) {
          this->try_finish_packet_();
          this->reset_decoder_();
        }
      } else if (in_sync_range) {
        // Looks like next packet's sync arrived — try to decode what we have
        this->try_finish_packet_();
        this->reset_decoder_();
        this->sync_count_ = 1;
        this->state_ = ST_SYNCING;
      } else {
        // Out-of-range pulse mid-packet
        this->try_finish_packet_();
        this->reset_decoder_();
      }
      break;
  }
}

void TFATX141W::try_finish_packet_() {
  if (this->pulse_count_ < 130) {
    this->stat_short_packets_++;
    if (this->debug_) {
      ESP_LOGD(TAG, "short packet: %u pulses (need 130)", this->pulse_count_);
    }
    return;
  }

  // Decode bits: pair (i, i+1) for i = 0, 2, 4, ...
  //   pair (long, short) = bit 1
  //   pair (short, long) = bit 0
  // Threshold: midpoint between t_short and t_long.
  uint32_t mid = (this->t_short_ + this->t_long_) / 2;

  uint8_t bytes[9] = {0};
  bool any_ambiguous = false;

  for (int i = 0; i < 65; i++) {
    uint32_t a = this->pulse_buf_[2 * i];
    uint32_t b = (2 * i + 1 < (int)this->pulse_count_) ? this->pulse_buf_[2 * i + 1] : 0;
    bool a_long = (a >= mid);
    bool b_long = (b >= mid);
    uint8_t bit;
    if (a_long && !b_long)
      bit = 1;
    else if (!a_long && b_long)
      bit = 0;
    else {
      // Ambiguous pair — fall back to thresholding just on 'a'
      bit = a_long ? 1 : 0;
      any_ambiguous = true;
    }
    bytes[i / 8] |= (bit << (7 - (i % 8)));
  }

  // Try as-is, then bit-inverted (in case sync was off by one pulse)
  uint8_t b_norm[9];
  uint8_t b_inv[9];
  std::memcpy(b_norm, bytes, 9);
  for (int i = 0; i < 9; i++) b_inv[i] = ~bytes[i];

  auto check = [&](const uint8_t *b) -> const char * {
    if ((b[0] >> 3) != 0x01) return "preamble";
    uint8_t crc = TFATX141W::crc8(b, 8, 0x31, 0x00);
    if (crc != 0) return "crc";
    return nullptr;
  };

  const char *err_norm = check(b_norm);
  const char *err_inv = check(b_inv);

  uint8_t *good = nullptr;
  if (err_norm == nullptr)
    good = b_norm;
  else if (err_inv == nullptr)
    good = b_inv;

  if (good != nullptr) {
    this->stat_valid_++;
    this->last_any_packet_ms_ = millis();
    if (this->sensor_pkts_valid_ != nullptr) this->sensor_pkts_valid_->publish_state(this->stat_valid_);
    this->publish_packet_(good);
  } else {
    this->stat_invalid_++;
    if (this->sensor_pkts_invalid_ != nullptr) this->sensor_pkts_invalid_->publish_state(this->stat_invalid_);
    if (err_norm != nullptr && std::strcmp(err_norm, "preamble") == 0 && err_inv != nullptr &&
        std::strcmp(err_inv, "preamble") == 0) {
      this->stat_preamble_fail_++;
    } else {
      this->stat_crc_fail_++;
    }

    // Publish the raw bytes for debugging — useful even on failure.
    char buf[64];
    std::snprintf(buf, sizeof(buf), "RAW: %02X %02X %02X %02X %02X %02X %02X %02X %02X (norm:%s inv:%s)", b_norm[0],
                  b_norm[1], b_norm[2], b_norm[3], b_norm[4], b_norm[5], b_norm[6], b_norm[7], b_norm[8],
                  err_norm ? err_norm : "OK", err_inv ? err_inv : "OK");
    ESP_LOGD(TAG, "%s%s", buf, any_ambiguous ? " [ambiguous bits]" : "");
#ifdef USE_TEXT_SENSOR
    if (this->sensor_pkt_hex_ != nullptr) this->sensor_pkt_hex_->publish_state(buf);
#endif
    this->publish_status_(std::string("decode fail: ") + (err_norm ? err_norm : "?") + "/" + (err_inv ? err_inv : "?"));
  }
}

// ============================================================
// Packet → sensors
// ============================================================
void TFATX141W::publish_packet_(const uint8_t *b) {
  uint32_t id = ((uint32_t)(b[0] & 0x07) << 16) | ((uint32_t)b[1] << 8) | b[2];
  uint8_t battery_low = (b[3] >> 7) & 1;
  uint8_t test = (b[3] >> 6) & 1;
  uint8_t channel = (b[3] >> 4) & 0x03;
  uint8_t type = b[3] & 0x0F;
  uint16_t val_a = ((uint16_t)b[4] << 4) | (b[5] >> 4);
  uint16_t val_b = ((uint16_t)(b[5] & 0x0F) << 8) | b[6];

  char buf[96];
  std::snprintf(buf, sizeof(buf),
                "OK type=%u id=%05X ch=%u bat=%s%s a=%u b=%u (%02X %02X %02X %02X %02X %02X %02X %02X)", type,
                (unsigned)id, channel, battery_low ? "LOW" : "ok", test ? " TEST" : "", val_a, val_b, b[0], b[1], b[2],
                b[3], b[4], b[5], b[6], b[7]);
  ESP_LOGD(TAG, "%s", buf);
#ifdef USE_TEXT_SENSOR
  if (this->sensor_pkt_hex_ != nullptr) this->sensor_pkt_hex_->publish_state(buf);
#endif

  if (this->sensor_id_ != nullptr) this->sensor_id_->publish_state(id);
  if (this->sensor_battery_ != nullptr) this->sensor_battery_->publish_state(battery_low ? 0 : 100);

  if (type == 1) {
    // Temperature + humidity
    // The cast to int32_t makes the signed subtraction explicit (val_a < 500
    // when below freezing). On 32-bit platforms the implicit promotion already
    // does the right thing, but being explicit removes a footgun for anyone
    // porting this elsewhere.
    float t = ((int32_t)val_a - 500) * 0.1f;
    float h = (float)val_b;
    if (t > -40.0f && t < 80.0f) {
      if (this->sensor_temp_ != nullptr) this->sensor_temp_->publish_state(t);
    }
    if (h >= 0.0f && h <= 100.0f) {
      if (this->sensor_hum_ != nullptr) this->sensor_hum_->publish_state(h);
    }
    // Status is a low-cardinality health token (deduped in publish_status_),
    // not a per-packet readout — the live T/H already have dedicated sensors,
    // and the full decode line is still logged at DEBUG above when tuning.
    this->publish_status_("OK");
  } else if (type == 2) {
    // Wind speed (+ direction if anemometer variant)
    float w = val_a * 0.1f;
    if (w >= 0.0f && w < 250.0f) {
      if (this->sensor_wind_ != nullptr) this->sensor_wind_->publish_state(w);
    }
    if (val_b <= 360) {
      if (this->sensor_wind_dir_ != nullptr) this->sensor_wind_dir_->publish_state(val_b);
    }
    this->publish_status_("OK");
  } else {
    ESP_LOGW(TAG, "Unknown subtype %u (passed CRC though)", type);
    char st[48];
    std::snprintf(st, sizeof(st), "unknown subtype %u", type);
    this->publish_status_(st);
  }
}

void TFATX141W::publish_status_(const std::string &s) {
#ifdef USE_TEXT_SENSOR
  // Dedup: only re-publish on an actual change. With the status reduced to a
  // low-cardinality token ("OK" / "decode fail: …"), steady-state operation
  // re-publishes nothing — turning ~2,880 publishes/day (one per ~30s packet,
  // each fanned out to the API + web_server SSE serializers) into a handful,
  // which is the point on a heap-starved ESP8266. Comparing against the stored
  // state is safe: it starts empty and we never publish "", so the first real
  // status always goes out, and HA reconnects re-read the stored state rather
  // than relying on a fresh publish.
  if (this->sensor_status_ != nullptr && this->sensor_status_->state != s) this->sensor_status_->publish_state(s);
#endif
}

// ============================================================
// CRC-8 (rtl_433 style)
// ============================================================
uint8_t TFATX141W::crc8(const uint8_t *data, size_t len, uint8_t poly, uint8_t init) {
  uint8_t crc = init;
  for (size_t i = 0; i < len; i++) {
    crc ^= data[i];
    for (int j = 0; j < 8; j++) {
      if (crc & 0x80)
        crc = (uint8_t)((crc << 1) ^ poly);
      else
        crc = (uint8_t)(crc << 1);
    }
  }
  return crc;
}

// ============================================================
// Debug helpers
// ============================================================
void TFATX141W::hist_add_(uint32_t width) {
  size_t b;
  if (width < 150)
    b = 0;
  else if (width < 200)
    b = 1;
  else if (width < 250)
    b = 2;
  else if (width < 300)
    b = 3;
  else if (width < 350)
    b = 4;
  else if (width < 400)
    b = 5;
  else if (width < 450)
    b = 6;
  else if (width < 500)
    b = 7;
  else if (width < 700)
    b = 8;
  else if (width < 900)
    b = 9;
  else if (width < 1100)
    b = 10;
  else if (width < 2000)
    b = 11;
  else if (width < 5000)
    b = 12;
  else
    b = 13;
  this->hist_[b]++;
}

void TFATX141W::dump_histogram_() {
  static const char *labels[HIST_N] = {"  <150 ", "150-200", "200-250", "250-300",  "300-350", "350-400", "400-450",
                                       "450-500", "500-700", "700-900", "900-1.1k", "1.1k-2k", "2k-5k  ", " >=5k  "};
  ESP_LOGI(TAG, "Pulse-width histogram (µs, last 30s):");
  for (size_t i = 0; i < HIST_N; i++) {
    const char *marker = "";
    // Mark the expected clusters
    if (i == 2)
      marker = "  <-- expect 'short' (~208µs)";
    else if (i == 6)
      marker = "  <-- expect 'long' (~417µs)";
    else if (i == 9)
      marker = "  <-- expect 'sync' (~833µs)";
    else if (i == 13)
      marker = "  <-- inter-packet gaps";
    ESP_LOGI(TAG, "  %s  %6u%s", labels[i], this->hist_[i], marker);
    this->hist_[i] = 0;
  }
}

void TFATX141W::log_stats_() {
  ESP_LOGI(TAG,
           "stats: edges=%u syncs=%u valid=%u invalid=%u "
           "(preamble_fail=%u crc_fail=%u short=%u) isr_overflows=%u",
           this->stat_total_edges_, this->stat_syncs_, this->stat_valid_, this->stat_invalid_,
           this->stat_preamble_fail_, this->stat_crc_fail_, this->stat_short_packets_, (unsigned)this->isr_overflows_);
}

}  // namespace esphome::tfa_tx141w
