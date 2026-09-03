#include <pebble.h>

#define SETTINGS_KEY 1
#define MAX_SLOTS 8

// HRV comes from the sensor's peak-to-peak (RR) intervals. The Goodix algorithm reports
// bursts of up to 4 consecutive intervals, each as its own HealthEventHRVUpdate, so RMSSD
// over a rolling window of them is a real measurement rather than an approximation.
// Holding an HRV sample period keeps the PPG sensor and the HRV algorithm running, so the
// sensor is only opened for a short measurement window every so often rather than left on.
// HRV moves slowly enough that a half-hourly reading loses nothing.
#define HRV_BUF_LEN 16              // Rolling window of RR intervals
#define HRV_MIN_DIFFS 4             // Successive differences needed before showing a number
#define HRV_PPI_MIN_MS 300          // 200 bpm - anything shorter is an artifact
#define HRV_PPI_MAX_MS 2000         // 30 bpm - anything longer is an artifact
#define HRV_MAX_DIFF_MS 250         // Reject ectopic beats and gaps between bursts
#define HRV_MEASURE_EVERY_SEC 1800  // Start a fresh measurement window every 30 min
#define HRV_BURST_PERIOD_SEC 5      // Sample fast inside a window so it closes quickly
#define HRV_WINDOW_MAX_SEC 120      // Abandon a window that isn't filling up
#define HRV_RESULT_STALE_SEC 7200   // Stop trusting a number after 2 h of failed windows

// The HRV API landed in SDK 4.32; older SDKs still build, they just report N/A.
// Testing the symbol pebble_sdk_version.h generates, rather than going through the
// PBL_API_EXISTS() wrapper, because the wrapper expands to defined() inside an #if.
#if defined(PBL_HEALTH) && defined(_PBL_API_EXISTS_health_service_set_hrv_sample_period)
#define DECK_HAS_HRV 1
#endif

// Master Complication Enum
typedef enum {
  COMP_NONE = 0,
  COMP_DATE,
  COMP_BATTERY,
  COMP_MEMORY,
  COMP_COMPASS,
  COMP_HEART_RATE,
  COMP_STEPS,
  COMP_WEATHER,
  COMP_ALT,
  COMP_CUSTOM_API,
  COMP_TZ2,
  COMP_TZ3,
  COMP_MOON_PHASE,
  COMP_HRV
} ComplicationType;

typedef struct ClaySettings {
  GColor BackgroundColor;
  GColor TextColor;
  ComplicationType ActiveSlots[MAX_SLOTS];
  int8_t Tz2Offset;
  int8_t Tz3Offset;
} ClaySettings;

static ClaySettings settings;

static Window *s_main_window;
static TextLayer *s_time_layer;
static TextLayer *s_ampm_layer;
static char s_ampm_buffer[3];

// Dynamic Slot Arrays
static TextLayer *s_slot_layers[MAX_SLOTS];
static char s_slot_buffers[MAX_SLOTS][32];

// Fonts
static GFont s_time_font;
static GFont s_sys_font;

// Global Data Caches
static int s_battery_level = 0;
static int s_compass_degrees = 0;
static const char *s_compass_dir = "---";
static bool s_compass_valid = false;
static int s_hr_value = 0;
static int s_steps_value = 0;
static char s_weather_cache[32] = "SCANNING...";
static char s_api_cache[16] = "NO DATA";
static int s_alt_cache = 0;
static char s_moon_cache[16] = "";
static int s_moon_day = -1;

// HRV state. UNSUPPORTED covers both "no HRV hardware" (only the Pebble Time 2 / obelix
// board enables it) and "user has heart rate turned off in settings".
typedef enum {
  HRV_UNSUPPORTED = 0,  // Sensor or user settings ruled it out up front
  HRV_NO_DATA,          // Subscription accepted, but nothing ever came back
  HRV_COLLECTING,
  HRV_READY
} HrvState;

static HrvState s_hrv_state = HRV_COLLECTING;
static int s_hrv_rmssd = 0;
static uint16_t s_hrv_ppi[HRV_BUF_LEN];
static int s_hrv_count = 0;
static int s_hrv_head = 0;
static bool s_hrv_measuring = false;  // True only while the sensor is actually open
static time_t s_hrv_window_start = 0;
static time_t s_hrv_result_at = 0;    // When the displayed number was measured

static AppTimer *s_compass_timer = NULL;
static bool s_compass_subscribed = false;
static bool s_health_subscribed = false;

static Layer *s_window_layer;
static Layer *s_hud_layer;
static GRect s_full_bounds;

static void prv_default_settings() {
  settings.BackgroundColor = GColorBlack;
  settings.TextColor = GColorCyan;
  settings.Tz2Offset = 0;
  settings.Tz3Offset = 0;

  // Default stack order of your original face
  settings.ActiveSlots[0] = COMP_DATE;
  settings.ActiveSlots[1] = COMP_BATTERY;
  settings.ActiveSlots[2] = COMP_MEMORY;
  settings.ActiveSlots[3] = COMP_COMPASS;
  settings.ActiveSlots[4] = COMP_HEART_RATE;
  settings.ActiveSlots[5] = COMP_STEPS;
  settings.ActiveSlots[6] = COMP_WEATHER;
  settings.ActiveSlots[7] = COMP_TZ2;
}

static void prv_save_settings() {
  persist_write_data(SETTINGS_KEY, &settings, sizeof(settings));
}

static void prv_load_settings() {
  prv_default_settings();
  persist_read_data(SETTINGS_KEY, &settings, sizeof(settings));
}

// --- HELPER: ASCII PROGRESS BAR ---
static void make_ascii_bar(int percent, char *buffer, int segments) {
  buffer[0] = '[';
  int fill = (percent * segments) / 100;
  if(fill > segments) fill = segments;
  for(int i = 0; i < segments; i++) {
    buffer[i+1] = (i < fill) ? '|' : '.';
  }
  buffer[segments+1] = ']';
  buffer[segments+2] = '\0';
}

// --- HELPER: IS A COMPLICATION ON SCREEN? ---
static bool prv_slot_active(ComplicationType comp) {
  for (int i = 0; i < MAX_SLOTS; i++) {
    if (settings.ActiveSlots[i] == comp) return true;
  }
  return false;
}

// --- HELPER: INTEGER SQUARE ROOT (NEWTON) ---
static uint32_t prv_isqrt(uint32_t n) {
  if (n == 0) return 0;
  uint32_t x = n;
  uint32_t y = (x + 1) / 2;
  while (y < x) {
    x = y;
    y = (x + n / x) / 2;
  }
  return x;
}

// --- MOON PHASE CALCULATION ---
static double moon_age(int y, int m, int d) {
  int a = (14 - m) / 12;
  int y2 = y + 4800 - a;
  int m2 = m + 12 * a - 3;
  double jd = d + (double)((153 * m2 + 2) / 5) + 365 * (double)y2
              + (double)(y2 / 4) - (double)(y2 / 100) + (double)(y2 / 400) - 32045.0;
  double age = (jd - 2451550.1) / 29.530588853;
  age = age - (int)age;
  if (age < 0) age += 1.0;
  return age * 29.530588853;
}

static const char *moon_phase_name(double age) {
  if (age < 1.84566)       return "NEW";
  if (age < 5.53699)       return "WXCRESC";
  if (age < 9.22831)       return "1ST QTR";
  if (age < 12.91963)      return "WXGIBBS";
  if (age < 16.61096)      return "FULL";
  if (age < 20.30228)      return "WNGIBBS";
  if (age < 23.99361)      return "3RD QTR";
  if (age < 27.68493)      return "WNCRESC";
  return "NEW";
}

static void health_handler(HealthEventType event, void *context);

// --- CORE RENDER ENGINE ---
static void render_slots() {
  for (int i = 0; i < MAX_SLOTS; i++) {
    ComplicationType current_comp = settings.ActiveSlots[i];
    char scratch[32];
    char *buffer = scratch;
    buffer[0] = '\0';

    switch(current_comp) {
      case COMP_DATE: {
        time_t temp = time(NULL);
        struct tm *tick_time = localtime(&temp);
        strftime(buffer, 32, "[SYS] >> %a %d.%m", tick_time);
        break;
      }
      case COMP_BATTERY: {
        char bar_str[10];
        make_ascii_bar(s_battery_level, bar_str, 6);
        snprintf(buffer, 32, "[PWR] >> %s %d%%", bar_str, s_battery_level);
        break;
      }
      case COMP_MEMORY:
        snprintf(buffer, 32, "[MEM] >> %d KB FREE", ((int)heap_bytes_free()) / 1024);
        break;
      case COMP_COMPASS:
        if (!s_compass_valid) {
          snprintf(buffer, 32, "[NAV] >> CALIBRATING");
        } else {
          snprintf(buffer, 32, "[NAV] >> %d DEG %s", s_compass_degrees, s_compass_dir);
        }
        break;
      case COMP_HEART_RATE:
#if defined(PBL_HEALTH)
        if (s_hr_value > 0) {
          snprintf(buffer, 32, "[BPM] >> %d BPM", s_hr_value);
        } else if (s_hr_value == -1) {
          snprintf(buffer, 32, "[BPM] >> SCANNING...");
        } else {
          snprintf(buffer, 32, "[BPM] >> OFFLINE");
        }
#else
        snprintf(buffer, 32, "[BPM] >> N/A");
#endif
        break;
      case COMP_HRV:
#if defined(DECK_HAS_HRV)
        if (s_hrv_state == HRV_READY) {
          snprintf(buffer, 32, "[HRV] >> %d MS", s_hrv_rmssd);
        } else if (s_hrv_state == HRV_COLLECTING) {
          snprintf(buffer, 32, "[HRV] >> COLLECTING");
        } else if (s_hrv_state == HRV_NO_DATA) {
          snprintf(buffer, 32, "[HRV] >> NO DATA");
        } else {
          snprintf(buffer, 32, "[HRV] >> UNSUPPORTED");
        }
#else
        snprintf(buffer, 32, "[HRV] >> N/A");
#endif
        break;
      case COMP_STEPS: {
#if defined(PBL_HEALTH)
        char bar_str[10];
        int percent = (s_steps_value * 100) / 10000;
        make_ascii_bar(percent, bar_str, 6);
        snprintf(buffer, 32, "[STP] >> %s %d", bar_str, s_steps_value);
#else
        snprintf(buffer, 32, "[STP] >> N/A");
#endif
        break;
      }
      case COMP_WEATHER:
        snprintf(buffer, 32, "[MET] >> %s", s_weather_cache);
        break;
      case COMP_CUSTOM_API:
        snprintf(buffer, 32, "[API] >> %s", s_api_cache);
        break;
      case COMP_ALT:
        snprintf(buffer, 32, "[ALT] >> %dft", s_alt_cache);
        break;
      case COMP_TZ2: {
        time_t utc_now = time(NULL);
        time_t tz2_time = utc_now + (settings.Tz2Offset * 3600);
        struct tm *tz2_tm = gmtime(&tz2_time);
        char time_str[6];
        strftime(time_str, sizeof(time_str), "%H:%M", tz2_tm);
        int off = settings.Tz2Offset;
        char sign = off >= 0 ? '+' : '-';
        if (off < 0) off = -off;

        if (off == 0) {
            snprintf(buffer, 32, "[TZ2] >> %s UTC", time_str);
        } else {
            snprintf(buffer, 32, "[TZ2] >> %s UTC%c%d", time_str, sign, off);
        }
        break;
      }
      case COMP_TZ3: {
        time_t utc_now = time(NULL);
        time_t tz3_time = utc_now + (settings.Tz3Offset * 3600);
        struct tm *tz3_tm = gmtime(&tz3_time);
        char time_str[6];
        strftime(time_str, sizeof(time_str), "%H:%M", tz3_tm);
        int off = settings.Tz3Offset;
        char sign = off >= 0 ? '+' : '-';
        if (off < 0) off = -off;

        if (off == 0) {
            snprintf(buffer, 32, "[TZ3] >> %s UTC", time_str);
        } else {
            snprintf(buffer, 32, "[TZ3] >> %s UTC%c%d", time_str, sign, off);
        }
        break;
      }
      case COMP_MOON_PHASE: {
        time_t now = time(NULL);
        struct tm *tm_now = gmtime(&now);
        // The double maths here is soft-float on this CPU and the phase only changes
        // once a day, so hold the answer until the date rolls over.
        if (tm_now->tm_yday != s_moon_day) {
          double age = moon_age(tm_now->tm_year + 1900, tm_now->tm_mon + 1, tm_now->tm_mday);
          snprintf(s_moon_cache, sizeof(s_moon_cache), "%s", moon_phase_name(age));
          s_moon_day = tm_now->tm_yday;
        }
        snprintf(buffer, 32, "[LUN] >> %s", s_moon_cache);
        break;
      }
      case COMP_NONE:
      default:
        snprintf(buffer, 32, "   ");
        break;
    }
    buffer[sizeof(scratch) - 1] = '\0';
    // Repainting costs more than comparing, and most events change nothing on screen.
    if (strcmp(buffer, s_slot_buffers[i]) != 0) {
      memcpy(s_slot_buffers[i], scratch, sizeof(scratch));
      text_layer_set_text(s_slot_layers[i], s_slot_buffers[i]);
    }
  }
}

static void prv_update_display() {
  window_set_background_color(s_main_window, settings.BackgroundColor);
  text_layer_set_text_color(s_time_layer, settings.TextColor);
  text_layer_set_text_color(s_ampm_layer, settings.TextColor);

  for(int i = 0; i < MAX_SLOTS; i++) {
    text_layer_set_text_color(s_slot_layers[i], settings.TextColor);
  }

  render_slots();
  layer_mark_dirty(s_hud_layer);
}

// --- DATA CACHE UPDATERS ---
static void update_time_data() {
  time_t temp = time(NULL);
  struct tm *tick_time = localtime(&temp);

  static char s_time_buffer[6];
  strftime(s_time_buffer, sizeof(s_time_buffer), clock_is_24h_style() ? "%H:%M" : "%I:%M", tick_time);
  text_layer_set_text(s_time_layer, s_time_buffer);

  if (clock_is_24h_style()) {
    layer_set_hidden(text_layer_get_layer(s_ampm_layer), true);
  } else {
    s_ampm_buffer[0] = tick_time->tm_hour >= 12 ? 'P' : 'A';
    s_ampm_buffer[1] = 'M';
    s_ampm_buffer[2] = '\0';
    text_layer_set_text(s_ampm_layer, s_ampm_buffer);
    layer_set_hidden(text_layer_get_layer(s_ampm_layer), false);
  }

  render_slots();
}

static void compass_handler(CompassHeadingData heading_data) {
  if (heading_data.compass_status == CompassStatusDataInvalid ||
      heading_data.compass_status == CompassStatusCalibrating) {
    s_compass_valid = false;
  } else {
    s_compass_valid = true;
    s_compass_degrees = 360 - TRIGANGLE_TO_DEG(heading_data.magnetic_heading);

    if(s_compass_degrees < 23 || s_compass_degrees > 337) s_compass_dir = "N";
    else if(s_compass_degrees < 68) s_compass_dir = "NE";
    else if(s_compass_degrees < 113) s_compass_dir = "E";
    else if(s_compass_degrees < 158) s_compass_dir = "SE";
    else if(s_compass_degrees < 203) s_compass_dir = "S";
    else if(s_compass_degrees < 248) s_compass_dir = "SW";
    else if(s_compass_degrees < 293) s_compass_dir = "W";
    else if(s_compass_degrees < 338) s_compass_dir = "NW";
  }
  render_slots();
}

static void compass_timer_callback(void *data) {
  if (!s_compass_valid) {
    s_compass_timer = app_timer_register(10000, compass_timer_callback, NULL);
    return;
  }

  if (s_compass_subscribed) {
    compass_service_unsubscribe();
    s_compass_subscribed = false;
  }
  s_compass_timer = NULL;
}

static void backlight_callback(bool backlight_on) {
  if (backlight_on) {
    if (s_compass_timer) {
      app_timer_cancel(s_compass_timer);
    }
    if (!s_compass_subscribed) {
      compass_service_subscribe(compass_handler);
      compass_service_set_heading_filter(2 * (TRIG_MAX_ANGLE / 360));
      s_compass_subscribed = true;
    }
    s_compass_timer = app_timer_register(5000, compass_timer_callback, NULL);
  }
}

static void update_hr() {
#if defined(PBL_HEALTH)
  HealthMetric metric = HealthMetricHeartRateBPM;
  time_t start = time(NULL);
  time_t end = time(NULL);

  if (health_service_metric_accessible(metric, start, end) & HealthServiceAccessibilityMaskAvailable) {
    HealthValue hr = health_service_peek_current_value(metric);
    s_hr_value = (hr > 0) ? (int)hr : -1;
  } else {
    s_hr_value = 0; // Offline
  }
#endif
  render_slots();
}

// --- HRV: RMSSD OVER THE SENSOR'S RR INTERVALS ---
#if defined(DECK_HAS_HRV)
// Clears the interval buffer only. The last computed RMSSD deliberately survives, so the
// slot keeps showing the previous reading while the next window is being collected.
static void prv_hrv_reset() {
  s_hrv_count = 0;
  s_hrv_head = 0;
}

static void prv_hrv_push_ppi(uint16_t ppi) {
  // A 0 here means off-wrist or no reading; out-of-range values are sensor artifacts.
  if (ppi < HRV_PPI_MIN_MS || ppi > HRV_PPI_MAX_MS) return;

  s_hrv_ppi[s_hrv_head] = ppi;
  s_hrv_head = (s_hrv_head + 1) % HRV_BUF_LEN;
  if (s_hrv_count < HRV_BUF_LEN) s_hrv_count++;
}

// Returns true if there were enough clean intervals to produce a number.
static bool prv_hrv_compute() {
  uint32_t sum_sq = 0;
  int diffs = 0;
  int start = (s_hrv_head - s_hrv_count + HRV_BUF_LEN) % HRV_BUF_LEN;

  for (int i = 1; i < s_hrv_count; i++) {
    int prev = s_hrv_ppi[(start + i - 1) % HRV_BUF_LEN];
    int delta = s_hrv_ppi[(start + i) % HRV_BUF_LEN] - prev;
    if (delta < 0) delta = -delta;
    // Skip jumps too big to be beat-to-beat: an ectopic beat, or the gap between two bursts.
    if (delta > HRV_MAX_DIFF_MS) continue;
    sum_sq += (uint32_t)(delta * delta);
    diffs++;
  }

  if (diffs < HRV_MIN_DIFFS) return false;
  s_hrv_rmssd = (int)prv_isqrt(sum_sq / (uint32_t)diffs);
  return true;
}

static void prv_hrv_stop_window() {
  health_service_set_hrv_sample_period(0);  // Let the sensor go back to sleep
  s_hrv_measuring = false;
}

static void prv_hrv_start_window() {
  if (!health_service_set_hrv_sample_period(HRV_BURST_PERIOD_SEC)) {
    s_hrv_state = HRV_UNSUPPORTED;
    return;
  }
  prv_hrv_reset();
  s_hrv_measuring = true;
  s_hrv_window_start = time(NULL);
  if (s_hrv_state == HRV_UNSUPPORTED) s_hrv_state = HRV_COLLECTING;
}

static void prv_hrv_finish_window(time_t now) {
  if (prv_hrv_compute()) {
    s_hrv_state = HRV_READY;
    s_hrv_result_at = now;
  } else if (s_hrv_state != HRV_READY) {
    // Nothing to show yet; say whether the sensor was silent or just too noisy.
    s_hrv_state = (s_hrv_count == 0) ? HRV_NO_DATA : HRV_COLLECTING;
  }
  // A failed window leaves any previous number on screen until it goes stale.
  prv_hrv_stop_window();
}
#endif

// Drives the measurement schedule. Called once a minute; the sensor is open only
// between prv_hrv_start_window() and prv_hrv_finish_window().
static void prv_hrv_tick(time_t now) {
#if defined(DECK_HAS_HRV)
  if (!prv_slot_active(COMP_HRV)) {
    if (s_hrv_measuring) prv_hrv_stop_window();
    return;
  }

  if (s_hrv_measuring) {
    if ((now - s_hrv_window_start) >= HRV_WINDOW_MAX_SEC) {
      prv_hrv_finish_window(now);
      render_slots();
    }
    return;
  }

  if (s_hrv_state == HRV_READY && (now - s_hrv_result_at) > HRV_RESULT_STALE_SEC) {
    s_hrv_state = HRV_NO_DATA;
    render_slots();
  }

  // s_hrv_result_at stays 0 until a window succeeds, so a watch whose activity service
  // wasn't ready at startup - or that has no HRV at all - simply retries each minute.
  if (s_hrv_result_at == 0 || (now - s_hrv_result_at) >= HRV_MEASURE_EVERY_SEC) {
    prv_hrv_start_window();
  }
#endif
}

// Re-evaluate when the slot layout changes: stop the sensor if HRV was removed, and
// take a reading straight away if it was just added.
static void prv_hrv_on_layout_change() {
#if defined(DECK_HAS_HRV)
  if (!prv_slot_active(COMP_HRV)) {
    if (s_hrv_measuring) prv_hrv_stop_window();
    prv_hrv_reset();
    s_hrv_rmssd = 0;
    s_hrv_state = HRV_COLLECTING;
    s_hrv_result_at = 0;
    return;
  }

  if (!s_health_subscribed) {
    s_health_subscribed = health_service_events_subscribe(health_handler, NULL);
  }
  if (!s_hrv_measuring) prv_hrv_start_window();
#endif
}

static void update_steps() {
#if defined(PBL_HEALTH)
  HealthMetric metric = HealthMetricStepCount;
  time_t start = time_start_of_today();
  time_t end = time(NULL);

  if(health_service_metric_accessible(metric, start, end) & HealthServiceAccessibilityMaskAvailable) {
    s_steps_value = (int)health_service_sum_today(metric);
  } else {
    s_steps_value = 0;
  }
#endif
  render_slots();
}

static void tick_handler(struct tm *tick_time, TimeUnits units_changed) {
  update_time_data();
  prv_hrv_tick(time(NULL));
  if (tick_time->tm_min % 30 == 0) {
    DictionaryIterator *iter;
    app_message_outbox_begin(&iter);
    dict_write_uint8(iter, MESSAGE_KEY_RequestWeather, 1);
    app_message_outbox_send();
  }
}

static void health_handler(HealthEventType event, void *context) {
  if (event == HealthEventMovementUpdate) {
    update_steps();
  } else if (event == HealthEventHeartRateUpdate) {
    update_hr();
  }
#if defined(DECK_HAS_HRV)
  // Each event carries one RR interval, and a burst arrives as several back-to-back
  // events, so every one has to be picked up to keep the intervals consecutive.
  else if (event == HealthEventHRVUpdate && s_hrv_measuring) {
    prv_hrv_push_ppi(health_service_peek_hrv_ppi_ms());
    // Close the window the moment there's enough data - every extra second is sensor power.
    if (s_hrv_count >= HRV_BUF_LEN) {
      prv_hrv_finish_window(time(NULL));
      render_slots();
    }
  }
#endif
}

static void battery_callback(BatteryChargeState state) {
  s_battery_level = state.charge_percent;
  render_slots();
}

// --- GRAPHICS UPDATERS ---
static void hud_update_proc(Layer *layer, GContext *ctx) {
  GRect bounds = layer_get_bounds(layer);
  graphics_context_set_stroke_color(ctx, settings.TextColor);
  graphics_context_set_stroke_width(ctx, 1);

  int mid_x = bounds.size.w / 2;
  graphics_draw_line(ctx, GPoint(mid_x - 12, 4), GPoint(mid_x + 12, 4));
  graphics_draw_line(ctx, GPoint(mid_x - 12, 4), GPoint(mid_x - 12, 8));
  graphics_draw_line(ctx, GPoint(mid_x + 12, 4), GPoint(mid_x + 12, 8));

  graphics_draw_line(ctx, GPoint(8, 74), GPoint(bounds.size.w - 8, 74));

  int len = 8;
  graphics_draw_line(ctx, GPoint(2, 2), GPoint(2 + len, 2));
  graphics_draw_line(ctx, GPoint(2, 2), GPoint(2, 2 + len));
  graphics_draw_line(ctx, GPoint(bounds.size.w - 3, 2), GPoint(bounds.size.w - 3 - len, 2));
  graphics_draw_line(ctx, GPoint(bounds.size.w - 3, 2), GPoint(bounds.size.w - 3, 2 + len));
  graphics_draw_line(ctx, GPoint(2, bounds.size.h - 3), GPoint(2 + len, bounds.size.h - 3));
  graphics_draw_line(ctx, GPoint(2, bounds.size.h - 3), GPoint(2, bounds.size.h - 3 - len));
  graphics_draw_line(ctx, GPoint(bounds.size.w - 3, bounds.size.h - 3), GPoint(bounds.size.w - 3 - len, bounds.size.h - 3));
  graphics_draw_line(ctx, GPoint(bounds.size.w - 3, bounds.size.h - 3), GPoint(bounds.size.w - 3, bounds.size.h - 3 - len));
}

// --- APPMESSAGE HANDLERS ---
static void inbox_received_callback(DictionaryIterator *iterator, void *context) {
  Tuple *conditions_tuple = dict_find(iterator, MESSAGE_KEY_Conditions);
  Tuple *alt_tuple = dict_find(iterator, MESSAGE_KEY_Alt);
  Tuple *custom_api_tuple = dict_find(iterator, MESSAGE_KEY_CustomApi);

  bool settings_changed = false;

  if (conditions_tuple) {
    snprintf(s_weather_cache, sizeof(s_weather_cache), "%s", conditions_tuple->value->cstring);
  }

  if (custom_api_tuple) {
    snprintf(s_api_cache, sizeof(s_api_cache), "%s", custom_api_tuple->value->cstring);
  }

  if (alt_tuple) {
    s_alt_cache = (int)alt_tuple->value->int32;
  }

  // Handle slot configuration from Clay
  Tuple *slot_tuples[MAX_SLOTS] = {
    dict_find(iterator, MESSAGE_KEY_ActiveSlots1),
    dict_find(iterator, MESSAGE_KEY_ActiveSlots2),
    dict_find(iterator, MESSAGE_KEY_ActiveSlots3),
    dict_find(iterator, MESSAGE_KEY_ActiveSlots4),
    dict_find(iterator, MESSAGE_KEY_ActiveSlots5),
    dict_find(iterator, MESSAGE_KEY_ActiveSlots6),
    dict_find(iterator, MESSAGE_KEY_ActiveSlots7),
    dict_find(iterator, MESSAGE_KEY_ActiveSlots8)
  };
  for (int i = 0; i < MAX_SLOTS; i++) {
    if (slot_tuples[i]) {
      settings.ActiveSlots[i] = (ComplicationType)atoi(slot_tuples[i]->value->cstring);
      settings_changed = true;
    }
  }

  Tuple *bg_tuple = dict_find(iterator, MESSAGE_KEY_BackgroundColor);
  if (bg_tuple) {
    settings.BackgroundColor = GColorFromHEX(bg_tuple->value->int32);
    settings_changed = true;
  }

  Tuple *fg_tuple = dict_find(iterator, MESSAGE_KEY_TextColor);
  if (fg_tuple) {
    settings.TextColor = GColorFromHEX(fg_tuple->value->int32);
    settings_changed = true;
  }

  Tuple *tz2_tuple = dict_find(iterator, MESSAGE_KEY_Tz2Offset);
  if (tz2_tuple) {
    settings.Tz2Offset = (int8_t)tz2_tuple->value->int32;
    settings_changed = true;
  }

  Tuple *tz3_tuple = dict_find(iterator, MESSAGE_KEY_Tz3Offset);
  if (tz3_tuple) {
    settings.Tz3Offset = (int8_t)tz3_tuple->value->int32;
    settings_changed = true;
  }

  if (settings_changed) {
    prv_save_settings();
    prv_hrv_on_layout_change();
    prv_update_display();
  }

  render_slots();
}

static void prv_unobstructed_change(AnimationProgress progress, void *context) {
  GRect bounds = layer_get_unobstructed_bounds(s_window_layer);
  bool peak_visible = (bounds.size.h < s_full_bounds.size.h);
  int base_y = 80;

  for(int i = 0; i < MAX_SLOTS; i++) {
    layer_set_frame(text_layer_get_layer(s_slot_layers[i]),
                    GRect(10, base_y + (i * 14), bounds.size.w - 20, 16));
    if (peak_visible && i >= 5) {
      layer_set_hidden(text_layer_get_layer(s_slot_layers[i]), true);
    } else {
      layer_set_hidden(text_layer_get_layer(s_slot_layers[i]), false);
    }
  }
}

// --- WINDOW LOAD / UNLOAD ---
static void main_window_load(Window *window) {
  s_window_layer = window_get_root_layer(window);
  GRect bounds = layer_get_bounds(s_window_layer);
  s_full_bounds = bounds;

  s_time_font = fonts_load_custom_font(resource_get_handle(RESOURCE_ID_FONT_CHAKRA_56));
  s_sys_font = fonts_load_custom_font(resource_get_handle(RESOURCE_ID_FONT_IBM_14));

  s_hud_layer = layer_create(bounds);
  layer_set_update_proc(s_hud_layer, hud_update_proc);
  layer_add_child(s_window_layer, s_hud_layer);

  s_time_layer = text_layer_create(GRect(0, 12, bounds.size.w, 60));
  text_layer_set_background_color(s_time_layer, GColorClear);
  text_layer_set_font(s_time_layer, s_time_font);
  text_layer_set_text_alignment(s_time_layer, GTextAlignmentCenter);
  layer_add_child(s_window_layer, text_layer_get_layer(s_time_layer));

  s_ampm_layer = text_layer_create(GRect(bounds.size.w - 39, 54, 24, 16));
  text_layer_set_background_color(s_ampm_layer, GColorClear);
  text_layer_set_font(s_ampm_layer, s_sys_font);
  text_layer_set_text_alignment(s_ampm_layer, GTextAlignmentRight);
  layer_add_child(s_window_layer, text_layer_get_layer(s_ampm_layer));

  // Loop generation for all console lines
  int y = 76;
  int step = 16;

  for(int i = 0; i < MAX_SLOTS; i++) {
    s_slot_layers[i] = text_layer_create(GRect(10, y + (i * step), bounds.size.w - 20, 16));
    text_layer_set_background_color(s_slot_layers[i], GColorClear);
    text_layer_set_font(s_slot_layers[i], s_sys_font);
    text_layer_set_text_alignment(s_slot_layers[i], GTextAlignmentLeft);
    layer_add_child(s_window_layer, text_layer_get_layer(s_slot_layers[i]));
  }

  // Properly subbing to Pebble's Quick View / Unobstructed Area API
  UnobstructedAreaHandlers handlers = { .change = prv_unobstructed_change };
  unobstructed_area_service_subscribe(handlers, NULL);

  prv_update_display();
}

static void main_window_unload(Window *window) {
  text_layer_destroy(s_time_layer);
  text_layer_destroy(s_ampm_layer);
  for(int i = 0; i < MAX_SLOTS; i++) {
    text_layer_destroy(s_slot_layers[i]);
  }
  fonts_unload_custom_font(s_time_font);
  fonts_unload_custom_font(s_sys_font);
  layer_destroy(s_hud_layer);
}

static void init() {
  prv_load_settings();
  s_main_window = window_create();
  window_set_window_handlers(s_main_window, (WindowHandlers) {
    .load = main_window_load,
    .unload = main_window_unload
  });
  window_stack_push(s_main_window, true);

  tick_timer_service_subscribe(MINUTE_UNIT, tick_handler);

  // Conditionally optimize background service subscriptions based on layout
  bool needs_health = false;
  bool needs_compass = false;
  for(int i=0; i<MAX_SLOTS; i++) {
    if(settings.ActiveSlots[i] == COMP_STEPS || settings.ActiveSlots[i] == COMP_HEART_RATE
       || settings.ActiveSlots[i] == COMP_HRV) needs_health = true;
    if(settings.ActiveSlots[i] == COMP_COMPASS) needs_compass = true;
  }

  if(needs_health) {
    s_health_subscribed = health_service_events_subscribe(health_handler, NULL);
  }
  if(needs_compass) {
    compass_service_subscribe(compass_handler);
    compass_service_set_heading_filter(2 * (TRIG_MAX_ANGLE / 360));
    s_compass_subscribed = true;
    s_compass_timer = app_timer_register(5000, compass_timer_callback, NULL);
    backlight_service_subscribe(backlight_callback);
  }

  battery_state_service_subscribe(battery_callback);

  // Open up AppMessage inbox to receive the weather and upcoming custom API strings
  app_message_register_inbox_received(inbox_received_callback);
  app_message_open(256, 256);

  // Request weather immediately on startup
  DictionaryIterator *iter;
  app_message_outbox_begin(&iter);
  dict_write_uint8(iter, MESSAGE_KEY_RequestWeather, 1);
  app_message_outbox_send();

  battery_callback(battery_state_service_peek());
  update_time_data();
  update_steps();
  update_hr();
  prv_hrv_on_layout_change();
  render_slots(); // Paint the settled HRV state now rather than waiting for the first tick
}

static void deinit() {
#if defined(DECK_HAS_HRV)
  // The request survives app exit if left set, so give the HRM its battery back.
  health_service_set_hrv_sample_period(0);
#endif
  if (s_compass_timer) {
    app_timer_cancel(s_compass_timer);
  }
  backlight_service_unsubscribe();
  compass_service_unsubscribe();
  health_service_events_unsubscribe();
  battery_state_service_unsubscribe();
  connection_service_unsubscribe();
  tick_timer_service_unsubscribe();
  unobstructed_area_service_unsubscribe();

  window_destroy(s_main_window);
}

int main(void) {
  init();
  app_event_loop();
  deinit();
}
