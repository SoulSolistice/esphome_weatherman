// TFA Dostmann 30.3222.02 (LaCrosse TX141W) wired-tap decoder
// for the WEATHERMAN 2.1 hardware on ESP8266 + ESPHome.
//
// Reads the baseband data tapped from the open-collector signal of the BC547C
// inside the wind head, pulled high on the controller board via R1 4k7 to 3.3V.
// Decoder protocol per rtl_433 lacrosse_tx141x.c.
//
// Frame: PRE5 ID19 BAT1 TEST1 CH2 TYPE4 VAL_A12 VAL_B12 CRC8 stop1   (65 bits)
//   type 1: temp+humidity   (temp = (VAL_A - 500)/10  °C ; hum = VAL_B  %)
//   type 2: wind+direction  (wind = VAL_A/10  km/h    ; dir = VAL_B  °)
//   CRC-8 poly 0x31 init 0x00 over bytes 0..6
//   Preamble: (b[0] >> 3) == 0x01

#pragma once

#include "esphome/core/component.h"
#include "esphome/core/hal.h"
#include "esphome/components/sensor/sensor.h"

#ifdef USE_TEXT_SENSOR
#include "esphome/components/text_sensor/text_sensor.h"
#endif

namespace esphome::tfa_tx141w {

class TFATX141W : public Component {
 public:
  // Ring-buffer size for edge captures. Must be a power of two.
  // 512 holds ~2 full packets worth of edges = plenty of slack for loop() jitter.
  static constexpr size_t EDGE_BUF_SIZE = 512;
  static constexpr size_t EDGE_BUF_MASK = EDGE_BUF_SIZE - 1;

  // Pulse-buffer for one packet. 65 bits * 2 pulses + slack.
  static constexpr size_t PULSE_BUF_SIZE = 140;

  // Histogram buckets used for debugging:
  // [<150, 150-200, 200-250, 250-300, 300-350, 350-400, 400-450,
  //  450-500, 500-700, 700-900, 900-1100, 1100-2000, 2000-5000, >=5000]
  static constexpr size_t HIST_N = 14;

  // -------- YAML setters --------
  void set_pin(uint8_t pin) { pin_ = pin; }
  void set_debug(bool d) { debug_ = d; }
  void set_t_sync(uint32_t t) { t_sync_ = t; }
  void set_t_short(uint32_t t) { t_short_ = t; }
  void set_t_long(uint32_t t) { t_long_ = t; }
  void set_t_tolerance(uint32_t t) { t_tol_ = t; }
  void set_t_reset(uint32_t t) { t_reset_ = t; }
  void set_min_sync_pulses(uint8_t n) { min_sync_pulses_ = n; }

  void set_temperature_sensor(sensor::Sensor *s) { sensor_temp_ = s; }
  void set_humidity_sensor(sensor::Sensor *s) { sensor_hum_ = s; }
  void set_wind_speed_sensor(sensor::Sensor *s) { sensor_wind_ = s; }
  void set_wind_direction_sensor(sensor::Sensor *s) { sensor_wind_dir_ = s; }
  void set_battery_level_sensor(sensor::Sensor *s) { sensor_battery_ = s; }
  void set_sensor_id_sensor(sensor::Sensor *s) { sensor_id_ = s; }
  void set_packets_valid_sensor(sensor::Sensor *s) { sensor_pkts_valid_ = s; }
  void set_packets_invalid_sensor(sensor::Sensor *s) { sensor_pkts_invalid_ = s; }
  void set_sync_detections_sensor(sensor::Sensor *s) { sensor_syncs_ = s; }
#ifdef USE_TEXT_SENSOR
  void set_last_packet_hex_sensor(text_sensor::TextSensor *s) { sensor_pkt_hex_ = s; }
  void set_last_status_sensor(text_sensor::TextSensor *s) { sensor_status_ = s; }
#endif

  // -------- Component lifecycle --------
  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::HARDWARE; }

  // -------- ISR (must be public for attachInterruptArg) --------
  void IRAM_ATTR on_edge();
  // Static trampoline; receives `this` as arg via attachInterruptArg.
  static void IRAM_ATTR isr_trampoline(void *arg);

 protected:
  // ----- Configuration -----
  uint8_t pin_ = 12;
  bool debug_ = false;
  uint32_t t_sync_ = 833;        // sync half-pulse width (µs)
  uint32_t t_short_ = 208;       // short data half (µs)
  uint32_t t_long_ = 417;        // long  data half (µs)
  uint32_t t_tol_ = 200;         // ± tolerance applied to all three above
  uint32_t t_reset_ = 2000;      // pulse longer than this resets sync state
  uint8_t min_sync_pulses_ = 8;  // require this many ~833µs in a row before data
                                 // (rtl_433 spec is 8 = 4 cycles; raise further if noisy)

  // ----- ISR ring buffer -----
  // Stores only the edge timestamp. The post-edge line level was captured here
  // and in the ISR (a digitalRead per edge) but never used: the decoder is
  // polarity-agnostic (see process_pulse_ / the CRC try-both-orientations path),
  // so `level` was pure overhead — 2 KB of dead SRAM (8-byte struct × 512) and a
  // digitalRead on every interrupt. A plain uint32_t ring halves it to 2 KB and
  // removes the ISR read.
  volatile uint32_t edge_buf_[EDGE_BUF_SIZE];
  volatile uint32_t isr_w_ = 0;  // ISR write index
  // volatile because the ISR reads it for the overflow check while the main
  // loop writes it. Defensive even on single-core where the compiler is
  // unlikely to cache across the function-pointer call into the ISR.
  volatile uint32_t isr_r_ = 0;
  volatile uint32_t isr_overflows_ = 0;

  // ----- Sub-sensors -----
  sensor::Sensor *sensor_temp_ = nullptr;
  sensor::Sensor *sensor_hum_ = nullptr;
  sensor::Sensor *sensor_wind_ = nullptr;
  sensor::Sensor *sensor_wind_dir_ = nullptr;
  sensor::Sensor *sensor_battery_ = nullptr;
  sensor::Sensor *sensor_id_ = nullptr;
  sensor::Sensor *sensor_pkts_valid_ = nullptr;
  sensor::Sensor *sensor_pkts_invalid_ = nullptr;
  sensor::Sensor *sensor_syncs_ = nullptr;
#ifdef USE_TEXT_SENSOR
  text_sensor::TextSensor *sensor_pkt_hex_ = nullptr;
  text_sensor::TextSensor *sensor_status_ = nullptr;
#endif

  // ----- Decoder state (main loop only) -----
  enum State { ST_IDLE, ST_SYNCING, ST_DATA };
  State state_ = ST_IDLE;
  uint8_t sync_count_ = 0;
  uint32_t pulse_count_ = 0;
  uint32_t pulse_buf_[PULSE_BUF_SIZE] = {0};
  uint32_t last_edge_ts_ = 0;
  bool have_prev_edge_ = false;

  // ----- Statistics -----
  uint32_t stat_valid_ = 0;
  uint32_t stat_invalid_ = 0;
  uint32_t stat_syncs_ = 0;
  uint32_t stat_preamble_fail_ = 0;
  uint32_t stat_crc_fail_ = 0;
  uint32_t stat_short_packets_ = 0;  // sync seen, < 128 pulses collected
  uint32_t stat_total_edges_ = 0;
  uint32_t last_stats_ms_ = 0;
  uint32_t last_hist_ms_ = 0;
  uint32_t last_any_packet_ms_ = 0;

  // ----- Histogram of all observed pulse widths -----
  uint32_t hist_[HIST_N] = {0};

  // ----- Helpers -----
  void process_pulse_(uint32_t width);
  void try_finish_packet_();
  void publish_packet_(const uint8_t *b);
  void publish_status_(const char *s);
  static uint8_t crc8(const uint8_t *data, size_t len, uint8_t poly, uint8_t init);
  void hist_add_(uint32_t width);
  void dump_histogram_();
  void log_stats_();
  void reset_decoder_();
};

}  // namespace esphome::tfa_tx141w
