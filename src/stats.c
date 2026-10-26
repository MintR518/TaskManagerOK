#define _GNU_SOURCE
#include "stats.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <glob.h>
#include <ifaddrs.h>
#include <mntent.h>
#include <net/if.h>
#include <netinet/in.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>

/* ---------- small helpers ---------- */

/* Copy with a clamp, not a format: snprintf("%s") would let the compiler guess
   how long the source can be and warn about every path buffer we hand it. */
static void scpy(char *dst, size_t dn, const char *src) {
  if (dn == 0)
    return;
  size_t l = strlen(src);
  if (l > dn - 1)
    l = dn - 1;
  memcpy(dst, src, l);
  dst[l] = '\0';
}

static double now_ms(void) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return t.tv_sec * 1000.0 + t.tv_nsec / 1e6;
}

static char *trim(char *s) {
  while (*s && isspace((unsigned char)*s))
    s++;
  size_t l = strlen(s);
  while (l > 0 && isspace((unsigned char)s[l - 1]))
    s[--l] = '\0';
  return s;
}

/* /proc/cpuinfo pads the model name with runs of spaces. */
static void squeeze(char *s) {
  char *r = s, *w = s;
  int space = 0;
  while (*r) {
    if (isspace((unsigned char)*r)) {
      if (!space && w > s)
        *w++ = ' ';
      space = 1;
      r++;
    } else {
      *w++ = *r++;
      space = 0;
    }
  }
  *w = '\0';
}

static int read_u64(const char *path, unsigned long long *out) {
  FILE *f = fopen(path, "r");
  if (!f)
    return 0;
  char buf[128];
  int ok = 0;
  if (fgets(buf, sizeof buf, f)) {
    char *end;
    unsigned long long v = strtoull(buf, &end, 10);
    if (end != buf) {
      *out = v;
      ok = 1;
    }
  }
  fclose(f);
  return ok;
}

static int read_line(const char *path, char *buf, size_t n) {
  FILE *f = fopen(path, "r");
  if (!f)
    return 0;
  int ok = fgets(buf, (int)n, f) != NULL;
  fclose(f);
  return ok;
}

static int first_glob(const char *pattern, char *out, size_t on) {
  glob_t gl;
  memset(&gl, 0, sizeof gl);
  int found = 0;
  if (glob(pattern, 0, NULL, &gl) == 0 && gl.gl_pathc > 0) {
    scpy(out, on, gl.gl_pathv[0]);
    found = 1;
  }
  globfree(&gl);
  return found;
}

/* ---------- resolved once at startup ---------- */

static char g_root_disk[64];
static char g_net_if[64];
static char g_gpu_dir[160];
static char g_gpu_busy[160];
static char g_gpu_temp[160];
static char g_gpu_vram_used[160];
static char g_gpu_vram_total[160];
static char g_gpu_name[96];
static char g_freq[160];

static void resolve_root_disk(void) {
  FILE *f = fopen("/proc/self/mountinfo", "r");
  if (!f)
    return;
  char line[512];
  char majmin[32] = "";
  while (fgets(line, sizeof line, f)) {
    char id[32], parent[32], mm[32], root[64], mp[256];
    if (sscanf(line, "%31s %31s %31s %63s %255s", id, parent, mm, root, mp) == 5 &&
        strcmp(mp, "/") == 0) {
      scpy(majmin, sizeof majmin, mm);
      break;
    }
  }
  fclose(f);
  if (!majmin[0])
    return;

  char path[128], link[512];
  snprintf(path, sizeof path, "/sys/dev/block/%s", majmin);
  ssize_t l = readlink(path, link, sizeof link - 1);
  if (l <= 0)
    return;
  link[l] = '\0';
  /* .../block/sda/sda3 -> "sda";  .../virtual/block/zram0 -> "zram0" */
  char *last = strrchr(link, '/');
  if (!last)
    return;
  *last = '\0';
  char *prev = strrchr(link, '/');
  const char *disk = prev ? prev + 1 : last + 1;
  if (prev && strcmp(prev + 1, "block") == 0)
    disk = last + 1;
  if (*disk)
    scpy(g_root_disk, sizeof g_root_disk, disk);
}

static void resolve_net_if(void) {
  FILE *f = fopen("/proc/net/route", "r");
  if (!f)
    return;
  char line[256];
  while (fgets(line, sizeof line, f)) {
    char ifn[64], dst[64];
    if (sscanf(line, "%63s %63s", ifn, dst) == 2 && strcmp(dst, "00000000") == 0 &&
        strcmp(ifn, "IFace") != 0) {
      scpy(g_net_if, sizeof g_net_if, ifn);
      break;
    }
  }
  fclose(f);
}

/* WEXT exposes the live Wi-Fi link in /proc/net/wireless; a wireless driver
   that cannot measure it reports noise -256, which we simply ignore. */
static void read_wireless(const char *ifn, SysStats *s) {
  s->net_wireless = s->net_quality = s->net_level_dbm = 0;
  FILE *f = fopen("/proc/net/wireless", "r");
  if (!f)
    return;
  char line[256];
  while (fgets(line, sizeof line, f)) {
    char name[64];
    int status, quality, level, noise;
    if (sscanf(line, "%63[^:]: %d %d. %d. %d", name, &status, &quality, &level, &noise) == 5 &&
        strcmp(name, ifn) == 0) {
      s->net_wireless = 1;
      s->net_quality = quality;
      s->net_level_dbm = level;
      break;
    }
  }
  fclose(f);
}

/* JEDEC SPD base memory type: byte 2 of the EEPROM on the module itself. */
static const char *spd_name(int b) {
  switch (b) {
    case 0x07:
      return "DDR";
    case 0x08:
      return "DDR2";
    case 0x0B:
      return "DDR3";
    case 0x0C:
    case 0x0E:
      return "DDR4";
    case 0x0F:
      return "LPDDR3";
    case 0x10:
      return "LPDDR4";
    case 0x11:
      return "LPDDR4X";
    case 0x12:
      return "DDR5";
    case 0x13:
      return "LPDDR5";
    default:
      return NULL;
  }
}

/* The kernel binds a DIMM's EEPROM as an i2c device named "spd" (at24, DDR/2/3)
   or "ee1004" (the DDR4 hub). Where there is none - soldered memory, DDR5 behind
   an i3c hub, or an eeprom file the user may not read - the type stays empty and
   the UI falls back to the plain size line. */
static void resolve_mem_type(char *out, size_t n) {
  DIR *d = opendir("/sys/bus/i2c/devices");
  if (!d)
    return;
  struct dirent *e;
  while (!out[0] && (e = readdir(d))) {
    if (e->d_name[0] == '.')
      continue;
    char dir[280], nm[64], path[352];
    snprintf(dir, sizeof dir, "/sys/bus/i2c/devices/%s", e->d_name);
    snprintf(path, sizeof path, "%s/name", dir);
    if (!read_line(path, nm, sizeof nm) ||
        (strcmp(trim(nm), "spd") != 0 && strcmp(trim(nm), "ee1004") != 0))
      continue;
    snprintf(path, sizeof path, "%s/eeprom", dir);
    FILE *f = fopen(path, "rb");
    if (!f)
      continue;
    unsigned char b[3];
    int ok = fread(b, 1, sizeof b, f) == sizeof b;
    fclose(f);
    if (ok && spd_name(b[2]))
      scpy(out, n, spd_name(b[2]));
  }
  closedir(d);
}

/* pci.ids is a plain text table of vendor and device names that ships with
   pciutils (Debian, Mint, Fedora: /usr/share/misc/pci.ids, Fedora also
   /usr/share/hwdata/) and is world readable. Reading it is optional: without it
   the adapter is still listed, just under its driver and numeric ids. The scan
   stops at the vendor we want and at the device under it, so it reads a fraction
   of the file; it runs once, at startup. */
/* pci.ids ids are four bare hex digits ("8086  Intel Corporation"); older and
   packaged copies carry an 0x prefix. Accept either. Requiring exactly four
   digits also rejects the two-digit class headings, which are not ids. */
static unsigned hex_id(const char *s) {
  if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
    s += 2;
  unsigned v = 0;
  int n = 0;
  while (n < 4 && isxdigit((unsigned char)s[n])) {
    int c = tolower((unsigned char)s[n]);
    v = v * 16 + (c <= '9' ? c - '0' : c - 'a' + 10);
    n++;
  }
  return n == 4 ? v : 0;
}

static void pci_ids_name(unsigned vid, unsigned did, char *out, size_t n) {
  static const char *paths[] = {"/usr/share/misc/pci.ids", "/usr/share/hwdata/pci.ids",
                                "/usr/share/pci.ids"};
  out[0] = '\0';
  for (size_t i = 0; i < sizeof paths / sizeof paths[0] && !out[0]; i++) {
    FILE *f = fopen(paths[i], "r");
    if (!f)
      continue;
    char line[512];
    int under = 0; /* inside the vendor we are looking for */
    while (fgets(line, sizeof line, f)) {
      char *t = line;
      int tabs = 0;
      while (*t == '\t') {
        tabs++;
        t++;
      }
      if (tabs > 2 || *t == '#' || *t == '\n' || *t == '\0')
        continue;
      if (tabs == 1 && under) { /* a device under that vendor */
        if (hex_id(t) == did) {
          char *nm = t + 4;
          while (*nm == ' ' || *nm == '\t')
            nm++;
          scpy(out, n, trim(nm));
          break;
        }
      } else if (tabs == 0) {
        unsigned v = hex_id(t);
        if (!v) {
          under = 0;
          continue; /* a header or a stray comment */
        }
        if (v == vid)
          under = 1;
        else if (under)
          break; /* vendors are in ascending order: the one we want is behind us */
      }
    }
    fclose(f);
  }
}

/* DRIVER= and PCI_ID= from a device uevent. Only a real adapter carries a
   PCI_ID, which is what separates the cards from the connector directories that
   sit beside them and symlink back to the same device. */
static int read_uevent_ids(const char *dir, unsigned *vid, unsigned *did, char *driver, size_t nd) {
  char p[220], line[128];
  snprintf(p, sizeof p, "%s/uevent", dir);
  FILE *f = fopen(p, "r");
  if (!f)
    return 0;
  *vid = *did = 0;
  driver[0] = '\0';
  while (fgets(line, sizeof line, f)) {
    if (!strncmp(line, "PCI_ID=", 7))
      sscanf(line + 7, "%4x:%4x", vid, did);
    else if (!strncmp(line, "DRIVER=", 7))
      scpy(driver, nd, trim(line + 7));
  }
  fclose(f);
  return *vid != 0;
}

/* A display adapter is anything under /sys/class/drm whose name is "card" plus
   digits and whose device uevent carries a PCI_ID, which leaves out the
   connector directories (card1-DP-1 & co, whose device symlink points back at
   the same adapter) and the render nodes.

   Whether one exists and whether it can be *measured* are two different things:
   gpu_busy_percent is exported by amdgpu and by some virtualisation drivers, not
   by i915, nouveau or the proprietary NVIDIA driver. So the adapter is named and
   listed either way, while the load counter stays off until a driver actually
   answers. Nothing here invents a zero. */
static void resolve_gpu(void) {
  DIR *d = opendir("/sys/class/drm");
  if (!d)
    return;
  struct dirent *e;
  while ((e = readdir(d))) {
    const char *n = e->d_name;
    if (strncmp(n, "card", 4) != 0)
      continue;
    int digits = 0;
    for (const char *p = n + 4; *p; p++) {
      if (*p < '0' || *p > '9') {
        digits = -1;
        break;
      }
      digits++;
    }
    if (digits <= 0)
      continue;

    char dir[128], p[192];
    unsigned vid, did;
    char driver[32];
    snprintf(dir, sizeof dir, "/sys/class/drm/%.64s/device", n);
    if (!read_uevent_ids(dir, &vid, &did, driver, sizeof driver))
      continue; /* a connector or a render node, not an adapter */

    snprintf(p, sizeof p, "%s/gpu_busy_percent", dir);
    unsigned long long v;
    int measurable = read_u64(p, &v);
    if (!g_gpu_dir[0] || (measurable && !g_gpu_busy[0])) {
      scpy(g_gpu_dir, sizeof g_gpu_dir, dir);
      if (measurable)
        scpy(g_gpu_busy, sizeof g_gpu_busy, p);
      else
        g_gpu_busy[0] = '\0';
      g_gpu_temp[0] = g_gpu_vram_used[0] = g_gpu_vram_total[0] = '\0';
      char pat[192];
      snprintf(pat, sizeof pat, "%s/hwmon/hwmon*/temp1_input", dir);
      if (!first_glob(pat, g_gpu_temp, sizeof g_gpu_temp)) {
        snprintf(pat, sizeof pat, "%s/hwmon/*/temp1_input", dir);
        first_glob(pat, g_gpu_temp, sizeof g_gpu_temp);
      }
      snprintf(pat, sizeof pat, "%s/mem_info_vram_used", dir);
      if (!first_glob(pat, g_gpu_vram_used, sizeof g_gpu_vram_used))
        g_gpu_vram_total[0] = '\0';
      else {
        snprintf(pat, sizeof pat, "%s/mem_info_vram_total", dir);
        if (!first_glob(pat, g_gpu_vram_total, sizeof g_gpu_vram_total))
          g_gpu_vram_used[0] = '\0';
      }
    }
    if (g_gpu_busy[0])
      break; /* a measurable adapter is worth no further looking */
  }
  closedir(d);

  if (!g_gpu_dir[0])
    return;

  /* Name it. A hypervisor driver tells us the label it wants shown; otherwise
     pci.ids has the marketing name the kernel would have printed in lspci; the
     last resort is the driver and the numeric ids, which is at least true. */
  char p[220], buf[256];
  unsigned vid = 0, did = 0;
  char driver[32] = "";
  snprintf(p, sizeof p, "%s/label", g_gpu_dir);
  if (read_line(p, buf, sizeof buf))
    scpy(g_gpu_name, sizeof g_gpu_name, trim(buf));
  if (!g_gpu_name[0]) {
    read_uevent_ids(g_gpu_dir, &vid, &did, driver, sizeof driver);
    pci_ids_name(vid, did, g_gpu_name, sizeof g_gpu_name);
    if (!g_gpu_name[0]) {
      if (driver[0] && vid)
        snprintf(g_gpu_name, sizeof g_gpu_name, "%s (%04x:%04x)", driver, vid, did);
      else if (vid)
        snprintf(g_gpu_name, sizeof g_gpu_name, "GPU (%04x:%04x)", vid, did);
      else
        scpy(g_gpu_name, sizeof g_gpu_name, "GPU");
    }
  }
}

/* ---------- temperatures ---------- */

/* A sensor reading in millidegrees, or nothing at all. strtol rather than the
   unsigned reader used elsewhere: a chip that reports a cold board as a negative
   number would otherwise come back as a value near 2^64, and a file caught
   mid-update would plot as a spike nobody can explain. The range check is the
   same shield — a plausible ceiling rules out the garbage, and no real sensor
   on a real machine sits outside it. */
static int read_milli(const char *path, int *out) {
  FILE *f = fopen(path, "r");
  if (!f)
    return 0;
  char buf[64];
  int ok = 0;
  if (fgets(buf, sizeof buf, f)) {
    char *end;
    long v = strtol(buf, &end, 10);
    if (end != buf && v > -40000 && v < 200000) {
      *out = (int)v;
      ok = 1;
    }
  }
  fclose(f);
  return ok;
}

/* Which of these sensors are the processor's. The chip is what says so —
   coretemp, k10temp and zenpower are the Intel, AMD and via-AMD package sensors
   — and so are the labels they carry. acpitz is deliberately not one of them:
   it is an ACPI zone of the board, and the first version of this file called it
   the CPU temperature because coretemp numbers its inputs temp2 and temp3 and
   the lookup only ever tried temp1. */
static int sensor_is_cpu(const char *chip, const char *label) {
  static const char *chips[] = {"coretemp", "k10temp",  "zenpower", "amdcpu",
                                "cpu_thermal", "cpu-thermal", "soc_thermal"};
  for (size_t i = 0; i < sizeof chips / sizeof chips[0]; i++)
    if (!strcasecmp(chip, chips[i]))
      return 1;
  static const char *labels[] = {"Package", "Tctl", "Tdie", "Tcse", "CPU", "Core "};
  for (size_t i = 0; i < sizeof labels / sizeof labels[0]; i++)
    if (!strncasecmp(label, labels[i], strlen(labels[i])))
      return 1;
  return 0;
}

/* Every sensor the kernel will show an ordinary user, read again on each sample:
   a driver can be loaded or a USB probe attached mid-session, and the whole pass
   is a dozen reads of a few bytes.

   The chips are visited in hwmon-number order, not readdir order, so the list
   does not reshuffle between samples and a row stays where the person left it.

   What is not here is a promise: a virtual machine, or a board whose sensors
   are all root-only, answers with zero sensors, and the interface hides the row
   rather than showing a blank one. */
static void collect_temps(SysStats *s) {
  s->nsens = 0;
  s->temp_max_mc = 0;
  s->temp_limit_mc = 0;
  s->cpu_temp_mc = 0;

  DIR *d = opendir("/sys/class/hwmon");
  if (!d)
    return;

  int no[64];
  char nm[64][24];
  int n = 0;
  struct dirent *e;
  while (n < 64 && (e = readdir(d))) {
    if (strncmp(e->d_name, "hwmon", 5) != 0)
      continue;
    char *end;
    long v = strtol(e->d_name + 5, &end, 10);
    if (*end != '\0' || v < 0)
      continue;
    no[n] = (int)v;
    scpy(nm[n], sizeof nm[n], e->d_name);
    n++;
  }
  closedir(d);

  for (int i = 1; i < n; i++) /* insertion sort: the list is a handful of entries */
    for (int j = i; j > 0 && no[j] < no[j - 1]; j--) {
      int t = no[j];
      no[j] = no[j - 1];
      no[j - 1] = t;
      char b[24];
      scpy(b, sizeof b, nm[j]);
      scpy(nm[j], sizeof nm[j], nm[j - 1]);
      scpy(nm[j - 1], sizeof nm[j - 1], b);
    }

  int best = -1; /* the hottest sensor seen so far, and the limit that goes with it */
  for (int i = 0; i < n && s->nsens < MAXSENS; i++) {
    char dir[128], p[192], chip[24];
    snprintf(dir, sizeof dir, "/sys/class/hwmon/%.24s", nm[i]);
    snprintf(p, sizeof p, "%s/name", dir);
    if (!read_line(p, chip, sizeof chip))
      continue;
    char *nl = strpbrk(chip, "\r\n");
    if (nl)
      *nl = '\0';

    /* Index 1 through 16, because a chip may start anywhere: coretemp on the
       development machine begins at 2, nvme and acpitz at 1. */
    for (int t = 1; t <= 16 && s->nsens < MAXSENS; t++) {
      int mc, lim;
      snprintf(p, sizeof p, "%s/temp%d_input", dir, t);
      if (!read_milli(p, &mc))
        continue;

      TempSensor *x = &s->sens[s->nsens];
      scpy(x->chip, sizeof x->chip, chip);
      x->label[0] = '\0';
      snprintf(p, sizeof p, "%s/temp%d_label", dir, t);
      if (read_line(p, x->label, sizeof x->label)) {
        nl = strpbrk(x->label, "\r\n");
        if (nl)
          *nl = '\0';
      }
      /* The critical limit is the one worth drawing; a driver that states only
         a maximum means the same thing to a person watching the graph. */
      lim = 0;
      snprintf(p, sizeof p, "%s/temp%d_crit", dir, t);
      if (!read_milli(p, &lim)) {
        snprintf(p, sizeof p, "%s/temp%d_max", dir, t);
        if (!read_milli(p, &lim))
          lim = 0;
      }
      x->mc = mc;
      x->limit_mc = lim;
      x->cpu = sensor_is_cpu(chip, x->label);

      if (x->cpu && mc > s->cpu_temp_mc)
        s->cpu_temp_mc = mc;
      if (best < 0 || mc > s->sens[best].mc)
        best = s->nsens;
      s->nsens++;
    }
  }

  if (best >= 0) {
    s->temp_max_mc = s->sens[best].mc;
    /* The line drawn is the lowest limit any sensor states, not the limit of
       the hottest one. The hottest input on a laptop is often an ACPI board
       zone that states nothing, and a danger line that vanishes because of that
       says less than the throttle point of the processor sitting beside it.
       Read it as "the temperature at which something in this machine objects". */
    int lim = 0;
    for (int i = 0; i < s->nsens; i++) {
      int l = s->sens[i].limit_mc;
      if (l > 0 && (lim == 0 || l < lim))
        lim = l;
    }
    s->temp_limit_mc = lim;
  }
}

static void resolve_freq(void) {
  const char *cands[] = {
      "/sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq",
      "/sys/devices/system/cpu/cpu0/cpufreq/cpuinfo_cur_freq",
  };
  for (size_t i = 0; i < sizeof cands / sizeof cands[0]; i++)
    if (access(cands[i], R_OK) == 0) {
      scpy(g_freq, sizeof g_freq, cands[i]);
      return;
    }
}

static int count_processes(void) {
  DIR *d = opendir("/proc");
  if (!d)
    return 0;
  int c = 0;
  struct dirent *e;
  while ((e = readdir(d))) {
    if (!isdigit((unsigned char)e->d_name[0]))
      continue;
    char *end;
    long v = strtol(e->d_name, &end, 10);
    if (v > 0 && *end == '\0')
      c++;
  }
  closedir(d);
  return c;
}

void stats_init(SysStats *s) {
  memset(s, 0, sizeof *s);
  resolve_root_disk();
  resolve_net_if();
  resolve_gpu();
  resolve_freq();
  /* Read once here as well as on every sample: the device list decides whether
     the Temperature row exists at all, and it is built before the first tick. */
  collect_temps(s);
  resolve_mem_type(s->mem_type, sizeof s->mem_type);

  FILE *f = fopen("/proc/cpuinfo", "r");
  char line[256];
  if (f) {
    while (fgets(line, sizeof line, f)) {
      if (!strncmp(line, "model name", 10)) {
        char *colon = strchr(line, ':');
        if (colon) {
          scpy(s->cpu_model, sizeof s->cpu_model, trim(colon + 1));
          squeeze(s->cpu_model);
        }
        break;
      }
    }
    fclose(f);
  }
  if (!s->cpu_model[0]) {
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    snprintf(s->cpu_model, sizeof s->cpu_model, "%ld CPU", n > 0 ? n : 1L);
  }

  if (g_root_disk[0]) {
    scpy(s->disk_name, sizeof s->disk_name, g_root_disk);
    char p[128], buf[128];
    snprintf(p, sizeof p, "/sys/block/%s/device/model", g_root_disk);
    if (read_line(p, buf, sizeof buf))
      scpy(s->disk_model, sizeof s->disk_model, trim(buf));
    snprintf(p, sizeof p, "/sys/block/%s/queue/rotational", g_root_disk);
    unsigned long long v;
    s->disk_rotational = read_u64(p, &v) ? (int)v : -1;
    snprintf(p, sizeof p, "/sys/block/%s/size", g_root_disk);
    if (read_u64(p, &v))
      s->disk_capacity_bytes = v * 512ULL;
  }

  if (g_net_if[0]) {
    scpy(s->net_if, sizeof s->net_if, g_net_if);
    char p[128];
    unsigned long long v;
    snprintf(p, sizeof p, "/sys/class/net/%s/speed", g_net_if);
    if (read_u64(p, &v) && v > 0)
      s->net_link_mbps = (long)v;
    else
      read_wireless(g_net_if, s);
  }

  if (g_gpu_dir[0]) {
    s->gpu_present = 1;
    s->gpu_measured = g_gpu_busy[0] != '\0';
    scpy(s->gpu_name, sizeof s->gpu_name, g_gpu_name);
  }
}

/* ---------- per-tick sampling ---------- */

typedef struct {
  unsigned long long total, idle;
} Snap;

static Snap prev_cpu;
static Snap prev_core[MAXCORE];
static unsigned long long prev_sect_rd, prev_sect_wr, prev_busy_ms;
static unsigned long long prev_rx, prev_tx;
static double last_ms;
static int have_prev;

static void parse_snap(Snap *out) {
  unsigned long long v[10] = {0};
  int nv = 0;
  char *rest;
  while (nv < 10 && (rest = strtok(NULL, " \n")))
    v[nv++] = strtoull(rest, NULL, 10);
  if (nv < 4)
    return;
  unsigned long long total = 0;
  for (int i = 0; i < nv; i++)
    total += v[i];
  out->total = total;
  out->idle = v[3] + (nv > 4 ? v[4] : 0);
}

static double pct_delta(const Snap *p, const Snap *c) {
  if (!p->total || c->total <= p->total)
    return 0.0;
  double dt = (double)(c->total - p->total);
  double di = (double)(c->idle - p->idle);
  if (di < 0)
    di = 0;
  if (di > dt)
    di = dt;
  return (dt - di) / dt * 100.0;
}

static void sample_cpu(SysStats *s, double el) {
  FILE *f = fopen("/proc/stat", "r");
  if (!f)
    return;
  char line[512];
  Snap tot = {0, 0};
  Snap cores[MAXCORE];
  memset(cores, 0, sizeof cores);
  memset(s->core_pct, 0, sizeof s->core_pct);
  int maxidx = -1;
  while (fgets(line, sizeof line, f)) {
    if (memcmp(line, "cpu", 3) != 0)
      continue;
    char *tok = strtok(line, " \n");
    if (tok[3] == '\0') {
      parse_snap(&tot);
    } else {
      int idx = atoi(tok + 3);
      if (idx >= 0 && idx < MAXCORE) {
        parse_snap(&cores[idx]);
        if (idx > maxidx)
          maxidx = idx;
      }
    }
  }
  fclose(f);
  s->ncore = maxidx + 1;

  if (have_prev && el > 0) {
    s->cpu_pct = pct_delta(&prev_cpu, &tot);
    for (int i = 0; i <= maxidx; i++)
      s->core_pct[i] = pct_delta(&prev_core[i], &cores[i]);
  }
  prev_cpu = tot;
  memcpy(prev_core, cores, sizeof cores);

  unsigned long long v;
  s->freq_ghz = (g_freq[0] && read_u64(g_freq, &v)) ? (double)v / 1e6 : 0.0;
  if (read_u64("/proc/uptime", &v))
    s->uptime_s = v;

  f = fopen("/proc/loadavg", "r");
  if (f) {
    if (fscanf(f, "%lf", &s->load1) != 1)
      s->load1 = 0.0;
    fclose(f);
  }
  s->nproc = count_processes();
}

static void sample_mem(SysStats *s) {
  FILE *f = fopen("/proc/meminfo", "r");
  if (!f)
    return;
  char line[160];
  double tot = 0, avail = 0, cached = 0, buffers = 0, swap = 0, swapfree = 0;
  while (fgets(line, sizeof line, f)) {
    char key[64];
    unsigned long long v;
    if (sscanf(line, "%63[^:]: %llu", key, &v) == 2) {
      if (!strcmp(key, "MemTotal")) tot = v;
      else if (!strcmp(key, "MemAvailable")) avail = v;
      else if (!strcmp(key, "Cached")) cached = v;
      else if (!strcmp(key, "Buffers")) buffers = v;
      else if (!strcmp(key, "SwapTotal")) swap = v;
      else if (!strcmp(key, "SwapFree")) swapfree = v;
    }
  }
  fclose(f);
  s->mem_total_kb = tot;
  s->mem_avail_kb = avail;
  s->mem_cached_kb = cached;
  s->mem_buffers_kb = buffers;
  s->swap_total_kb = swap;
  s->swap_used_kb = swap > swapfree ? swap - swapfree : 0;
}

/* /proc/diskstats, tokens after the device name:
   0 reads  1 reads_merged  2 sectors_read  3 ms_reading  4 writes  5 writes_merged
   6 sectors_written  7 ms_writing  8 inflight  9 ms_doing_io  10 weighted_ms */
static void sample_disk(SysStats *s, double el) {
  if (!g_root_disk[0])
    return;
  FILE *f = fopen("/proc/diskstats", "r");
  if (!f)
    return;
  char line[256];
  unsigned long long sect_rd = 0, sect_wr = 0, busy_ms = 0, inflight = 0;
  int found = 0;
  while (fgets(line, sizeof line, f)) {
    unsigned long long maj, min, t[11];
    char name[64];
    int n = sscanf(line,
                   "%llu %llu %63s %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu",
                   &maj, &min, name, &t[0], &t[1], &t[2], &t[3], &t[4], &t[5], &t[6], &t[7],
                   &t[8], &t[9], &t[10]);
    if (n < 14 || strcmp(name, g_root_disk) != 0)
      continue;
    sect_rd = t[2];
    sect_wr = t[6];
    inflight = t[8];
    busy_ms = t[9];
    found = 1;
    break;
  }
  fclose(f);
  if (!found)
    return;

  if (have_prev && el > 0) {
    double rd = (double)(sect_rd - prev_sect_rd) * 512.0 / (1024.0 * 1024.0) / el;
    double wr = (double)(sect_wr - prev_sect_wr) * 512.0 / (1024.0 * 1024.0) / el;
    double ut = (double)(busy_ms - prev_busy_ms) / (el * 1000.0) * 100.0;
    s->disk_rd_mbps = rd > 0 ? rd : 0;
    s->disk_wr_mbps = wr > 0 ? wr : 0;
    s->disk_util_pct = ut < 0 ? 0 : (ut > 100 ? 100 : ut);
  }
  prev_sect_rd = sect_rd;
  prev_sect_wr = sect_wr;
  prev_busy_ms = busy_ms;
  s->disk_io_inflight = inflight;
}

static void sample_net(SysStats *s, double el) {
  if (!g_net_if[0])
    return;
  if (s->net_link_mbps <= 0)
    read_wireless(g_net_if, s);
  char p[128];
  unsigned long long r = 0, t = 0;
  snprintf(p, sizeof p, "/sys/class/net/%s/statistics/rx_bytes", g_net_if);
  int ok1 = read_u64(p, &r);
  snprintf(p, sizeof p, "/sys/class/net/%s/statistics/tx_bytes", g_net_if);
  int ok2 = read_u64(p, &t);
  if (!ok1 || !ok2)
    return;
  s->net_rx_total = r;
  s->net_tx_total = t;
  if (have_prev && el > 0) {
    double rx = (double)(r - prev_rx) / (1024.0 * 1024.0) / el;
    double tx = (double)(t - prev_tx) / (1024.0 * 1024.0) / el;
    s->net_rx_mbps = rx > 0 ? rx : 0;
    s->net_tx_mbps = tx > 0 ? tx : 0;
  }
  prev_rx = r;
  prev_tx = t;
}

static void sample_gpu(SysStats *s) {
  if (!g_gpu_busy[0])
    return;
  unsigned long long v;
  if (read_u64(g_gpu_busy, &v))
    s->gpu_pct = (double)v;
  if (g_gpu_temp[0] && read_u64(g_gpu_temp, &v))
    s->gpu_temp_mc = (int)v;
  if (g_gpu_vram_used[0] && read_u64(g_gpu_vram_used, &v))
    s->gpu_vram_used = v;
  if (g_gpu_vram_total[0] && read_u64(g_gpu_vram_total, &v))
    s->gpu_vram_total = v;
}

/* ---------- processes ---------- */

/* Counters from the previous sweep, hashed by pid. Two tables, because the
   sweep writes the next one while still reading the old one: that way a pid
   that disappeared is forgotten instead of lingering and being matched by a
   recycled pid later, which would produce a large and completely false
   percentage. A negative delta (pid reuse between the two reads) is reported
   as unknown rather than as a number. */
#define PREV_SLOTS 4096

typedef struct {
  int pid;
  unsigned long long cpu;
  unsigned long long rd, wr;
} ProcPrev;

static ProcPrev g_prev[PREV_SLOTS], g_next[PREV_SLOTS];
static double g_proc_prev_ms;

static ProcPrev *prev_find(ProcPrev *tab, int pid) {
  unsigned h = (unsigned)pid * 2654435761u;
  for (int i = 0; i < 8; i++) {
    ProcPrev *p = &tab[(h + i) & (PREV_SLOTS - 1)];
    if (p->pid == pid)
      return p;
    if (p->pid == 0)
      return NULL; /* linear probing never skips an empty slot */
  }
  return NULL;
}

static void prev_store(ProcPrev *tab, int pid, unsigned long long cpu, unsigned long long rd,
                       unsigned long long wr) {
  unsigned h = (unsigned)pid * 2654435761u;
  for (int i = 0; i < 8; i++) {
    ProcPrev *p = &tab[(h + i) & (PREV_SLOTS - 1)];
    if (p->pid == 0 || p->pid == pid) {
      p->pid = pid;
      p->cpu = cpu;
      p->rd = rd;
      p->wr = wr;
      return;
    }
  }
}

/* Clock ticks per second of the counters in /proc: 100 on every Linux the
   project targets, read anyway so an unusual kernel cannot skew a percentage. */
static double proc_hz(void) {
  static long hz = 0;
  if (hz <= 0) {
    hz = sysconf(_SC_CLK_TCK);
    if (hz <= 0)
      hz = 100;
  }
  return (double)hz;
}

/* The wall-clock second the machine booted, cached for the life of the process.
   /proc/<pid>/stat 22 counts from the boot, so a start time is only a date once
   this is known. Booting is not something a session notices mid-run, and if it
   somehow did, the process list would have far bigger problems than a wrong
   elapsed time. */
static unsigned long long boot_epoch(void) {
  static unsigned long long btime = 0;
  if (btime)
    return btime;
  FILE *f = fopen("/proc/stat", "r");
  if (f) {
    char line[128];
    while (fgets(line, sizeof line, f)) {
      if (!strncmp(line, "btime ", 6)) {
        btime = strtoull(line + 6, NULL, 10);
        break;
      }
    }
    fclose(f);
  }
  return btime;
}

/* Two things can be wrong with a field that came out of the kernel a byte at a
   time: the truncation can stop inside a multi-byte character — comm is cut at
   15 bytes and the command line at our own limit — and an argument may simply
   hold bytes that are not UTF-8 at all. Pango answers either way with a warning
   and a box, so the collector fixes both here: a character the cut left
   unfinished is dropped, and a byte that cannot start a well-formed sequence
   becomes a dot. */
static void clean_utf8(char *s) {
  char *r = s, *w = s;
  while (*r) {
    unsigned char c = (unsigned char)*r;
    int extra;
    if (c < 0x80)
      extra = 0;
    else if (c >= 0xC2 && c <= 0xDF)
      extra = 1;
    else if (c >= 0xE0 && c <= 0xEF)
      extra = 2;
    else if (c >= 0xF0 && c <= 0xF4)
      extra = 3;
    else { /* a continuation byte on its own, an overlong lead, or 0xF5 and up */
      *w++ = '.';
      r++;
      continue;
    }
    int cut = 0, bad = 0;
    for (int i = 1; i <= extra; i++) {
      if (r[i] == '\0') {
        cut = 1;
        break;
      }
      if (((unsigned char)r[i] & 0xC0) != 0x80) {
        bad = 1;
        break;
      }
    }
    if (cut)
      break; /* the field ends inside this character: leave it out */
    if (bad) {
      *w++ = '.';
      r++;
      continue;
    }
    for (int i = 0; i <= extra; i++)
      *w++ = *r++;
  }
  *w = '\0';
}

/* The owner's login name. The kernel only gives a number, and one uid repeats
   across dozens of rows, so the passwd lookup happens once per uid rather than
   once per process. An uid with no entry — a removed account, a container mapping
   — is shown as its number, which is the honest answer rather than a blank. */
static void user_name(uid_t uid, char *dst, size_t dn) {
  struct entry {
    uid_t uid;
    char name[20];
  };
  static struct entry cache[16];
  static int n, next;

  for (int i = 0; i < n; i++) {
    if (cache[i].uid == uid) {
      scpy(dst, dn, cache[i].name);
      return;
    }
  }
  char nm[20];
  struct passwd *pw = getpwuid(uid);
  if (pw && pw->pw_name && *pw->pw_name)
    scpy(nm, sizeof nm, pw->pw_name);
  else
    snprintf(nm, sizeof nm, "%lu", (unsigned long)uid);

  if (n < 16) {
    cache[n].uid = uid;
    scpy(cache[n].name, sizeof cache[n].name, nm);
    n++;
  } else {
    cache[next].uid = uid;
    scpy(cache[next].name, sizeof cache[next].name, nm);
    next = (next + 1) % 16;
  }
  scpy(dst, dn, nm);
  clean_utf8(dst);
}


/* argv as the shell would have typed it. The kernel stores the arguments
   separated by NULs; a kernel thread has an empty file and stays blank, the way
   the Details list leaves it. Reading the file costs one open per process, so it
   only happens on a details sweep.

   Control bytes become spaces and a run of them collapses to one, because a
   table cell is one line tall and an argument may be a whole script: `sh -c`
   with a here-document, or `node --eval`, both carry newlines, and Pango obeys
   them — the row turns into two lines and the list looks like it has empty rows
   in it. */
static void read_cmdline(int pid, char *dst, size_t dn) {
  dst[0] = '\0';
  char path[64], buf[512];
  snprintf(path, sizeof path, "/proc/%d/cmdline", pid);
  FILE *f = fopen(path, "r");
  if (!f)
    return;
  size_t got = fread(buf, 1, sizeof buf - 1, f);
  fclose(f);
  if (got == 0)
    return;
  buf[got] = '\0';

  size_t o = 0;
  for (int first = 1, i = 0; (size_t)i < got; first = 0) {
    size_t l = strlen(buf + i);
    if (!first && o < dn - 1)
      dst[o++] = ' '; /* the separator between two arguments */
    int space = o > 0 && dst[o - 1] == ' ';
    for (size_t k = 0; k < l && o < dn - 1; k++) {
      unsigned char c = (unsigned char)buf[i + k];
      if (c < 0x20 || c == 0x7f)
        c = ' ';
      if (c == ' ' && space)
        continue;
      space = (c == ' ');
      dst[o++] = (char)c;
    }
    i += l + 1;
    if (i == 0 || (size_t)i > got)
      break; /* the buffer was full of one argument, or ran past the end */
  }
  while (o && dst[o - 1] == ' ')
    o--;
  dst[o] = '\0';
  clean_utf8(dst);
}

/* One read gives the name, the CPU time, the resident set and the rest of the
   process's own accounting: /proc/<pid>/stat carries all of it, so the list
   costs one open+read per process instead of several. The name is what sits
   between the first '(' and the last ')' because a process may put parentheses
   in it.

   Everything the Details tab shows that is not a rate lives here — state, the
   parent, thread count, priority and nice, the lifetime CPU time and the start
   time — because they are more tokens of a line we already read. Only the two
   fields that need a syscall of their own (the owner's name and the command
   line) are left to the caller. */
static int read_stat(int pid, ProcRow *r, unsigned long long *cpu_out) {
  char path[64], buf[1024];
  snprintf(path, sizeof path, "/proc/%d/stat", pid);
  FILE *f = fopen(path, "r");
  if (!f)
    return 0;
  size_t got = fread(buf, 1, sizeof buf - 1, f);
  struct stat sb;
  int is_own = fstat(fileno(f), &sb) == 0 && sb.st_uid == geteuid();
  uid_t owner = sb.st_uid;
  fclose(f);
  if (got < 16)
    return 0;
  buf[got] = '\0';

  char *op = strchr(buf, '('), *cl = strrchr(buf, ')');
  if (!op || !cl || cl < op)
    return 0;
  size_t len = (size_t)(cl - op - 1);
  if (len > sizeof r->name - 1)
    len = sizeof r->name - 1;
  memcpy(r->name, op + 1, len);
  /* A process may put anything at all in its own name, including bytes that are
     not text: one newline would make the row two lines tall in either table, and
     half a character is a box. */
  for (size_t i = 0; i < len; i++)
    if ((unsigned char)r->name[i] < 0x20 || r->name[i] == 0x7f)
      r->name[i] = ' ';
  r->name[len] = '\0';
  clean_utf8(r->name);

  static long page_kb;
  if (page_kb <= 0) {
    page_kb = sysconf(_SC_PAGESIZE) / 1024;
    if (page_kb <= 0)
      page_kb = 4;
  }

  /* After the name, field 3 is the state, so the tokens that follow are
     numbered from there. */
  unsigned long long ut = 0, st_ = 0, rss = 0, born = 0;
  char state = 0;
  long ppid = 0, prio = 0, nice_val = 0, threads = 0;
  int field = 2;
  char *p = cl + 1;
  while (*p && field < 24) {
    while (*p == ' ')
      p++;
    if (!*p)
      break;
    char *tok = p;
    while (*p && *p != ' ')
      p++;
    int last = (*p == '\0');
    *p = '\0';
    field++;
    switch (field) {
    case 3:
      state = tok[0];
      break;
    case 4:
      ppid = strtol(tok, NULL, 10);
      break;
    case 14:
      ut = strtoull(tok, NULL, 10);
      break;
    case 15:
      st_ = strtoull(tok, NULL, 10);
      break;
    case 18:
      prio = strtol(tok, NULL, 10);
      break;
    case 19:
      nice_val = strtol(tok, NULL, 10);
      break;
    case 20:
      threads = strtol(tok, NULL, 10);
      break;
    case 22:
      born = strtoull(tok, NULL, 10);
      break;
    case 24:
      rss = strtoull(tok, NULL, 10);
      break;
    }
    if (last)
      break;
    p++;
  }
  if (field < 24)
    return 0;

  *cpu_out = ut + st_;
  r->mem_mb = (double)rss * (double)page_kb / 1024.0;
  r->own = is_own;
  r->uid = owner;
  r->state = state;
  r->ppid = (int)ppid;
  r->threads = (int)threads;
  r->priority = prio;
  r->nice = (int)nice_val;
  r->cpu_s = (double)(ut + st_) / proc_hz();
  r->start_epoch = born ? boot_epoch() + (unsigned long long)(born / proc_hz()) : 0;
  return 1;
}

/* Bytes actually fetched from and written to the block device. This file is
   0400 and owned by the process, so on a normal desktop only our own
   processes answer; the rest keep the column empty. */
static int read_io(int pid, unsigned long long *rd, unsigned long long *wr) {
  char path[64], line[128];
  snprintf(path, sizeof path, "/proc/%d/io", pid);
  FILE *f = fopen(path, "r");
  if (!f)
    return 0;
  int got = 0;
  while (fgets(line, sizeof line, f)) {
    if (!strncmp(line, "read_bytes:", 11)) {
      *rd = strtoull(line + 11, NULL, 10);
      got = 1;
    } else if (!strncmp(line, "write_bytes:", 12)) {
      *wr = strtoull(line + 12, NULL, 10);
      got = 1;
    }
  }
  fclose(f);
  return got;
}

void stats_sample_procs(ProcTable *t, int with_details) {
  memset(t, 0, sizeof *t);
  double now = now_ms();
  double el = (g_proc_prev_ms > 0 && now > g_proc_prev_ms) ? (now - g_proc_prev_ms) / 1000.0 : 0.0;
  if (el > 3600.0)
    el = 3600.0;
  double hz = proc_hz();

  memset(g_next, 0, sizeof g_next);
  DIR *d = opendir("/proc");
  if (!d) {
    memcpy(g_prev, g_next, sizeof g_prev);
    g_proc_prev_ms = now;
    return;
  }
  struct dirent *e;
  while ((e = readdir(d))) {
    if (e->d_name[0] < '0' || e->d_name[0] > '9')
      continue; /* "self" and "thread-self" are not processes */
    int pid = atoi(e->d_name);
    if (pid <= 0)
      continue;
    if (t->n >= MAXPROC) {
      t->overflow = 1;
      break;
    }

    ProcRow *r = &t->rows[t->n];
    memset(r, 0, sizeof *r);
    r->pid = pid;
    unsigned long long cpu = 0;
    if (!read_stat(pid, r, &cpu))
      continue; /* exited between readdir and the read */
    t->n++;

    unsigned long long rd = 0, wr = 0;
    int have_io = r->own && read_io(pid, &rd, &wr);
    if (have_io) {
      r->io_rd = rd;
      r->io_wr = wr;
    }
    if (with_details) {
      user_name(r->uid, r->user, sizeof r->user);
      read_cmdline(pid, r->cmd, sizeof r->cmd);
    }

    double mem = r->mem_mb;
    int own = r->own;
    r->cpu_pct = -1.0;
    r->disk_mbps = -1.0;
    if (own)
      t->n_own++;

    ProcPrev *pp = prev_find(g_prev, pid);
    if (pp && el > 0) {
      if (cpu >= pp->cpu) {
        r->cpu_pct = (double)(cpu - pp->cpu) / ((double)hz * el) * 100.0;
        t->sum_cpu_pct += r->cpu_pct;
      }
      if (have_io && rd >= pp->rd && wr >= pp->wr)
        r->disk_mbps = (double)((rd - pp->rd) + (wr - pp->wr)) / (1024.0 * 1024.0) / el;
    }
    t->sum_mem_mb += mem;
    prev_store(g_next, pid, cpu, rd, wr);
  }
  closedir(d);
  memcpy(g_prev, g_next, sizeof g_prev);
  g_proc_prev_ms = now;
}

void stats_user_name(uid_t uid, char *dst, size_t dn) { user_name(uid, dst, dn); }

/* One pid, read again on demand rather than on a sweep: enough of
   /proc/<pid>/stat to tell a process that is still the one we signalled apart
   from a stranger that has been handed the same number since. */
int stats_pid_same(int pid, unsigned long long start_epoch, char *state_out) {
  char path[64], buf[1024];
  snprintf(path, sizeof path, "/proc/%d/stat", pid);
  FILE *f = fopen(path, "r");
  if (!f)
    return 0;
  size_t got = fread(buf, 1, sizeof buf - 1, f);
  fclose(f);
  if (got < 16)
    return 0;
  buf[got] = '\0';

  /* The name may hold parentheses, so the numbered fields start after the last
     one: field 3 is the state letter, field 22 the time it was born. */
  char *cl = strrchr(buf, ')');
  if (!cl || !cl[1])
    return 0;

  char state = 0;
  unsigned long long born = 0;
  char *p = cl + 1;
  int field = 2;
  while (*p) {
    while (*p == ' ')
      p++;
    if (!*p)
      break;
    field++;
    char *tok = p;
    while (*p && *p != ' ')
      p++;
    if (field == 3)
      state = *tok;
    else if (field == 22) {
      born = strtoull(tok, NULL, 10);
      break;
    }
  }

  if (state_out)
    *state_out = state;
  if (!state)
    return 0;
  if (!start_epoch)
    return 1; /* the snapshot did not know when it started: existence is all this can say */
  return born && boot_epoch() + (unsigned long long)(born / proc_hz()) == start_epoch;
}

void stats_sample(SysStats *s) {
  double now = now_ms();
  double el = (last_ms > 0 && now > last_ms) ? (now - last_ms) / 1000.0 : 1.0;
  if (el > 60.0)
    el = 60.0;
  last_ms = now;

  s->cpu_pct = 0;
  memset(s->core_pct, 0, sizeof s->core_pct);
  s->disk_rd_mbps = s->disk_wr_mbps = s->disk_util_pct = 0;
  s->net_rx_mbps = s->net_tx_mbps = 0;

  sample_cpu(s, el);
  sample_mem(s);
  sample_disk(s, el);
  sample_net(s, el);
  sample_gpu(s);
  collect_temps(s);

  s->have_rates = have_prev;
  have_prev = 1;
}

/* ---------- autostart ---------- */

/* Where the entries live, highest priority first: the user's own directory wins
   over every system one, and inside $XDG_CONFIG_DIRS the earlier entry wins.
   Resolved per scan rather than cached, because $HOME is exactly the thing that
   differs between a session, a build tree and a stick. */
static int startup_dirs(char dirs[][384], int max) {
  int n = 0;
  const char *cfg = getenv("XDG_CONFIG_HOME");
  if (cfg && *cfg) {
    snprintf(dirs[n++], 384, "%.350s/autostart", cfg);
  } else {
    const char *home = getenv("HOME");
    if (!home || !*home) {
      struct passwd *pw = getpwuid(getuid());
      if (pw && pw->pw_dir && *pw->pw_dir)
        home = pw->pw_dir;
    }
    if (home)
      snprintf(dirs[n++], 384, "%.360s/.config/autostart", home);
  }
  if (n == 0)
    return 0; /* no home we can name means no list to show */

  char buf[1024];
  const char *xdg = getenv("XDG_CONFIG_DIRS");
  if (!xdg || !*xdg)
    xdg = "/etc/xdg";
  scpy(buf, sizeof buf, xdg);
  char *save = NULL;
  for (char *tok = strtok_r(buf, ":", &save); tok && n < max; tok = strtok_r(NULL, ":", &save)) {
    char cand[384];
    snprintf(cand, sizeof cand, "%.350s/autostart", tok);
    int dup = 0;
    for (int i = 0; i < n; i++)
      if (!strcmp(dirs[i], cand))
        dup = 1;
    if (!dup)
      scpy(dirs[n++], 384, cand);
  }
  return n;
}

/* XDG_CURRENT_DESKTOP is a colon list ("GNOME-Flashback:GNOME"); OnlyShowIn and
   NotShowIn are semicolon lists. The specification compares them
   case-insensitively. With neither variable set we cannot judge the entry, and
   the honest answer is to leave it alone rather than hide it. */
static const char *current_desktop(void) {
  const char *d = getenv("XDG_CURRENT_DESKTOP");
  if (d && *d)
    return d;
  d = getenv("DESKTOP_SESSION");
  return (d && *d) ? d : NULL;
}

static int colon_list_has(const char *list, const char *name) {
  size_t l = strlen(name);
  if (!l)
    return 0;
  while (*list) {
    const char *e = strchr(list, ':');
    if (!e)
      e = list + strlen(list);
    if ((size_t)(e - list) == l && !strncasecmp(list, name, l))
      return 1;
    list = (*e == ':') ? e + 1 : e;
  }
  return 0;
}

static int semi_list_hits(const char *list, const char *desktop) {
  while (*list) {
    const char *e = list;
    while (*e && *e != ';')
      e++;
    char tok[64];
    size_t l = (size_t)(e - list);
    if (l && l < sizeof tok) {
      memcpy(tok, list, l);
      tok[l] = '\0';
      if (colon_list_has(desktop, tok))
        return 1;
    }
    list = (*e == ';') ? e + 1 : e;
  }
  return 0;
}

typedef struct {
  char name[64], exec[192], comment[128], only[192], notin[192];
  int hidden, gnome_off, have_exec;
} Entry;

/* The %f %u %d ... placeholders stand for files the launcher would pass. There
   are none at login, so the command line is shown without them; %% is a literal
   percent and stays. */
static void strip_field_codes(const char *in, char *out, size_t n) {
  size_t o = 0;
  for (const char *p = in; *p; p++) {
    if (*p == '%' && p[1]) {
      if (p[1] == '%') {
        if (o + 1 < n)
          out[o++] = '%';
        p++;
        continue;
      }
      if (strchr("fudnkicvm", p[1]) && (!p[2] || p[2] == ' ' || p[2] == '\t')) {
        p++;
        continue;
      }
    }
    if (o + 1 < n)
      out[o++] = *p;
  }
  out[o] = '\0';
  scpy(out, n, trim(out));
}

static int read_entry(const char *path, Entry *e) {
  FILE *f = fopen(path, "r");
  if (!f)
    return 0;
  memset(e, 0, sizeof *e);
  char line[512];
  int in_group = 0;
  while (fgets(line, sizeof line, f)) {
    char *t = trim(line);
    if (!*t || *t == '#')
      continue;
    if (*t == '[') {
      in_group = !strncmp(t, "[Desktop Entry]", 15);
      continue;
    }
    if (!in_group)
      continue;
    char *eq = strchr(t, '=');
    if (!eq)
      continue;
    *eq = '\0';
    char *key = trim(t), *val = trim(eq + 1);
    if (!strcmp(key, "Name"))
      scpy(e->name, sizeof e->name, val);
    else if (!strcmp(key, "Exec")) {
      e->have_exec = 1;
      strip_field_codes(val, e->exec, sizeof e->exec);
    } else if (!strcmp(key, "Comment"))
      scpy(e->comment, sizeof e->comment, val);
    else if (!strcmp(key, "OnlyShowIn"))
      scpy(e->only, sizeof e->only, val);
    else if (!strcmp(key, "NotShowIn"))
      scpy(e->notin, sizeof e->notin, val);
    else if (!strcmp(key, "Hidden") && !strcmp(val, "true"))
      e->hidden = 1;
    else if (!strcmp(key, "X-GNOME-Autostart-enabled") && !strcmp(val, "false"))
      e->gnome_off = 1;
  }
  fclose(f);
  return e->name[0] || e->have_exec;
}

static int entry_state(const Entry *e, const char *desk) {
  if (desk) {
    if (e->only[0] && !semi_list_hits(e->only, desk))
      return RUN_OTHER_DE;
    if (e->notin[0] && semi_list_hits(e->notin, desk))
      return RUN_OTHER_DE;
  }
  if (e->hidden || e->gnome_off)
    return RUN_OFF;
  return RUN_ON;
}

static int row_cmp(const void *a, const void *b) {
  const StartupRow *x = a, *y = b;
  int r = strcasecmp(x->name, y->name);
  if (!r)
    r = strcmp(x->base, y->base); /* two entries with the same display name still need one order */
  return r;
}

void stats_scan_startup(StartupTable *t) {
  memset(t, 0, sizeof *t);
  char dirs[8][384];
  int nd = startup_dirs(dirs, 8);
  if (!nd)
    return;
  const char *desk = current_desktop();
  int writable = !access(dirs[0], W_OK);

  for (int d = 0; d < nd; d++) {
    DIR *dir = opendir(dirs[d]);
    if (!dir)
      continue;
    struct dirent *e;
    while ((e = readdir(dir))) {
      const char *n = e->d_name;
      size_t nl = strlen(n);
      if (nl < 9 || strcmp(n + nl - 8, ".desktop"))
        continue;
      int taken = 0;
      for (int i = 0; i < t->n; i++)
        if (!strcmp(t->rows[i].base, n))
          taken = 1; /* a higher-priority directory already claimed this name */
      if (taken)
        continue;
      if (t->n >= MAXSTARTUP) {
        t->overflow = 1;
        break;
      }
      char path[640];
      snprintf(path, sizeof path, "%.380s/%.250s", dirs[d], n);
      Entry en;
      if (!read_entry(path, &en))
        continue;

      StartupRow *r = &t->rows[t->n++];
      scpy(r->base, sizeof r->base, n);
      if (en.name[0])
        scpy(r->name, sizeof r->name, en.name);
      else {
        char *dot = strchr(r->base, '.'); /* an entry without a name is still the file it is */
        if (dot)
          *dot = '\0';
        scpy(r->name, sizeof r->name, r->base);
        if (dot)
          *dot = '.';
      }
      scpy(r->exec, sizeof r->exec, en.exec);
      scpy(r->comment, sizeof r->comment, en.comment);
      r->state = entry_state(&en, desk);
      r->writable = writable;
      if (d == 0)
        scpy(r->user_path, sizeof r->user_path, path);
      else
        scpy(r->sys_path, sizeof r->sys_path, path);

      /* The same name in a lower-priority directory: remember it, because that is
         what tells an override apart from an entry the user wrote themselves. */
      for (int dd = d + 1; dd < nd; dd++) {
        char p2[640];
        snprintf(p2, sizeof p2, "%.380s/%.250s", dirs[dd], n);
        if (access(p2, F_OK) == 0) {
          scpy(r->sys_path, sizeof r->sys_path, p2);
          break;
        }
      }
    }
    closedir(dir);
  }

  qsort(t->rows, (size_t)t->n, sizeof t->rows[0], row_cmp);
  for (int i = 0; i < t->n; i++) {
    if (t->rows[i].state == RUN_ON)
      t->n_on++;
    else if (t->rows[i].state == RUN_OFF)
      t->n_off++;
    else
      t->n_other++;
  }
}

/* Rewrite the two keys that decide the state inside [Desktop Entry], leaving
   every other byte of the file as its author wrote it. Both keys are written
   because both are in use out there: Hidden is what this machine's XFCE reads,
   X-GNOME-Autostart-enabled is what GNOME, Cinnamon, MATE and LXQt read, and a
   file carrying both is understood by all of them. The temporary file and the
   rename mean a crash cannot leave the entry half written. */
#define DESKTOP_MAX_LINES 256

static int set_desktop_keys(const char *path, int enable) {
  static char lines[DESKTOP_MAX_LINES][512];
  struct stat before;
  if (stat(path, &before) && errno != ENOENT)
    return errno;

  int n = 0;
  FILE *f = fopen(path, "r");
  if (f) {
    while (n < DESKTOP_MAX_LINES) {
      if (!fgets(lines[n], sizeof lines[n], f))
        break;
      n++;
    }
    if (n == DESKTOP_MAX_LINES) {
      fclose(f);
      return E2BIG; /* not rewriting a file we cannot hold: refuse rather than truncate */
    }
    fclose(f);
  } else if (errno != ENOENT) {
    return errno;
  }

  int g = -1;
  for (int i = 0; i < n; i++) {
    char *t = lines[i];
    while (*t == ' ' || *t == '\t')
      t++;
    if (!strncmp(t, "[Desktop Entry]", 15)) {
      g = i;
      break;
    }
  }
  int end = n;
  if (g >= 0) {
    for (int i = g + 1; i < n; i++) {
      char *t = lines[i];
      while (*t == ' ' || *t == '\t')
        t++;
      if (*t == '[') {
        end = i;
        break;
      }
    }
  }

  char out[DESKTOP_MAX_LINES][512];
  int o = 0, wrote_hidden = 0, wrote_gnome = 0;
  const char *hv = enable ? "Hidden=false\n" : "Hidden=true\n";
  const char *gv = enable ? "X-GNOME-Autostart-enabled=true\n" : "X-GNOME-Autostart-enabled=false\n";

  for (int i = 0; i < n; i++) {
    if (i == g) {
      scpy(out[o++], sizeof out[0], lines[i]);
      snprintf(out[o++], sizeof out[0], "%s", hv);
      snprintf(out[o++], sizeof out[0], "%s", gv);
      wrote_hidden = wrote_gnome = 1;
      continue;
    }
    if (i > g && i < end) {
      char *t = lines[i];
      while (*t == ' ' || *t == '\t')
        t++;
      if (!strncmp(t, "Hidden=", 7) || !strncmp(t, "X-GNOME-Autostart-Enabled=", 26) ||
          !strncmp(t, "X-GNOME-Autostart-enabled=", 26)) {
        if (!strncmp(t, "Hidden=", 7)) {
          if (wrote_hidden)
            continue; /* a duplicate would only argue with the first */
          wrote_hidden = 1;
        } else {
          if (wrote_gnome)
            continue;
          wrote_gnome = 1;
        }
        snprintf(out[o++], sizeof out[0], "%s", strncmp(t, "Hidden=", 7) ? gv : hv);
        continue;
      }
    }
    scpy(out[o++], sizeof out[0], lines[i]);
  }
  if (g < 0) { /* a file without the group header is not a valid entry; make ours one */
    snprintf(out[o++], sizeof out[0], "\n[Desktop Entry]\n");
    snprintf(out[o++], sizeof out[0], "%s", hv);
    snprintf(out[o++], sizeof out[0], "%s", gv);
  }

  char tmp[512];
  snprintf(tmp, sizeof tmp, "%.480s.%ld.tmp", path, (long)getpid());
  FILE *w = fopen(tmp, "w");
  if (!w)
    return errno;
  for (int i = 0; i < o; i++)
    fputs(out[i], w);
  if (fflush(w) || ferror(w)) {
    int err = errno;
    fclose(w);
    unlink(tmp);
    return err ? err : EIO;
  }
  if (fclose(w)) {
    int err = errno;
    unlink(tmp);
    return err;
  }
  if (!S_ISREG(before.st_mode))
    before.st_mode = 0644;
  if (chmod(tmp, before.st_mode & 07777)) {
    int err = errno;
    unlink(tmp);
    return err;
  }
  if (rename(tmp, path)) {
    int err = errno;
    unlink(tmp);
    return err;
  }
  return 0;
}

static int copy_file(const char *from, const char *to) {
  FILE *in = fopen(from, "r");
  if (!in)
    return errno;
  char tmp[512];
  snprintf(tmp, sizeof tmp, "%.480s.%ld.tmp", to, (long)getpid());
  FILE *out = fopen(tmp, "w");
  if (!out) {
    int err = errno;
    fclose(in);
    return err;
  }
  char buf[4096];
  size_t got;
  int err = 0;
  while ((got = fread(buf, 1, sizeof buf, in)) > 0) {
    if (fwrite(buf, 1, got, out) != got) {
      err = EIO;
      break;
    }
  }
  fclose(in);
  if (!err && fflush(out))
    err = errno;
  if (fclose(out) && !err)
    err = errno;
  if (err) {
    unlink(tmp);
    return err;
  }
  struct stat st;
  if (!stat(from, &st))
    chmod(tmp, st.st_mode & 07777);
  if (rename(tmp, to)) {
    unlink(tmp);
    return errno;
  }
  return 0;
}

int stats_startup_set(StartupRow *r, int enable) {
  if (r->state == RUN_OTHER_DE)
    return EACCES; /* its OnlyShowIn decides, not us, and editing that would lie about the entry */
  if (enable && !r->user_path[0] && r->state == RUN_ON)
    return 0; /* a system entry that is already on needs no file */

  if (!r->user_path[0]) {
    char dirs[1][384];
    if (!startup_dirs(dirs, 1))
      return ENOENT;
    if (access(dirs[0], W_OK))
      return errno ? errno : EACCES;
    if (mkdir(dirs[0], 0755) && errno != EEXIST)
      return errno;
    char composed[640];
    int used = snprintf(composed, sizeof composed, "%s/%s", dirs[0], r->base);
    if (used < 0 || (size_t)used >= sizeof composed || strlen(composed) >= sizeof r->user_path)
      return ENAMETOOLONG; /* refusing to write a path we cannot hold in full */
    scpy(r->user_path, sizeof r->user_path, composed);

    /* The override has to carry the entry itself: a user file with the same name
       replaces the system one completely, so an empty one would delete the
       autostart rather than switch it off. */
    if (r->sys_path[0]) {
      int err = copy_file(r->sys_path, r->user_path);
      if (err)
        return err;
    }
  }
  return set_desktop_keys(r->user_path, enable);
}

/* ---------- machine information ---------- */

/* One world-readable file into a fixed buffer, with the newline and the padding
   spaces that SMBIOS strings carry removed. An unreadable file leaves the field
   empty, which is what tells the tab to drop the row. */
static void info_file(char *out, size_t n, const char *path) {
  out[0] = '\0';
  char buf[192];
  if (!read_line(path, buf, sizeof buf))
    return;
  scpy(out, n, trim(buf));
}

/* The block kinds that are not disks: loop devices behind snap squashfs, ram
   disks, the device-mapper rows that describe the same partition twice, and the
   optical drive, which is a reader rather than storage. */
static int info_skipped(const char *name) {
  static const char *pre[] = {"loop", "ram", "zram", "sr", "fd", "dm-", "md"};
  for (size_t i = 0; i < sizeof pre / sizeof pre[0]; i++)
    if (strncmp(name, pre[i], strlen(pre[i])) == 0)
      return 1;
  return 0;
}

static void info_os_release(char *out, size_t n) {
  out[0] = '\0';
  FILE *f = fopen("/etc/os-release", "r");
  if (!f)
    return;
  char line[256];
  while (fgets(line, sizeof line, f)) {
    if (strncmp(line, "PRETTY_NAME=", 12) != 0)
      continue;
    char *v = trim(line + 12);
    size_t l = strlen(v);
    if (l > 1 && v[0] == '"' && v[l - 1] == '"') {
      v[l - 1] = '\0';
      v++;
    }
    scpy(out, n, v);
    break;
  }
  fclose(f);
}

/* x86 calls the field "model name", ARM boards call theirs "Model"; both are a
   label, a colon and the string the vendor wrote. */
static void info_cpuinfo(SysInfo *i) {
  FILE *f = fopen("/proc/cpuinfo", "r");
  if (!f)
    return;
  char line[256];
  while (fgets(line, sizeof line, f)) {
    char *v = strchr(line, ':');
    if (!v)
      continue;
    /* x86 writes a tab between the field name and the colon, ARM boards write
       none, so the name is compared after trimming instead of as a prefix. */
    *v++ = '\0';
    char *key = trim(line);
    squeeze(key);
    if (!i->cpu_model[0] && (!strcmp(key, "model name") || !strcmp(key, "Model"))) {
      scpy(i->cpu_model, sizeof i->cpu_model, trim(v));
      squeeze(i->cpu_model);
    } else if (!i->cpu_vendor[0] && !strcmp(key, "vendor_id")) {
      scpy(i->cpu_vendor, sizeof i->cpu_vendor, trim(v));
    } else if (!i->cpu_cores && !strcmp(key, "cpu cores"))
      i->cpu_cores = (int)strtol(v, NULL, 10);
    /* Only the first package answers: a second one repeats the model and would
       contradict it if the machine mixed processors. */
    if (i->cpu_model[0] && i->cpu_vendor[0] && i->cpu_cores)
      break;
  }
  fclose(f);
}

static void info_cache(SysInfo *i) {
  for (int k = 0; k < 10 && i->ncache < INFO_CACHE; k++) {
    char dir[96], p[128], v[32];
    snprintf(dir, sizeof dir, "/sys/devices/system/cpu/cpu0/cache/index%d", k);
    snprintf(p, sizeof p, "%s/size", dir);
    if (!read_line(p, v, sizeof v))
      continue;
    long kb = strtol(v, NULL, 10);
    if (strchr(v, 'M'))
      kb *= 1024;
    if (kb <= 0)
      continue;
    int level = 0;
    snprintf(p, sizeof p, "%s/level", dir);
    if (read_line(p, v, sizeof v))
      level = (int)strtol(v, NULL, 10);
    char type[12] = "";
    snprintf(p, sizeof p, "%s/type", dir);
    if (read_line(p, v, sizeof v))
      scpy(type, sizeof type, trim(v));
    i->cache[i->ncache].size_kb = (int)kb;
    i->cache[i->ncache].level = level;
    scpy(i->cache[i->ncache].type, sizeof i->cache[i->ncache].type, type);
    i->ncache++;
  }
}

/* The EEPROMs the kernel bound from the memory slots. Counting them says how
   many modules the firmware saw, which is the one hardware fact here that is
   not already in /proc/meminfo; the type of memory is not repeated because the
   Memory page resolves it on every sample. */
static int info_modules(void) {
  glob_t gl;
  int n = 0;
  if (glob("/sys/bus/i2c/devices/*/name", 0, NULL, &gl) != 0)
    return 0;
  for (size_t i = 0; i < gl.gl_pathc; i++) {
    char nm[32];
    info_file(nm, sizeof nm, gl.gl_pathv[i]);
    if (strcmp(nm, "spd") == 0 || strcmp(nm, "ee1004") == 0)
      n++;
  }
  globfree(&gl);
  return n;
}

/* What the block layer decided about this disk. `queue/scheduler` lists every
   choice with the active one in brackets, so the brackets are what the row
   answers; a disk whose driver states none of this keeps the fields empty and
   contributes no row. `rev` is a firmware version, not a unit identifier. */
static void info_queue(DiskInfo *k, const char *dev) {
  char p[288];
  unsigned long long u;
  k->read_ahead_kb = -1;
  snprintf(p, sizeof p, "/sys/block/%s/queue/logical_block_size", dev);
  if (read_u64(p, &u))
    k->logical_bs = (int)u;
  snprintf(p, sizeof p, "/sys/block/%s/queue/physical_block_size", dev);
  if (read_u64(p, &u))
    k->physical_bs = (int)u;
  snprintf(p, sizeof p, "/sys/block/%s/queue/read_ahead_kb", dev);
  if (read_u64(p, &u))
    k->read_ahead_kb = (long)u;
  snprintf(p, sizeof p, "/sys/block/%s/queue/scheduler", dev);
  char line[96];
  if (read_line(p, line, sizeof line)) {
    char *b = strchr(line, '[');
    if (b) {
      char *e = strchr(b, ']');
      if (e)
        *e = '\0';
      scpy(k->sched, sizeof k->sched, trim(b + 1));
    } else {
      scpy(k->sched, sizeof k->sched, trim(line));
    }
  }
  snprintf(p, sizeof p, "/sys/block/%s/device/rev", dev);
  info_file(k->rev, sizeof k->rev, p);
}

static void info_disks(SysInfo *i) {
  DIR *d = opendir("/sys/block");
  if (!d)
    return;
  struct dirent *e;
  while ((e = readdir(d)) && i->ndisk < INFO_DISK) {
    if (e->d_name[0] == '.' || info_skipped(e->d_name))
      continue;
    char p[288];
    unsigned long long sectors;
    snprintf(p, sizeof p, "/sys/block/%s/size", e->d_name);
    if (!read_u64(p, &sectors) || sectors == 0)
      continue;
    DiskInfo *k = &i->disk[i->ndisk];
    scpy(k->name, sizeof k->name, e->d_name);
    k->bytes = sectors * 512ULL;
    unsigned long long rot;
    snprintf(p, sizeof p, "/sys/block/%s/queue/rotational", e->d_name);
    k->rotational = read_u64(p, &rot) && rot;
    snprintf(p, sizeof p, "/sys/block/%s/device/model", e->d_name);
    info_file(k->model, sizeof k->model, p);
    info_queue(k, e->d_name);
    i->ndisk++;
  }
  closedir(d);
}

static void info_filesystems(SysInfo *i) {
  FILE *m = setmntent("/proc/mounts", "r");
  if (!m)
    return;
  struct mntent *e;
  while ((e = getmntent(m)) && i->nfs < INFO_FS) {
    if (strncmp(e->mnt_fsname, "/dev/", 5) != 0)
      continue;
    const char *base = e->mnt_fsname + 5;
    if (info_skipped(base))
      continue;
    int dup = 0;
    for (int k = 0; k < i->nfs; k++)
      if (strcmp(i->fs[k].mnt, e->mnt_dir) == 0)
        dup = 1;
    if (dup)
      continue; /* a bind mount of a filesystem already listed */
    struct statvfs sv;
    if (statvfs(e->mnt_dir, &sv) != 0)
      continue;
    FsInfo *f = &i->fs[i->nfs];
    scpy(f->dev, sizeof f->dev, base);
    scpy(f->mnt, sizeof f->mnt, e->mnt_dir);
    scpy(f->fstype, sizeof f->fstype, e->mnt_type);
    f->total = (unsigned long long)sv.f_blocks * sv.f_frsize;
    f->free = (unsigned long long)sv.f_bavail * sv.f_frsize;
    i->nfs++;
  }
  endmntent(m);
}

/* ---------- the rest of the Information tab ---------- */

/* The kernel writes one file per speculative-execution flaw it knows about, and
   the answer is a sentence: "Mitigation: PTI", "Vulnerable: Clear CPU buffers
   attempted, no microcode; SMT disabled". The row carries the leading answer and
   the tooltip the whole line, because the sentence is only interesting once a
   person has found the flaw they were asking about. */
static void vuln_state(const char *text, char *out, size_t n) {
  const char *semi = strchr(text, ';');
  size_t len = semi ? (size_t)(semi - text) : strlen(text);
  while (len && text[len - 1] == ' ')
    len--;
  /* Only a first clause that will not fit the column is cut back to its leading
     word: "Mitigation: PTI" and "KVM: Mitigation: Split huge pages" are both
     answers a person can read at a glance, and neither is a truncated one. */
  if (len >= n) {
    const char *colon = memchr(text, ':', len);
    len = colon ? (size_t)(colon - text) : n - 1;
    if (len >= n)
      len = n - 1;
  }
  memcpy(out, text, len);
  out[len] = '\0';
}

static int vuln_cmp(const void *a, const void *b) {
  return strcmp(((const VulnInfo *)a)->name, ((const VulnInfo *)b)->name);
}

static void info_vulnerabilities(SysInfo *i) {
  const char *dir = "/sys/devices/system/cpu/vulnerabilities";
  DIR *d = opendir(dir);
  if (!d)
    return;
  struct dirent *e;
  while ((e = readdir(d)) && i->nvuln < INFO_VULN) {
    if (e->d_name[0] == '.')
      continue;
    char p[320]; /* the directory plus a d_name the compiler cannot bound */
    snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
    char line[192];
    if (!read_line(p, line, sizeof line))
      continue;
    VulnInfo *v = &i->vuln[i->nvuln++];
    scpy(v->name, sizeof v->name, e->d_name);
    scpy(v->text, sizeof v->text, trim(line));
    vuln_state(v->text, v->state, sizeof v->state);
  }
  closedir(d);
  /* sysfs hands back the directory order, which is creation order and changes
     with the kernel's own list; a table that reshuffles itself between two
     openings of one tab cannot be read. */
  if (i->nvuln > 1)
    qsort(i->vuln, (size_t)i->nvuln, sizeof i->vuln[0], vuln_cmp);
}

static void info_swaps(SysInfo *i) {
  FILE *f = fopen("/proc/swaps", "r");
  if (!f)
    return;
  char line[256];
  if (!fgets(line, sizeof line, f)) { /* the column names */
    fclose(f);
    return;
  }
  while (fgets(line, sizeof line, f) && i->nswap < INFO_SWAP) {
    char file[64], type[16];
    unsigned long long size, used;
    long prio;
    if (sscanf(line, "%63s %15s %llu %llu %ld", file, type, &size, &used, &prio) != 5)
      continue;
    SwapInfo *s = &i->swap[i->nswap++];
    scpy(s->file, sizeof s->file, file);
    scpy(s->type, sizeof s->type, type);
    s->size_kb = size;
    s->used_kb = used;
    s->prio = prio;
  }
  fclose(f);
}

/* The shape of the session rather than the desktop inside it. All four answers
   are a single world-readable file or a stat(), and each is left out of the tab
   when its own source is missing: a container has no /sys/power/state, and a
   system that booted through a BIOS hands out no /sys/firmware/efi. */
static void info_session(SysInfo *i) {
  info_file(i->init_name, sizeof i->init_name, "/proc/1/comm");
  char line[96];
  if (read_line("/sys/power/state", line, sizeof line))
    scpy(i->sleep_states, sizeof i->sleep_states, trim(line));
  if (read_line("/sys/power/mem_sleep", line, sizeof line)) {
    char *b = strchr(line, '[');
    if (b) {
      char *e = strchr(b, ']');
      if (e)
        *e = '\0';
      scpy(i->sleep_now, sizeof i->sleep_now, trim(b + 1));
    }
  }
  /* Three answers, not two: a machine whose firmware directory is not mounted
     at all — a container — knows nothing about how it was booted, and "Legacy
     BIOS" there would be a claim rather than a reading. */
  struct stat sb;
  if (stat("/sys/firmware/efi", &sb) == 0)
    i->efi = 1;
  else if (stat("/sys/firmware", &sb) == 0)
    i->efi = 0;
  else
    i->efi = -1;
  i->cgroup_v2 = stat("/sys/fs/cgroup/cgroup.controllers", &sb) == 0;
}

/* One card per line, as `0 [Intel ]: HDA-Intel - HDA Intel`. The name the
   driver gives comes second and is the one a person recognises, so the two are
   swapped into "HDA Intel (HDA-Intel)". */
static void info_audio(SysInfo *i) {
  FILE *f = fopen("/proc/asound/cards", "r");
  if (!f)
    return;
  char line[192];
  while (fgets(line, sizeof line, f)) {
    char *c = strstr(line, "]: ");
    if (!c)
      continue;
    c += 3;
    char *dash = strstr(c, " - ");
    if (dash) {
      *dash = '\0';
      scpy(i->audio, sizeof i->audio, trim(dash + 3));
      char one[64];
      scpy(one, sizeof one, trim(c));
      if (i->audio[0]) {
        char joined[160];
        snprintf(joined, sizeof joined, "%s (%s)", i->audio, one);
        scpy(i->audio, sizeof i->audio, joined);
      }
    } else {
      scpy(i->audio, sizeof i->audio, trim(c));
    }
    break;
  }
  fclose(f);
}

/* The knob the screen brightness goes through. Drivers disagree about whether
   the value is a percentage, a step or a raw register, so the row shows the pair
   the driver states and lets the ratio speak; a machine with no backlight class
   device — a desktop, or a laptop whose firmware keeps the knob — gets no row. */
static void info_backlight(SysInfo *i) {
  i->bl_now = i->bl_max = -1;
  DIR *d = opendir("/sys/class/backlight");
  if (!d)
    return;
  struct dirent *e;
  while ((e = readdir(d))) {
    if (e->d_name[0] == '.')
      continue;
    char p[320]; /* the class path plus a d_name the compiler cannot bound */
    unsigned long long now, max;
    snprintf(p, sizeof p, "/sys/class/backlight/%s/brightness", e->d_name);
    if (!read_u64(p, &now))
      continue;
    snprintf(p, sizeof p, "/sys/class/backlight/%s/max_brightness", e->d_name);
    if (!read_u64(p, &max) || max == 0)
      continue;
    scpy(i->backlight, sizeof i->backlight, e->d_name);
    i->bl_now = (long)now;
    i->bl_max = (long)max;
    break;
  }
  closedir(d);
}

/* getifaddrs walks the same kernel list `ip` prints, through libc: no external
   program, no socket of its own, no privilege. Loopback and an interface still
   at 0.0.0.0 answer nothing a person would read here, so they are skipped. */
static void info_net(SysInfo *i) {
  struct ifaddrs *first = NULL;
  if (getifaddrs(&first) != 0)
    return;
  for (struct ifaddrs *p = first; p && i->nnet < INFO_NET; p = p->ifa_next) {
    if (!p->ifa_addr || p->ifa_addr->sa_family != AF_INET || !p->ifa_netmask)
      continue;
    if (p->ifa_flags & IFF_LOOPBACK)
      continue;
    char ip[INET_ADDRSTRLEN];
    const struct sockaddr_in *sa = (const struct sockaddr_in *)p->ifa_addr;
    if (!inet_ntop(AF_INET, &sa->sin_addr, ip, sizeof ip))
      continue;
    if (!strcmp(ip, "0.0.0.0"))
      continue;
    unsigned m = ntohl(((const struct sockaddr_in *)p->ifa_netmask)->sin_addr.s_addr);
    int bits = 0;
    for (; m; m >>= 1)
      bits += (int)(m & 1u);
    NetInfo *n = &i->net[i->nnet++];
    scpy(n->ifn, sizeof n->ifn, p->ifa_name);
    snprintf(n->addr, sizeof n->addr, "%s/%d", ip, bits);
  }
  freeifaddrs(first);
}

void stats_collect_info(SysInfo *i) {
  memset(i, 0, sizeof *i);

  info_os_release(i->os, sizeof i->os);
  info_file(i->kernel, sizeof i->kernel, "/proc/sys/kernel/osrelease");
  struct utsname u;
  if (uname(&u) == 0)
    scpy(i->machine, sizeof i->machine, u.machine);
  if (gethostname(i->hostname, sizeof i->hostname) != 0)
    i->hostname[0] = '\0';
  user_name(geteuid(), i->user, sizeof i->user);
  const char *env = getenv("XDG_SESSION_TYPE");
  if (env)
    scpy(i->session, sizeof i->session, trim((char *)env));
  env = getenv("XDG_CURRENT_DESKTOP");
  if (env)
    scpy(i->desktop, sizeof i->desktop, trim((char *)env));

  info_cpuinfo(i);
  info_cache(i);
  info_file(i->cpu_gov, sizeof i->cpu_gov,
            "/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor");
  info_file(i->cpu_driver, sizeof i->cpu_driver,
            "/sys/devices/system/cpu/cpu0/cpufreq/scaling_driver");
  unsigned long long khz;
  if (read_u64("/sys/devices/system/cpu/cpu0/cpufreq/cpuinfo_min_freq", &khz))
    i->cpu_min_khz = (long)khz;
  if (read_u64("/sys/devices/system/cpu/cpu0/cpufreq/cpuinfo_max_freq", &khz))
    i->cpu_max_khz = (long)khz;

  i->mem_modules = info_modules();

  info_file(i->sys_vendor, sizeof i->sys_vendor, "/sys/class/dmi/id/sys_vendor");
  info_file(i->product, sizeof i->product, "/sys/class/dmi/id/product_name");
  info_file(i->board_vendor, sizeof i->board_vendor, "/sys/class/dmi/id/board_vendor");
  info_file(i->board_name, sizeof i->board_name, "/sys/class/dmi/id/board_name");
  info_file(i->bios_vendor, sizeof i->bios_vendor, "/sys/class/dmi/id/bios_vendor");
  info_file(i->bios_version, sizeof i->bios_version, "/sys/class/dmi/id/bios_version");
  info_file(i->bios_date, sizeof i->bios_date, "/sys/class/dmi/id/bios_date");
  unsigned long long ch;
  if (read_u64("/sys/class/dmi/id/chassis_type", &ch) && ch < 64)
    i->chassis = (int)ch;

  /* The adapter the GPU page already chose, plus the driver name that its own
     sysfs symlink carries. resolve_gpu() ran in stats_init, so this is a lookup
     of something already found, not a second discovery pass. */
  if (g_gpu_dir[0]) {
    char parent[160];
    scpy(parent, sizeof parent, g_gpu_dir);
    char *slash = strrchr(parent, '/');
    if (slash) {
      *slash = '\0'; /* drop /device, leaving /sys/class/drm/card1 */
      slash = strrchr(parent, '/');
    }
    scpy(i->gpu_node, sizeof i->gpu_node, slash ? slash + 1 : "");

    char p[224];
    snprintf(p, sizeof p, "%s/driver", g_gpu_dir);
    char link[256];
    ssize_t l = readlink(p, link, sizeof link - 1);
    if (l > 0) {
      link[l] = '\0';
      const char *b = strrchr(link, '/');
      scpy(i->gpu_driver, sizeof i->gpu_driver, b ? b + 1 : link);
    }
  }

  info_vulnerabilities(i);
  info_disks(i);
  info_filesystems(i);
  info_swaps(i);
  info_session(i);
  info_audio(i);
  info_backlight(i);
  info_net(i);
}

/* ---------- batteries ---------- */

/* A discharge current is signed on the drivers that report one at all, so the
   unsigned reader used elsewhere would turn -35000 into 18446744073715117616. */
static int read_ll(const char *path, long long *out) {
  FILE *f = fopen(path, "r");
  if (!f)
    return 0;
  char buf[64];
  int ok = 0;
  if (fgets(buf, sizeof buf, f)) {
    char *end;
    long long v = strtoll(buf, &end, 10);
    if (end != buf) {
      *out = v;
      ok = 1;
    }
  }
  fclose(f);
  return ok;
}

static void batt_text(char *dst, size_t dn, const char *dir, const char *field) {
  char p[192];
  snprintf(p, sizeof p, "%s/%s", dir, field);
  char buf[192];
  if (!read_line(p, buf, sizeof buf))
    return;
  scpy(dst, dn, trim(buf));
}

static void batt_num(long long *dst, const char *dir, const char *field) {
  char p[192];
  snprintf(p, sizeof p, "%s/%s", dir, field);
  long long v;
  if (read_ll(p, &v))
    *dst = v;
}

/* One supply directory as far as it answers. Anything a driver leaves out stays
   unsaid here rather than being guessed at, because the tab decides what to show
   by asking whether the field has a value. */
static void batt_read(BatteryRow *r, const char *dir) {
  const char *base = strrchr(dir, '/');
  scpy(r->name, sizeof r->name, base ? base + 1 : dir);

  char type[24] = "";
  batt_text(type, sizeof type, dir, "type");
  r->is_battery = !strcmp(type, "Battery");

  /* A Mains supply is not a row — it is a battery that cannot be unplugged —
     but whether it is online is what turns "4 h left" into "charging". */
  if (!r->is_battery) {
    unsigned long long v;
    char p[192];
    snprintf(p, sizeof p, "%s/online", dir);
    if (!strcmp(type, "Mains") && read_u64(p, &v))
      r->online = v != 0;
    return;
  }

  batt_text(r->model, sizeof r->model, dir, "model_name");
  batt_text(r->manufacturer, sizeof r->manufacturer, dir, "manufacturer");
  batt_text(r->technology, sizeof r->technology, dir, "technology");
  batt_text(r->status, sizeof r->status, dir, "status");
  batt_text(r->level_word, sizeof r->level_word, dir, "capacity_level");
  batt_text(r->scope, sizeof r->scope, dir, "scope");

  /* -1 means the file is missing, which is not the same answer as a cell that
     really is at 0 %. */
  r->capacity = -1;
  long long v = -1;
  batt_num(&v, dir, "capacity");
  if (v >= 0 && v <= 100)
    r->capacity = (int)v;

  /* Charge is in microamp-hours, energy in microwatt-hours; a driver supplies
     one family or the other, and the two are not mixed within one supply. */
  r->now_uah = r->full_uah = r->design_uah = r->current_ua = r->voltage_uv = -1;
  batt_num(&r->now_uah, dir, "charge_now");
  if (r->now_uah < 0)
    batt_num(&r->now_uah, dir, "energy_now");
  batt_num(&r->full_uah, dir, "charge_full");
  if (r->full_uah < 0)
    batt_num(&r->full_uah, dir, "energy_full");
  batt_num(&r->design_uah, dir, "charge_full_design");
  if (r->design_uah < 0)
    batt_num(&r->design_uah, dir, "energy_full_design");
  batt_num(&r->current_ua, dir, "current_now");
  batt_num(&r->voltage_uv, dir, "voltage_now");

  /* A cycle count of 0 is a real answer — a new cell — so the field is only
     left unsaid when the file itself is missing. */
  r->cycles = -1;
  long long c = -1;
  batt_num(&c, dir, "cycle_count");
  if (c >= 0)
    r->cycles = (int)c;

  /* `present` is optional; a supply that states it and says no is a slot with
     nothing in it, and a row for it would be a lie about hardware. */
  unsigned long long p0;
  char pp[192];
  snprintf(pp, sizeof pp, "%s/present", dir);
  if (read_u64(pp, &p0) && p0 == 0)
    r->is_battery = 0;
}

void stats_scan_batteries(BatteryTable *t) {
  memset(t, 0, sizeof *t);
  DIR *d = opendir("/sys/class/power_supply");
  if (!d)
    return;

  /* readdir hands out directory order, which is registration order: a mouse
     that re-pairs moves to the end of the list and would jump rows. */
  char names[MAXBATT + 1][48];
  int found = 0;
  struct dirent *e;
  while ((e = readdir(d))) {
    if (e->d_name[0] == '.')
      continue;
    if (found > MAXBATT) {
      t->overflow = 1;
      break;
    }
    int at = found;
    while (at > 0 && strcmp(names[at - 1], e->d_name) > 0) {
      memcpy(names[at], names[at - 1], sizeof names[0]);
      at--;
    }
    scpy(names[at], sizeof names[0], e->d_name);
    found++;
  }
  closedir(d);
  if (found > MAXBATT) {
    t->overflow = 1;
    found = MAXBATT;
  }

  for (int i = 0; i < found; i++) {
    char dir[128];
    scpy(dir, sizeof dir, "/sys/class/power_supply/");
    scpy(dir + strlen(dir), sizeof dir - strlen(dir), names[i]);
    BatteryRow r;
    memset(&r, 0, sizeof r);
    batt_read(&r, dir);
    if (r.online)
      t->external = 1;
    if (!r.is_battery || t->n >= MAXBATT)
      continue;
    t->rows[t->n++] = r;
  }
}
