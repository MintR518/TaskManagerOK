/* Settings file and translation catalogues. See settings.h for the contract. */
#include "settings.h"

#include <libintl.h>
#include <stdio.h>
#include <string.h>

#define _(s) gettext(s)

#ifndef LOCALEDIR
#define LOCALEDIR "/usr/local/share/locale"
#endif

#define SET_MIN 0.1
#define SET_MAX 30.0

static char locale_dir[512];
static char conf_path[512];

/* The three shapes the translations live in: beside the binary for a checkout
   and for an AppImage (which unpacks its own tree next to itself), and the
   compiled-in prefix for `make install`. */
static const char *const rel[3] = {"locale", "share/locale", "../share/locale"};

const char *settings_locale_dir(void) {
  if (locale_dir[0])
    return locale_dir;

  gchar *exe = g_file_read_link("/proc/self/exe", NULL);
  if (exe) {
    gchar *bindir = g_path_get_dirname(exe);
    for (unsigned i = 0; i < 3 && !locale_dir[0]; i++) {
      gchar *dir = g_build_filename(bindir, rel[i], NULL);
      gchar *probe = g_build_filename(dir, "uk", "LC_MESSAGES", GETTEXT_PACKAGE ".mo", NULL);
      if (g_file_test(probe, G_FILE_TEST_EXISTS))
        snprintf(locale_dir, sizeof locale_dir, "%s", dir);
      g_free(probe);
      g_free(dir);
    }
    g_free(bindir);
    g_free(exe);
  }
  if (!locale_dir[0])
    snprintf(locale_dir, sizeof locale_dir, "%s", LOCALEDIR);
  return locale_dir;
}

static int cmp_code(const void *a, const void *b) { return strcmp((const char *)a, (const char *)b); }

int settings_languages(char codes[][16], int max) {
  const char *dir = settings_locale_dir();
  int n = 0;
  GDir *d = g_dir_open(dir, 0, NULL);
  if (!d)
    return 0;
  const char *name;
  while ((name = g_dir_read_name(d)) != NULL && n < max) {
    if (name[0] == '.' || strlen(name) > 15)
      continue;
    gchar *mo = g_build_filename(dir, name, "LC_MESSAGES", GETTEXT_PACKAGE ".mo", NULL);
    gboolean ok = g_file_test(mo, G_FILE_TEST_EXISTS);
    g_free(mo);
    if (!ok)
      continue;
    snprintf(codes[n++], 16, "%s", name);
  }
  g_dir_close(d);
  qsort(codes, (size_t)n, 16, cmp_code);
  return n;
}

const char *settings_path(void) {
  if (conf_path[0])
    return conf_path;
  gchar *p = g_build_filename(g_get_user_config_dir(), "taskmanagerok", "settings.ini", NULL);
  snprintf(conf_path, sizeof conf_path, "%s", p);
  g_free(p);
  return conf_path;
}

static double clamp_period(double v) {
  /* `>=` rather than `>` so that a NaN from a hand-edited file falls in too. */
  if (!(v >= SET_MIN))
    return SET_MIN;
  return v > SET_MAX ? SET_MAX : v;
}

static double get_period(GKeyFile *kf, const char *key, double def) {
  GError *e = NULL;
  double v = g_key_file_get_double(kf, "refresh", key, &e);
  if (e) {
    g_error_free(e);
    return def;
  }
  return clamp_period(v);
}

void settings_load(Settings *s) {
  /* The numbers this program has always run on, so an untouched machine keeps
     exactly the behaviour that was measured. */
  s->perf_s = 1.0;
  s->proc_s = 4.0;
  s->det_s = 6.0;
  s->batt_s = 5.0;
  s->lang[0] = '\0';

  GKeyFile *kf = g_key_file_new();
  if (!g_key_file_load_from_file(kf, settings_path(), G_KEY_FILE_NONE, NULL)) {
    g_key_file_free(kf);
    return;
  }
  s->perf_s = get_period(kf, "performance", s->perf_s);
  s->proc_s = get_period(kf, "processes", s->proc_s);
  s->det_s = get_period(kf, "details", s->det_s);
  s->batt_s = get_period(kf, "battery", s->batt_s);

  gchar *lang = g_key_file_get_string(kf, "interface", "language", NULL);
  if (lang && strlen(lang) < sizeof s->lang)
    snprintf(s->lang, sizeof s->lang, "%s", lang);
  g_free(lang);
  g_key_file_free(kf);
}

gboolean settings_save(const Settings *s, char *why, size_t whylen) {
  why[0] = '\0';

  GKeyFile *kf = g_key_file_new();
  g_key_file_set_double(kf, "refresh", "performance", s->perf_s);
  g_key_file_set_double(kf, "refresh", "processes", s->proc_s);
  g_key_file_set_double(kf, "refresh", "details", s->det_s);
  g_key_file_set_double(kf, "refresh", "battery", s->batt_s);
  g_key_file_set_string(kf, "interface", "language", s->lang);

  gchar *dir = g_path_get_dirname(settings_path());
  gboolean saved = g_mkdir_with_parents(dir, 0700) == 0;
  if (!saved)
    snprintf(why, whylen, _("Could not create %s"), dir);
  g_free(dir);

  if (saved) {
    GError *e = NULL;
    if (!g_key_file_save_to_file(kf, settings_path(), &e)) {
      snprintf(why, whylen, "%s", e && e->message ? e->message : _("the settings could not be written"));
      g_clear_error(&e);
      saved = FALSE;
    }
  }
  g_key_file_free(kf);
  return saved;
}
