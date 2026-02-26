#pragma once
// ═══════════════════════════════════════════════════════════════════
// sensor_history.h — Static RAM ring buffer for 15-minute averaged
//                    sensor readings on ESP32-C3
//
// PURPOSE:
//   Stores up to 24 hours of temperature and humidity averages in
//   RAM using fixed-size (BSS) buffers.  No heap allocation, no
//   flash wear, no filesystem needed.  Data is lost on reboot.
//
// ARCHITECTURE:
//   HistoryBuffer — generic ring buffer holding (epoch, float) pairs.
//   Two global instances: office_temp_history, office_hum_history.
//   Two output formats:
//     1. Temperature display — formatted for ESP web page temp card
//     2. Humidity display — formatted for ESP web page humidity card
//   Both are also parsed by the HTML dashboard's parseFormattedHistory().
//
// CAPACITY:
//   96 entries = 24 hours × 4 entries/hour (one every 15 minutes)
//
// MEMORY BUDGET (BSS segment, zero heap fragmentation):
//   Ring buffers    — CAP × 8 bytes × 2 sensors  =  1,536 B
//   Display buffers — (CAP × 60 + 128) × 2       = 11,776 B
//   Total static                                  ≈  13.3 KB
//   (ESP32-C3 has ~300 KB usable RAM; this leaves plenty for WiFi,
//    BLE stack, web server, and future expansion.)
//
// HEAP IMPACT (via publish_state() copies at full buffer):
//   Temp display  — ~5.6 KB    Hum display — ~5.6 KB
//   Total heap copies ≈ 11.2 KB (vs 42.8 KB in v9)
//
// SAFETY:
//   All snprintf calls are bounds-checked.  If a write would exceed
//   the remaining buffer, the loop breaks cleanly and the buffer is
//   null-terminated.  No out-of-bounds writes are possible.
//
// TEMPERATURE DISPLAY:
//   Temperature values are shown in dual format: °C / °F
//   Conversion: °F = °C × 9/5 + 32
//   This applies to both the ESP web page card and the HTML dashboard.
//
// ═══════════════════════════════════════════════════════════════════
// v10 changes (stability optimization):
//   • CAP reduced from 144 to 96 (15-min intervals, still 24h coverage)
//     WHY: Each entry costs 8B ring + ~60B display + ~60B heap copy.
//     Going from 144→96 saves ~6 KB BSS + ~6 KB heap ≈ 12 KB total.
//     15-min resolution is more than adequate for room temp/humidity.
//   • Removed history_json buffer and build_history_json_static()
//     WHY: The JSON text_sensor was internal:true and never consumed
//     by any client. The HTML dashboard reads from the display
//     text_sensors instead. Removing it saves 14.1 KB BSS + ~13.8 KB
//     heap (the publish_state() copy). This was the single largest
//     unnecessary memory consumer.
//   • Removed write_json_to() method from HistoryBuffer class
//     (was only used by the deleted JSON builder)
//   • Updated buffer size derivation comments for new capacity
//   • Net savings vs v9: ~20 KB BSS + ~20 KB heap ≈ 40 KB total
//     Expected free heap: ~87-95 KB (vs ~48-56 KB in v9)
// ═══════════════════════════════════════════════════════════════════

#include <cstdio>
#include <ctime>
#include <cmath>
#include <string>

// ─── Data structure ──────────────────────────────────────────────
// Each history entry is a (unix_epoch, float_value) pair.
// Epoch is uint32_t (good until 2106) to save 4 bytes vs time_t.
// Value is the 15-minute average computed by the YAML lambda.
struct HistEntry {
  uint32_t epoch;   // UTC seconds since 1970-01-01
  float    value;   // averaged sensor reading (°C or %RH)
};

// ─── Ring buffer class ───────────────────────────────────────────
// Fixed-capacity circular buffer.  Oldest entry is overwritten when
// full.  head_ always points to the NEXT write position.  count_
// tracks how many valid entries exist (0..CAP).
class HistoryBuffer {
 public:
  // v10: 96 entries × 15 min = 1440 min = 24 hours of history
  static constexpr int CAP = 96;

  // ── add() ──────────────────────────────────────────────────────
  // Insert a new (epoch, value) pair.  O(1), no allocation.
  // When buffer is full, overwrites the oldest entry.
  void add(uint32_t epoch, float value) {
    buf_[head_] = {epoch, value};
    head_ = (head_ + 1) % CAP;
    if (count_ < CAP) count_++;
  }

  // ── count() ────────────────────────────────────────────────────
  // Number of valid entries currently in the buffer (0..CAP).
  int count() const { return count_; }

  // ── write_formatted_to() ───────────────────────────────────────
  // Writes human-readable lines for the ESP web page history cards.
  // Each line: timestamp, value with unit.
  //
  // Temperature output (show_fahrenheit = true):
  //   2026-02-19 00:15:00, 21.6°C / 70.9°F
  //
  // Humidity output (show_fahrenheit = false):
  //   2026-02-19 00:15:00, 45%
  //
  // The same output is also parsed by the HTML dashboard's
  // parseFormattedHistory() function — it extracts the timestamp
  // and the FIRST numeric value via regex, so the appended °F
  // value doesn't interfere with parsing.
  //
  // Parameters:
  //   dst, dst_size  — output buffer and its total size
  //   unit           — unit string to append (e.g. "°C" or "%")
  //   decimals       — decimal places for the value (1 for temp, 0 for hum)
  //   show_fahrenheit — if true, append " / XX.X°F" after the °C value
  //
  // Returns: number of chars written (excluding null terminator).
  int write_formatted_to(char* dst, int dst_size, const char* unit,
                         int decimals, bool show_fahrenheit) const {
    int pos = 0;
    int start = (count_ < CAP) ? 0 : head_;

    for (int i = 0; i < count_; i++) {
      int idx = (start + i) % CAP;
      time_t ts = (time_t)buf_[idx].epoch;
      struct tm ti;
      localtime_r(&ts, &ti);

      // Reserve space: need at least 70 chars for longest line
      // "2026-02-19 00:15:00, -12.3°C / 9.9°F\n" = ~42 chars
      int remaining = dst_size - pos - 1;
      if (remaining < 70) break;

      int len;
      if (show_fahrenheit) {
        // Compute Fahrenheit: °F = °C × 9/5 + 32
        float fahrenheit = buf_[idx].value * 9.0f / 5.0f + 32.0f;
        len = snprintf(dst + pos, remaining,
                       "%04d-%02d-%02d %02d:%02d:%02d, %.*f%s / %.1f\xC2\xB0" "F\n",
                       ti.tm_year + 1900, ti.tm_mon + 1, ti.tm_mday,
                       ti.tm_hour, ti.tm_min, ti.tm_sec,
                       decimals, buf_[idx].value, unit, fahrenheit);
      } else {
        len = snprintf(dst + pos, remaining,
                       "%04d-%02d-%02d %02d:%02d:%02d, %.*f%s\n",
                       ti.tm_year + 1900, ti.tm_mon + 1, ti.tm_mday,
                       ti.tm_hour, ti.tm_min, ti.tm_sec,
                       decimals, buf_[idx].value, unit);
      }
      if (len < 0 || len >= remaining) break;
      pos += len;
    }
    dst[pos] = '\0';
    return pos;
  }

 private:
  HistEntry buf_[CAP] = {};  // zero-initialized at startup
  int head_  = 0;            // next write position
  int count_ = 0;            // valid entries (0..CAP)
};

// ═══════════════════════════════════════════════════════════════════
// Buffer size derivation
// ═══════════════════════════════════════════════════════════════════
// Rather than using magic numbers, buffer sizes are calculated from
// CAP to stay correct if history depth is changed.

// Display: temp lines are ~60 chars with °F, hum lines ~45 chars.
// Use 60 as worst case, plus header line.
// Formula: CAP × 60 + 128 bytes for header/separator per sensor
// v10: 96 × 60 + 128 = 5,888 B per sensor (was 8,768 B at CAP=144)
static constexpr int HISTORY_DISPLAY_SIZE = HistoryBuffer::CAP * 60 + 128;

// ═══════════════════════════════════════════════════════════════════
// Global instances — one per physical sensor
// ═══════════════════════════════════════════════════════════════════
// These are file-scope statics included via esphome's `includes:`.
// For multi-sensor, add more instances (e.g. outdoor_temp_history).
static HistoryBuffer office_temp_history;
static HistoryBuffer office_hum_history;

// ═══════════════════════════════════════════════════════════════════
// Static output buffers — allocated in BSS segment
// ═══════════════════════════════════════════════════════════════════
// BSS means they're zero-initialized at boot with no runtime cost.
// No heap allocation, no fragmentation, predictable memory usage.
//
// v10: Only display buffers remain. JSON buffer removed (see above).
// Total BSS for display: 2 × 5,888 = 11,776 B (was 31,616 B in v9)
static char history_temp_display_buf[HISTORY_DISPLAY_SIZE];
static char history_hum_display_buf[HISTORY_DISPLAY_SIZE];

// ── safe_append() ────────────────────────────────────────────────
// Appends a string to buf at position pos, never exceeding buf_size.
// Returns: new position after the appended string.
// If there's not enough room, returns pos unchanged (no partial write).
static int safe_append(char* buf, int buf_size, int pos, const char* str) {
  int remaining = buf_size - pos - 1;  // reserve 1 byte for '\0'
  if (remaining <= 0) return pos;
  int len = snprintf(buf + pos, remaining, "%s", str);
  if (len < 0 || len >= remaining) return pos;  // would truncate → skip
  return pos + len;
}

// ═══════════════════════════════════════════════════════════════════
// build_temp_history_display()
// ═══════════════════════════════════════════════════════════════════
// Builds the formatted text for the TEMPERATURE history card on
// the ESP web page.  Also parsed by the HTML dashboard.
//
// Output format:
//   Office temperature
//   2026-02-19 00:15:00, 21.6°C / 70.9°F
//   2026-02-19 00:30:00, 21.5°C / 70.7°F
static const char* build_temp_history_display() {
  int pos = 0;
  pos = safe_append(history_temp_display_buf, HISTORY_DISPLAY_SIZE, pos,
                    "Office temperature\n");
  pos += office_temp_history.write_formatted_to(
             history_temp_display_buf + pos, HISTORY_DISPLAY_SIZE - pos,
             "\xC2\xB0" "C", 1, true);  // °C in UTF-8, 1 decimal, show °F
  history_temp_display_buf[pos] = '\0';
  return history_temp_display_buf;
}

// ═══════════════════════════════════════════════════════════════════
// build_hum_history_display()
// ═══════════════════════════════════════════════════════════════════
// Builds the formatted text for the HUMIDITY history card on
// the ESP web page.  Also parsed by the HTML dashboard.
//
// Output format:
//   Office humidity
//   2026-02-19 00:15:00, 45%
//   2026-02-19 00:30:00, 44%
static const char* build_hum_history_display() {
  int pos = 0;
  pos = safe_append(history_hum_display_buf, HISTORY_DISPLAY_SIZE, pos,
                    "Office humidity\n");
  pos += office_hum_history.write_formatted_to(
             history_hum_display_buf + pos, HISTORY_DISPLAY_SIZE - pos,
             "%", 0, false);  // %, 0 decimals, no Fahrenheit
  history_hum_display_buf[pos] = '\0';
  return history_hum_display_buf;
}
