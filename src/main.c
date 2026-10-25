#include <errno.h>
#include <gtk/gtk.h>
#include <libintl.h>
#include <locale.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "stats.h"

#define GETTEXT_PACKAGE "taskmanagerok"
#define _(s) gettext(s)
#define N_(s) s

#ifndef LOCALEDIR
#define LOCALEDIR "/usr/local/share/locale"
#endif

static char radix_char = '.';

static void apply_radix(char *s) {
  if (radix_char == '.')
    return;
  for (; *s; s++)
    if (*s == '.')
      *s = radix_char;
}

#define FMT(dst, size, ...)           \
  do {                                \
    snprintf(dst, size, __VA_ARGS__); \
    apply_radix(dst);                 \
  } while (0)

/* Translations are looked up next to the binary first so that a checkout and
   an AppImage work without installing anything; the compiled-in path covers
   `make install`. */
static void init_i18n(void) {
  setlocale(LC_ALL, "");

  const char *rel[3] = {"locale", "share/locale", "../share/locale"};
  int found = 0;
  gchar *exe = g_file_read_link("/proc/self/exe", NULL);
  if (exe) {
    gchar *bindir = g_path_get_dirname(exe);
    for (unsigned i = 0; i < 3 && !found; i++) {
      gchar *dir = g_build_filename(bindir, rel[i], NULL);
      gchar *probe = g_build_filename(dir, "uk", "LC_MESSAGES", GETTEXT_PACKAGE ".mo", NULL);
      if (g_file_test(probe, G_FILE_TEST_EXISTS)) {
        bindtextdomain(GETTEXT_PACKAGE, dir);
        found = 1;
      }
      g_free(probe);
      g_free(dir);
    }
    g_free(bindir);
    g_free(exe);
  }
  if (!found)
    bindtextdomain(GETTEXT_PACKAGE, LOCALEDIR);
  bind_textdomain_codeset(GETTEXT_PACKAGE, "UTF-8");
  textdomain(GETTEXT_PACKAGE);

  const char *cr = localeconv()->decimal_point;
  if (cr && cr[0])
    radix_char = (char)cr[0];
}

#define NDEV 6
#define DEV_CPU 0
#define DEV_MEM 1
#define DEV_DISK 2
#define DEV_NET 3
#define DEV_GPU 4
#define DEV_TEMP 5

#define HIST 3600
#define WINLEN 60
#define NST 4

#define RAIL_Y0 26.0
#define RAIL_PITCH 38.0
#define RAIL_ICONS 5

/* The process list is sampled every fourth tick. Reading 205 processes costs
   5.5 ms of CPU, but painting the rows that changed costs four times that, so
   the cadence is what the redraw is worth rather than what the collector needs:
   measured on the development machine, a sweep every 3 s holds the process tab
   at 1.05 % of a core, every 4 s at 0.90 % and every 5 s at 0.76 %, against the
   0.31 % the Performance tab costs. Four seconds buys the headroom to stay under
   one per cent. The Performance graph keeps its 1 s step either way. */
#define PROC_EVERY 4

/* The rail icon index is also the tab id: the first icon opens the process list,
   the second is Performance, the third Startup, the fourth Details and the fifth
   the machine information. Only the two process tabs have work that repeats; the
   autostart list is a directory scan and the information is a set of files read
   once, so both are re-collected when the tab opens (see set_tab). */
enum { TAB_PROC, TAB_PERF, TAB_STARTUP, TAB_DETAILS, TAB_INFO };

enum { U_PCT, U_RATE, U_TEMP };

static SysStats st;

static double hist[NDEV][HIST];
static int hn[NDEV];
static double chist[MAXCORE][HIST];
static int chn;

static const char *dev_name[NDEV] = {N_("CPU"), N_("Memory"), N_("Disk"), N_("Network"),
                                     N_("GPU"), N_("Temperature")};
static const char *dev_key[NDEV] = {"cpu", "mem", "disk", "net", "gpu", "temp"};
static const int dev_unit[NDEV] = {U_PCT, U_PCT, U_RATE, U_RATE, U_PCT, U_TEMP};

static int nvisible;
static int row_to_dev[NDEV];

static GtkWidget *window, *dev_list, *dev_sep, *graph_area, *cores_area, *page_title, *page_sub, *note_label;
static GtkWidget *rail, *head_sub, *perf_card, *proc_card, *proc_tree, *proc_scroll, *proc_note;
static GtkListStore *proc_store;
static GtkWidget *sens_scroll, *sens_tree;
static GtkListStore *sens_store;
static ProcTable pt;
static GtkWidget *start_card, *start_tree, *start_scroll, *start_note, *start_btn;
static GtkListStore *start_store;
static GtkTreeSelection *start_sel;
static StartupTable startup;
static int tab = -1;
static int proc_countdown;
static GtkWidget *stat_value[NST], *stat_label[NST], *stat_sep[NST];
static int sel = DEV_CPU;
static double hover_px = -1;

/* The header under the window title says which tab is open. Indexing this by the
   tab id is what the four-deep ternary in set_tab had become. */
static const char *tab_head[5] = {N_("Processes"), N_("Performance"), N_("Startup"), N_("Details"),
                                  N_("Information")};

static const double accent[3] = {0.30, 0.76, 1.00};

/* Which of the readings the headline and the curve show. The collector reports
   the value; the name is looked up here, and the first of two tied sensors is as
   good an answer as any. */
static int hottest_sensor(void) {
  int best = -1;
  for (int i = 0; i < st.nsens; i++)
    if (best < 0 || st.sens[i].mc > st.sens[best].mc)
      best = i;
  return best;
}

/* A sensor is called what its driver calls it. Most name their inputs ("Core 0",
   "Composite"); a driver that states nothing is named by its chip, which is the
   honest answer rather than an invented one. */
static void sensor_name(int i, char *out, size_t n) {
  const char *l = st.sens[i].label;
  snprintf(out, n, "%s", l[0] ? l : st.sens[i].chip);
}

static void push_sample(int d, double v) {
  hist[d][hn[d] % HIST] = v;
  hn[d]++;
}

static double series_value(int d, int i, int total) {
  long j = (long)hn[d] - (total - 1 - i);
  if (j < 0 || j >= hn[d])
    return -1.0;
  return hist[d][j % HIST];
}

static double core_value(int c, int i, int total) {
  long j = (long)chn - (total - 1 - i);
  if (j < 0 || j >= chn)
    return -1.0;
  return chist[c][j % HIST];
}

static double nice_ceil(double x) {
  if (x <= 0)
    return 1;
  double p = pow(10, floor(log10(x)));
  double m = x / p;
  const double steps[4] = {1, 2, 5, 10};
  for (int i = 0; i < 4; i++)
    if (m <= steps[i] + 1e-9)
      return steps[i] * p;
  return 10 * p;
}

static void temp_range(double *lo, double *hi);

static double axis_max(int d) {
  if (dev_unit[d] != U_RATE) {
    if (dev_unit[d] == U_PCT)
      return 100.0;
    double hi;
    temp_range(NULL, &hi);
    return hi;
  }
  double m = 0;
  for (int i = 0; i < WINLEN; i++) {
    double v = series_value(d, i, WINLEN);
    if (v > m)
      m = v;
  }
  double s = nice_ceil(m * 1.25);
  if (d == DEV_NET && st.net_link_mbps > 0) {
    double cap = st.net_link_mbps / 8.0;
    if (s > cap)
      s = cap;
  }
  return s < 0.5 ? 0.5 : s;
}

/* Every series but temperature starts at zero; a percentage and a transfer rate
   have no other honest floor. */
static double axis_min(int d) {
  if (dev_unit[d] != U_TEMP)
    return 0;
  double lo;
  temp_range(&lo, NULL);
  return lo;
}

/* The temperature plot cannot start at zero the way a percentage does: a core
   that idles at 60 and throttles at 105 would spend the whole hour pinned to one
   edge of the card and show nothing. The window follows the readings, but the
   floor is quantised to ten degrees so the axis does not slide under the curve
   every time the room warms up by one. The limit line is drawn when it falls
   inside the window and left out when it does not, because a scale stretched far
   enough to reach 105 is what flattens the curve a person came here to read; the
   number itself is in the row under the table either way. */
static void temp_range(double *lo, double *hi) {
  double mn = 1e9, mx = -1e9;
  for (int i = 0; i < WINLEN; i++) {
    double v = series_value(DEV_TEMP, i, WINLEN);
    if (v < 0)
      continue;
    if (v < mn)
      mn = v;
    if (v > mx)
      mx = v;
  }
  if (mx < mn) { /* nothing sampled yet: show the range a laptop lives in */
    mn = 30;
    mx = 80;
  }
  double a = floor((mn - 5) / 10) * 10;
  if (a < 0)
    a = 0;
  double b = ceil((mx + 5) / 5) * 5;
  if (b - a < 15)
    b = a + 15;
  /* The middle gridline is labelled, so an odd span would print a whole number
     at a half-degree height. */
  if (((int)(b - a)) % 2)
    b += 1;
  if (lo)
    *lo = a;
  if (hi)
    *hi = b;
}

static void fmt(int unit, double v, char *out, size_t n) {
  if (unit == U_PCT)
    snprintf(out, n, "%.0f%%", v);
  else if (unit == U_TEMP)
    FMT(out, n, _("%.0f °C"), v);
  else if (v >= 1.0)
    FMT(out, n, _("%.1f MB/s"), v);
  else
    FMT(out, n, _("%.0f KB/s"), v * 1024.0);
}

static void fmt_unit_label(int unit, double v, char *out, size_t n) {
  if (unit == U_PCT)
    snprintf(out, n, "%.0f%%", v);
  else if (unit == U_TEMP)
    snprintf(out, n, "%.0f°", v);
  else
    FMT(out, n, _("%.2f MB/s"), v);
}

/* ---------- layout helpers ---------- */

typedef struct {
  double x, y, w, h;
} Rect;

static Rect plot_rect(int widget_w, int widget_h, gboolean with_axes) {
  Rect r;
  r.x = with_axes ? 80 : 2;
  r.y = 8;
  r.w = widget_w - r.x - (with_axes ? 10 : 2);
  r.h = widget_h - r.y - 18;
  if (r.w < 1)
    r.w = 1;
  if (r.h < 1)
    r.h = 1;
  return r;
}

static void draw_series(cairo_t *cr, Rect r, const double *vals, int total, int amax_unit,
                        double amin, double amax, int hover_index, gboolean with_labels) {
  double span = amax - amin;
  if (span < 1)
    span = 1;

  if (with_labels) {
    cairo_set_line_width(cr, 1.0);
    cairo_set_source_rgb(cr, 0.23, 0.23, 0.23);
    for (int i = 0; i <= 4; i++) {
      double y = floor(r.y + r.h * i / 4.0) + 0.5;
      cairo_move_to(cr, r.x, y);
      cairo_line_to(cr, r.x + r.w, y);
      cairo_stroke(cr);
    }

    cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, 10);
    cairo_set_source_rgb(cr, 0.60, 0.60, 0.60);
    for (int i = 0; i <= 4; i += 2) {
      char b[32];
      fmt_unit_label(amax_unit, amax - span * i / 4.0, b, sizeof b);
      cairo_text_extents_t te;
      cairo_text_extents(cr, b, &te);
      cairo_move_to(cr, r.x - 8 - te.width, r.y + r.h * i / 4.0 + te.height / 3.0);
      cairo_show_text(cr, b);
    }
  }

  int first = -1, last = -1;
  for (int i = 0; i < total; i++) {
    if (vals[i] >= 0) {
      if (first < 0)
        first = i;
      last = i;
    }
  }
  if (first < 0)
    return;

  double px[WINLEN], py[WINLEN];
  int npts = 0;
  for (int i = first; i <= last; i++) {
    if (vals[i] < 0)
      continue;
    double v = vals[i] > amax ? amax : vals[i];
    if (v < amin)
      v = amin;
    px[npts] = r.x + r.w * i / (total - 1.0);
    py[npts] = r.y + r.h * (1.0 - (v - amin) / span);
    npts++;
  }
  if (npts < 2)
    return;

  /* The limit line goes under the fill rather than over it: it is the thing the
     curve is measured against, and it should read as part of the scale. */
  double lim_y = 0;
  gboolean draw_lim = with_labels && st.temp_limit_mc > 0 && sel == DEV_TEMP;
  if (draw_lim) {
    double lv = st.temp_limit_mc / 1000.0;
    if (lv <= amax && lv >= amin) {
      lim_y = floor(r.y + r.h * (1.0 - (lv - amin) / span)) + 0.5;
      const double dash[2] = {4.0, 4.0};
      cairo_set_dash(cr, dash, 2, 0);
      cairo_set_line_width(cr, 1.0);
      cairo_set_source_rgb(cr, 0.78, 0.36, 0.32);
      cairo_move_to(cr, r.x, lim_y);
      cairo_line_to(cr, r.x + r.w, lim_y);
      cairo_stroke(cr);
      cairo_set_dash(cr, NULL, 0, 0);
    } else {
      draw_lim = FALSE;
    }
  }

  cairo_new_path(cr);
  cairo_move_to(cr, px[0], r.y + r.h);
  for (int i = 0; i < npts; i++)
    cairo_line_to(cr, px[i], py[i]);
  cairo_line_to(cr, px[npts - 1], r.y + r.h);
  cairo_close_path(cr);
  cairo_pattern_t *pat = cairo_pattern_create_linear(0, r.y, 0, r.y + r.h);
  cairo_pattern_add_color_stop_rgba(pat, 0.0, accent[0], accent[1], accent[2], 0.30);
  cairo_pattern_add_color_stop_rgba(pat, 1.0, accent[0], accent[1], accent[2], 0.04);
  cairo_set_source(cr, pat);
  cairo_fill(cr);
  cairo_pattern_destroy(pat);

  cairo_new_path(cr);
  cairo_move_to(cr, px[0], py[0]);
  for (int i = 1; i < npts; i++)
    cairo_line_to(cr, px[i], py[i]);
  cairo_set_line_width(cr, 1.6);
  cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
  cairo_set_source_rgb(cr, accent[0], accent[1], accent[2]);
  cairo_stroke(cr);

  if (draw_lim) {
    char lb[24];
    fmt_unit_label(amax_unit, st.temp_limit_mc / 1000.0, lb, sizeof lb);
    cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, 10);
    cairo_text_extents_t te;
    cairo_text_extents(cr, lb, &te);
    cairo_set_source_rgb(cr, 0.78, 0.36, 0.32);
    /* The baseline: a limit at the very top of the window would otherwise have
       its own label cut off by the card. */
    cairo_move_to(cr, r.x + r.w - te.width - 2, lim_y - 4 < r.y + 10 ? lim_y + 11 : lim_y - 4);
    cairo_show_text(cr, lb);
  }

  if (hover_index >= first && hover_index <= last) {
    double v = vals[hover_index];
    if (v >= 0) {
      if (v > amax)
        v = amax;
      if (v < amin)
        v = amin;
      double hx = r.x + r.w * hover_index / (total - 1.0);
      double hy = r.y + r.h * (1.0 - (v - amin) / span);
      cairo_set_line_width(cr, 1.0);
      cairo_set_source_rgba(cr, 1, 1, 1, 0.25);
      cairo_move_to(cr, floor(hx) + 0.5, r.y);
      cairo_line_to(cr, floor(hx) + 0.5, r.y + r.h);
      cairo_stroke(cr);
      cairo_set_source_rgb(cr, accent[0], accent[1], accent[2]);
      cairo_arc(cr, hx, hy, 3.0, 0, 2 * M_PI);
      cairo_fill(cr);

      char b[48];
      fmt(amax_unit, v, b, sizeof b);
      char age[24];
      FMT(age, sizeof age, _("  -%d s"), total - 1 - hover_index);
      cairo_set_font_size(cr, 11);
      cairo_text_extents_t te;
      cairo_text_extents(cr, b, &te);
      cairo_text_extents_t te2;
      cairo_text_extents(cr, age, &te2);
      double bw = te.width + te2.width + 14;
      double bx = hx + 10;
      if (bx + bw > r.x + r.w)
        bx = hx - 10 - bw;
      cairo_set_source_rgba(cr, 0.10, 0.10, 0.10, 0.93);
      cairo_rectangle(cr, bx, r.y + 2, bw, 20);
      cairo_fill(cr);
      cairo_set_source_rgb(cr, 0.95, 0.95, 0.95);
      cairo_move_to(cr, bx + 7, r.y + 16);
      cairo_show_text(cr, b);
      cairo_set_source_rgb(cr, 0.62, 0.62, 0.62);
      cairo_show_text(cr, age);
    }
  }
}

static double window_buf[WINLEN];
static double core_buf[WINLEN];

static gboolean on_draw_graph(GtkWidget *w, cairo_t *cr, gpointer data) {
  (void)data;
  GtkAllocation a;
  gtk_widget_get_allocation(w, &a);
  cairo_set_source_rgb(cr, 0.145, 0.145, 0.145);
  cairo_paint(cr);

  for (int i = 0; i < WINLEN; i++)
    window_buf[i] = series_value(sel, i, WINLEN);

  Rect r = plot_rect(a.width, a.height, TRUE);
  int hi = -1;
  if (hover_px >= r.x - 3 && hover_px <= r.x + r.w + 3)
    hi = (int)lround((hover_px - r.x) / r.w * (WINLEN - 1));
  if (hi < 0 || hi >= WINLEN)
    hi = -1;

  double amax = axis_max(sel);
  draw_series(cr, r, window_buf, WINLEN, dev_unit[sel], axis_min(sel), amax, hi, TRUE);

  cairo_set_font_size(cr, 10);
  cairo_set_source_rgb(cr, 0.52, 0.52, 0.52);
  cairo_move_to(cr, r.x, a.height - 4);
  cairo_show_text(cr, _("-60 s"));
  cairo_text_extents_t te;
  cairo_text_extents(cr, _("now"), &te);
  cairo_move_to(cr, r.x + r.w - te.width, a.height - 4);
  cairo_show_text(cr, _("now"));
  return FALSE;
}

static gboolean on_draw_cores(GtkWidget *w, cairo_t *cr, gpointer data) {
  (void)data;
  GtkAllocation a;
  gtk_widget_get_allocation(w, &a);
  cairo_set_source_rgb(cr, 0.145, 0.145, 0.145);
  cairo_paint(cr);
  if (st.ncore <= 0)
    return FALSE;

  int cols = st.ncore <= 4 ? st.ncore : (st.ncore <= 16 ? 4 : 8);
  int rows = (st.ncore + cols - 1) / cols;
  double cw = (double)a.width / cols, ch = (double)a.height / rows;
  cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
  cairo_set_font_size(cr, 9);

  for (int i = 0; i < st.ncore; i++) {
    double ox = (i % cols) * cw, oy = (i / cols) * ch;
    cairo_set_source_rgb(cr, 0.22, 0.22, 0.22);
    cairo_rectangle(cr, ox + 2.5, oy + 2.5, cw - 5, ch - 5);
    cairo_stroke(cr);

    for (int k = 0; k < WINLEN; k++)
      core_buf[k] = core_value(i, k, WINLEN);
    Rect r = {ox + 16, oy + 8, cw - 22, ch - 16};
    if (r.w > 4 && r.h > 4)
      draw_series(cr, r, core_buf, WINLEN, U_PCT, 0, 100.0, -1, FALSE);

    char lab[16];
    snprintf(lab, sizeof lab, "%d", i);
    cairo_set_source_rgb(cr, 0.60, 0.60, 0.60);
    cairo_move_to(cr, ox + 6, oy + ch / 2.0 + 3);
    cairo_show_text(cr, lab);
  }
  return FALSE;
}

static gboolean on_motion(GtkWidget *w, GdkEventMotion *e, gpointer data) {
  (void)data;
  GtkAllocation a;
  gtk_widget_get_allocation(w, &a);
  double px = e->x < 0 ? -1 : (e->x > a.width ? -1 : e->x);
  if (px != hover_px) {
    hover_px = px;
    gtk_widget_queue_draw(w);
  }
  return TRUE;
}

static gboolean on_leave(GtkWidget *w, GdkEventCrossing *e, gpointer data) {
  (void)e;
  (void)data;
  hover_px = -1;
  gtk_widget_queue_draw(w);
  return TRUE;
}

/* ---------- process list ---------- */

static void update_panel(void);
static void set_stats(int n, const char *const *vals, const char *const *labs);

/* Each column shows a formatted string and keeps the raw number beside it, so
   sorting works on values rather than on text like "1,5 ГБ". */
enum { C_NAME, C_PID, C_CPU, C_MEM, C_DISK, C_CPU_TXT, C_MEM_TXT, C_DISK_TXT, NCOL };

static void reload_startup(void);

static void fmt_cpu(double pct, char *out, size_t n) {
  if (pct < 0)
    snprintf(out, n, "%s", "—");
  else
    FMT(out, n, _("%.1f%%"), pct);
}

static void fmt_mem(double mb, char *out, size_t n) {
  if (mb >= 1024.0)
    FMT(out, n, _("%.2f GiB"), mb / 1024.0);
  else
    FMT(out, n, _("%.0f MiB"), mb);
}

static void fmt_disk(double mbps, char *out, size_t n) {
  if (mbps < 0)
    snprintf(out, n, "%s", "—");
  else if (mbps >= 1.0)
    FMT(out, n, _("%.1f MB/s"), mbps);
  else
    FMT(out, n, _("%.0f KB/s"), mbps * 1024.0);
}

/* The column header is a real GTK sortable column: clicking it makes GTK flip
   the store's own (column, order) pair, and this function reads that pair back,
   because GtkTreeIterCompareFunc is never told which column asked for it.

   The comparison is written big-first on purpose. GTK draws a descending sort
   with the arrow pointing up and an ascending one pointing down, which is the
   other way round from Windows, so "ascending" here already means the order a
   Windows user sees under a downward arrow: the hungriest process on top.

   The one rule GTK cannot know about is the "—" cell: a process whose counters
   have no previous sample yet is not a zero, so it belongs at the bottom
   whichever way the list is sorted, which is why the two checks below answer in
   the direction that survives GTK reversing the result. */
static gint proc_sort(GtkTreeModel *m, GtkTreeIter *a, GtkTreeIter *b, gpointer data) {
  (void)data;
  gint col = C_CPU;
  GtkSortType order = GTK_SORT_ASCENDING;
  gtk_tree_sortable_get_sort_column_id(GTK_TREE_SORTABLE(m), &col, &order);
  gboolean desc = (order == GTK_SORT_DESCENDING);

  if (col == C_NAME) {
    char *x, *y;
    gtk_tree_model_get(m, a, C_NAME, &x, -1);
    gtk_tree_model_get(m, b, C_NAME, &y, -1);
    int r = g_ascii_strcasecmp(y, x); /* Z to A, so GTK's reversal gives A to Z */
    if (r == 0)
      r = strcmp(y, x); /* otherwise two rows that differ only in case trade places */
    g_free(x);
    g_free(y);
    return r;
  }

  double va = -1, vb = -1;
  gtk_tree_model_get(m, a, col, &va, -1);
  gtk_tree_model_get(m, b, col, &vb, -1);
  if (va < 0 && vb >= 0)
    return desc ? -1 : 1;
  if (vb < 0 && va >= 0)
    return desc ? 1 : -1;
  if (va == vb) {
    int pa = 0, pb = 0;
    gtk_tree_model_get(m, a, C_PID, &pa, -1);
    gtk_tree_model_get(m, b, C_PID, &pb, -1);
    return pa - pb; /* a stable tie keeps rows from trading places every tick */
  }
  return va > vb ? -1 : 1;
}

/* A click on a column that is not the sorted one yet starts at ascending, which
   under proc_sort's big-first comparator means Z to A for names — the opposite
   of the first click on Name in Windows. The per-column default that would
   express this does not survive GTK syncing its own indicators, so the model is
   nudged here instead, once per column change. */
static gint g_last_sort_col = C_CPU;

static void on_sort_changed(GtkTreeSortable *s, gpointer data) {
  (void)data;
  gint col = C_CPU;
  GtkSortType order = GTK_SORT_ASCENDING;
  gtk_tree_sortable_get_sort_column_id(s, &col, &order);
  if (col < 0 || col == g_last_sort_col)
    return; /* a flip of the current column is the user's own doing: leave it */
  g_last_sort_col = col;
  gtk_tree_sortable_set_sort_column_id(s, col, col == C_NAME ? GTK_SORT_DESCENDING : GTK_SORT_ASCENDING);
}

/* A row is not a value: GTK repaints and relayouts whatever it is told changed,
   so the list is updated in place and a cell is only touched when its text
   really differs from the last sweep. Rebuilding from scratch measured 100 ms of
   CPU per sweep for 205 processes on this machine; keeping the rows and writing
   the handful that changed costs about 8 ms.

   slot[] mirrors the store, one entry per row, and remembers what is displayed.
   The iters themselves cannot be remembered: a re-sort moves rows, so each slot
   holds a row reference instead, which GTK keeps pointed at the same process
   through both inserts and removals. */
typedef struct {
  int pid;
  unsigned gen;             /* last sweep that still saw this process alive */
  GtkTreeRowReference *ref; /* NULL if the row could not be referenced */
  char name[32], cpu[32], mem[32], disk[32];
} ProcSlot;

static ProcSlot slot[MAXPROC];
static int nslot;
static unsigned sweep_gen;

static void slot_texts(ProcSlot *sl, const ProcRow *r, const char *cpu, const char *mem,
                       const char *disk) {
  g_strlcpy(sl->name, r->name, sizeof sl->name);
  g_strlcpy(sl->cpu, cpu, sizeof sl->cpu);
  g_strlcpy(sl->mem, mem, sizeof sl->mem);
  g_strlcpy(sl->disk, disk, sizeof sl->disk);
}

/* Sort state is GTK's, so the sync only has to remember it and put it back:
   leaving the pair untouched would skip the re-sort entirely. */
static void sync_proc_rows(void) {
  GtkTreeSortable *s = GTK_TREE_SORTABLE(proc_store);
  GtkTreeModel *m = GTK_TREE_MODEL(proc_store);
  gint keep_col = C_CPU;
  GtkSortType keep_order = GTK_SORT_ASCENDING;
  gtk_tree_sortable_get_sort_column_id(s, &keep_col, &keep_order);
  if (keep_col < 0)
    keep_col = C_CPU; /* unsorted or the default comparator: fall back to CPU */

  /* Writing a cell of a sorted store makes GTK reposition that one row, once per
     row that changed, so sorting is switched off for the update and done once at
     the end. */
  gtk_tree_sortable_set_sort_column_id(s, GTK_TREE_SORTABLE_UNSORTED_SORT_COLUMN_ID,
                                       GTK_SORT_ASCENDING);

  /* A list that jumps to the top every few seconds is unusable, so the reader's
     place in it is kept across the update. */
  GtkAdjustment *adj = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(proc_scroll));
  double keep = adj ? gtk_adjustment_get_value(adj) : 0;

  ++sweep_gen;
  for (int i = 0; i < pt.n; i++) {
    ProcRow *r = &pt.rows[i];
    char cpu[32], mem[32], disk[32];
    fmt_cpu(r->cpu_pct, cpu, sizeof cpu);
    fmt_mem(r->mem_mb, mem, sizeof mem);
    fmt_disk(r->disk_mbps, disk, sizeof disk);

    int j = -1;
    for (int k = 0; k < nslot; k++)
      if (slot[k].pid == r->pid) {
        j = k;
        break;
      }

    if (j < 0) { /* started since the last sweep */
      if (nslot >= MAXPROC)
        break; /* the collector stopped at its own limit; there is no room */
      GtkTreeIter it;
      gtk_list_store_insert_with_values(proc_store, &it, -1, C_NAME, r->name, C_PID, r->pid, C_CPU,
                                        r->cpu_pct, C_MEM, r->mem_mb, C_DISK, r->disk_mbps,
                                        C_CPU_TXT, cpu, C_MEM_TXT, mem, C_DISK_TXT, disk, -1);
      ProcSlot *sl = &slot[nslot++];
      memset(sl, 0, sizeof *sl);
      sl->pid = r->pid;
      sl->gen = sweep_gen;
      GtkTreePath *p = gtk_tree_model_get_path(m, &it);
      if (p) {
        sl->ref = gtk_tree_row_reference_new(m, p);
        gtk_tree_path_free(p);
      }
      slot_texts(sl, r, cpu, mem, disk);
      continue;
    }

    ProcSlot *sl = &slot[j];
    sl->gen = sweep_gen;
    if (!sl->ref || (!strcmp(sl->cpu, cpu) && !strcmp(sl->mem, mem) && !strcmp(sl->disk, disk) &&
                     !strcmp(sl->name, r->name)))
      continue; /* unchanged, or a row we can no longer find: GTK hears nothing */

    GtkTreePath *p = gtk_tree_row_reference_get_path(sl->ref);
    if (p) {
      GtkTreeIter it;
      if (gtk_tree_model_get_iter(m, &it, p))
        gtk_list_store_set(proc_store, &it, C_NAME, r->name, C_CPU, r->cpu_pct, C_MEM, r->mem_mb,
                           C_DISK, r->disk_mbps, C_CPU_TXT, cpu, C_MEM_TXT, mem, C_DISK_TXT, disk,
                           -1);
      gtk_tree_path_free(p);
    }
    slot_texts(sl, r, cpu, mem, disk);
  }

  /* Anything the sweep did not stamp has exited. Filling the hole from the last
     slot keeps the array dense, and its order does not matter: rows are found by
     pid, not by position. */
  for (int k = nslot - 1; k >= 0; k--) {
    if (slot[k].gen == sweep_gen)
      continue;
    GtkTreePath *p = slot[k].ref ? gtk_tree_row_reference_get_path(slot[k].ref) : NULL;
    if (p) {
      GtkTreeIter it;
      if (gtk_tree_model_get_iter(m, &it, p))
        gtk_list_store_remove(proc_store, &it);
      gtk_tree_path_free(p);
    }
    if (slot[k].ref)
      gtk_tree_row_reference_free(slot[k].ref);
    slot[k] = slot[--nslot];
  }

  gtk_tree_sortable_set_sort_column_id(s, keep_col, keep_order);
  if (adj)
    gtk_adjustment_set_value(adj, keep);
}

static void update_proc_panel(void) {
  gtk_label_set_text(GTK_LABEL(page_title), _("Processes"));
  char sub[256];
  if (pt.n == 0)
    snprintf(sub, sizeof sub, "%s", _("Collecting the process list"));
  else
    FMT(sub, sizeof sub, _("%d processes  ·  updated every %d s"), pt.n, (int)PROC_EVERY);
  gtk_label_set_text(GTK_LABEL(page_sub), sub);

  char vbuf[NST][80];
  char lbuf[NST][96];
  const char *vals[NST];
  const char *labs[NST];
  snprintf(vbuf[0], 80, "%.0f%%", st.cpu_pct);
  snprintf(lbuf[0], 96, _("Total CPU"));
  FMT(vbuf[1], 80, _("%.2f / %.2f GiB"), (st.mem_total_kb - st.mem_avail_kb) / 1048576.0,
      st.mem_total_kb / 1048576.0);
  snprintf(lbuf[1], 96, _("Memory in use"));
  fmt(U_RATE, st.disk_rd_mbps + st.disk_wr_mbps, vbuf[2], 80);
  snprintf(lbuf[2], 96, _("Disk activity"));
  FMT(vbuf[3], 80, _("%d  ·  %d yours"), pt.n, pt.n_own);
  snprintf(lbuf[3], 96, _("Processes"));
  for (int i = 0; i < NST; i++) {
    vals[i] = vbuf[i];
    labs[i] = lbuf[i];
  }
  set_stats(NST, vals, labs);
}

/* ---------- autostart list ---------- */

/* Each column shows a formatted string and keeps either the raw text or a small
   rank beside it, so sorting works on meaning rather than on the order of the
   words: "Enabled" has to sit above "Other desktop", not below it. */
enum { S_NAME, S_EXEC, S_STATE, S_SRC, S_ORDER, S_SRC_RANK, S_INDEX, SNCOL };

static const char *state_name[3] = {N_("Enabled"), N_("Disabled"), N_("Other desktop")};

/* The comparison is plain A to Z / lowest rank first, which is the opposite of
   proc_sort's big-first convention: Windows also opens the startup list sorted
   by name, and a startup entry has no "biggest first" reading. The rank ordering
   is the collector's own: enabled, then disabled, then the entries that belong
   to a different desktop and are never started here at all. */
static gint start_sort(GtkTreeModel *m, GtkTreeIter *a, GtkTreeIter *b, gpointer data) {
  (void)data;
  gint col = S_NAME;
  GtkSortType order = GTK_SORT_ASCENDING;
  gtk_tree_sortable_get_sort_column_id(GTK_TREE_SORTABLE(m), &col, &order);

  int r = 0;
  if (col == S_ORDER) {
    int ra = 0, rb = 0;
    gtk_tree_model_get(m, a, S_ORDER, &ra, -1);
    gtk_tree_model_get(m, b, S_ORDER, &rb, -1);
    r = ra - rb;
  } else if (col == S_SRC) {
    int ra = 0, rb = 0;
    gtk_tree_model_get(m, a, S_SRC_RANK, &ra, -1);
    gtk_tree_model_get(m, b, S_SRC_RANK, &rb, -1);
    r = ra - rb; /* the user's own overrides first, then the packaged entries */
  } else {
    char *sa = NULL, *sb = NULL;
    gtk_tree_model_get(m, a, col, &sa, -1);
    gtk_tree_model_get(m, b, col, &sb, -1);
    r = g_ascii_strcasecmp(sa ? sa : "", sb ? sb : "");
    if (r == 0)
      r = strcmp(sa ? sa : "", sb ? sb : ""); /* two names that differ only in case */
    g_free(sa);
    g_free(sb);
  }

  if (r == 0) { /* inside one rank, names read A to Z */
    char *sa = NULL, *sb = NULL;
    gtk_tree_model_get(m, a, S_NAME, &sa, -1);
    gtk_tree_model_get(m, b, S_NAME, &sb, -1);
    r = g_ascii_strcasecmp(sa ? sa : "", sb ? sb : "");
    g_free(sa);
    g_free(sb);
  }
  if (r == 0) {
    int ia = 0, ib = 0;
    gtk_tree_model_get(m, a, S_INDEX, &ia, -1);
    gtk_tree_model_get(m, b, S_INDEX, &ib, -1);
    r = ia - ib; /* one entry per file, so this is the only tie left */
  }
  return r;
}

/* The switch is one button at the end of the list, like the Enable/Disable
   button under the startup table in Windows, rather than a checkbox in every
   row: writing the file is the same two-key edit either way, and a button that
   says what it will do to the row you are looking at leaves no doubt about which
   row that is. */
static StartupRow *selected_startup_row(void) {
  GtkTreeModel *m = NULL;
  GtkTreeIter it;
  if (!start_sel || !gtk_tree_selection_get_selected(start_sel, &m, &it))
    return NULL;
  int idx = -1;
  gtk_tree_model_get(m, &it, S_INDEX, &idx, -1);
  if (idx < 0 || idx >= startup.n)
    return NULL;
  return &startup.rows[idx];
}

static void update_start_button(void) {
  if (!start_btn)
    return;
  StartupRow *r = selected_startup_row();
  const char *why = NULL; /* why the button is greyed, or what it will do */
  gboolean can = FALSE, on = FALSE;

  if (!r)
    why = _("Select an entry to change it");
  else if (r->state == RUN_OTHER_DE)
    why = _("This entry names another desktop; what decides is its OnlyShowIn line, not a switch");
  else if (!r->writable)
    why = _("The autostart directory of this user is not writable");
  else {
    can = TRUE;
    on = (r->state == RUN_ON);
    why = on ? _("Do not start this at login") : _("Start this at login");
  }
  gtk_widget_set_sensitive(start_btn, can);
  gtk_label_set_text(GTK_LABEL(gtk_bin_get_child(GTK_BIN(start_btn))),
                     can && !on ? _("Enable") : _("Disable"));
  gtk_widget_set_tooltip_text(start_btn, why);
}

static void on_start_selection_changed(GtkTreeSelection *s, gpointer data) {
  (void)s;
  (void)data;
  update_start_button();
}

static void on_start_clicked(GtkButton *b, gpointer data) {
  (void)b;
  (void)data;
  StartupRow *r = selected_startup_row();
  if (!r)
    return;
  int enable = (r->state != RUN_ON);
  int err = stats_startup_set(r, enable);
  if (err) {
    /* The note line is where the list explains itself, so it is where a refusal
       belongs too; the next reload puts the original sentence back. */
    char msg[256];
    FMT(msg, sizeof msg, _("Could not change %s: %s"), r->base, strerror(err));
    gtk_label_set_text(GTK_LABEL(start_note), msg);
    return;
  }
  /* Re-read the directories instead of editing the row in place: the file is the
     answer, and a scan costs a fraction of a millisecond. */
  reload_startup();
  update_panel();
}

/* A directory scan is cheap — 0.6 ms for the 33 entries on the development
   machine — and the list only changes when someone installs or disables an app,
   so the whole table is rebuilt rather than diffed row by row like the process
   list, and only when the tab opens. S_INDEX is the place in the collector's own
   array, which is what a switch has to write back to. */
static void reload_startup(void) {
  stats_scan_startup(&startup);

  GtkTreeSortable *s = GTK_TREE_SORTABLE(start_store);
  gint keep_col = S_NAME;
  GtkSortType keep_order = GTK_SORT_ASCENDING;
  gtk_tree_sortable_get_sort_column_id(s, &keep_col, &keep_order);
  if (keep_col < 0 || keep_col == GTK_TREE_SORTABLE_DEFAULT_SORT_COLUMN_ID)
    keep_col = S_NAME;

  gtk_tree_sortable_set_sort_column_id(s, GTK_TREE_SORTABLE_UNSORTED_SORT_COLUMN_ID,
                                       GTK_SORT_ASCENDING);
  GtkAdjustment *adj = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(start_scroll));
  double keep = adj ? gtk_adjustment_get_value(adj) : 0;

  /* The entry that is highlighted stays highlighted through a rebuild, which is
   * what makes switching one row off feel like editing a list rather than like
   * watching it reload. S_INDEX survives because the collector's own order is by
   * name and switching does not rename anything. */
  int keep_index = -1;
  GtkTreeModel *sm = NULL;
  GtkTreeIter si;
  if (start_sel && gtk_tree_selection_get_selected(start_sel, &sm, &si))
    gtk_tree_model_get(sm, &si, S_INDEX, &keep_index, -1);

  gtk_list_store_clear(start_store);

  for (int i = 0; i < startup.n; i++) {
    StartupRow *r = &startup.rows[i];
    gtk_list_store_insert_with_values(start_store, NULL, -1, S_NAME, r->name, S_EXEC, r->exec,
                                      S_STATE, _(state_name[r->state]), S_SRC,
                                      r->user_path[0] ? _("User") : _("System"), S_ORDER, r->state,
                                      S_SRC_RANK, r->user_path[0] ? 0 : 1, S_INDEX, i, -1);
  }

  gtk_tree_sortable_set_sort_column_id(s, keep_col, keep_order);
  if (adj)
    gtk_adjustment_set_value(adj, keep);

  if (keep_index >= 0) {
    GtkTreeIter it;
    if (gtk_tree_model_get_iter_first(GTK_TREE_MODEL(start_store), &it)) {
      do {
        int idx = -1;
        gtk_tree_model_get(GTK_TREE_MODEL(start_store), &it, S_INDEX, &idx, -1);
        if (idx == keep_index) {
          gtk_tree_selection_select_iter(start_sel, &it);
          break;
        }
      } while (gtk_tree_model_iter_next(GTK_TREE_MODEL(start_store), &it));
    }
  }
  update_start_button();

  if (startup.n == 0)
    gtk_label_set_text(GTK_LABEL(start_note),
                       _("No autostart entries were found in the directories this session reads."));
  else if (startup.overflow)
    gtk_label_set_text(GTK_LABEL(start_note),
                       _("Only entries that name another desktop are never started here. The list "
                         "stops at the first 160 entries."));
  else
    gtk_label_set_text(GTK_LABEL(start_note),
                       _("Only entries that name another desktop are never started here."));
}

static void update_startup_panel(void) {
  gtk_label_set_text(GTK_LABEL(page_title), _("Startup"));
  char sub[256];
  char vbuf[NST][80];
  char lbuf[NST][96];
  const char *vals[NST];
  const char *labs[NST];
  if (startup.n == 0) {
    /* An empty list gets no numbers at all: three zeros would read as "nothing
       is enabled", which is a different statement from "nothing was found". */
    snprintf(sub, sizeof sub, "%s", _("No entries"));
    gtk_label_set_text(GTK_LABEL(page_sub), sub);
    set_stats(0, NULL, NULL);
    return;
  }
  FMT(sub, sizeof sub, _("%d entries  ·  %d start at login"), startup.n, startup.n_on);
  gtk_label_set_text(GTK_LABEL(page_sub), sub);

  int n = 0;
  snprintf(vbuf[n], 80, "%d", startup.n);
  snprintf(lbuf[n], 96, _("Entries"));
  n++;
  snprintf(vbuf[n], 80, "%d", startup.n_on);
  snprintf(lbuf[n], 96, _("Enabled"));
  n++;
  snprintf(vbuf[n], 80, "%d", startup.n_off);
  snprintf(lbuf[n], 96, _("Disabled"));
  n++;
  if (startup.n_other) { /* a zero here would read as "nothing is off", not as "no other desktop" */
    snprintf(vbuf[n], 80, "%d", startup.n_other);
    snprintf(lbuf[n], 96, _("Other desktop"));
    n++;
  }
  for (int i = 0; i < n; i++) {
    vals[i] = vbuf[i];
    labs[i] = lbuf[i];
  }
  set_stats(n, vals, labs);
}

/* ---------- details list ---------- */

/* The wide table Windows shows under the same name: one row per process and
   every fact the kernel gives away about it. Nothing in it needs two samples —
   the CPU time, the byte counters and the start time are all cumulative — so
   this tab reads the same sweep the process list reads, only asking for the two
   fields that cost a syscall each (the owner's name and the command line).

   Like the process list, every column carries its formatted text and its raw
   value next to each other: "1:22:07" cannot be compared as a string when the
   sort has to know it is longer than "0:59:59".

   The cadence is its own, and it is slower than the process list's: a sweep with
   details costs 9.4 ms of CPU against 5.7 ms without them on the development
   machine, for 208 processes. Nothing here is a rate, so the extra second buys
   the budget back and no number on the screen goes wrong for want of it. */
#define DET_EVERY 6
static int det_countdown;

enum {
  D_NAME, D_PID, D_PID_T, D_STATE, D_STATE_ORD, D_USER, D_CPU, D_CPU_T, D_ELAP, D_ELAP_T, D_MEM,
  D_MEM_T, D_RD, D_RD_T, D_WR, D_WR_T, D_THR, D_THR_T, D_PRI, D_PRI_T, D_PPID, D_PPID_T, D_CMD,
  DNCOL
};

static GtkWidget *det_card, *det_tree, *det_scroll, *det_note;
static GtkListStore *det_store;

/* The columns in the order the table shows them. `det_val` names the column the
   comparator reads: the text itself for the words, the raw number for the
   measurements. */
static const char *det_title[13] = {N_("Name"),       N_("PID"),    N_("Status"),
                                    N_("User name"),  N_("CPU time"), N_("Time elapsed"),
                                    N_("Memory"),     N_("Disk read"), N_("Disk write"),
                                    N_("Threads"),    N_("Priority"), N_("Parent PID"),
                                    N_("Command line")};
static const int det_col[13] = {D_NAME, D_PID_T, D_STATE, D_USER, D_CPU_T, D_ELAP_T, D_MEM_T,
                                D_RD_T, D_WR_T,  D_THR_T, D_PRI_T, D_PPID_T, D_CMD};
static const int det_val[13] = {D_NAME, D_PID,  D_STATE_ORD, D_USER, D_CPU,  D_ELAP, D_MEM,
                                 D_RD,   D_WR,   D_THR,       D_PRI,  D_PPID, D_CMD};
static const int det_w[13] = {150, 60, 112, 96, 80, 96, 88, 92, 92, 64, 64, 72, 300};

/* Which way the first click on a header sorts. The measurements are big-first,
   the same way the process list is: the busiest process belongs on top. The
   counts that are not "more is better" — the PID, the parent, the priority, the
   order the states fall in — sort lowest-first like the autostart list, because
   a big PID is a young process, not an interesting one. Names sort A to Z. */
static const int det_big[13] = {0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 0, 0, 0};

/* Kernel states are letters, and the letters are not the answer for a person
   reading a task manager. "D" is not a kind of sleep a machine needs; it is a
   process that cannot be woken until the disk answers, and that is what the
   column should say. Anything the kernel invents later falls back to the letter
   rather than to a guess. */
static const char *state_word(char c) {
  switch (c) {
    case 'R':
      return _("Running");
    case 'S':
      return _("Sleeping");
    case 'D':
      return _("Waiting for disk");
    case 'Z':
      return _("Zombie");
    case 'T':
    case 't':
      return _("Stopped");
    case 'I':
      return _("Idle");
    default:
      return NULL;
  }
}

/* Lowest number first: the states that need attention, then the resting ones. */
static int state_rank(char c) {
  switch (c) {
    case 'R':
      return 0;
    case 'D':
      return 1;
    case 'Z':
      return 2;
    case 'T':
    case 't':
      return 3;
    case 'S':
      return 4;
    case 'I':
      return 5;
    default:
      return 6;
  }
}

/* Lifetime CPU time, in the shape Windows uses: unbounded hours, so a process
   that has burned three days still fits and still sorts. */
static void fmt_secs(double s, char *out, size_t n) {
  if (s < 0)
    snprintf(out, n, "%s", "—");
  else {
    unsigned long t = (unsigned long)(s + 0.5);
    FMT(out, n, _("%lu:%02lu:%02lu"), t / 3600, (t / 60) % 60, t % 60);
  }
}

/* How long ago it started. Below a day this is the same clock shape as the CPU
   time; above it the days carry, because "74:11:02" makes a reader count hours
   that mean nothing. */
static void fmt_elapsed(double s, char *out, size_t n) {
  if (s < 0)
    snprintf(out, n, "%s", "—");
  else if (s >= 86400.0) {
    unsigned long t = (unsigned long)s;
    FMT(out, n, _("%lu d %02lu:%02lu"), t / 86400, (t / 3600) % 24, (t / 60) % 60);
  } else
    fmt_secs(s, out, n);
}

/* Byte counters are absolute, so they read in the units they land in rather than
   as a rate. */
static void fmt_bytes(unsigned long long b, char *out, size_t n) {
  if (b >= 1099511627776ULL)
    FMT(out, n, _("%.2f TiB"), (double)b / 1099511627776.0);
  else if (b >= 1073741824ULL)
    FMT(out, n, _("%.2f GiB"), (double)b / 1073741824.0);
  else if (b >= 1048576ULL)
    FMT(out, n, _("%.1f MiB"), (double)b / 1048576.0);
  else if (b >= 1024ULL)
    FMT(out, n, _("%.0f KiB"), (double)b / 1024.0);
  else
    FMT(out, n, _("%llu B"), b);
}

/* The value the Priority column sorts on. The kernel keeps real-time tasks below
   20 in this field and ordinary tasks at exactly 20 + nice, so one number already
   ranks every process from "will preempt everyone" to "yielded"; the text beside
   it spells the part a person can act on. */
static void fmt_priority(long field18, int nice, char *out, size_t n) {
  if (field18 > 0 && field18 < 20)
    FMT(out, n, _("real-time %ld"), field18);
  else
    FMT(out, n, _("nice %d"), nice);
}

/* The twelve formatted cells of one row. Written into a plain struct so the
   update can compare a whole row at once: a details row with one changed cell
   still repaints the row, so there is nothing to gain from diffing columns
   separately. */
typedef struct {
  char pid[12], name[32], state[40], user[24], cpu[24], elapsed[24], mem[24], rd[28], wr[28],
      thr[16], pri[24], ppid[12], cmd[204];
} DetTexts;

static void det_texts(ProcRow *r, unsigned long long now, DetTexts *t) {
  snprintf(t->pid, sizeof t->pid, "%d", r->pid);
  g_strlcpy(t->name, r->name, sizeof t->name);
  const char *w = r->state ? state_word(r->state) : NULL;
  if (w)
    g_strlcpy(t->state, w, sizeof t->state);
  else
    snprintf(t->state, sizeof t->state, _("(%c)"), r->state ? r->state : '?');
  if (r->user[0])
    g_strlcpy(t->user, r->user, sizeof t->user);
  else
    g_strlcpy(t->user, "—", sizeof t->user);
  fmt_secs(r->cpu_s, t->cpu, sizeof t->cpu);
  fmt_elapsed(r->start_epoch && now > r->start_epoch ? (double)(now - r->start_epoch) : -1.0,
              t->elapsed, sizeof t->elapsed);
  fmt_mem(r->mem_mb, t->mem, sizeof t->mem);
  if (r->own) {
    fmt_bytes(r->io_rd, t->rd, sizeof t->rd);
    fmt_bytes(r->io_wr, t->wr, sizeof t->wr);
  } else {
    g_strlcpy(t->rd, "—", sizeof t->rd);
    g_strlcpy(t->wr, "—", sizeof t->wr);
  }
  snprintf(t->thr, sizeof t->thr, "%d", r->threads);
  fmt_priority(r->priority, r->nice, t->pri, sizeof t->pri);
  snprintf(t->ppid, sizeof t->ppid, "%d", r->ppid);
  g_strlcpy(t->cmd, r->cmd[0] ? r->cmd : "—", sizeof t->cmd);
}

/* One comparator for all thirteen headers, in the same shape proc_sort uses: it
   cannot be told which column asked, so it reads the store's own sort pair back.
   Unknown cells (a negative raw value: no start time, no readable byte counter)
   stay at the bottom whichever way the list is turned. */
static gint det_cmp(GtkTreeModel *m, GtkTreeIter *a, GtkTreeIter *b, gpointer data) {
  (void)data;
  gint col = D_NAME;
  GtkSortType order = GTK_SORT_ASCENDING;
  gtk_tree_sortable_get_sort_column_id(GTK_TREE_SORTABLE(m), &col, &order);
  gboolean desc = (order == GTK_SORT_DESCENDING);

  int i = -1;
  for (int k = 0; k < 13; k++)
    if (det_val[k] == col)
      i = k;
  if (i < 0)
    return 0; /* not a column of ours: leave the rows where they are */

  int r = 0;
  if (col == D_NAME || col == D_USER || col == D_CMD) {
    char *x = NULL, *y = NULL;
    gtk_tree_model_get(m, a, col, &x, -1);
    gtk_tree_model_get(m, b, col, &y, -1);
    /* The dash is not a word: an unknown user name or command line belongs at
       the bottom of an A to Z list, not at the top of it. It is also three bytes
       in UTF-8, which is why the test is a prefix rather than a character. */
    int ax = (x && g_str_has_prefix(x, "—")) ? 1 : 0, ay = (y && g_str_has_prefix(y, "—")) ? 1 : 0;
    if (ax != ay)
      r = desc ? (ax ? 1 : -1) : (ax ? -1 : 1);
    else {
      r = g_ascii_strcasecmp(y, x); /* Z to A, so GTK's reversal gives A to Z */
      if (!r)
        r = strcmp(y, x);
    }
    g_free(x);
    g_free(y);
  } else {
    double va = -1, vb = -1;
    gtk_tree_model_get(m, a, col, &va, -1);
    gtk_tree_model_get(m, b, col, &vb, -1);
    if (va < 0 && vb >= 0)
      return desc ? -1 : 1;
    if (vb < 0 && va >= 0)
      return desc ? 1 : -1;
    if (!det_big[i]) {
      int t = (va < vb) ? -1 : (va > vb ? 1 : 0);
      r = desc ? -t : t; /* lowest first, and the reversal is the user's own */
    } else
      r = va > vb ? -1 : (va < vb ? 1 : 0);
  }
  if (r)
    return r;

  double da = 0, db = 0;
  gtk_tree_model_get(m, a, D_PID, &da, -1);
  gtk_tree_model_get(m, b, D_PID, &db, -1);
  return da < db ? -1 : (da > db ? 1 : 0); /* no two rows share a pid: this ends
                                              every argument */
}

/* Rows are remembered by pid, exactly as in the process list, and a row is
   written only when one of its cells really changed. The difference is what
   "changed" means here: a sleeping process's CPU time, byte counters and start
   time all stay put for hours, so an idle desktop repaints a handful of rows per
   sweep rather than two hundred. */
typedef struct {
  int pid;
  unsigned gen;
  GtkTreeRowReference *ref;
  DetTexts t;
} DetSlot;

static DetSlot dslot[MAXPROC];
static int ndslot;

/* One place writes a whole row, so the insert and the update cannot drift apart
   column by column. The text cells come from the diffed struct and the sort keys
   from the row itself; a value nobody gave is -1.0, which the comparator puts at
   the bottom whichever way the list is turned. */
static void det_write(GtkTreeIter *it, const ProcRow *r, const DetTexts *t,
                      unsigned long long now) {
  double el = (r->start_epoch && now > r->start_epoch) ? (double)(now - r->start_epoch) : -1.0;
  gtk_list_store_set(det_store, it, D_NAME, t->name, D_PID, (double)r->pid, D_PID_T, t->pid,
                     D_STATE, t->state, D_STATE_ORD, (double)state_rank(r->state), D_USER, t->user,
                     D_CPU, r->cpu_s, D_CPU_T, t->cpu, D_ELAP, el, D_ELAP_T, t->elapsed, D_MEM,
                     r->mem_mb, D_MEM_T, t->mem, D_RD, r->own ? (double)r->io_rd : -1.0, D_RD_T,
                     t->rd, D_WR, r->own ? (double)r->io_wr : -1.0, D_WR_T, t->wr, D_THR,
                     (double)r->threads, D_THR_T, t->thr, D_PRI, (double)r->priority, D_PRI_T,
                     t->pri, D_PPID, (double)r->ppid, D_PPID_T, t->ppid, D_CMD, t->cmd, -1);
}

static void sync_det_rows(void) {
  GtkTreeSortable *s = GTK_TREE_SORTABLE(det_store);
  GtkTreeModel *m = GTK_TREE_MODEL(det_store);
  gint keep_col = D_NAME;
  GtkSortType keep_order = GTK_SORT_ASCENDING;
  gtk_tree_sortable_get_sort_column_id(s, &keep_col, &keep_order);

  /* Writing into a sorted store moves rows one by one, so the update happens
     unsorted and the whole list is put back in order once. */
  gtk_tree_sortable_set_sort_column_id(s, GTK_TREE_SORTABLE_UNSORTED_SORT_COLUMN_ID,
                                       GTK_SORT_ASCENDING);
  GtkAdjustment *adj = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(det_scroll));
  double keep = adj ? gtk_adjustment_get_value(adj) : 0;

  unsigned long long now = (unsigned long long)time(NULL);
  ++sweep_gen;
  for (int i = 0; i < pt.n; i++) {
    ProcRow *r = &pt.rows[i];
    DetTexts t;
    det_texts(r, now, &t);

    int j = -1;
    for (int k = 0; k < ndslot; k++)
      if (dslot[k].pid == r->pid) {
        j = k;
        break;
      }

    if (j < 0) { /* started since the last sweep */
      if (ndslot >= MAXPROC)
        break;
      GtkTreeIter it;
      gtk_list_store_insert_with_values(det_store, &it, -1, D_PID, (double)r->pid, -1);
      det_write(&it, r, &t, now);
      DetSlot *sl = &dslot[ndslot++];
      memset(sl, 0, sizeof *sl);
      sl->pid = r->pid;
      sl->gen = sweep_gen;
      sl->t = t;
      GtkTreePath *p = gtk_tree_model_get_path(m, &it);
      if (p) {
        sl->ref = gtk_tree_row_reference_new(m, p);
        gtk_tree_path_free(p);
      }
      continue;
    }

    DetSlot *sl = &dslot[j];
    sl->gen = sweep_gen;
    if (!sl->ref || !memcmp(&sl->t, &t, sizeof t))
      continue; /* nothing in the row changed: GTK hears nothing */

    GtkTreePath *p = gtk_tree_row_reference_get_path(sl->ref);
    if (p) {
      GtkTreeIter it;
      if (gtk_tree_model_get_iter(m, &it, p))
        det_write(&it, r, &t, now);
      gtk_tree_path_free(p);
    }
    sl->t = t;
  }

  for (int k = ndslot - 1; k >= 0; k--) {
    if (dslot[k].gen == sweep_gen)
      continue;
    GtkTreePath *p = dslot[k].ref ? gtk_tree_row_reference_get_path(dslot[k].ref) : NULL;
    if (p) {
      GtkTreeIter it;
      if (gtk_tree_model_get_iter(m, &it, p))
        gtk_list_store_remove(det_store, &it);
      gtk_tree_path_free(p);
    }
    if (dslot[k].ref)
      gtk_tree_row_reference_free(dslot[k].ref);
    dslot[k] = dslot[--ndslot];
  }

  gtk_tree_sortable_set_sort_column_id(s, keep_col, keep_order);
  if (adj)
    gtk_adjustment_set_value(adj, keep);

}

static void update_details_panel(void) {
  gtk_label_set_text(GTK_LABEL(page_title), _("Details"));
  char sub[256];
  if (pt.n == 0)
    snprintf(sub, sizeof sub, "%s", _("Collecting the process list"));
  else
    FMT(sub, sizeof sub, _("%d processes  ·  updated every %d s"), pt.n, DET_EVERY);
  gtk_label_set_text(GTK_LABEL(page_sub), sub);

  char vbuf[NST][80];
  char lbuf[NST][96];
  const char *vals[NST];
  const char *labs[NST];
  snprintf(vbuf[0], 80, "%d", pt.n);
  snprintf(lbuf[0], 96, _("Processes"));
  FMT(vbuf[1], 80, _("%d  ·  %.0f MiB"), pt.n_own, pt.sum_mem_mb);
  snprintf(lbuf[1], 96, _("Yours  ·  memory"));
  int nstate = 0;
  for (int i = 0; i < pt.n; i++)
    if (pt.rows[i].state == 'D')
      nstate++;
  snprintf(vbuf[2], 80, "%d", nstate);
  snprintf(lbuf[2], 96, _("Waiting for disk"));
  int nthr = 0;
  for (int i = 0; i < pt.n; i++)
    nthr += pt.rows[i].threads;
  snprintf(vbuf[3], 80, "%d", nthr);
  snprintf(lbuf[3], 96, _("Threads"));
  for (int i = 0; i < NST; i++) {
    vals[i] = vbuf[i];
    labs[i] = lbuf[i];
  }
  set_stats(NST, vals, labs);
}

/* ---------- machine information tab ---------- */

enum { K_KEY, K_VAL, KVCOL };

/* The section list is the same shape the Performance tab uses for its devices:
   pick one on the left, the card shows it. Six sections because those are the
   six questions a person asks about a machine — what system it runs, what
   processor and memory it has, who built the board, what draws the screen and
   what stores the files. */
#define INFO_SECTIONS 6
static const char *info_sec_name[INFO_SECTIONS] = {N_("System"), N_("Processor"), N_("Memory"),
                                                   N_("Motherboard"), N_("Video"), N_("Disks")};

/* A section never has more rows than this, and the count is what a machine with
   every field readable produces. A machine that answers less shows less. */
#define INFO_ROWS 16

static SysInfo inf;
static int info_sec;
static GtkWidget *info_card, *info_scroll, *info_tree, *info_list, *info_sep;
static GtkWidget *card_stack;
static GtkListStore *info_store;

/* SMBIOS type 3. The code is a number in a sysfs file; a machine that states
   none, or one whose number is not in this table, is described by its number
   rather than by a guess at a shape. */
static const char *chassis_name(int c) {
  switch (c) {
    case 3: return _("Desktop computer");
    case 4: return _("Low profile desktop");
    case 6: return _("Tower");
    case 7: case 8: case 9: return _("Portable");
    case 10: return _("Notebook");
    case 11: return _("Hand held");
    case 12: return _("Docking station");
    case 13: return _("All in one");
    case 14: return _("Subnotebook");
    case 16: return _("Lunch box");
    case 17: return _("Server chassis");
    case 21: case 22: case 23: return _("Blade");
    default: return NULL;
  }
}

/* The rows of one section, as two columns of text. A field that came back empty
   from the collector is not added at all, so the table can never show a blank
   answer or a placeholder; it shows the questions this machine can answer. */
static int info_rows(int sec, char k[INFO_ROWS][96], char v[INFO_ROWS][192]) {
  int n = 0;
#define ADD_KEYVAL(key, val)                        \
  do {                                              \
    if (n < INFO_ROWS) {                            \
      snprintf(k[n], sizeof k[n], "%s", (key));      \
      snprintf(v[n], sizeof v[n], "%s", (val));      \
      n++;                                          \
    }                                               \
  } while (0)
#define ADD_ROW(key, ...)                                    \
  do {                                                       \
    char ar_[192];                                           \
    FMT(ar_, sizeof ar_, __VA_ARGS__);                       \
    if (n < INFO_ROWS) {                                     \
      snprintf(k[n], sizeof k[n], "%s", (key));              \
      snprintf(v[n], sizeof v[n], "%s", ar_);                \
      n++;                                                   \
    }                                                        \
  } while (0)

  char buf[192];

  if (sec == 0) {
    if (inf.os[0]) ADD_KEYVAL(_("Operating system"), inf.os);
    if (inf.kernel[0]) ADD_KEYVAL(_("Kernel"), inf.kernel);
    if (inf.machine[0]) ADD_KEYVAL(_("Architecture"), inf.machine);
    if (inf.session[0]) {
      if (inf.desktop[0])
        FMT(buf, sizeof buf, "%s  ·  %s", inf.session, inf.desktop);
      else
        snprintf(buf, sizeof buf, "%s", inf.session);
      ADD_KEYVAL(_("Session"), buf);
    }
    if (inf.hostname[0]) ADD_KEYVAL(_("Computer name"), inf.hostname);
    if (inf.user[0]) ADD_KEYVAL(_("User"), inf.user);
    fmt_elapsed((double)st.uptime_s, buf, sizeof buf);
    ADD_KEYVAL(_("Uptime"), buf);
  } else if (sec == 1) {
    if (inf.cpu_model[0]) ADD_KEYVAL(_("Model"), inf.cpu_model);
    if (inf.cpu_vendor[0]) ADD_KEYVAL(_("Vendor"), inf.cpu_vendor);
    /* Both come from the first package, so a machine that does not state them
       simply has no rows here rather than a pair of zeros. */
    if (inf.cpu_cores) ADD_ROW(_("Cores"), "%d", inf.cpu_cores);
    ADD_ROW(_("Logical processors"), "%d", st.ncore);
    for (int lv = 1; lv <= 4; lv++) {
      char val[192] = "";
      for (int c = 0; c < inf.ncache; c++) {
        if (inf.cache[c].level != lv || inf.cache[c].size_kb <= 0)
          continue;
        const char *t = inf.cache[c].type;
        const char *tn = !g_ascii_strcasecmp(t, "Data")        ? _("Data")
                         : !g_ascii_strcasecmp(t, "Instruction") ? _("Instruction")
                         : !g_ascii_strcasecmp(t, "Unified")     ? _("Unified")
                                                                  : t;
        /* The firmware states these in KiB; the byte formatter is what every
           other size in the app goes through, so L2 reads 6,00 MiB rather than
           6144 KiB. */
        char one[40];
        fmt_bytes((unsigned long long)inf.cache[c].size_kb * 1024, one, sizeof one);
        char joined[192];
        FMT(joined, sizeof joined, "%s%s%s (%s)", val, val[0] ? "  ·  " : "", one, tn);
        snprintf(val, sizeof val, "%s", joined);
      }
      if (!val[0])
        continue;
      char key[96];
      FMT(key, sizeof key, _("Cache L%d"), lv);
      ADD_KEYVAL(key, val);
    }
    if (inf.cpu_min_khz > 0 && inf.cpu_max_khz > 0)
      ADD_ROW(_("Speed range"), _("%.2f–%.2f GHz"), inf.cpu_min_khz / 1e6, inf.cpu_max_khz / 1e6);
    if (st.freq_ghz > 0)
      ADD_ROW(_("Current speed"), _("%.2f GHz"), st.freq_ghz);
    if (inf.cpu_gov[0]) ADD_KEYVAL(_("Speed control"), inf.cpu_gov);
  } else if (sec == 2) {
    fmt_bytes((unsigned long long)(st.mem_total_kb * 1024), buf, sizeof buf);
    ADD_KEYVAL(_("Total"), buf);
    fmt_bytes((unsigned long long)(st.mem_avail_kb * 1024), buf, sizeof buf);
    ADD_KEYVAL(_("Free"), buf);
    if (st.mem_type[0]) ADD_KEYVAL(_("Type"), st.mem_type);
    /* The kernel binds one EEPROM per module it can see in the memory slots. A
       machine whose firmware says nothing gives no row, which is the honest
       answer and not a claim that the memory is soldered. */
    if (inf.mem_modules > 0) ADD_ROW(_("Modules"), "%d", inf.mem_modules);
    if (st.swap_total_kb > 0) {
      char a[40], b[40];
      fmt_bytes((unsigned long long)(st.swap_used_kb * 1024), a, sizeof a);
      fmt_bytes((unsigned long long)(st.swap_total_kb * 1024), b, sizeof b);
      FMT(buf, sizeof buf, "%s / %s", a, b);
      ADD_KEYVAL(_("Swap"), buf);
    }
  } else if (sec == 3) {
    if (inf.sys_vendor[0]) ADD_KEYVAL(_("Manufacturer"), inf.sys_vendor);
    if (inf.product[0]) ADD_KEYVAL(_("Product"), inf.product);
    if (inf.board_vendor[0] || inf.board_name[0]) {
      if (inf.board_vendor[0] && inf.board_name[0])
        FMT(buf, sizeof buf, "%s  ·  %s", inf.board_vendor, inf.board_name);
      else
        snprintf(buf, sizeof buf, "%s", inf.board_vendor[0] ? inf.board_vendor : inf.board_name);
      ADD_KEYVAL(_("Board"), buf);
    }
    if (inf.bios_vendor[0] || inf.bios_version[0]) {
      if (inf.bios_vendor[0] && inf.bios_version[0])
        FMT(buf, sizeof buf, "%s %s", inf.bios_vendor, inf.bios_version);
      else
        snprintf(buf, sizeof buf, "%s", inf.bios_vendor[0] ? inf.bios_vendor : inf.bios_version);
      ADD_KEYVAL(_("BIOS"), buf);
    }
    if (inf.bios_date[0]) ADD_KEYVAL(_("BIOS date"), inf.bios_date);
    if (inf.chassis) {
      const char *nm = chassis_name(inf.chassis);
      if (nm)
        ADD_KEYVAL(_("Chassis"), nm);
      else
        ADD_ROW(_("Chassis"), _("Type %d"), inf.chassis);
    }
  } else if (sec == 4) {
    if (st.gpu_present && st.gpu_name[0]) ADD_KEYVAL(_("Adapter"), st.gpu_name);
    if (inf.gpu_driver[0]) {
      if (inf.gpu_node[0])
        FMT(buf, sizeof buf, _("%s (%s)"), inf.gpu_driver, inf.gpu_node);
      else
        snprintf(buf, sizeof buf, "%s", inf.gpu_driver);
      ADD_KEYVAL(_("Driver"), buf);
    }
    /* The panel the screen is actually running at, from the session rather than
       from a mode list: what the user sees, not what the adapter could do. */
    GdkDisplay *dpy = gdk_display_get_default();
    int nmon = dpy ? gdk_display_get_n_monitors(dpy) : 0;
    for (int m = 0; m < nmon; m++) {
      GdkRectangle g;
      gdk_monitor_get_geometry(gdk_display_get_monitor(dpy, m), &g);
      if (g.width <= 0 || g.height <= 0)
        continue;
      int hz = gdk_monitor_get_refresh_rate(gdk_display_get_monitor(dpy, m));
      char key[96];
      if (nmon > 1)
        FMT(key, sizeof key, _("Display %d"), m + 1);
      else
        snprintf(key, sizeof key, "%s", _("Display"));
      if (hz > 0)
        ADD_ROW(key, _("%d×%d  ·  %.2f Hz"), g.width, g.height, hz / 1000.0);
      else
        ADD_ROW(key, _("%d×%d"), g.width, g.height);
    }
  } else {
    for (int d = 0; d < inf.ndisk; d++) {
      char sz[40];
      fmt_bytes(inf.disk[d].bytes, sz, sizeof sz);
      /* The same two abbreviations the Disk device row uses; they are not words
         that gain from translation. */
      const char *kind = inf.disk[d].rotational ? "HDD" : "SSD";
      if (inf.disk[d].model[0])
        FMT(buf, sizeof buf, "%s  ·  %s  ·  %s", inf.disk[d].model, kind, sz);
      else
        FMT(buf, sizeof buf, "%s  ·  %s", kind, sz);
      ADD_KEYVAL(inf.disk[d].name, buf);
    }
    for (int f = 0; f < inf.nfs; f++) {
      char free_[40], total[40];
      fmt_bytes(inf.fs[f].free, free_, sizeof free_);
      fmt_bytes(inf.fs[f].total, total, sizeof total);
      FMT(buf, sizeof buf, "%s  ·  %s  ·  %s / %s", inf.fs[f].dev, inf.fs[f].fstype, free_, total);
      ADD_KEYVAL(inf.fs[f].mnt, buf);
    }
  }
#undef ADD_ROW
#undef ADD_KEYVAL
  return n;
}

/* Written only when the text changed, like the sensor table: on this tab the
   only thing that moves is the uptime, and it moves once a minute. */
static void sync_info_rows(void) {
  if (!info_store)
    return;
  static char k[INFO_ROWS][96], v[INFO_ROWS][192];
  int want = info_rows(info_sec, k, v);
  GtkTreeIter it;
  int n = gtk_tree_model_iter_n_children(GTK_TREE_MODEL(info_store), NULL);
  while (n < want) {
    gtk_list_store_append(info_store, &it);
    gtk_list_store_set(info_store, &it, K_KEY, "", K_VAL, "", -1);
    n++;
  }
  while (n > want) {
    if (!gtk_tree_model_iter_nth_child(GTK_TREE_MODEL(info_store), &it, NULL, n - 1))
      break;
    gtk_list_store_remove(info_store, &it);
    n--;
  }
  for (int i = 0; i < want; i++) {
    if (!gtk_tree_model_iter_nth_child(GTK_TREE_MODEL(info_store), &it, NULL, i))
      break;
    char *hk = NULL, *hv = NULL;
    gtk_tree_model_get(GTK_TREE_MODEL(info_store), &it, K_KEY, &hk, K_VAL, &hv, -1);
    gboolean diff = !hk || !hv || strcmp(hk, k[i]) != 0 || strcmp(hv, v[i]) != 0;
    g_free(hk);
    g_free(hv);
    if (diff)
      gtk_list_store_set(info_store, &it, K_KEY, k[i], K_VAL, v[i], -1);
  }
}

static void update_info_panel(void) {
  gtk_label_set_text(GTK_LABEL(page_title), _("Information"));
  char sub[256] = "";
  if (inf.os[0] && inf.kernel[0])
    FMT(sub, sizeof sub, "%s  ·  %s  ·  %s", inf.os, inf.kernel, inf.machine);
  else if (inf.hostname[0])
    snprintf(sub, sizeof sub, "%s", inf.hostname);
  gtk_label_set_text(GTK_LABEL(page_sub), sub);
  set_stats(0, NULL, NULL);
  sync_info_rows();
}

static void on_info_row_selected(GtkListBox *box, GtkListBoxRow *row, gpointer u) {
  (void)box;
  (void)u;
  if (!row)
    return;
  info_sec = gtk_list_box_row_get_index(row);
  sync_info_rows();
}

/* The line under each process table is where the outcome of an action is
   reported, so the explanation it carries by default has to be reachable again.
   It goes back when the tab is opened, the way the Startup tab's note does. */
static const char proc_note_text[] = N_("CPU and memory cover every process; disk activity is "
                                        "only readable for the processes you own.");
static const char det_note_text[] = N_("The byte counters come from the kernel's per-process io "
                                       "file, which only your own processes let anyone read.");

static void set_tab(int t) {
  if (t == tab)
    return;
  tab = t;
  gtk_label_set_text(GTK_LABEL(head_sub), _(tab_head[t]));
  gboolean perf = (t == TAB_PERF);
  /* One child of the stack is on screen at a time, which is what these five
     cards always meant to be. */
  static const char *const tab_card[5] = {"proc", "perf", "start", "det", "info"};
  gtk_stack_set_visible_child_name(GTK_STACK(card_stack), tab_card[t]);
  /* The device list belongs to the Performance tab and the section list to the
     Information tab; on the other three either would only take the width the
     table needs. */
  gtk_widget_set_visible(dev_list, perf);
  gtk_widget_set_visible(dev_sep, perf);
  gtk_widget_set_visible(info_list, t == TAB_INFO);
  gtk_widget_set_visible(info_sep, t == TAB_INFO);
  if (t == TAB_PROC) {
    stats_sample_procs(&pt, 0); /* remember the counters; real numbers arrive next tick */
    proc_countdown = 1;
    sync_proc_rows();
  } else if (t == TAB_STARTUP) {
    reload_startup();
  } else if (t == TAB_DETAILS) {
    stats_sample_procs(&pt, 1);
    det_countdown = 1;
    sync_det_rows();
  } else if (t == TAB_INFO) {
    /* Once per opening, the way the autostart scan works: these files describe
       hardware that does not change while the window is up, and reading them on
       a timer would cost for no new answer. */
    stats_collect_info(&inf);
  }
  /* Opening a tab again clears whatever the last action left there. */
  if (proc_note)
    gtk_label_set_text(GTK_LABEL(proc_note), _(proc_note_text));
  if (det_note)
    gtk_label_set_text(GTK_LABEL(det_note), _(det_note_text));
  if (rail)
    gtk_widget_queue_draw(rail);
  update_panel();
}

/* ---------- panel content ---------- */

static void set_stats(int n, const char *const *vals, const char *const *labs) {
  for (int i = 0; i < NST; i++) {
    gboolean show = i < n;
    gtk_widget_set_visible(stat_value[i], show);
    gtk_widget_set_visible(stat_label[i], show);
    /* A machine that answers three of the four questions gets three columns and
       two dividers, not a rule standing at the edge of an empty one. */
    if (i > 0 && stat_sep[i])
      gtk_widget_set_visible(stat_sep[i], show);
    if (show) {
      gtk_label_set_text(GTK_LABEL(stat_value[i]), vals[i]);
      gtk_label_set_text(GTK_LABEL(stat_label[i]), labs[i]);
    }
  }
}

/* ---------- sensor list ---------- */

enum { T_NAME, T_VALUE, T_LIMIT, TCOL };

/* The table under the temperature graph. A cell is written only when its text
   actually changed: the process tab learned that repainting rows, not reading
   them, is what costs, and these rows are rewritten once a second for as long
   as the row is on screen. */
static void sync_sens_rows(void) {
  GtkTreeIter it;
  int n = gtk_tree_model_iter_n_children(GTK_TREE_MODEL(sens_store), NULL);
  while (n < st.nsens) {
    gtk_list_store_append(sens_store, &it);
    gtk_list_store_set(sens_store, &it, T_NAME, "", T_VALUE, "", T_LIMIT, "", -1);
    n++;
  }
  while (n > st.nsens) {
    if (!gtk_tree_model_iter_nth_child(GTK_TREE_MODEL(sens_store), &it, NULL, n - 1))
      break;
    gtk_list_store_remove(sens_store, &it);
    n--;
  }

  for (int i = 0; i < st.nsens; i++) {
    if (!gtk_tree_model_iter_nth_child(GTK_TREE_MODEL(sens_store), &it, NULL, i))
      break;
    char nm[48], val[24], lim[24];
    sensor_name(i, nm, sizeof nm);
    FMT(val, sizeof val, _("%.0f °C"), st.sens[i].mc / 1000.0);
    if (st.sens[i].limit_mc)
      FMT(lim, sizeof lim, _("%.0f °C"), st.sens[i].limit_mc / 1000.0);
    else
      snprintf(lim, sizeof lim, "%s", "—");

    char *want[TCOL] = {nm, val, lim};
    gboolean diff = FALSE;
    for (int c = 0; c < TCOL && !diff; c++) {
      char *have = NULL;
      gtk_tree_model_get(GTK_TREE_MODEL(sens_store), &it, c, &have, -1);
      diff = !have || strcmp(have, want[c]) != 0;
      g_free(have);
    }
    if (diff)
      gtk_list_store_set(sens_store, &it, T_NAME, want[0], T_VALUE, want[1], T_LIMIT, want[2], -1);
  }
}

static void update_panel(void) {
  if (tab == TAB_PROC) {
    update_proc_panel();
    return;
  }
  if (tab == TAB_STARTUP) {
    update_startup_panel();
    return;
  }
  if (tab == TAB_DETAILS) {
    update_details_panel();
    return;
  }
  if (tab == TAB_INFO) {
    update_info_panel();
    return;
  }
  gtk_label_set_text(GTK_LABEL(page_title), _(dev_name[sel]));

  char sub[256] = "";
  switch (sel) {
    case DEV_CPU:
      snprintf(sub, sizeof sub, "%s", st.cpu_model);
      break;
    case DEV_MEM: {
      double g = st.mem_total_kb / 1048576.0;
      if (st.mem_type[0])
        FMT(sub, sizeof sub, _("%s  ·  %.2f GiB  ·  cache %.2f GiB"), st.mem_type, g,
            st.mem_cached_kb / 1048576.0);
      else
        FMT(sub, sizeof sub, _("Installed: %.2f GiB  ·  cache %.2f GiB"), g,
            st.mem_cached_kb / 1048576.0);
      break;
    }
    case DEV_DISK: {
      const char *kind = st.disk_rotational == 0 ? "SSD" : (st.disk_rotational == 1 ? "HDD" : "Disk");
      snprintf(sub, sizeof sub, "%s (%s)  ·  %s", st.disk_model, st.disk_name, kind);
      break;
    }
    case DEV_NET:
      if (st.net_link_mbps > 0)
        FMT(sub, sizeof sub, _("%s  ·  %ld Mbit/s"), st.net_if, st.net_link_mbps);
      else if (st.net_wireless)
        FMT(sub, sizeof sub, _("%s  ·  Wi-Fi  ·  %d dBm  ·  quality %d"), st.net_if,
            st.net_level_dbm, st.net_quality);
      else
        FMT(sub, sizeof sub, _("%s  ·  link speed unavailable"), st.net_if);
      break;
    case DEV_GPU:
      snprintf(sub, sizeof sub, "%s", st.gpu_name[0] ? st.gpu_name : "GPU");
      break;
    case DEV_TEMP: {
      int h = hottest_sensor();
      if (h < 0) {
        snprintf(sub, sizeof sub, "%s", "");
      } else {
        char nm[48];
        sensor_name(h, nm, sizeof nm);
        if (st.temp_limit_mc > 0)
          FMT(sub, sizeof sub, _("%d sensors  ·  highest %s  ·  limit %.0f °C"), st.nsens, nm,
              st.temp_limit_mc / 1000.0);
        else
          FMT(sub, sizeof sub, _("%d sensors  ·  highest %s"), st.nsens, nm);
      }
      break;
    }
  }
  gtk_label_set_text(GTK_LABEL(page_sub), sub);

  gboolean has_graph = !(sel == DEV_GPU && !st.gpu_measured);
  gtk_widget_set_visible(graph_area, has_graph);
  gtk_widget_set_visible(cores_area, has_graph && sel == DEV_CPU && st.ncore > 0);
  gtk_widget_set_visible(sens_scroll, has_graph && sel == DEV_TEMP);
  gtk_widget_set_visible(note_label, !has_graph);
  if (!has_graph)
    gtk_label_set_text(GTK_LABEL(note_label),
                       _("This driver does not report GPU utilisation"));

  char vbuf[NST][80];
  char lbuf[NST][96];
  const char *vals[NST];
  const char *labs[NST];
  int n = 0;
  double lastv = series_value(sel, WINLEN - 1, WINLEN);
  if (lastv < 0)
    lastv = 0;

  if (sel == DEV_CPU) {
    snprintf(vbuf[0], 80, "%.0f%%", st.cpu_pct);
    lbuf[0][0] = 0;
    snprintf(lbuf[0], 96, _("Load"));
    n++;
    if (st.freq_ghz > 0)
      FMT(vbuf[1], 80, _("%.2f GHz"), st.freq_ghz);
    else
      snprintf(vbuf[1], 80, _("Unavailable"));
    snprintf(lbuf[1], 96, _("Speed"));
    n++;
    FMT(vbuf[2], 80, _("%d processes · %d cores"), st.nproc, st.ncore);
    snprintf(lbuf[2], 96, _("System processor"));
    n++;
    snprintf(vbuf[3], 80, "%llu:%02llu:%02llu", st.uptime_s / 3600, (st.uptime_s % 3600) / 60,
             st.uptime_s % 60);
    FMT(lbuf[3], 96, _("Uptime · %.2f average"), st.load1);
    n++;
  } else if (sel == DEV_MEM) {
    double used = (st.mem_total_kb - st.mem_avail_kb) / 1048576.0;
    double tot = st.mem_total_kb / 1048576.0;
    double avail = st.mem_avail_kb / 1048576.0;
    snprintf(vbuf[0], 80, "%.0f%%", tot > 0 ? used / tot * 100.0 : 0.0);
    snprintf(lbuf[0], 96, _("Load"));
    n++;
    FMT(vbuf[1], 80, _("%.2f / %.2f GiB"), used, tot);
    snprintf(lbuf[1], 96, _("In use / Total"));
    n++;
    FMT(vbuf[2], 80, _("%.2f GiB"), avail);
    snprintf(lbuf[2], 96, _("Available"));
    n++;
    FMT(vbuf[3], 80, _("%.2f / %.2f GiB"), st.swap_used_kb / 1048576.0,
        st.swap_total_kb / 1048576.0);
    snprintf(lbuf[3], 96, _("Paging file"));
    n++;
  } else if (sel == DEV_DISK) {
    snprintf(vbuf[0], 80, "%.0f%%", st.disk_util_pct);
    snprintf(lbuf[0], 96, _("Load"));
    n++;
    fmt(U_RATE, st.disk_rd_mbps, vbuf[1], 80);
    snprintf(lbuf[1], 96, _("Read"));
    n++;
    fmt(U_RATE, st.disk_wr_mbps, vbuf[2], 80);
    snprintf(lbuf[2], 96, _("Write"));
    n++;
    FMT(vbuf[3], 80, _("%.0f GB"), (double)st.disk_capacity_bytes / 1e9);
    snprintf(lbuf[3], 96, _("Capacity"));
    n++;
  } else if (sel == DEV_NET) {
    fmt(U_RATE, st.net_rx_mbps, vbuf[0], 80);
    snprintf(lbuf[0], 96, _("Receive"));
    n++;
    fmt(U_RATE, st.net_tx_mbps, vbuf[1], 80);
    snprintf(lbuf[1], 96, _("Send"));
    n++;
    fmt(U_RATE, st.net_rx_mbps + st.net_tx_mbps, vbuf[2], 80);
    snprintf(lbuf[2], 96, _("Total"));
    n++;
    FMT(vbuf[3], 80, _("%.1f GiB received · %.1f GiB sent"),
        (double)st.net_rx_total / 1073741824.0, (double)st.net_tx_total / 1073741824.0);
    snprintf(lbuf[3], 96, _("Interface totals"));
    n++;
  } else if (sel == DEV_TEMP) {
    if (st.nsens == 0) {
      /* The row outlived its driver: say so rather than print 0 °C, which is
         the temperature of nothing at all. */
      snprintf(vbuf[0], 80, "%s", "—");
      snprintf(lbuf[0], 96, _("Sensors"));
      n++;
    } else {
      FMT(vbuf[0], 80, _("%.0f °C"), st.temp_max_mc / 1000.0);
      snprintf(lbuf[0], 96, _("Highest"));
      n++;
      /* Only the columns the machine can actually answer. A limit line reading "—"
         next to a graph that already draws the limit would be decoration. */
      if (st.cpu_temp_mc > 0) {
        FMT(vbuf[n], 80, _("%.0f °C"), st.cpu_temp_mc / 1000.0);
        snprintf(lbuf[n], 96, _("Processor"));
        n++;
      }
      if (st.temp_limit_mc > 0) {
        FMT(vbuf[n], 80, _("%.0f °C"), st.temp_limit_mc / 1000.0);
        snprintf(lbuf[n], 96, _("Limit"));
        n++;
      }
      FMT(vbuf[n], 80, _("%d readable"), st.nsens);
      snprintf(lbuf[n], 96, _("Sensors"));
      n++;
    }
    sync_sens_rows();
  } else {
    /* A driver that answers nothing gets the word "unavailable", not a zero:
       the number would look exactly like an idle graphics chip. */
    if (st.gpu_measured)
      snprintf(vbuf[0], 80, "%.0f%%", st.gpu_pct);
    else
      snprintf(vbuf[0], 80, _("Unavailable"));
    snprintf(lbuf[0], 96, _("Load"));
    n++;
    if (st.gpu_temp_mc > 0)
      FMT(vbuf[1], 80, _("%.0f °C"), st.gpu_temp_mc / 1000.0);
    else
      snprintf(vbuf[1], 80, _("Unavailable"));
    snprintf(lbuf[1], 96, _("Temperature"));
    n++;
    if (st.gpu_vram_total > 0) {
      FMT(vbuf[2], 80, _("%.2f / %.2f GiB"), st.gpu_vram_used / 1073741824.0,
          st.gpu_vram_total / 1073741824.0);
      snprintf(lbuf[2], 96, _("Dedicated memory"));
      n++;
    }
  }
  for (int i = 0; i < n; i++) {
    vals[i] = vbuf[i];
    labs[i] = lbuf[i];
  }
  set_stats(n, vals, labs);

  gtk_widget_queue_draw(graph_area);
  gtk_widget_queue_draw(cores_area);
}

static void on_row_selected(GtkListBox *b, GtkListBoxRow *row, gpointer data) {
  (void)b;
  (void)data;
  if (!row)
    return;
  int idx = gtk_list_box_row_get_index(row);
  if (idx < 0 || idx >= nvisible)
    return;
  sel = row_to_dev[idx];
  hover_px = -1;
  update_panel();
}

/* ---------- end task ---------- */

/* Windows' End task is one hard terminate. Here it is two signals with a pause
   between them, because what a person usually means by "end task" is "this
   stopped answering", and a process that could still clean up after itself
   should get the second it needs. The pause is a timer, not a sleep: the 1 s
   tick and the redraws keep running through it. */
#define END_GRACE_MS 3000

static GtkWidget *end_menu, *end_item;

/* The row the menu was opened on, copied out of the snapshot: the list under the
   pointer is rebuilt from /proc every few seconds, and the process may be gone
   by the time the answer comes back. */
static struct {
  int pid;
  char name[32];
  uid_t uid;
  int own;
  char state;
  double mem_mb;
  unsigned long long start_epoch;
} target;

/* The line under the table is where the Startup tab reports a write that did not
   happen; the same convention here, because a click that changed nothing is the
   one thing this program must not leave unexplained. */
static void end_task_say(const char *text) {
  GtkWidget *note = tab == TAB_DETAILS ? det_note : proc_note;
  if (note)
    gtk_label_set_text(GTK_LABEL(note), text);
}

/* Why the command cannot be used on this row, or NULL for "it can". The answer
   goes into the menu item's own text rather than a tooltip: an insensitive menu
   item is sent no motion events, so GTK shows no tooltip for it — and a
   greyed-out command that says nothing is what this program does not show. */
static const char *end_task_refusal(void) {
  static char why[96];
  if (target.pid == getpid()) {
    snprintf(why, sizeof why, "%s", _("this is the task manager itself"));
    return why;
  }
  if (!target.own) {
    char user[24];
    stats_user_name(target.uid, user, sizeof user);
    snprintf(why, sizeof why, _("it is %s's process"), user);
    return why;
  }
  if (target.state == 'Z') {
    snprintf(why, sizeof why, "%s", _("it has already finished"));
    return why;
  }
  return NULL;
}

static void end_menu_prepare(void) {
  const char *why = end_task_refusal();
  char label[200];
  if (why)
    snprintf(label, sizeof label, _("%s — %s"), _("End task"), why);
  else
    snprintf(label, sizeof label, "%s", _("End task"));
  gtk_menu_item_set_label(GTK_MENU_ITEM(end_item), label);
  gtk_widget_set_sensitive(end_item, !why);
}

typedef struct {
  int pid;
  unsigned long long start_epoch;
  char name[32];
} EndWait;

static gboolean end_task_grace(gpointer p) {
  EndWait *w = p;
  /* Three ways there is nothing left to do: the pid is gone, the number has been
     handed to a different process since, or what is left is a zombie — which
     takes no more notice of SIGKILL than it did of SIGTERM. */
  if (stats_pid_same(w->pid, w->start_epoch, NULL) && kill(w->pid, SIGKILL) != 0 &&
      errno == EPERM) {
    char m[200];
    snprintf(m, sizeof m, _("“%s” did not answer and cannot be terminated."), w->name);
    end_task_say(m);
  }
  g_free(w);
  return G_SOURCE_REMOVE;
}

static void end_task_send(void) {
  if (kill(target.pid, SIGTERM) == 0) {
    EndWait *w = g_new0(EndWait, 1);
    w->pid = target.pid;
    w->start_epoch = target.start_epoch;
    g_strlcpy(w->name, target.name, sizeof w->name);
    g_timeout_add(END_GRACE_MS, end_task_grace, w);
    return; /* silence: the row leaving the list is the answer */
  }
  char m[224];
  if (errno == ESRCH)
    snprintf(m, sizeof m, _("“%s” had already ended."), target.name);
  else
    snprintf(m, sizeof m, _("Could not signal “%s”: %s"), target.name, g_strerror(errno));
  end_task_say(m);
}

static void ask_end_task(void) {
  char user[24], mem[24], info[128], body[400];
  stats_user_name(target.uid, user, sizeof user);
  fmt_mem(target.mem_mb, mem, sizeof mem);
  snprintf(info, sizeof info, _("PID %d  ·  %s  ·  %s"), target.pid, user, mem);
  snprintf(body, sizeof body,
           _("%s\n\nThe process gets no chance to save what it has open. It is asked to exit "
             "first, and if it is still there in %d s it is terminated."),
           info, END_GRACE_MS / 1000);

  GtkWidget *d = gtk_message_dialog_new(GTK_WINDOW(window), GTK_DIALOG_MODAL, GTK_MESSAGE_WARNING,
                                        GTK_BUTTONS_NONE, _("End “%s”?"), target.name);
  gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(d), "%s", body);
  gtk_dialog_add_button(GTK_DIALOG(d), _("_Cancel"), GTK_RESPONSE_CANCEL);
  gtk_dialog_add_button(GTK_DIALOG(d), _("_End task"), GTK_RESPONSE_ACCEPT);
  /* The default is the answer that changes nothing, so Enter and Escape both back
     out and the destructive button has to be aimed at. */
  gtk_dialog_set_default_response(GTK_DIALOG(d), GTK_RESPONSE_CANCEL);
  gtk_window_set_position(GTK_WINDOW(d), GTK_WIN_POS_CENTER_ON_PARENT);
  gint res = gtk_dialog_run(GTK_DIALOG(d));
  gtk_widget_destroy(d);
  if (res == GTK_RESPONSE_ACCEPT)
    end_task_send();
}

static void on_end_activated(GtkMenuItem *mi, gpointer u) {
  (void)mi;
  (void)u;
  ask_end_task();
}

static ProcRow *find_proc(int pid) {
  for (int i = 0; i < pt.n; i++)
    if (pt.rows[i].pid == pid)
      return &pt.rows[i];
  return NULL;
}

static int target_from_pid(int pid) {
  ProcRow *r = pid > 0 ? find_proc(pid) : NULL;
  if (!r)
    return 0; /* the sweep no longer sees it: there is no row to act on */
  target.pid = r->pid;
  g_strlcpy(target.name, r->name, sizeof target.name);
  target.uid = r->uid;
  target.own = r->own;
  target.state = r->state;
  target.mem_mb = r->mem_mb;
  target.start_epoch = r->start_epoch;
  return 1;
}

/* The two tables keep the pid in different columns and different types; the
   rest of this code does not care which table it was asked from. */
static int row_pid(GtkWidget *tree, GtkTreeModel *m, GtkTreeIter *it) {
  if (tree == proc_tree) {
    int p = 0;
    gtk_tree_model_get(m, it, C_PID, &p, -1);
    return p;
  }
  double d = 0;
  gtk_tree_model_get(m, it, D_PID, &d, -1);
  return (int)d;
}

static void end_menu_popup(GtkWidget *w, GdkEvent *ev) {
  end_menu_prepare();
  if (ev)
    gtk_menu_popup_at_pointer(GTK_MENU(end_menu), ev);
  else
    gtk_menu_popup_at_widget(GTK_MENU(end_menu), w, GDK_GRAVITY_SOUTH_WEST, GDK_GRAVITY_NORTH_WEST,
                             NULL);
}

/* The right button selects the row it lands on before it asks anything about it:
   a command that acts on a row the pointer is merely over, without marking it,
   is how people end the wrong process. */
static gboolean on_tree_button(GtkWidget *w, GdkEventButton *ev, gpointer u) {
  (void)u;
  if (ev->button != 3 || ev->type != GDK_BUTTON_PRESS)
    return FALSE;
  GtkTreeView *tv = GTK_TREE_VIEW(w);
  GtkTreePath *path = NULL;
  if (!gtk_tree_view_get_path_at_pos(tv, (int)ev->x, (int)ev->y, &path, NULL, NULL, NULL))
    return FALSE; /* the header, or the empty space under the last row */
  GtkTreeModel *m = gtk_tree_view_get_model(tv);
  GtkTreeIter it;
  gtk_tree_model_get_iter(m, &it, path);
  gtk_tree_path_free(path);
  if (!target_from_pid(row_pid(w, m, &it)))
    return FALSE;
  gtk_tree_selection_select_iter(gtk_tree_view_get_selection(tv), &it);
  end_menu_popup(w, (GdkEvent *)ev);
  return TRUE;
}

/* The Menu key, for the same command on the row the caret is on: a destructive
   action that only answers to a mouse is not reachable from a keyboard. */
static gboolean on_tree_menu_key(GtkWidget *w, gpointer u) {
  (void)u;
  GtkTreeSelection *sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(w));
  GtkTreeModel *m = NULL;
  GtkTreeIter it;
  if (!gtk_tree_selection_get_selected(sel, &m, &it) || !target_from_pid(row_pid(w, m, &it)))
    return FALSE;
  end_menu_popup(w, NULL);
  return TRUE;
}

static void build_end_menu(void) {
  end_menu = gtk_menu_new();
  end_item = gtk_menu_item_new_with_label(_("End task"));
  gtk_menu_shell_append(GTK_MENU_SHELL(end_menu), end_item);
  g_signal_connect(end_item, "activate", G_CALLBACK(on_end_activated), NULL);
  gtk_widget_show(end_item);

  /* Both tables list processes, so both get the command; the row is the same
     process either way. */
  GtkWidget *trees[2] = {proc_tree, det_tree};
  for (int i = 0; i < 2; i++) {
    gtk_widget_add_events(trees[i], GDK_BUTTON_PRESS_MASK);
    g_signal_connect(trees[i], "button-press-event", G_CALLBACK(on_tree_button), NULL);
    g_signal_connect(trees[i], "popup-menu", G_CALLBACK(on_tree_menu_key), NULL);
  }
}

/* ---------- left rail ---------- */

static gboolean on_draw_rail(GtkWidget *w, cairo_t *cr, gpointer data) {
  (void)data;
  GtkAllocation a;
  gtk_widget_get_allocation(w, &a);
  cairo_set_source_rgb(cr, 0.118, 0.118, 0.118);
  cairo_paint(cr);

  const double cy0 = RAIL_Y0;
  for (int i = 0; i < RAIL_ICONS; i++) {
    double cx = a.width / 2.0;
    double cy = cy0 + i * RAIL_PITCH;
    gboolean active = (i == tab);
    cairo_set_line_width(cr, 1.4);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    if (active) {
      cairo_set_source_rgb(cr, accent[0], accent[1], accent[2]);
      cairo_rectangle(cr, 1.5, cy - 10, 3, 20);
      cairo_fill(cr);
      cairo_set_source_rgb(cr, 0.95, 0.95, 0.95);
    } else {
      cairo_set_source_rgb(cr, 0.40, 0.40, 0.40);
    }
    switch (i) {
      case 0:
        for (int r = 0; r < 3; r++) {
          cairo_move_to(cr, cx - 7, cy - 6 + r * 6);
          cairo_line_to(cr, cx + 7, cy - 6 + r * 6);
        }
        break;
      case 1:
        cairo_move_to(cr, cx - 7, cy + 5);
        cairo_line_to(cr, cx - 3, cy - 2);
        cairo_line_to(cr, cx + 1, cy + 2);
        cairo_line_to(cr, cx + 7, cy - 6);
        break;
      case 2:
        /* A power glyph with the top of the ring left open: this tab is about
           what starts by itself, and the same shape is what the switch in the
           list turns on and off. */
        cairo_arc(cr, cx, cy + 1, 6, -M_PI / 2 + 0.55, -M_PI / 2 + 2 * M_PI - 0.55);
        cairo_move_to(cr, cx, cy - 7);
        cairo_line_to(cr, cx, cy - 1);
        break;
      case 3:
        cairo_arc(cr, cx, cy - 3, 3.2, 0, 2 * M_PI);
        cairo_move_to(cr, cx + 5.5, cy + 7);
        cairo_arc(cr, cx, cy + 7, 5.5, M_PI, 2 * M_PI);
        break;
      case 4:
        /* An "i" in a circle: this tab answers questions about the machine
           rather than drawing a curve about them. */
        cairo_arc(cr, cx, cy, 7, 0, 2 * M_PI);
        cairo_new_sub_path(cr);
        cairo_arc(cr, cx, cy - 3.7, 0.9, 0, 2 * M_PI);
        cairo_move_to(cr, cx, cy - 1.2);
        cairo_line_to(cr, cx, cy + 3.8);
        break;
    }
    cairo_stroke(cr);
  }
  return FALSE;
}

/* The rail is one drawing area, so the icons are hit-tested by hand. The five
   icons are the five tabs, in order. Clicking the icon of the tab you are
   already on folds the device list away and back. */
static gboolean on_rail_click(GtkWidget *w, GdkEventButton *ev, gpointer data) {
  (void)w;
  (void)data;
  if (ev->button != 1)
    return FALSE;
  int i = (int)lround((ev->y - RAIL_Y0) / RAIL_PITCH);
  if (i < 0 || i >= RAIL_ICONS || fabs(ev->y - (RAIL_Y0 + i * RAIL_PITCH)) > RAIL_PITCH / 2.0)
    return FALSE;
  if (i == tab && tab == TAB_PERF) {
    gboolean shown = gtk_widget_get_visible(dev_list);
    gtk_widget_set_visible(dev_list, !shown);
    gtk_widget_set_visible(dev_sep, !shown);
    return TRUE;
  }
  set_tab(i);
  return TRUE;
}

/* ---------- sampling ---------- */

static gboolean on_tick(gpointer data) {
  (void)data;
  stats_sample(&st);

  push_sample(DEV_CPU, st.cpu_pct);
  push_sample(DEV_MEM, st.mem_total_kb > 0 ? (st.mem_total_kb - st.mem_avail_kb) / st.mem_total_kb * 100.0 : 0);
  push_sample(DEV_DISK, st.disk_rd_mbps + st.disk_wr_mbps);
  push_sample(DEV_NET, st.net_rx_mbps + st.net_tx_mbps);
  push_sample(DEV_GPU, st.gpu_pct);
  /* A driver can be unloaded mid-session; a gap in the curve says that better
     than a line down to zero, which would read as the machine cooling off. */
  push_sample(DEV_TEMP, st.nsens ? st.temp_max_mc / 1000.0 : -1);

  for (int i = 0; i < st.ncore && i < MAXCORE; i++)
    chist[i][chn % HIST] = st.core_pct[i];
  chn++;

  /* The table is the expensive part, so it lags the graph by a few seconds and
     is only swept while the process tab is on screen. The history keeps
     filling either way, so switching back shows no hole in the curve. */
  if (tab == TAB_PROC && --proc_countdown <= 0) {
    proc_countdown = PROC_EVERY;
    stats_sample_procs(&pt, 0);
    sync_proc_rows();
  } else if (tab == TAB_DETAILS && --det_countdown <= 0) {
    det_countdown = DET_EVERY;
    stats_sample_procs(&pt, 1);
    sync_det_rows();
  }

  update_panel();

  return G_SOURCE_CONTINUE;
}

/* ---------- ui construction ---------- */

static const char *CSS =
    "window { background:#1e1e1e; color:#f2f2f2; }"
    "headerbar { background:#272727; min-height:38px; border-bottom:1px solid #141414; }"
    ".page_title { font-size:20px; font-weight:600; }"
    ".page_sub { font-size:11px; color:#a4a4a4; }"
    ".card { background:#252525; border:1px solid #141414; border-radius:6px; }"
    ".statv { font-size:12px; font-weight:600; }"
    ".statl { font-size:11px; color:#a4a4a4; }"
    ".note { font-size:11px; color:#a4a4a4; }"
    "list, row { background:#1e1e1e; color:#f2f2f2; border:0; }"
    "row { padding:7px 10px; font-size:12px; border-radius:4px; margin:2px 6px; }"
    "row:hover { background:#2b2b2b; }"
    "row:selected { background:#2f2f2f; color:#ffffff; }"
    "separator { background:#141414; min-width:1px; }"
    "scrolledwindow { background:#1e1e1e; border:0; }"
    "treeview.view { background:#1e1e1e; color:#f2f2f2; font-size:12px; }"
    "treeview.view:selected { background:#2f2f2f; color:#ffffff; }"
    "treeview.view header button { background:#252525; color:#a4a4a4; border:0; "
    "border-bottom:1px solid #141414; padding:5px 8px; font-size:11px; font-weight:600; }"
    "treeview.view header button:hover { color:#f2f2f2; }"
    /* The sensor table sits inside the card, so it takes the card's grey rather
       than the page's; the rules come after the page-wide ones on purpose. */
    "scrolledwindow.sens { background:#252525; }"
    "treeview.view.sens { background:#252525; }"
    /* Scoped to the switch: a bare `button` rule would restyle the window
       controls in the header bar too. */
    "button.toggleswitch { background:#2f2f2f; color:#f2f2f2; border:1px solid #3d3d3d; "
    "border-radius:4px; padding:4px 14px; font-size:12px; }"
    "button.toggleswitch:hover { background:#383838; border-color:#4a4a4a; }"
    "button.toggleswitch:disabled { background:#272727; color:#767676; border-color:#313131; }"
    "scrollbar { background:#1e1e1e; border:0; }"
    "scrollbar slider { background:#3d3d3d; min-width:9px; min-height:9px; border-radius:5px; }"
    "scrollbar slider:hover { background:#4d4d4d; }"
    /* A popup window and a dialog are their own top-levels, so nothing in the
       rules above reaches them: without these the menu arrives in the system's
       light palette and floats over a window that is #1e1e1e. */
    "menu { background:#252525; color:#f2f2f2; border:1px solid #141414; padding:3px; }"
    "menuitem { padding:6px 12px; font-size:12px; color:#f2f2f2; border-radius:4px; }"
    "menuitem:hover { background:#2f2f2f; }"
    "menuitem:disabled { color:#767676; }"
    "menuitem:disabled label { color:#767676; }"
    "dialog { background:#1e1e1e; color:#f2f2f2; }"
    "dialog label { color:#f2f2f2; }"
    "dialog .titlebar { background:#272727; color:#f2f2f2; }"
    "dialog button { background:#2f2f2f; color:#f2f2f2; border:1px solid #3d3d3d; "
    "border-radius:4px; padding:5px 14px; }"
    "dialog button:hover { background:#383838; border-color:#4a4a4a; }";

/* One 16×16 glyph per device, drawn the way the rail icons are: a 1.4 px round
   line, no font and no icon theme, so nothing on this list can come out as a
   box because the system happens to have no emoji installed. The colour stays
   under the label's own brightness — the text is what is being read. */
/* An ellipse as four beziers rather than a scaled arc, so the stroke keeps one
   width all the way round. */
static void icon_ellipse(cairo_t *cr, double cx, double cy, double rx, double ry) {
  const double k = 0.5522847498;
  cairo_move_to(cr, cx, cy - ry);
  cairo_curve_to(cr, cx + rx * k, cy - ry, cx + rx, cy - ry * k, cx + rx, cy);
  cairo_curve_to(cr, cx + rx, cy + ry * k, cx + rx * k, cy + ry, cx, cy + ry);
  cairo_curve_to(cr, cx - rx * k, cy + ry, cx - rx, cy + ry * k, cx - rx, cy);
  cairo_curve_to(cr, cx - rx, cy - ry * k, cx - rx * k, cy - ry, cx, cy - ry);
}

static void icon_ellipse_bottom(cairo_t *cr, double cx, double cy, double rx, double ry) {
  const double k = 0.5522847498;
  cairo_move_to(cr, cx - rx, cy);
  cairo_curve_to(cr, cx - rx, cy + ry * k, cx - rx * k, cy + ry, cx, cy + ry);
  cairo_curve_to(cr, cx + rx * k, cy + ry, cx + rx, cy + ry * k, cx + rx, cy);
}

static void draw_dev_icon(cairo_t *cr, int dev, int wifi) {
  switch (dev) {
    case DEV_CPU: /* a chip, three pins on each side */
      cairo_rectangle(cr, 4.5, 4.5, 7, 7);
      for (int k = 0; k < 3; k++) {
        double p = 6 + k * 2;
        cairo_move_to(cr, p, 4.5);
        cairo_line_to(cr, p, 2.2);
        cairo_move_to(cr, p, 11.5);
        cairo_line_to(cr, p, 13.8);
        cairo_move_to(cr, 4.5, p);
        cairo_line_to(cr, 2.2, p);
        cairo_move_to(cr, 11.5, p);
        cairo_line_to(cr, 13.8, p);
      }
      break;
    case DEV_MEM: /* a module: body, its chips, the rail under it */
      cairo_rectangle(cr, 2.5, 4.2, 11, 6);
      for (int k = 0; k < 3; k++) {
        cairo_move_to(cr, 5 + k * 3, 4.2);
        cairo_line_to(cr, 5 + k * 3, 10.2);
      }
      cairo_move_to(cr, 3.5, 12.6);
      cairo_line_to(cr, 12.5, 12.6);
      break;
    case DEV_DISK: /* a drive: the stack, not a clock face */
      icon_ellipse(cr, 8, 4.9, 5.2, 2.2);
      cairo_new_sub_path(cr);
      cairo_move_to(cr, 2.8, 4.9);
      cairo_line_to(cr, 2.8, 11.1);
      cairo_new_sub_path(cr);
      cairo_move_to(cr, 13.2, 4.9);
      cairo_line_to(cr, 13.2, 11.1);
      cairo_new_sub_path(cr);
      icon_ellipse_bottom(cr, 8, 11.1, 5.2, 2.2);
      break;
    case DEV_NET:
      if (wifi) { /* the aerial point and the three waves over it */
        for (int k = 0; k < 3; k++) {
          cairo_new_sub_path(cr);
          cairo_arc(cr, 8, 12.4, 2.6 + k * 2.5, -M_PI * 0.78, -M_PI * 0.22);
        }
        cairo_new_sub_path(cr);
        cairo_arc(cr, 8, 12.4, 1.1, 0, 2 * M_PI);
      } else { /* a plug with two prongs and the cable leaving it */
        cairo_rectangle(cr, 4.5, 4.6, 7, 4.4);
        cairo_move_to(cr, 6.5, 4.6);
        cairo_line_to(cr, 6.5, 2.4);
        cairo_move_to(cr, 9.5, 4.6);
        cairo_line_to(cr, 9.5, 2.4);
        cairo_move_to(cr, 8, 9);
        cairo_line_to(cr, 8, 11.4);
        cairo_line_to(cr, 11.2, 13.6);
      }
      break;
    case DEV_GPU: /* a monitor on a stand */
      cairo_rectangle(cr, 2.5, 3.4, 11, 7.6);
      cairo_move_to(cr, 8, 11);
      cairo_line_to(cr, 8, 13);
      cairo_move_to(cr, 5.2, 13.6);
      cairo_line_to(cr, 10.8, 13.6);
      break;
    default: /* a thermometer: bulb, stem, mercury, one scale tick */
      cairo_arc(cr, 8, 11.3, 2.9, 0, 2 * M_PI);
      cairo_new_sub_path(cr);
      cairo_move_to(cr, 6.7, 9.1);
      cairo_line_to(cr, 6.7, 3.9);
      cairo_new_sub_path(cr);
      cairo_arc(cr, 8, 3.9, 1.3, M_PI, 2 * M_PI);
      cairo_new_sub_path(cr);
      cairo_move_to(cr, 9.3, 3.9);
      cairo_line_to(cr, 9.3, 9.1);
      cairo_new_sub_path(cr);
      cairo_move_to(cr, 8, 8.6);
      cairo_line_to(cr, 8, 5.6);
      cairo_new_sub_path(cr);
      cairo_move_to(cr, 10.6, 5.2);
      cairo_line_to(cr, 12.3, 5.2);
      break;
  }
}

static gboolean on_draw_dev_icon(GtkWidget *w, cairo_t *cr, gpointer data) {
  (void)data;
  int dev = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(w), "dev"));
  int wifi = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(w), "wifi"));
  GtkStateFlags f = gtk_widget_get_state_flags(w);
  double v = (f & GTK_STATE_FLAG_SELECTED) ? 0.90 : (f & GTK_STATE_FLAG_PRELIGHT) ? 0.72 : 0.58;
  cairo_set_line_width(cr, 1.4);
  cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
  cairo_set_source_rgb(cr, v, v, v);
  draw_dev_icon(cr, dev, wifi);
  cairo_stroke(cr);
  return FALSE;
}

static void build_device_list(void) {
  /* The network glyph follows the interface the row names, not whatever carries
     the default route when the list is painted: the label is written once from
     this same snapshot, and the two have to agree. */
  int wifi = 0;
  if (st.net_if[0]) {
    char p[128];
    snprintf(p, sizeof p, "/sys/class/net/%s/wireless", st.net_if);
    wifi = access(p, F_OK) == 0;
  }
  for (int d = 0; d < NDEV; d++) {
    gboolean show;
    if (d == DEV_DISK)
      show = st.disk_name[0] != '\0';
    else if (d == DEV_NET)
      show = st.net_if[0] != '\0';
    else if (d == DEV_GPU)
      show = st.gpu_present;
    else if (d == DEV_TEMP)
      show = st.nsens > 0; /* no sensor readable, no row: a blank line would be the bug */
    else
      show = TRUE;
    if (!show)
      continue;
    char b[160];
    if (d == DEV_DISK)
      snprintf(b, sizeof b, "%s (%s)", _(dev_name[d]), st.disk_name);
    else if (d == DEV_NET)
      snprintf(b, sizeof b, "%s (%s)", _(dev_name[d]), st.net_if);
    else
      snprintf(b, sizeof b, "%s", _(dev_name[d]));
    GtkWidget *l = gtk_label_new(b);
    gtk_widget_set_halign(l, GTK_ALIGN_START);
    GtkWidget *ic = gtk_drawing_area_new();
    gtk_widget_set_size_request(ic, 16, 16);
    gtk_widget_set_valign(ic, GTK_ALIGN_CENTER);
    g_object_set_data(G_OBJECT(ic), "dev", GINT_TO_POINTER(d));
    g_object_set_data(G_OBJECT(ic), "wifi", GINT_TO_POINTER(wifi));
    g_signal_connect(ic, "draw", G_CALLBACK(on_draw_dev_icon), NULL);
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_pack_start(GTK_BOX(box), ic, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), l, FALSE, FALSE, 0);
    GtkWidget *row = gtk_list_box_row_new();
    gtk_container_add(GTK_CONTAINER(row), box);
    gtk_list_box_insert(GTK_LIST_BOX(dev_list), row, -1);
    row_to_dev[nvisible++] = d;
  }
}

int main(int argc, char **argv) {
  init_i18n();
  gtk_init(&argc, &argv);

  const char *want = NULL;
  for (int i = 1; i < argc; i++)
    if (!strncmp(argv[i], "--device=", 9))
      want = argv[i] + 9;

  stats_init(&st);

  GtkCssProvider *prov = gtk_css_provider_new();
  gtk_css_provider_load_from_data(prov, CSS, -1, NULL);
  gtk_style_context_add_provider_for_screen(gdk_screen_get_default(), GTK_STYLE_PROVIDER(prov),
                                            GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
  g_object_unref(prov);

  window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
  gtk_window_set_title(GTK_WINDOW(window), "TaskManagerOK");
  gtk_window_set_default_size(GTK_WINDOW(window), 920, 620);
  gtk_window_set_position(GTK_WINDOW(window), GTK_WIN_POS_CENTER);

  GtkWidget *hb = gtk_header_bar_new();
  gtk_header_bar_set_show_close_button(GTK_HEADER_BAR(hb), TRUE);
  GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *h1 = gtk_label_new("TaskManagerOK");
  gtk_widget_set_halign(h1, GTK_ALIGN_START);
  head_sub = gtk_label_new(_("Performance"));
  gtk_widget_set_halign(head_sub, GTK_ALIGN_START);
  gtk_style_context_add_class(gtk_widget_get_style_context(head_sub), "page_sub");
  gtk_box_pack_start(GTK_BOX(hbox), h1, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(hbox), head_sub, FALSE, FALSE, 0);
  gtk_header_bar_set_custom_title(GTK_HEADER_BAR(hb), hbox);
  gtk_window_set_titlebar(GTK_WINDOW(window), hb);

  GtkWidget *body = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_container_add(GTK_CONTAINER(window), body);

  rail = gtk_drawing_area_new();
  gtk_widget_set_size_request(rail, 44, -1);
  gtk_widget_add_events(rail, GDK_BUTTON_PRESS_MASK);
  g_signal_connect(rail, "draw", G_CALLBACK(on_draw_rail), NULL);
  g_signal_connect(rail, "button-press-event", G_CALLBACK(on_rail_click), NULL);
  gtk_box_pack_start(GTK_BOX(body), rail, FALSE, FALSE, 0);

  dev_list = gtk_list_box_new();
  gtk_list_box_set_selection_mode(GTK_LIST_BOX(dev_list), GTK_SELECTION_SINGLE);
  gtk_widget_set_size_request(dev_list, 196, -1);
  g_signal_connect(dev_list, "row-selected", G_CALLBACK(on_row_selected), NULL);
  gtk_box_pack_start(GTK_BOX(body), dev_list, FALSE, FALSE, 0);
  build_device_list();

  dev_sep = gtk_separator_new(GTK_ORIENTATION_VERTICAL);
  gtk_box_pack_start(GTK_BOX(body), dev_sep, FALSE, FALSE, 0);

  /* The Information tab's section list, same width and same widget as the
     device list, because it does the same job: pick one of several things and
     the card on the right shows it. */
  info_list = gtk_list_box_new();
  gtk_list_box_set_selection_mode(GTK_LIST_BOX(info_list), GTK_SELECTION_SINGLE);
  gtk_widget_set_size_request(info_list, 196, -1);
  gtk_box_pack_start(GTK_BOX(body), info_list, FALSE, FALSE, 0);
  for (int i = 0; i < INFO_SECTIONS; i++) {
    GtkWidget *l = gtk_label_new(_(info_sec_name[i]));
    gtk_widget_set_halign(l, GTK_ALIGN_START);
    GtkWidget *row = gtk_list_box_row_new();
    gtk_container_add(GTK_CONTAINER(row), l);
    gtk_list_box_insert(GTK_LIST_BOX(info_list), row, -1);
  }
  g_signal_connect(info_list, "row-selected", G_CALLBACK(on_info_row_selected), NULL);

  info_sep = gtk_separator_new(GTK_ORIENTATION_VERTICAL);
  gtk_box_pack_start(GTK_BOX(body), info_sep, FALSE, FALSE, 0);

  GtkWidget *panel = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
  gtk_widget_set_margin_start(panel, 14);
  gtk_widget_set_margin_end(panel, 14);
  gtk_widget_set_margin_top(panel, 10);
  gtk_widget_set_margin_bottom(panel, 10);
  gtk_box_pack_start(GTK_BOX(body), panel, TRUE, TRUE, 0);

  page_title = gtk_label_new(_(dev_name[sel]));
  gtk_widget_set_halign(page_title, GTK_ALIGN_START);
  gtk_style_context_add_class(gtk_widget_get_style_context(page_title), "page_title");
  gtk_box_pack_start(GTK_BOX(panel), page_title, FALSE, FALSE, 0);

  page_sub = gtk_label_new("");
  gtk_widget_set_halign(page_sub, GTK_ALIGN_START);
  gtk_style_context_add_class(gtk_widget_get_style_context(page_sub), "page_sub");
  gtk_box_pack_start(GTK_BOX(panel), page_sub, FALSE, FALSE, 0);

  /* Packing all five cards into the panel makes all five part of every size
     request. Before the window manager has mapped the window, GTK runs one
     allocation pass at a height that cannot hold five tables, and each table
     then tells its scrollbar to be negative pixels tall — the Gtk-CRITICAL a
     clean start used to print. A stack asks for one child and gives one child
     the space, so that pass only ever sees the tab the user is on. */
  card_stack = gtk_stack_new();
  gtk_stack_set_transition_type(GTK_STACK(card_stack), GTK_STACK_TRANSITION_TYPE_NONE);
  gtk_box_pack_start(GTK_BOX(panel), card_stack, TRUE, TRUE, 0);

  perf_card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_style_context_add_class(gtk_widget_get_style_context(perf_card), "card");
  gtk_widget_set_margin_top(perf_card, 6);
  gtk_stack_add_named(GTK_STACK(card_stack), perf_card, "perf");

  graph_area = gtk_drawing_area_new();
  gtk_widget_set_hexpand(graph_area, TRUE);
  gtk_widget_set_vexpand(graph_area, TRUE);
  gtk_widget_add_events(graph_area, GDK_POINTER_MOTION_MASK | GDK_LEAVE_NOTIFY_MASK);
  g_signal_connect(graph_area, "draw", G_CALLBACK(on_draw_graph), NULL);
  g_signal_connect(graph_area, "motion-notify-event", G_CALLBACK(on_motion), NULL);
  g_signal_connect(graph_area, "leave-notify-event", G_CALLBACK(on_leave), NULL);
  gtk_box_pack_start(GTK_BOX(perf_card), graph_area, TRUE, TRUE, 0);

  cores_area = gtk_drawing_area_new();
  gtk_widget_set_size_request(cores_area, -1, 80);
  g_signal_connect(cores_area, "draw", G_CALLBACK(on_draw_cores), NULL);
  gtk_box_pack_start(GTK_BOX(perf_card), cores_area, FALSE, FALSE, 0);

  /* The sensors a machine reports are not a fixed handful — a desktop with a
     chip reader and an NVMe can show a dozen — so this is a scrolling table
     rather than a strip that would have to hide rows to fit. */
  sens_scroll = gtk_scrolled_window_new(NULL, NULL);
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sens_scroll), GTK_POLICY_NEVER,
                                 GTK_POLICY_AUTOMATIC);
  gtk_widget_set_size_request(sens_scroll, -1, 132);
  sens_store = gtk_list_store_new(TCOL, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING);
  sens_tree = gtk_tree_view_new_with_model(GTK_TREE_MODEL(sens_store));
  g_object_unref(sens_store);
  gtk_style_context_add_class(gtk_widget_get_style_context(sens_tree), "sens");
  gtk_style_context_add_class(gtk_widget_get_style_context(sens_scroll), "sens");
  gtk_container_add(GTK_CONTAINER(sens_scroll), sens_tree);

  static const char *sens_title[TCOL] = {N_("Sensor"), N_("Value"), N_("Limit")};
  static const int sens_w[TCOL] = {220, 90, 90};
  for (int i = 0; i < TCOL; i++) {
    GtkCellRenderer *cr = gtk_cell_renderer_text_new();
    if (i > 0)
      g_object_set(cr, "xalign", 1.0, NULL);
    GtkTreeViewColumn *c = GTK_TREE_VIEW_COLUMN(
        gtk_tree_view_column_new_with_attributes(_(sens_title[i]), cr, "text", i, NULL));
    gtk_tree_view_column_set_min_width(c, sens_w[i]);
    if (i == 0)
      gtk_tree_view_column_set_expand(c, TRUE);
    gtk_tree_view_append_column(GTK_TREE_VIEW(sens_tree), c);
  }
  gtk_box_pack_start(GTK_BOX(perf_card), sens_scroll, FALSE, FALSE, 0);

  note_label = gtk_label_new("");
  gtk_widget_set_halign(note_label, GTK_ALIGN_CENTER);
  gtk_widget_set_valign(note_label, GTK_ALIGN_CENTER);
  gtk_style_context_add_class(gtk_widget_get_style_context(note_label), "note");
  gtk_box_pack_start(GTK_BOX(perf_card), note_label, TRUE, TRUE, 0);

  proc_card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_style_context_add_class(gtk_widget_get_style_context(proc_card), "card");
  gtk_widget_set_margin_top(proc_card, 6);
  gtk_stack_add_named(GTK_STACK(card_stack), proc_card, "proc");

  proc_scroll = gtk_scrolled_window_new(NULL, NULL);
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(proc_scroll), GTK_POLICY_NEVER,
                                 GTK_POLICY_AUTOMATIC);
  gtk_widget_set_margin_start(proc_scroll, 8);
  gtk_widget_set_margin_end(proc_scroll, 8);
  gtk_widget_set_margin_top(proc_scroll, 4);
  gtk_box_pack_start(GTK_BOX(proc_card), proc_scroll, TRUE, TRUE, 0);

  proc_store = gtk_list_store_new(NCOL, G_TYPE_STRING, G_TYPE_INT, G_TYPE_DOUBLE, G_TYPE_DOUBLE,
                                  G_TYPE_DOUBLE, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING);
  proc_tree = gtk_tree_view_new_with_model(GTK_TREE_MODEL(proc_store));
  g_object_unref(proc_store);
  gtk_tree_view_set_headers_clickable(GTK_TREE_VIEW(proc_tree), TRUE);
  gtk_container_add(GTK_CONTAINER(proc_scroll), proc_tree);

  static const char *proc_title[4] = {N_("Name"), N_("CPU"), N_("Memory"), N_("Disk")};
  static const int proc_txt[4] = {C_NAME, C_CPU_TXT, C_MEM_TXT, C_DISK_TXT};
  static const int proc_val[4] = {C_NAME, C_CPU, C_MEM, C_DISK};
  static const int proc_w[4] = {240, 76, 100, 100};
  for (int i = 0; i < 4; i++) {
    GtkCellRenderer *cr = gtk_cell_renderer_text_new();
    if (i > 0)
      g_object_set(cr, "xalign", 1.0, NULL); /* numbers read as a column on the right */
    GtkTreeViewColumn *c = GTK_TREE_VIEW_COLUMN(
        gtk_tree_view_column_new_with_attributes(_(proc_title[i]), cr, "text", proc_txt[i], NULL));
    gtk_tree_view_column_set_min_width(c, proc_w[i]);
    /* A column only becomes clickable at all once it carries a sort column id,
       and then GTK does the rest by itself: it moves the arrow, and a click on
       the arrow's own column flips the direction. The orders below are named
       after proc_sort's big-first convention, so the first click on a number
       puts the biggest consumer on top and the first click on a name reads A to
       Z, both under a downward arrow like in Windows. */
    gtk_tree_view_column_set_sort_column_id(c, proc_val[i]);
    gtk_tree_view_column_set_sort_order(c, i == 0 ? GTK_SORT_DESCENDING : GTK_SORT_ASCENDING);
    if (i == 0)
      gtk_tree_view_column_set_expand(c, TRUE);
    gtk_tree_view_append_column(GTK_TREE_VIEW(proc_tree), c);
  }

  GtkTreeSortable *srt = GTK_TREE_SORTABLE(proc_store);
  g_signal_connect(srt, "sort-column-changed", G_CALLBACK(on_sort_changed), NULL);
  for (int i = 0; i < 4; i++)
    gtk_tree_sortable_set_sort_func(srt, proc_val[i], proc_sort, NULL, NULL);
  gtk_tree_sortable_set_sort_column_id(srt, C_CPU, GTK_SORT_ASCENDING);

  proc_note = gtk_label_new(_(proc_note_text));
  gtk_widget_set_halign(proc_note, GTK_ALIGN_START);
  gtk_widget_set_margin_top(proc_note, 6);
  gtk_style_context_add_class(gtk_widget_get_style_context(proc_note), "note");
  gtk_box_pack_start(GTK_BOX(proc_card), proc_note, FALSE, FALSE, 0);

  start_card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_style_context_add_class(gtk_widget_get_style_context(start_card), "card");
  gtk_widget_set_margin_top(start_card, 6);
  gtk_stack_add_named(GTK_STACK(card_stack), start_card, "start");

  start_scroll = gtk_scrolled_window_new(NULL, NULL);
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(start_scroll), GTK_POLICY_NEVER,
                                 GTK_POLICY_AUTOMATIC);
  gtk_widget_set_margin_start(start_scroll, 8);
  gtk_widget_set_margin_end(start_scroll, 8);
  gtk_widget_set_margin_top(start_scroll, 4);
  gtk_box_pack_start(GTK_BOX(start_card), start_scroll, TRUE, TRUE, 0);

  start_store = gtk_list_store_new(SNCOL, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING,
                                   G_TYPE_INT, G_TYPE_INT, G_TYPE_INT);
  start_tree = gtk_tree_view_new_with_model(GTK_TREE_MODEL(start_store));
  g_object_unref(start_store);
  gtk_tree_view_set_headers_clickable(GTK_TREE_VIEW(start_tree), TRUE);
  gtk_container_add(GTK_CONTAINER(start_scroll), start_tree);

  /* Name and Source are the two things that identify an entry to a person; the
     command line is there because on Linux it is the only honest description of
     what an entry runs, and it is the widest, so it is the one that gets cut. */
  static const char *start_title[4] = {N_("Name"), N_("State"), N_("Source"), N_("Command")};
  static const int start_sort_col[4] = {S_NAME, S_ORDER, S_SRC, S_EXEC};
  static const int start_txt[4] = {S_NAME, S_STATE, S_SRC, S_EXEC};
  static const int start_w[4] = {200, 120, 90, 250};
  for (int i = 0; i < 4; i++) {
    GtkCellRenderer *cr = gtk_cell_renderer_text_new();
    g_object_set(cr, "ellipsize", PANGO_ELLIPSIZE_END, NULL);
    GtkTreeViewColumn *c = GTK_TREE_VIEW_COLUMN(
        gtk_tree_view_column_new_with_attributes(_(start_title[i]), cr, "text", start_txt[i], NULL));
    gtk_tree_view_column_set_min_width(c, start_w[i]);
    gtk_tree_view_column_set_sort_column_id(c, start_sort_col[i]);
    gtk_tree_view_column_set_sort_order(c, GTK_SORT_ASCENDING);
    if (i == 0)
      gtk_tree_view_column_set_expand(c, TRUE);
    gtk_tree_view_append_column(GTK_TREE_VIEW(start_tree), c);
  }

  GtkTreeSortable *start_srt = GTK_TREE_SORTABLE(start_store);
  for (int i = 0; i < 4; i++)
    gtk_tree_sortable_set_sort_func(start_srt, start_sort_col[i], start_sort, NULL, NULL);
  gtk_tree_sortable_set_sort_column_id(start_srt, S_NAME, GTK_SORT_ASCENDING);

  start_sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(start_tree));
  g_signal_connect(start_sel, "changed", G_CALLBACK(on_start_selection_changed), NULL);

  GtkWidget *start_foot = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_set_margin_start(start_foot, 8);
  gtk_widget_set_margin_end(start_foot, 8);
  gtk_widget_set_margin_top(start_foot, 6);
  gtk_box_pack_start(GTK_BOX(start_card), start_foot, FALSE, FALSE, 0);

  start_note = gtk_label_new("");
  gtk_widget_set_halign(start_note, GTK_ALIGN_START);
  gtk_style_context_add_class(gtk_widget_get_style_context(start_note), "note");
  gtk_box_pack_start(GTK_BOX(start_foot), start_note, TRUE, TRUE, 0);

  start_btn = gtk_button_new_with_label(_("Disable"));
  gtk_style_context_add_class(gtk_widget_get_style_context(start_btn), "toggleswitch");
  gtk_widget_set_halign(start_btn, GTK_ALIGN_END);
  gtk_widget_set_valign(start_btn, GTK_ALIGN_CENTER);
  g_signal_connect(start_btn, "clicked", G_CALLBACK(on_start_clicked), NULL);
  gtk_box_pack_end(GTK_BOX(start_foot), start_btn, FALSE, FALSE, 0);

  /* The Details card is the wide table: thirteen columns that do not fit on the
     screen, scrolled sideways the way Windows scrolls its Details tab rather
     than squeezed until a command line is three letters. */
  det_card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_style_context_add_class(gtk_widget_get_style_context(det_card), "card");
  gtk_widget_set_margin_top(det_card, 6);
  gtk_stack_add_named(GTK_STACK(card_stack), det_card, "det");

  det_scroll = gtk_scrolled_window_new(NULL, NULL);
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(det_scroll), GTK_POLICY_AUTOMATIC,
                                 GTK_POLICY_AUTOMATIC);
  /* An overlay scrollbar is invisible until the pointer finds it, and on a
     1366-wide screen half this table is only reachable through it: the bar has
     to be there to look at, the way Windows shows its own. */
  gtk_scrolled_window_set_overlay_scrolling(GTK_SCROLLED_WINDOW(det_scroll), FALSE);
  gtk_widget_set_margin_start(det_scroll, 8);
  gtk_widget_set_margin_end(det_scroll, 8);
  gtk_widget_set_margin_top(det_scroll, 4);
  gtk_box_pack_start(GTK_BOX(det_card), det_scroll, TRUE, TRUE, 0);

  /* Every column is a displayed text and the number behind it, side by side, so
     a sort knows 9 KiB is less than 1 MiB. */
  det_store = gtk_list_store_new(DNCOL, G_TYPE_STRING, /* D_NAME */
                                 G_TYPE_DOUBLE,        /* D_PID */
                                 G_TYPE_STRING,        /* D_PID_T */
                                 G_TYPE_STRING,        /* D_STATE */
                                 G_TYPE_DOUBLE,        /* D_STATE_ORD */
                                 G_TYPE_STRING,        /* D_USER */
                                 G_TYPE_DOUBLE,        /* D_CPU */
                                 G_TYPE_STRING,        /* D_CPU_T */
                                 G_TYPE_DOUBLE,        /* D_ELAP */
                                 G_TYPE_STRING,        /* D_ELAP_T */
                                 G_TYPE_DOUBLE,        /* D_MEM */
                                 G_TYPE_STRING,        /* D_MEM_T */
                                 G_TYPE_DOUBLE,        /* D_RD */
                                 G_TYPE_STRING,        /* D_RD_T */
                                 G_TYPE_DOUBLE,        /* D_WR */
                                 G_TYPE_STRING,        /* D_WR_T */
                                 G_TYPE_DOUBLE,        /* D_THR */
                                 G_TYPE_STRING,        /* D_THR_T */
                                 G_TYPE_DOUBLE,        /* D_PRI */
                                 G_TYPE_STRING,        /* D_PRI_T */
                                 G_TYPE_DOUBLE,        /* D_PPID */
                                 G_TYPE_STRING,        /* D_PPID_T */
                                 G_TYPE_STRING         /* D_CMD */
  );
  det_tree = gtk_tree_view_new_with_model(GTK_TREE_MODEL(det_store));
  /* One row height for the whole table, for the same reason the columns are
     fixed: see the note where the columns are made. It is only honest because
     every cell is flattened to a single line by the collector. */
  gtk_tree_view_set_fixed_height_mode(GTK_TREE_VIEW(det_tree), TRUE);
  g_object_unref(det_store);
  gtk_tree_view_set_headers_clickable(GTK_TREE_VIEW(det_tree), TRUE);
  gtk_container_add(GTK_CONTAINER(det_scroll), det_tree);

  for (int i = 0; i < 13; i++) {
    GtkCellRenderer *cr = gtk_cell_renderer_text_new();
    g_object_set(cr, "ellipsize", PANGO_ELLIPSIZE_END, NULL);
    /* Numbers read as a column when they share a right edge; words keep the left
       one. The word columns are Name, Status, User name and Command line. */
    if (i != 0 && i != 2 && i != 3 && i != 12)
      g_object_set(cr, "xalign", 1.0, NULL);
    GtkTreeViewColumn *c = GTK_TREE_VIEW_COLUMN(
        gtk_tree_view_column_new_with_attributes(_(det_title[i]), cr, "text", det_col[i], NULL));
    /* Fixed sizing on purpose. With the default GROW_ONLY, GtkTreeView re-measures
       a row's height and every column's natural width each time the row changes,
       which on 200 rows turned one sweep into 237 ms of CPU in the main loop —
       twenty times the cost of the drawing. Every cell here is one line of text
       at one width, so the measurements are declared once instead. */
    gtk_tree_view_column_set_sizing(c, GTK_TREE_VIEW_COLUMN_FIXED);
    gtk_tree_view_column_set_fixed_width(c, det_w[i]);
    gtk_tree_view_column_set_sort_column_id(c, det_val[i]);
    /* The order a header click starts in. The text comparator answers Z to A so
       that GTK's reversal shows A to Z, which is what the first click on a word
       column has to do; the measurements start at the biggest, and the plain
       counts start at the smallest, which det_cmp already respects. */
    gtk_tree_view_column_set_sort_order(c, (i == 0 || i == 2 || i == 3 || i == 12)
                                                 ? GTK_SORT_DESCENDING
                                                 : GTK_SORT_ASCENDING);
    gtk_tree_view_append_column(GTK_TREE_VIEW(det_tree), c);
  }

  GtkTreeSortable *det_srt = GTK_TREE_SORTABLE(det_store);
  for (int i = 0; i < 13; i++)
    gtk_tree_sortable_set_sort_func(det_srt, det_val[i], det_cmp, NULL, NULL);
  gtk_tree_sortable_set_sort_column_id(det_srt, D_NAME, GTK_SORT_DESCENDING);

  det_note = gtk_label_new(_(det_note_text));
  gtk_widget_set_halign(det_note, GTK_ALIGN_START);
  gtk_widget_set_margin_start(det_note, 8);
  gtk_widget_set_margin_end(det_note, 8);
  gtk_widget_set_margin_top(det_note, 6);
  gtk_style_context_add_class(gtk_widget_get_style_context(det_note), "note");
  gtk_box_pack_start(GTK_BOX(det_card), det_note, FALSE, FALSE, 0);

  /* The Information card carries no graph and no strip of live numbers: it is
     one list of what the machine said about itself, read once when the tab
     opens, so the table gets the whole height. */
  info_card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_style_context_add_class(gtk_widget_get_style_context(info_card), "card");
  gtk_widget_set_margin_top(info_card, 6);
  gtk_stack_add_named(GTK_STACK(card_stack), info_card, "info");

  info_scroll = gtk_scrolled_window_new(NULL, NULL);
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(info_scroll), GTK_POLICY_NEVER,
                                 GTK_POLICY_AUTOMATIC);
  gtk_widget_set_margin_start(info_scroll, 8);
  gtk_widget_set_margin_end(info_scroll, 8);
  gtk_widget_set_margin_top(info_scroll, 4);
  gtk_box_pack_start(GTK_BOX(info_card), info_scroll, TRUE, TRUE, 0);

  info_store = gtk_list_store_new(KVCOL, G_TYPE_STRING, G_TYPE_STRING);
  info_tree = gtk_tree_view_new_with_model(GTK_TREE_MODEL(info_store));
  g_object_unref(info_store);
  /* No header: the left column already names each row, and a header cell over it
     would have to be the name of both, which says nothing. */
  gtk_tree_view_set_headers_visible(GTK_TREE_VIEW(info_tree), FALSE);
  gtk_container_add(GTK_CONTAINER(info_scroll), info_tree);

  for (int i = 0; i < KVCOL; i++) {
    GtkCellRenderer *cr = gtk_cell_renderer_text_new();
    if (i == K_VAL)
      g_object_set(cr, "ellipsize", PANGO_ELLIPSIZE_END, NULL);
    GtkTreeViewColumn *c = GTK_TREE_VIEW_COLUMN(
        gtk_tree_view_column_new_with_attributes("", cr, "text", i, NULL));
    if (i == K_KEY)
      gtk_tree_view_column_set_min_width(c, 220);
    else
      gtk_tree_view_column_set_expand(c, TRUE);
    gtk_tree_view_append_column(GTK_TREE_VIEW(info_tree), c);
  }

  GtkWidget *strip = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_set_margin_top(strip, 8);
  for (int i = 0; i < NST; i++) {
    GtkWidget *cell = gtk_box_new(GTK_ORIENTATION_VERTICAL, 1);
    GtkWidget *v = gtk_label_new("—");
    gtk_widget_set_halign(v, GTK_ALIGN_START);
    gtk_style_context_add_class(gtk_widget_get_style_context(v), "statv");
    GtkWidget *l = gtk_label_new("");
    gtk_widget_set_halign(l, GTK_ALIGN_START);
    gtk_style_context_add_class(gtk_widget_get_style_context(l), "statl");
    gtk_box_pack_start(GTK_BOX(cell), v, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(cell), l, FALSE, FALSE, 0);
    if (i > 0) {
      GtkWidget *sep = gtk_separator_new(GTK_ORIENTATION_VERTICAL);
      gtk_widget_set_margin_start(sep, 6);
      gtk_widget_set_margin_end(sep, 6);
      gtk_box_pack_start(GTK_BOX(strip), sep, FALSE, FALSE, 0);
      stat_sep[i] = sep;
    }
    gtk_box_pack_start(GTK_BOX(strip), cell, TRUE, TRUE, 0);
    stat_value[i] = v;
    stat_label[i] = l;
  }
  gtk_box_pack_start(GTK_BOX(panel), strip, FALSE, FALSE, 0);

  g_signal_connect(window, "destroy", G_CALLBACK(gtk_main_quit), NULL);
  build_end_menu();
  gtk_widget_show_all(window);
  set_tab(TAB_PERF); /* show_all() has just made both cards visible */

  int target = 0;
  if (want) {
    for (int i = 0; i < nvisible; i++)
      if (!g_ascii_strcasecmp(want, dev_key[row_to_dev[i]]))
        target = i;
  }
  if (nvisible > 0)
    gtk_list_box_select_row(GTK_LIST_BOX(dev_list),
                           gtk_list_box_get_row_at_index(GTK_LIST_BOX(dev_list), target));
  gtk_list_box_select_row(GTK_LIST_BOX(info_list),
                          gtk_list_box_get_row_at_index(GTK_LIST_BOX(info_list), 0));
  update_panel();
  on_tick(NULL);
  g_timeout_add(1000, on_tick, NULL);

  gtk_main();
  return 0;
}
