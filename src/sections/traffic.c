#include "sections/traffic.h"

#include "sections/helpers.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define TRAFFIC_HISTORY 600
#define TRAFFIC_WINDOW_US (300 * G_USEC_PER_SEC)
#define TRAFFIC_GAP_US (6 * G_USEC_PER_SEC)
#define TRAFFIC_MIN_SCALE 10000.0
#define TRAFFIC_DAYS 30
#define TRAFFIC_VNSTAT_REFRESH_US (60 * G_USEC_PER_SEC)

typedef struct {
  gint64 time;
  double rx;
  double tx;
} RateSample;

typedef struct {
  int year;
  int month;
  int day;
  guint64 rx;
  guint64 tx;
} DayUsage;

struct _NetworkSidebarTraffic {
  NMClient *client;
  GtkWidget *group;
  GtkWidget *rate_label;
  GtkWidget *live_area;
  GtkWidget *usage_box;
  GtkWidget *today_label;
  GtkWidget *month_label;
  GtkWidget *days_area;
  char *iface;
  guint64 last_rx;
  guint64 last_tx;
  gint64 last_time;
  gboolean have_last;
  RateSample samples[TRAFFIC_HISTORY];
  guint head;
  guint count;
  DayUsage days[TRAFFIC_DAYS + 1];
  guint n_days;
  gint64 vnstat_time;
  GCancellable *vnstat_cancel;
  guint sample_source;
  gboolean active;
};

typedef struct {
  GPtrArray *stack;
  DayUsage current;
  gboolean in_day;
  GArray *days;
} VnstatParser;

/* Download and upload: categorical slots 1 and 2, stepped per mode. */
static void
series_color(int series, GdkRGBA *color)
{
  gboolean dark = adw_style_manager_get_dark(adw_style_manager_get_default());
  const char *hex = series == 0 ? (dark ? "#3987e5" : "#2a78d6") : (dark ? "#d95926" : "#eb6834");
  gdk_rgba_parse(color, hex);
}

static char *
series_dot(int series)
{
  GdkRGBA color;
  g_autofree char *hex = NULL;
  series_color(series, &color);
  hex = g_strdup_printf("#%02x%02x%02x", (int) (color.red * 255), (int) (color.green * 255), (int) (color.blue * 255));
  return g_strdup_printf("<span foreground=\"%s\">●</span>", hex);
}

static char *
format_bytes(double bytes)
{
  if (bytes < 1000)
    return g_strdup_printf("%.0f B", bytes);
  return g_format_size((guint64) bytes);
}

static char *
format_rate(double bytes_per_second)
{
  g_autofree char *size = format_bytes(bytes_per_second);
  return g_strdup_printf("%s/s", size);
}

static const RateSample *
sample_at(NetworkSidebarTraffic *self, guint index_from_oldest)
{
  guint oldest = (self->head + TRAFFIC_HISTORY - self->count) % TRAFFIC_HISTORY;
  return &self->samples[(oldest + index_from_oldest) % TRAFFIC_HISTORY];
}

static const char *
wifi_iface(NMClient *client)
{
  const GPtrArray *devices = nm_client_get_devices(client);
  for (guint i = 0; devices != NULL && i < devices->len; i++) {
    NMDevice *device = g_ptr_array_index((GPtrArray *) devices, i);
    if (nm_device_get_device_type(device) == NM_DEVICE_TYPE_WIFI)
      return nm_device_get_iface(device);
  }
  return NULL;
}

static gboolean
read_counter(const char *iface, const char *name, guint64 *value)
{
  g_autofree char *path = g_strdup_printf("/sys/class/net/%s/statistics/%s", iface, name);
  g_autofree char *contents = NULL;
  if (!g_file_get_contents(path, &contents, NULL, NULL))
    return FALSE;
  *value = g_ascii_strtoull(contents, NULL, 10);
  return TRUE;
}

static void
update_rate_label(NetworkSidebarTraffic *self)
{
  g_autofree char *down_dot = series_dot(0);
  g_autofree char *up_dot = series_dot(1);
  g_autofree char *down = NULL;
  g_autofree char *up = NULL;
  g_autofree char *markup = NULL;
  double rx = 0, tx = 0;

  if (self->count > 0) {
    const RateSample *last = sample_at(self, self->count - 1);
    rx = last->rx;
    tx = last->tx;
  }
  down = format_rate(rx);
  up = format_rate(tx);
  markup = g_strdup_printf("%s ↓ %s   %s ↑ %s", down_dot, down, up_dot, up);
  gtk_label_set_markup(GTK_LABEL(self->rate_label), markup);
}

static void
take_sample(NetworkSidebarTraffic *self)
{
  const char *iface = wifi_iface(self->client);
  guint64 rx, tx;
  gint64 now = g_get_monotonic_time();

  if (iface == NULL || !read_counter(iface, "rx_bytes", &rx) || !read_counter(iface, "tx_bytes", &tx)) {
    self->have_last = FALSE;
    return;
  }
  if (g_strcmp0(iface, self->iface) != 0) {
    g_free(self->iface);
    self->iface = g_strdup(iface);
    self->have_last = FALSE;
    self->count = 0;
    self->n_days = 0;
    self->vnstat_time = 0;
  }
  /* Counters restart when the interface is recreated; rebase instead of
   * plotting a negative rate. */
  if (self->have_last && rx >= self->last_rx && tx >= self->last_tx && now > self->last_time) {
    double seconds = (now - self->last_time) / (double) G_USEC_PER_SEC;
    RateSample *sample = &self->samples[self->head];
    sample->time = now;
    sample->rx = (rx - self->last_rx) / seconds;
    sample->tx = (tx - self->last_tx) / seconds;
    self->head = (self->head + 1) % TRAFFIC_HISTORY;
    self->count = MIN(self->count + 1, TRAFFIC_HISTORY);
  }
  self->last_rx = rx;
  self->last_tx = tx;
  self->last_time = now;
  self->have_last = TRUE;
}

/* vnstat --xml: <days><day id=".."><date><year/><month/><day/></date><rx/><tx/></day>... */
static const char *
stack_name(VnstatParser *parser, guint from_top)
{
  if (parser->stack->len <= from_top)
    return NULL;
  return g_ptr_array_index(parser->stack, parser->stack->len - 1 - from_top);
}

static void
vnstat_start(GMarkupParseContext *context, const char *name, const char **attribute_names,
             const char **attribute_values, gpointer user_data, GError **error)
{
  VnstatParser *parser = user_data;
  (void) context;
  (void) attribute_names;
  (void) attribute_values;
  (void) error;

  if (g_strcmp0(name, "day") == 0 && g_strcmp0(stack_name(parser, 0), "days") == 0) {
    memset(&parser->current, 0, sizeof parser->current);
    parser->in_day = TRUE;
  }
  g_ptr_array_add(parser->stack, g_strdup(name));
}

static void
vnstat_end(GMarkupParseContext *context, const char *name, gpointer user_data, GError **error)
{
  VnstatParser *parser = user_data;
  (void) context;
  (void) error;

  if (parser->stack->len > 0)
    g_ptr_array_remove_index(parser->stack, parser->stack->len - 1);
  if (parser->in_day && g_strcmp0(name, "day") == 0 && g_strcmp0(stack_name(parser, 0), "days") == 0) {
    g_array_append_val(parser->days, parser->current);
    parser->in_day = FALSE;
  }
}

static void
vnstat_text(GMarkupParseContext *context, const char *text, gsize text_len, gpointer user_data, GError **error)
{
  VnstatParser *parser = user_data;
  const char *element = stack_name(parser, 0);
  const char *parent = stack_name(parser, 1);
  g_autofree char *value = NULL;
  (void) context;
  (void) error;

  if (!parser->in_day || element == NULL || parent == NULL)
    return;
  value = g_strndup(text, text_len);
  if (g_strcmp0(parent, "date") == 0) {
    int number = (int) g_ascii_strtoll(value, NULL, 10);
    if (g_strcmp0(element, "year") == 0) parser->current.year = number;
    else if (g_strcmp0(element, "month") == 0) parser->current.month = number;
    else if (g_strcmp0(element, "day") == 0) parser->current.day = number;
  } else if (g_strcmp0(parent, "day") == 0) {
    if (g_strcmp0(element, "rx") == 0) parser->current.rx = g_ascii_strtoull(value, NULL, 10);
    else if (g_strcmp0(element, "tx") == 0) parser->current.tx = g_ascii_strtoull(value, NULL, 10);
  }
}

static gboolean
parse_vnstat(const char *xml, GArray *days)
{
  static const GMarkupParser callbacks = { vnstat_start, vnstat_end, vnstat_text, NULL, NULL };
  VnstatParser parser = { g_ptr_array_new_with_free_func(g_free), { 0 }, FALSE, days };
  GMarkupParseContext *context = g_markup_parse_context_new(&callbacks, 0, &parser, NULL);
  gboolean ok = g_markup_parse_context_parse(context, xml, -1, NULL) && g_markup_parse_context_end_parse(context, NULL);
  g_markup_parse_context_free(context);
  g_ptr_array_unref(parser.stack);
  return ok;
}

static void
update_usage(NetworkSidebarTraffic *self)
{
  g_autoptr(GDateTime) now = g_date_time_new_now_local();
  g_autofree char *month_name = g_date_time_format(now, "%B");
  guint64 today_rx = 0, today_tx = 0, month_rx = 0, month_tx = 0;
  g_autofree char *today_down = NULL;
  g_autofree char *today_up = NULL;
  g_autofree char *month_down = NULL;
  g_autofree char *month_up = NULL;
  g_autofree char *today = NULL;
  g_autofree char *month = NULL;

  gtk_widget_set_visible(self->usage_box, self->n_days > 0);
  if (self->n_days == 0)
    return;
  for (guint i = 0; i < self->n_days; i++) {
    const DayUsage *day = &self->days[i];
    if (day->year != g_date_time_get_year(now) || day->month != g_date_time_get_month(now))
      continue;
    month_rx += day->rx;
    month_tx += day->tx;
    if (day->day == g_date_time_get_day_of_month(now)) {
      today_rx = day->rx;
      today_tx = day->tx;
    }
  }
  today_down = format_bytes(today_rx);
  today_up = format_bytes(today_tx);
  month_down = format_bytes(month_rx);
  month_up = format_bytes(month_tx);
  today = g_strdup_printf("Today  ↓ %s  ↑ %s", today_down, today_up);
  month = g_strdup_printf("%s  ↓ %s  ↑ %s", month_name, month_down, month_up);
  gtk_label_set_label(GTK_LABEL(self->today_label), today);
  gtk_label_set_label(GTK_LABEL(self->month_label), month);
  gtk_widget_queue_draw(self->days_area);
}

static void
vnstat_finished(GObject *source, GAsyncResult *result, gpointer user_data)
{
  NetworkSidebarTraffic *self = user_data;
  g_autofree char *output = NULL;
  g_autoptr(GError) error = NULL;
  g_autoptr(GArray) days = g_array_new(FALSE, TRUE, sizeof(DayUsage));

  if (!g_subprocess_communicate_utf8_finish(G_SUBPROCESS(source), result, &output, NULL, &error)) {
    if (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
      return;
  }
  g_clear_object(&self->vnstat_cancel);
  self->n_days = 0;
  if (output != NULL && g_subprocess_get_successful(G_SUBPROCESS(source)) && parse_vnstat(output, days)) {
    guint first = days->len > TRAFFIC_DAYS + 1 ? days->len - (TRAFFIC_DAYS + 1) : 0;
    for (guint i = first; i < days->len; i++)
      self->days[self->n_days++] = g_array_index(days, DayUsage, i);
  }
  update_usage(self);
}

static const char *
vnstat_program(void)
{
  static char *program = NULL;
  if (program == NULL) {
    program = g_find_program_in_path("vnstat");
    if (program == NULL && g_file_test("/run/current-system/sw/bin/vnstat", G_FILE_TEST_IS_EXECUTABLE))
      program = g_strdup("/run/current-system/sw/bin/vnstat");
  }
  return program;
}

static void
refresh_vnstat(NetworkSidebarTraffic *self)
{
  const char *program = vnstat_program();
  g_autoptr(GSubprocess) process = NULL;

  if (self->iface == NULL || program == NULL || self->vnstat_cancel != NULL)
    return;
  self->vnstat_time = g_get_monotonic_time();
  process = g_subprocess_new(G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_SILENCE, NULL,
                             program, "-i", self->iface, "--xml", "d", "31", NULL);
  if (process == NULL)
    return;
  self->vnstat_cancel = g_cancellable_new();
  g_subprocess_communicate_utf8_async(process, NULL, self->vnstat_cancel, vnstat_finished, self);
}

static void restart_sampling(NetworkSidebarTraffic *self);

static gboolean
sample_cb(gpointer user_data)
{
  NetworkSidebarTraffic *self = user_data;

  take_sample(self);
  if (self->active) {
    update_rate_label(self);
    gtk_widget_queue_draw(self->live_area);
    if (g_get_monotonic_time() - self->vnstat_time >= TRAFFIC_VNSTAT_REFRESH_US)
      refresh_vnstat(self);
  }
  return G_SOURCE_CONTINUE;
}

static void
restart_sampling(NetworkSidebarTraffic *self)
{
  g_clear_handle_id(&self->sample_source, g_source_remove);
  /* Hidden: second-granularity timers coalesce with other wakeups. */
  self->sample_source = self->active ? g_timeout_add(1000, sample_cb, self)
                                     : g_timeout_add_seconds(2, sample_cb, self);
}

static void
set_source_rgba_alpha(cairo_t *cr, const GdkRGBA *color, double alpha)
{
  cairo_set_source_rgba(cr, color->red, color->green, color->blue, color->alpha * alpha);
}

static void
draw_scale_label(GtkWidget *widget, cairo_t *cr, const char *text)
{
  PangoLayout *layout = gtk_widget_create_pango_layout(widget, text);
  PangoAttrList *attrs = pango_attr_list_new();
  GdkRGBA fg;

  pango_attr_list_insert(attrs, pango_attr_scale_new(0.8));
  pango_layout_set_attributes(layout, attrs);
  gtk_widget_get_color(widget, &fg);
  set_source_rgba_alpha(cr, &fg, 0.55);
  cairo_move_to(cr, 2, 0);
  pango_cairo_show_layout(cr, layout);
  pango_attr_list_unref(attrs);
  g_object_unref(layout);
}

static double
nice_ceiling(double value)
{
  double magnitude = pow(10, floor(log10(value)));
  double steps[] = { 1, 2, 5, 10 };
  for (guint i = 0; i < G_N_ELEMENTS(steps); i++)
    if (value <= steps[i] * magnitude)
      return steps[i] * magnitude;
  return 10 * magnitude;
}

static void
draw_baseline(GtkWidget *widget, cairo_t *cr, int width, int height)
{
  GdkRGBA fg;
  gtk_widget_get_color(widget, &fg);
  set_source_rgba_alpha(cr, &fg, 0.14);
  cairo_rectangle(cr, 0, height - 1, width, 1);
  cairo_fill(cr);
}

static void
trace_series(cairo_t *cr, NetworkSidebarTraffic *self, int series, gint64 now, int width, int height, double scale, gboolean close)
{
  gboolean drawing = FALSE;
  double first_x = 0, last_x = 0;
  gint64 previous = 0;

  for (guint i = 0; i < self->count; i++) {
    const RateSample *sample = sample_at(self, i);
    double x, y;
    if (now - sample->time > TRAFFIC_WINDOW_US) {
      previous = sample->time;
      continue;
    }
    x = width - (now - sample->time) / (double) TRAFFIC_WINDOW_US * width;
    y = height - 1 - (series == 0 ? sample->rx : sample->tx) / scale * (height - 14);
    /* Break the trace across gaps such as suspend. */
    if (drawing && sample->time - previous > TRAFFIC_GAP_US) {
      if (close) {
        cairo_line_to(cr, last_x, height - 1);
        cairo_line_to(cr, first_x, height - 1);
        cairo_close_path(cr);
      }
      drawing = FALSE;
    }
    if (!drawing) {
      first_x = x;
      if (close) {
        cairo_move_to(cr, x, height - 1);
        cairo_line_to(cr, x, y);
      } else {
        cairo_move_to(cr, x, y);
      }
      drawing = TRUE;
    } else {
      cairo_line_to(cr, x, y);
    }
    last_x = x;
    previous = sample->time;
  }
  if (drawing && close) {
    cairo_line_to(cr, last_x, height - 1);
    cairo_line_to(cr, first_x, height - 1);
    cairo_close_path(cr);
  }
}

static void
draw_live(GtkDrawingArea *area, cairo_t *cr, int width, int height, gpointer user_data)
{
  NetworkSidebarTraffic *self = user_data;
  GdkRGBA down, up;
  double peak = 0;
  gint64 now;
  g_autofree char *scale_text = NULL;
  double scale;

  draw_baseline(GTK_WIDGET(area), cr, width, height);
  if (self->count == 0)
    return;
  now = sample_at(self, self->count - 1)->time;
  for (guint i = 0; i < self->count; i++) {
    const RateSample *sample = sample_at(self, i);
    if (now - sample->time <= TRAFFIC_WINDOW_US)
      peak = MAX(peak, MAX(sample->rx, sample->tx));
  }
  scale = nice_ceiling(MAX(peak, TRAFFIC_MIN_SCALE));
  series_color(0, &down);
  series_color(1, &up);

  cairo_set_line_width(cr, 2);
  cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
  trace_series(cr, self, 0, now, width, height, scale, TRUE);
  set_source_rgba_alpha(cr, &down, 0.22);
  cairo_fill(cr);
  trace_series(cr, self, 0, now, width, height, scale, FALSE);
  set_source_rgba_alpha(cr, &down, 1);
  cairo_stroke(cr);
  trace_series(cr, self, 1, now, width, height, scale, FALSE);
  set_source_rgba_alpha(cr, &up, 1);
  cairo_stroke(cr);

  scale_text = format_rate(scale);
  draw_scale_label(GTK_WIDGET(area), cr, scale_text);
}

static void
rounded_top(cairo_t *cr, double x, double y, double width, double height, double radius)
{
  radius = MIN(radius, MIN(width / 2, height));
  cairo_move_to(cr, x, y + height);
  cairo_line_to(cr, x, y + radius);
  cairo_arc(cr, x + radius, y + radius, radius, G_PI, 1.5 * G_PI);
  cairo_arc(cr, x + width - radius, y + radius, radius, 1.5 * G_PI, 2 * G_PI);
  cairo_line_to(cr, x + width, y + height);
  cairo_close_path(cr);
}

/* Day slots, oldest first, ending today; NULL for days vnstat has no row for. */
static const DayUsage *
day_for_slot(NetworkSidebarTraffic *self, GDateTime *today, int slot)
{
  g_autoptr(GDateTime) date = g_date_time_add_days(today, slot - (TRAFFIC_DAYS - 1));
  for (guint i = 0; i < self->n_days; i++) {
    const DayUsage *day = &self->days[i];
    if (day->year == g_date_time_get_year(date) && day->month == g_date_time_get_month(date) &&
        day->day == g_date_time_get_day_of_month(date))
      return day;
  }
  return NULL;
}

static void
draw_days(GtkDrawingArea *area, cairo_t *cr, int width, int height, gpointer user_data)
{
  NetworkSidebarTraffic *self = user_data;
  g_autoptr(GDateTime) today = g_date_time_new_now_local();
  GdkRGBA down, up;
  double peak = 0, scale, slot_width = width / (double) TRAFFIC_DAYS;
  double plot = height - 14;
  g_autofree char *scale_text = NULL;

  draw_baseline(GTK_WIDGET(area), cr, width, height);
  for (int slot = 0; slot < TRAFFIC_DAYS; slot++) {
    const DayUsage *day = day_for_slot(self, today, slot);
    if (day != NULL)
      peak = MAX(peak, (double) (day->rx + day->tx));
  }
  if (peak <= 0)
    return;
  scale = nice_ceiling(peak);
  series_color(0, &down);
  series_color(1, &up);

  for (int slot = 0; slot < TRAFFIC_DAYS; slot++) {
    const DayUsage *day = day_for_slot(self, today, slot);
    double x = slot * slot_width + 1, bar = slot_width - 2;
    double rx_h, tx_h, base = height - 1;
    if (day == NULL)
      continue;
    rx_h = day->rx / scale * plot;
    tx_h = day->tx / scale * plot;
    /* Download sits on the baseline, upload stacks above a 2px gap. */
    if (tx_h >= 1) {
      rounded_top(cr, x, base - rx_h - tx_h - 2, bar, tx_h, 2);
      set_source_rgba_alpha(cr, &up, 1);
      cairo_fill(cr);
    }
    if (rx_h >= 0.5) {
      if (tx_h >= 1) cairo_rectangle(cr, x, base - rx_h, bar, rx_h);
      else rounded_top(cr, x, base - rx_h, bar, rx_h, 2);
      set_source_rgba_alpha(cr, &down, 1);
      cairo_fill(cr);
    }
  }
  scale_text = format_bytes(scale);
  draw_scale_label(GTK_WIDGET(area), cr, scale_text);
}

static gboolean
live_tooltip(GtkWidget *widget, int x, int y, gboolean keyboard, GtkTooltip *tooltip, gpointer user_data)
{
  NetworkSidebarTraffic *self = user_data;
  int width = gtk_widget_get_width(widget);
  const RateSample *best = NULL;
  gint64 now, target;
  g_autofree char *down = NULL;
  g_autofree char *up = NULL;
  g_autofree char *text = NULL;
  (void) y;
  (void) keyboard;

  if (self->count == 0 || width <= 0)
    return FALSE;
  now = sample_at(self, self->count - 1)->time;
  target = now - (gint64) ((width - x) / (double) width * TRAFFIC_WINDOW_US);
  for (guint i = 0; i < self->count; i++) {
    const RateSample *sample = sample_at(self, i);
    if (best == NULL || llabs(sample->time - target) < llabs(best->time - target))
      best = sample;
  }
  if (best == NULL || llabs(best->time - target) > TRAFFIC_GAP_US)
    return FALSE;
  down = format_rate(best->rx);
  up = format_rate(best->tx);
  text = g_strdup_printf("%" G_GINT64_FORMAT " s ago\n↓ %s   ↑ %s", (now - best->time) / G_USEC_PER_SEC, down, up);
  gtk_tooltip_set_text(tooltip, text);
  return TRUE;
}

static gboolean
days_tooltip(GtkWidget *widget, int x, int y, gboolean keyboard, GtkTooltip *tooltip, gpointer user_data)
{
  NetworkSidebarTraffic *self = user_data;
  int width = gtk_widget_get_width(widget);
  g_autoptr(GDateTime) today = g_date_time_new_now_local();
  g_autoptr(GDateTime) date = NULL;
  const DayUsage *day;
  int slot;
  g_autofree char *label = NULL;
  g_autofree char *down = NULL;
  g_autofree char *up = NULL;
  g_autofree char *text = NULL;
  (void) y;
  (void) keyboard;

  if (width <= 0)
    return FALSE;
  slot = CLAMP((int) (x / (width / (double) TRAFFIC_DAYS)), 0, TRAFFIC_DAYS - 1);
  day = day_for_slot(self, today, slot);
  if (day == NULL)
    return FALSE;
  date = g_date_time_add_days(today, slot - (TRAFFIC_DAYS - 1));
  label = slot == TRAFFIC_DAYS - 1 ? g_strdup("Today") : g_date_time_format(date, "%a %b %-d");
  down = format_bytes(day->rx);
  up = format_bytes(day->tx);
  text = g_strdup_printf("%s\n↓ %s   ↑ %s", label, down, up);
  gtk_tooltip_set_text(tooltip, text);
  return TRUE;
}

static GtkWidget *
caption(const char *text, float xalign)
{
  GtkWidget *label = gtk_label_new(text);
  gtk_widget_add_css_class(label, "caption");
  gtk_widget_add_css_class(label, "dim-label");
  gtk_label_set_xalign(GTK_LABEL(label), xalign);
  gtk_widget_set_hexpand(label, TRUE);
  return label;
}

static GtkWidget *
chart(int height, GtkDrawingAreaDrawFunc draw, GCallback tooltip, NetworkSidebarTraffic *self)
{
  GtkWidget *area = gtk_drawing_area_new();
  gtk_drawing_area_set_content_height(GTK_DRAWING_AREA(area), height);
  gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(area), draw, self, NULL);
  gtk_widget_set_has_tooltip(area, TRUE);
  g_signal_connect(area, "query-tooltip", tooltip, self);
  return area;
}

static void
on_dark_changed(GObject *object, GParamSpec *pspec, gpointer user_data)
{
  NetworkSidebarTraffic *self = user_data;
  (void) object;
  (void) pspec;
  update_rate_label(self);
  gtk_widget_queue_draw(self->live_area);
  gtk_widget_queue_draw(self->days_area);
}

NetworkSidebarTraffic *
network_sidebar_traffic_new(NMClient *client)
{
  NetworkSidebarTraffic *self = g_new0(NetworkSidebarTraffic, 1);
  GtkWidget *card, *box, *captions, *usage_text;

  self->client = g_object_ref(client);
  self->group = network_sidebar_section_group("Traffic");
  g_object_ref_sink(self->group);

  self->rate_label = gtk_label_new(NULL);
  gtk_widget_add_css_class(self->rate_label, "numeric");
  gtk_widget_set_valign(self->rate_label, GTK_ALIGN_CENTER);
  adw_preferences_group_set_header_suffix(ADW_PREFERENCES_GROUP(self->group), self->rate_label);

  card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_add_css_class(card, "card");
  gtk_widget_add_css_class(card, "traffic-card");
  box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
  gtk_box_append(GTK_BOX(card), box);

  self->live_area = chart(64, draw_live, G_CALLBACK(live_tooltip), self);
  gtk_box_append(GTK_BOX(box), self->live_area);
  captions = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_box_append(GTK_BOX(captions), caption("5 min ago", 0));
  gtk_box_append(GTK_BOX(captions), caption("now", 1));
  gtk_box_append(GTK_BOX(box), captions);

  self->usage_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
  gtk_widget_add_css_class(self->usage_box, "traffic-usage");
  usage_text = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
  self->today_label = gtk_label_new(NULL);
  self->month_label = gtk_label_new(NULL);
  gtk_widget_add_css_class(self->today_label, "numeric");
  gtk_widget_add_css_class(self->month_label, "numeric");
  gtk_label_set_xalign(GTK_LABEL(self->today_label), 0);
  gtk_label_set_xalign(GTK_LABEL(self->month_label), 1);
  gtk_widget_set_hexpand(self->today_label, TRUE);
  gtk_widget_set_hexpand(self->month_label, TRUE);
  gtk_box_append(GTK_BOX(usage_text), self->today_label);
  gtk_box_append(GTK_BOX(usage_text), self->month_label);
  gtk_box_append(GTK_BOX(self->usage_box), usage_text);
  self->days_area = chart(48, draw_days, G_CALLBACK(days_tooltip), self);
  gtk_box_append(GTK_BOX(self->usage_box), self->days_area);
  captions = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_box_append(GTK_BOX(captions), caption("30 days ago", 0));
  gtk_box_append(GTK_BOX(captions), caption("today", 1));
  gtk_box_append(GTK_BOX(self->usage_box), captions);
  gtk_widget_set_visible(self->usage_box, FALSE);
  gtk_box_append(GTK_BOX(box), self->usage_box);

  adw_preferences_group_add(ADW_PREFERENCES_GROUP(self->group), card);
  update_rate_label(self);
  g_signal_connect_object(adw_style_manager_get_default(), "notify::dark", G_CALLBACK(on_dark_changed),
                          self->group, G_CONNECT_SWAPPED);

  take_sample(self);
  restart_sampling(self);
  return self;
}

void
network_sidebar_traffic_free(NetworkSidebarTraffic *self)
{
  if (self == NULL)
    return;
  g_clear_handle_id(&self->sample_source, g_source_remove);
  if (self->vnstat_cancel != NULL)
    g_cancellable_cancel(self->vnstat_cancel);
  g_clear_object(&self->vnstat_cancel);
  g_clear_object(&self->group);
  g_clear_object(&self->client);
  g_free(self->iface);
  g_free(self);
}

void
network_sidebar_traffic_attach(NetworkSidebarTraffic *self, GtkBox *content)
{
  if (self == NULL || wifi_iface(self->client) == NULL)
    return;
  GtkWidget *parent = gtk_widget_get_parent(self->group);
  if (parent != NULL && GTK_IS_BOX(parent))
    gtk_box_remove(GTK_BOX(parent), self->group);
  gtk_box_append(content, self->group);
}

void
network_sidebar_traffic_set_active(NetworkSidebarTraffic *self, gboolean active)
{
  if (self == NULL || self->active == active)
    return;
  self->active = active;
  restart_sampling(self);
  if (active) {
    update_rate_label(self);
    gtk_widget_queue_draw(self->live_area);
    refresh_vnstat(self);
  }
}
