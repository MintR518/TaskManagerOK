#ifndef TASKMGR_STATS_H
#define TASKMGR_STATS_H

#include <sys/types.h>

#define MAXCORE 32

/* ---------- temperatures ---------- */

/* One sensor as the kernel publishes it under /sys/class/hwmon. The chip is the
   driver that owns it (`coretemp`, `k10temp`, `nvme`, `acpitz_0`), the label is
   what the driver calls this particular input (`Core 0`, `Composite`, `Tctl`)
   and is empty when the driver states none. */
#define MAXSENS 24

typedef struct {
  char chip[24];
  char label[32];
  int mc;        /* millidegrees Celsius; a sensor may legitimately read below zero */
  int limit_mc;  /* its critical limit, else its maximum, 0 when it states neither */
  int cpu;       /* the sensor belongs to the processor package */
} TempSensor;

typedef struct {
  /* CPU */
  char cpu_model[96];
  double cpu_pct;
  double core_pct[MAXCORE];
  int ncore;
  double freq_ghz;
  int nproc;
  double load1;
  unsigned long long uptime_s;
  int cpu_temp_mc; /* the hottest processor-package sensor, 0 when none is readable */

  /* Memory */
  char mem_type[16];
  double mem_total_kb, mem_avail_kb, mem_cached_kb, mem_buffers_kb;
  double swap_total_kb, swap_used_kb;

  /* Disk */
  char disk_name[64];
  char disk_model[64];
  int disk_rotational;
  double disk_rd_mbps, disk_wr_mbps, disk_util_pct;
  unsigned long long disk_capacity_bytes;
  unsigned long long disk_io_inflight;

  /* Network */
  char net_if[64];
  long net_link_mbps;
  int net_wireless;
  int net_quality;
  int net_level_dbm;
  double net_rx_mbps, net_tx_mbps;
  unsigned long long net_rx_total, net_tx_total;

  /* GPU */
  int gpu_present;  /* a display adapter was identified under /sys/class/drm */
  int gpu_measured; /* and its driver answers a load counter we can read */
  char gpu_name[96];
  double gpu_pct;
  int gpu_temp_mc; /* millidegrees Celsius, 0 when the driver does not say */
  unsigned long long gpu_vram_used, gpu_vram_total; /* bytes, 0 = not reported */

  /* Every temperature sensor readable without root, in /sys/class/hwmon order.
     A machine with no readable sensor — a virtual machine, or one whose drivers
     hide behind root — reports nsens 0 rather than a row of zeros. */
  TempSensor sens[MAXSENS];
  int nsens;
  int temp_max_mc;   /* the hottest of them */
  int temp_limit_mc; /* the limit of that one, 0 when it states none */

  int have_rates;
} SysStats;

/* One row of the process list. The kernel truncates the name in /proc/<pid>/stat
   to 15 characters, so the field is small on purpose.

   The fields below the first six cost nothing extra when the sweep already
   happens — they are more tokens of the same /proc/<pid>/stat line — so they are
   always filled. The two that cost another syscall each (`user` and `cmd`) are
   filled only when the caller asks for details, because the Processes tab does
   not read them and the sweep is what sets the refresh cadence. */
#define MAXPROC 4096

typedef struct {
  int pid;
  char name[32];
  double cpu_pct;   /* -1 until a previous sample exists */
  double mem_mb;    /* resident set, MiB */
  double disk_mbps; /* -1 when /proc/<pid>/io is not readable, i.e. not our process */
  int own;          /* runs under the same uid as us */

  char state;                    /* the kernel's letter: R S D Z T I */
  int ppid;                      /* field 4; 1 for the children of an exited process */
  int threads;                   /* field 20 */
  long priority;                 /* field 18, kernel units */
  int nice;                      /* field 19 */
  double cpu_s;                  /* utime + stime over the whole lifetime, seconds */
  unsigned long long start_epoch; /* wall-clock second the process started, 0 if unknown */
  unsigned long long io_rd, io_wr; /* bytes from /proc/<pid>/io, 0 unless readable */

  uid_t uid;
  char user[20]; /* "" until a details sweep names it */
  char cmd[200]; /* argv joined with spaces; "" for a kernel thread, which has none */
} ProcRow;

typedef struct {
  ProcRow rows[MAXPROC];
  int n;
  int n_own;    /* rows that run under our uid, i.e. the ones whose io is readable */
  int overflow; /* the sweep hit MAXPROC, so the totals below are not the whole system */
  double sum_cpu_pct;
  double sum_mem_mb;
} ProcTable;

/* ---------- autostart ---------- */

/* One entry of the XDG autostart directories, and what the desktop will actually
   do with it. The three states are not a decoration: an entry whose OnlyShowIn
   names another desktop is never started here, and saying "enabled" about it
   would be a guess about a session the user is not in.

   NoDisplay is deliberately not consulted. It hides an entry from menus, not
   from the session — every one of nm-applet, light-locker and the polkit agent
   on the development machine carries NoDisplay=true and runs anyway. */
#define MAXSTARTUP 160

enum { RUN_ON = 0, RUN_OFF = 1, RUN_OTHER_DE = 2 };

typedef struct {
  char name[64];
  char exec[192];
  char comment[128];
  char base[128];                        /* file name, the key the merge works on */
  char user_path[384];                   /* set when ~/.config/autostart has this entry */
  char sys_path[384];                    /* set when a system directory has it */
  int state;                             /* RUN_ON / RUN_OFF / RUN_OTHER_DE */
  int writable;                          /* our uid owns the user directory */
} StartupRow;

typedef struct {
  StartupRow rows[MAXSTARTUP];
  int n;
  int n_on, n_off, n_other;
  int overflow; /* more entries than MAXSTARTUP, so the counts below are short */
} StartupTable;

/* Re-reads both kinds of directory from scratch; cheap enough to run whenever
   the tab opens. */
void stats_scan_startup(StartupTable *t);

/* Turns one entry on or off. Nothing is ever written outside the user's own
   autostart directory: switching off a system entry creates the usual
   ~/.config/autostart/<name>.desktop override, exactly the mechanism the
   desktop's own tools use. Returns 0, or the errno of the failed step. */
int stats_startup_set(StartupRow *r, int enable);

/* ---------- machine information ---------- */

/* What the machine *is*, as opposed to what it is doing right now: the fields
   here are read once, when the Information tab opens, and never again. Nothing
   in the structure is sampled on a timer, and nothing in it needs privileges.
   Every string is empty when the file behind it was missing or unreadable, and
   the tab leaves such a row out rather than printing a placeholder.
   Volatile numbers the same page shows — memory in use, uptime, temperature,
   current frequency — come from SysStats, which is already sampled, and are
   deliberately not duplicated here. */

#define INFO_CACHE 6
#define INFO_DISK 8
#define INFO_FS 8

typedef struct {
  char name[16], model[48];
  unsigned long long bytes;
  int rotational;
} DiskInfo;

/* One /dev/ row of /proc/mounts that statvfs answered for, with the free space
   a normal user may actually write into. */
typedef struct {
  char dev[24], mnt[48], fstype[16];
  unsigned long long total, free;
} FsInfo;

typedef struct {
  char os[64];       /* PRETTY_NAME of /etc/os-release, quotes removed */
  char kernel[64];   /* /proc/sys/kernel/osrelease */
  char machine[32];  /* uname().machine, e.g. x86_64 */
  char hostname[64]; /* gethostname() */
  char user[48];     /* the login name this program runs as */
  char session[16];  /* XDG_SESSION_TYPE: x11, wayland, tty */
  char desktop[48];  /* XDG_CURRENT_DESKTOP as the session sets it */

  char cpu_model[96]; /* /proc/cpuinfo "model name" of the first processor */
  char cpu_vendor[48];
  int cpu_cores; /* "cpu cores" of the first package, 0 = unsaid. The count of the
                    logical processors the system is running is already in SysStats:
                    one sweep of /proc/stat reads them all. */
  char cpu_gov[24];   /* the scaling governor in force on cpu0 */
  long cpu_min_khz, cpu_max_khz; /* what the hardware says it can run at, 0 = unsaid */
  struct {
    int level, size_kb;
    char type[12]; /* Data, Instruction or Unified */
  } cache[INFO_CACHE];
  int ncache;

  int mem_modules;   /* how many module EEPROMs the kernel saw; the type itself
                        is already in SysStats, sampled for the Memory page */

  char sys_vendor[64]; /* SMBIOS, all world-readable */
  char product[64];
  char board_vendor[64];
  char board_name[64];
  char bios_vendor[64];
  char bios_version[48];
  char bios_date[24];
  int chassis; /* the SMBIOS type-3 code, 0 when the machine states none */

  char gpu_node[16];  /* card1, the DRM node the adapter answers on */
  char gpu_driver[24];

  DiskInfo disk[INFO_DISK];
  int ndisk;

  FsInfo fs[INFO_FS];
  int nfs;
} SysInfo;

void stats_collect_info(SysInfo *i);

/* ---------- batteries ---------- */

/* Every power supply the kernel publishes under /sys/class/power_supply. The
   machine's own battery and a Bluetooth mouse arrive through the same door, so
   they are kept in one list and separated by `scope`: a supply with no scope
   file belongs to the machine, one that says "Device" is a peripheral.
   `serial_number` is deliberately never opened — it identifies the unit, not
   the battery, and no row on this tab needs it.
   A supply can answer some fields and not others, which is why the numeric
   ones are -1 rather than 0 when unsaid: 0 % and "no number" are different
   answers, and a battery that is genuinely empty must not look like a driver
   that says nothing. */
#define MAXBATT 8

typedef struct {
  char name[48];        /* the sysfs directory: BAT1, hidpp_battery_0 */
  char model[96];       /* model_name, empty when the driver states none */
  char manufacturer[64];
  char technology[24];  /* Li-ion, LiPo … */
  char status[24];      /* Charging, Discharging, Full, Not charging … */
  char level_word[24];  /* capacity_level: Full, Normal, Low, Critical … */
  char scope[24];       /* "" for the machine's own, "Device" for a peripheral */
  int capacity;         /* 0..100, -1 when the kernel gives no number */
  long long now_uah;    /* charge_now, or energy_now where only that exists */
  long long full_uah;   /* charge_full, or energy_full */
  long long design_uah; /* charge_full_design, or energy_full_design */
  long long current_ua; /* signed; 0 or unsaid means no rate to divide by */
  long long voltage_uv;
  int cycles; /* -1 unsaid */
  int is_battery;
  int online; /* Mains: the adapter is plugged in */
} BatteryRow;

typedef struct {
  BatteryRow rows[MAXBATT];
  int n;
  int external;   /* some supply says it is feeding the machine */
  int overflow;   /* more supplies than MAXBATT, so the rows below are short */
} BatteryTable;

/* Re-reads the whole directory. One small file per field of one supply, so a
   machine with a battery and two peripherals costs a few dozen reads. */
void stats_scan_batteries(BatteryTable *t);

void stats_init(SysStats *s);
void stats_sample(SysStats *s);
void stats_sample_procs(ProcTable *t, int with_details);

/* The login name of one uid, from the same cache the Details sweep uses. An uid
   with no passwd entry answers with its number, which is the honest reply
   rather than a blank. */
void stats_user_name(uid_t uid, char *dst, size_t dn);

/* Is this pid still the process that started at `start_epoch`? Copies the
   kernel's state letter and returns 1 for that same process, 0 when it is gone
   or when the number has been handed to a new one — a pid is a recycled index,
   so a signal sent late could otherwise reach a stranger. `start_epoch` of 0
   means the snapshot did not know the start time, and the answer then degrades
   to "this pid exists". */
int stats_pid_same(int pid, unsigned long long start_epoch, char *state_out);

#endif
