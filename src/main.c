#include <pebble.h>

/* Terminal-style watchface. Complications, prompt name and units are set from the phone's
   settings page. Fonts: add DejaVuSansMono.ttf twice under Resources, named MONO_12 and
   MONO_16 (the number at the end is the pixel size). */

#if defined(PBL_PLATFORM_EMERY) || defined(PBL_PLATFORM_GABBRO)
  #define FONT_RES RESOURCE_ID_MONO_16
  #define LINE_H 19
  #define PAD 5
#else
  #define FONT_RES RESOURCE_ID_MONO_12
  #define LINE_H 14
  #define PAD 3
#endif

// AppMessage keys (numbers, to match the phone script)
#define KEY_TEMP 0
#define KEY_COND 1
#define KEY_MASK 2
#define KEY_PROMPT 3
#define KEY_DIST_UNIT 4
#define KEY_SECONDS 5

// Persistent storage keys
#define PERSIST_TEMP 1
#define PERSIST_COND 2
#define PERSIST_MASK 3
#define PERSIST_PROMPT 4
#define PERSIST_MILES 5
#define PERSIST_SECONDS 6

#define SECONDS_SHOW_MS 15000   // after a wrist flick, show seconds this long

// Complication bit positions in the mask the phone sends
enum { COMP_TIME = 0, COMP_DATE, COMP_TEMP, COMP_BATT, COMP_STEP, COMP_DIST, COMP_HR };

typedef struct {
  const char *label;
  const char *value;
  GColor color;
} Row;

static Window *s_window;
static Layer *s_layer;
static GFont s_font;
static int s_label_w = 0;

// Settings (defaults until the phone sends the saved ones)
static uint32_t s_mask = 0x7F;               // everything on
static char s_prompt[20] = "user@pebble";
static bool s_miles = false;
static bool s_seconds_enabled = true;

static char s_time[20] = "";
static char s_date[20] = "";
static char s_temp[16] = "--";
static char s_cond[24] = "";
static char s_batt[24] = "";
static int s_batt_pct = 100;
static char s_steps[24] = "";
static char s_dist[24] = "";
static char s_hr[16] = "";
static bool s_has_health = false;
static bool s_hr_available = false;

static bool s_show_seconds = false;
static AppTimer *s_seconds_timer = NULL;

static bool comp_on(int comp) {
  return (s_mask & (1u << comp)) != 0;
}

/* On round watches the corners are cut off: draw inside the largest square in the circle. */
static GRect safe_bounds(GRect full) {
#if defined(PBL_ROUND)
  int side = (full.size.w * 707) / 1000;
  return GRect((full.size.w - side) / 2, (full.size.h - side) / 2, side, side);
#else
  return full;
#endif
}

/* ---------- Data ---------- */

static void update_time(void) {
  time_t now = time(NULL);
  struct tm *t = localtime(&now);

  if (clock_is_24h_style()) {
    strftime(s_time, sizeof(s_time), s_show_seconds ? "%H:%M:%S" : "%H:%M", t);
  } else {
    strftime(s_time, sizeof(s_time), s_show_seconds ? "%I:%M:%S %p" : "%I:%M %p", t);
    if (s_time[0] == '0') memmove(s_time, s_time + 1, strlen(s_time));   // 07:05 -> 7:05
  }

  char mon[8], wday[8];
  strftime(mon, sizeof(mon), "%b", t);
  strftime(wday, sizeof(wday), "%a", t);
  snprintf(s_date, sizeof(s_date), "%s %d, %s", mon, t->tm_mday, wday);
}

static void update_battery(void) {
  BatteryChargeState st = battery_state_service_peek();
  s_batt_pct = st.charge_percent;
  int filled = (st.charge_percent + 10) / 20;   // 5 cells
  if (filled > 5) filled = 5;
  char bar[8];
  for (int i = 0; i < 5; i++) bar[i] = i < filled ? '#' : '.';
  bar[5] = '\0';
  snprintf(s_batt, sizeof(s_batt), "[%s] %d%%%s", bar, st.charge_percent, st.is_charging ? "+" : "");
}

#if defined(PBL_HEALTH)
static void update_health(void) {
  s_has_health = true;

  HealthValue steps = health_service_sum_today(HealthMetricStepCount);
  snprintf(s_steps, sizeof(s_steps), "%d steps", (int)steps);

  // Distance in meters -> tenths of a km or mile (no float printing on Pebble)
  HealthValue meters = health_service_sum_today(HealthMetricWalkedDistanceMeters);
  int tenths = s_miles ? (int)((meters * 10) / 1609) : (int)(meters / 100);
  snprintf(s_dist, sizeof(s_dist), "%d.%d %s", tenths / 10, tenths % 10, s_miles ? "mi" : "km");

  time_t now = time(NULL);
  HealthServiceAccessibilityMask mask =
      health_service_metric_accessible(HealthMetricHeartRateBPM, now, now);
  s_hr_available = (mask & HealthServiceAccessibilityMaskAvailable) != 0;
  if (s_hr_available) {
    HealthValue hr = health_service_peek_current_value(HealthMetricHeartRateBPM);
    if (hr > 0) snprintf(s_hr, sizeof(s_hr), "%d BPM", (int)hr);
    else snprintf(s_hr, sizeof(s_hr), "-- BPM");
  }
}

static void health_handler(HealthEventType event, void *context) {
  update_health();
  layer_mark_dirty(s_layer);
}
#endif

/* ---------- Drawing ---------- */

static void draw_text(GContext *ctx, const char *text, int x, int y, int w, GColor color) {
  graphics_context_set_text_color(ctx, color);
  graphics_draw_text(ctx, text, s_font, GRect(x, y, w, LINE_H + 6),
                     GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft, NULL);
}

static void layer_update(Layer *layer, GContext *ctx) {
  GRect b = layer_get_bounds(layer);
  int w = b.size.w;

  char temp_line[40];
  snprintf(temp_line, sizeof(temp_line), "%s%s%s", s_temp, s_cond[0] ? " " : "", s_cond);
  GColor batt_color = s_batt_pct <= 20 ? GColorRed : (s_batt_pct <= 40 ? GColorYellow : GColorGreen);

  // Build the list of rows that are switched on (and available on this watch)
  Row list[8];
  int n = 0;
  if (comp_on(COMP_TIME)) list[n++] = (Row){ "[TIME]", s_time, GColorGreen };
  if (comp_on(COMP_DATE)) list[n++] = (Row){ "[DATE]", s_date, GColorPictonBlue };
  if (comp_on(COMP_TEMP)) list[n++] = (Row){ "[TEMP]", temp_line, GColorOrange };
  if (comp_on(COMP_BATT)) list[n++] = (Row){ "[BATT]", s_batt, batt_color };
  if (s_has_health) {
    if (comp_on(COMP_STEP)) list[n++] = (Row){ "[STEP]", s_steps, GColorLavenderIndigo };
    if (comp_on(COMP_DIST)) list[n++] = (Row){ "[DIST]", s_dist, GColorCyan };
    if (comp_on(COMP_HR) && s_hr_available) list[n++] = (Row){ "[L_HR]", s_hr, GColorSunsetOrange };
  }

  int total = n + 2;   // plus the two prompt lines
  int y = (b.size.h - total * LINE_H) / 2;
  if (y < 0) y = 0;

  char prompt[40];
  snprintf(prompt, sizeof(prompt), "%s:~ $ now", s_prompt);
  draw_text(ctx, prompt, PAD, y, w - PAD, GColorWhite);
  y += LINE_H;

  for (int i = 0; i < n; i++) {
    draw_text(ctx, list[i].label, PAD, y, w - PAD, GColorWhite);
    draw_text(ctx, list[i].value, PAD + s_label_w, y, w - PAD - s_label_w, list[i].color);
    y += LINE_H;
  }

  snprintf(prompt, sizeof(prompt), "%s:~ $ _", s_prompt);
  draw_text(ctx, prompt, PAD, y, w - PAD, GColorWhite);
}

/* ---------- Events ---------- */

static void tick_handler(struct tm *tick_time, TimeUnits units_changed) {
  update_time();
  layer_mark_dirty(s_layer);
}

static void seconds_off_cb(void *data) {
  s_seconds_timer = NULL;
  s_show_seconds = false;
  tick_timer_service_subscribe(MINUTE_UNIT, tick_handler);
  update_time();
  layer_mark_dirty(s_layer);
}

// A wrist flick shows seconds for a few moments. Updating every second all day costs battery.
static void tap_handler(AccelAxisType axis, int32_t direction) {
  if (!s_seconds_enabled || !comp_on(COMP_TIME)) return;
  s_show_seconds = true;
  tick_timer_service_subscribe(SECOND_UNIT, tick_handler);
  update_time();
  layer_mark_dirty(s_layer);
  if (s_seconds_timer) app_timer_cancel(s_seconds_timer);
  s_seconds_timer = app_timer_register(SECONDS_SHOW_MS, seconds_off_cb, NULL);
}

static void battery_handler(BatteryChargeState state) {
  update_battery();
  layer_mark_dirty(s_layer);
}

static void in_received_handler(DictionaryIterator *iter, void *context) {
  Tuple *t = dict_find(iter, KEY_TEMP);
  Tuple *c = dict_find(iter, KEY_COND);
  if (t) {
    snprintf(s_temp, sizeof(s_temp), "%s", t->value->cstring);
    persist_write_string(PERSIST_TEMP, s_temp);
  }
  if (c) {
    snprintf(s_cond, sizeof(s_cond), "%s", c->value->cstring);
    persist_write_string(PERSIST_COND, s_cond);
  }

  // Settings from the phone
  Tuple *mask = dict_find(iter, KEY_MASK);
  if (mask) {
    s_mask = (uint32_t)mask->value->int32;
    persist_write_int(PERSIST_MASK, (int32_t)s_mask);
  }
  Tuple *prompt = dict_find(iter, KEY_PROMPT);
  if (prompt) {
    snprintf(s_prompt, sizeof(s_prompt), "%s", prompt->value->cstring);
    persist_write_string(PERSIST_PROMPT, s_prompt);
  }
  Tuple *miles = dict_find(iter, KEY_DIST_UNIT);
  if (miles) {
    s_miles = miles->value->int32 != 0;
    persist_write_bool(PERSIST_MILES, s_miles);
  }
  Tuple *secs = dict_find(iter, KEY_SECONDS);
  if (secs) {
    s_seconds_enabled = secs->value->int32 != 0;
    persist_write_bool(PERSIST_SECONDS, s_seconds_enabled);
  }

#if defined(PBL_HEALTH)
  if (miles) update_health();   // redo the distance in the new unit
#endif
  layer_mark_dirty(s_layer);
}

/* ---------- Window ---------- */

static void load_settings(void) {
  if (persist_exists(PERSIST_MASK)) s_mask = (uint32_t)persist_read_int(PERSIST_MASK);
  if (persist_exists(PERSIST_PROMPT)) persist_read_string(PERSIST_PROMPT, s_prompt, sizeof(s_prompt));
  if (persist_exists(PERSIST_MILES)) s_miles = persist_read_bool(PERSIST_MILES);
  if (persist_exists(PERSIST_SECONDS)) s_seconds_enabled = persist_read_bool(PERSIST_SECONDS);
  if (persist_exists(PERSIST_TEMP)) persist_read_string(PERSIST_TEMP, s_temp, sizeof(s_temp));
  if (persist_exists(PERSIST_COND)) persist_read_string(PERSIST_COND, s_cond, sizeof(s_cond));
}

static void window_load(Window *window) {
  Layer *root = window_get_root_layer(window);
  s_font = fonts_load_custom_font(resource_get_handle(FONT_RES));

  // All label cells are the same width in a monospace font, so measure one
  s_label_w = graphics_text_layout_get_content_size("[TIME]", s_font, GRect(0, 0, 400, 40),
                                                    GTextOverflowModeWordWrap, GTextAlignmentLeft).w;

  s_layer = layer_create(safe_bounds(layer_get_bounds(root)));
  layer_set_update_proc(s_layer, layer_update);
  layer_add_child(root, s_layer);

  load_settings();
  update_time();
  update_battery();
#if defined(PBL_HEALTH)
  update_health();
#endif
}

static void window_unload(Window *window) {
  layer_destroy(s_layer);
  fonts_unload_custom_font(s_font);
}

static void init(void) {
  s_window = window_create();
  window_set_background_color(s_window, GColorBlack);
  window_set_window_handlers(s_window, (WindowHandlers) {
    .load = window_load,
    .unload = window_unload
  });
  window_stack_push(s_window, true);

  tick_timer_service_subscribe(MINUTE_UNIT, tick_handler);
  battery_state_service_subscribe(battery_handler);
  accel_tap_service_subscribe(tap_handler);
#if defined(PBL_HEALTH)
  health_service_events_subscribe(health_handler, NULL);
#endif

  app_message_register_inbox_received(in_received_handler);
  app_message_open(256, 0);
}

static void deinit(void) {
  if (s_seconds_timer) app_timer_cancel(s_seconds_timer);
  tick_timer_service_unsubscribe();
  battery_state_service_unsubscribe();
  accel_tap_service_unsubscribe();
#if defined(PBL_HEALTH)
  health_service_events_unsubscribe();
#endif
  window_destroy(s_window);
}

int main(void) {
  init();
  app_event_loop();
  deinit();
}
