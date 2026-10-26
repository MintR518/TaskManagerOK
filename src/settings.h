/* Where the settings live, what they mean and how they are found.
 *
 * This module owns the file and the catalogue discovery only; what the numbers
 * are worth is the user's business and is shown next to the control that changes
 * them (see the settings card in main.c).
 */
#ifndef SETTINGS_H
#define SETTINGS_H

#include <glib.h>
#include <stddef.h>

#define GETTEXT_PACKAGE "taskmanagerok"

#define SET_NLANG 16

/* Seconds between two refreshes. Every one of them is a period, not a rate: the
   collector is the expensive part and the timer only decides how often it runs. */
typedef struct {
  double perf_s;   /* the graph tick, which is also the heartbeat of the window */
  double proc_s;   /* the process table */
  double det_s;    /* the details table */
  double batt_s;   /* /sys/class/power_supply, while that device is on screen */
  char lang[16];   /* a language code, or empty to follow the system */
} Settings;

/* The directory holding <code>/LC_MESSAGES/taskmanagerok.mo`, found next to the
   binary so that a checkout and an AppImage both work without installing; NULL
   falls back to the compiled-in LOCALEDIR. */
const char *settings_locale_dir(void);

/* The language codes with a catalogue in that directory, sorted, without the
   system default. Returns how many were written into `codes`. */
int settings_languages(char codes[][16], int max);

/* Defaults, then whatever the file says; a missing or broken file is simply the
   defaults, because the first run has not written anything yet. */
void settings_load(Settings *s);

/* Written atomically by GLib. Returns FALSE and fills `why` with the reason —
   the card has to say it out loud rather than keep a change that never landed. */
gboolean settings_save(const Settings *s, char *why, size_t whylen);

/* The full path, for the line that tells the reader where their settings are. */
const char *settings_path(void);

#endif /* SETTINGS_H */
