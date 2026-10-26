/* Development probe: prints every field the collectors produce, on the machine
   it runs on, so a number in the README or a screenshot can be traced back to
   the file it came from. This is what `make test` runs. */

#include "stats.h"
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static double ms_now(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

/* The budget the README quotes is CPU, not wall clock: a sweep that spends 30 ms
   waiting in the kernel while it walks 200 /proc entries can still cost 6 ms of
   a core. Both are timed here, because the Details tab is only affordable if the
   difference between them stays large. */
static double cpu_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts);
  return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

int main(void) {
  SysStats s;
  stats_init(&s);
  stats_sample(&s);
  sleep(2);
  stats_sample(&s);

  printf("cpu model    : %s\n", s.cpu_model);
  printf("cpu pct      : %.1f   cores=%d  freq=%.3f GHz  nproc=%d  load1=%.2f  uptime=%llu s\n",
         s.cpu_pct, s.ncore, s.freq_ghz, s.nproc, s.load1, s.uptime_s);
  for (int i = 0; i < s.ncore; i++)
    printf("  core %d      : %.1f%%\n", i, s.core_pct[i]);
  printf("cpu temp     : %.1f C\n", s.cpu_temp_mc / 1000.0);
  printf("sensors      : %d readable, hottest %.1f C", s.nsens, s.temp_max_mc / 1000.0);
  if (s.temp_limit_mc)
    printf(" (limit %.1f C)", s.temp_limit_mc / 1000.0);
  printf("\n");
  for (int i = 0; i < s.nsens; i++)
    printf("  %-14s %-14s %6.1f C   %s%s\n", s.sens[i].chip,
           s.sens[i].label[0] ? s.sens[i].label : "-", s.sens[i].mc / 1000.0,
           s.sens[i].limit_mc ? "" : "no limit stated ",
           s.sens[i].cpu ? "[cpu]" : "");

  printf("mem type     : '%s'\n", s.mem_type);
  double used = s.mem_total_kb - s.mem_avail_kb;
  printf("mem          : total %.2f GiB  used %.2f GiB  avail %.2f GiB  cached %.2f GiB  "
         "swap %.2f/%.2f GiB  -> %.1f%%\n",
         s.mem_total_kb / 1048576.0, used / 1048576.0, s.mem_avail_kb / 1048576.0,
         s.mem_cached_kb / 1048576.0,
         s.swap_used_kb / 1048576.0, s.swap_total_kb / 1048576.0,
         s.mem_total_kb ? (s.mem_total_kb - s.mem_avail_kb) / s.mem_total_kb * 100.0 : 0.0);

  printf("disk         : name='%s' model='%s' rotational=%d cap=%.1f GB\n", s.disk_name,
         s.disk_model, s.disk_rotational, (double)s.disk_capacity_bytes / 1e9);
  printf("disk rates   : rd %.3f MB/s  wr %.3f MB/s  util %.1f%%  inflight=%llu  (have_rates=%d)\n",
         s.disk_rd_mbps, s.disk_wr_mbps, s.disk_util_pct, s.disk_io_inflight, s.have_rates);

  printf("net          : if='%s' link=%ld Mbps wireless=%d quality=%d level=%d dBm  "
         "rx %.3f MB/s  tx %.3f MB/s  total rx=%llu MB tx=%llu MB\n",
         s.net_if, s.net_link_mbps, s.net_wireless, s.net_quality, s.net_level_dbm, s.net_rx_mbps,
         s.net_tx_mbps, s.net_rx_total / 1048576ULL, s.net_tx_total / 1048576ULL);

  printf("gpu          : present=%d measured=%d name='%s' pct=%.0f temp=%d vram=%llu/%llu MiB\n",
         s.gpu_present, s.gpu_measured, s.gpu_name, s.gpu_pct, s.gpu_temp_mc,
         s.gpu_vram_used / 1048576, s.gpu_vram_total / 1048576);

  /* A busy child is the honest check of the tick maths: one core busy for the
     whole sampling window has to read back as about 100 %, neither 100/clk_tck
     nor 100*core_count. It therefore burns until the window closes rather than
     for a fixed number of iterations, which a fast core can finish early. The
     deadline has to outlive the second sweep on purpose: a child that already
     exited is a zombie, and a zombie's rss reads 0, which looks exactly like a
     bug in the collector. It is stopped and reaped below instead. */
  pid_t kid = fork();
  if (kid == 0) {
    double until = ms_now() + 4000.0;
    volatile double x = 1.0;
    while (ms_now() < until)
      for (int i = 0; i < 20000; i++)
        x = x * 1.000000001 + 0.5;
    _exit(0);
  }

  /* static, not automatic: a ProcTable is a few megabytes and the probe holds
     two of them at once. */
  static ProcTable pt;
  double t0 = ms_now(), c0 = cpu_ms();
  stats_sample_procs(&pt, 0);
  double prime_ms = ms_now() - t0;
  sleep(2);
  t0 = ms_now();
  c0 = cpu_ms();
  stats_sample_procs(&pt, 0);
  double sweep_ms = ms_now() - t0, sweep_cpu = cpu_ms() - c0;

  /* The Details sweep, timed against the plain one: the extra fields are more
     tokens of a line already read, plus two syscalls per process for the owner's
     name and the command line. The child stays alive for it — and its status
     file is copied before it dies, so the comparison below is between the child
     as the collector saw it and the child as the kernel described it, not between
     a live row and a reaped one. */
  static ProcTable dt;
  t0 = ms_now();
  c0 = cpu_ms();
  stats_sample_procs(&dt, 1);
  double detail_ms = ms_now() - t0, detail_cpu = cpu_ms() - c0;
  t0 = ms_now();
  c0 = cpu_ms();
  stats_sample_procs(&dt, 1);
  double detail2_ms = ms_now() - t0, detail2_cpu = cpu_ms() - c0;

  char want[3][32] = {{"unreadable"}, {"unreadable"}, {"unreadable"}};
  {
    char path[64], line[128];
    snprintf(path, sizeof path, "/proc/%d/status", kid);
    FILE *f = fopen(path, "r");
    if (f) {
      while (fgets(line, sizeof line, f)) {
        if (!strncmp(line, "State:\t", 7))
          snprintf(want[0], 32, "%c", line[7]);
        else if (!strncmp(line, "PPid:\t", 6))
          snprintf(want[1], 32, "%ld", strtol(line + 6, NULL, 10));
        else if (!strncmp(line, "Threads:\t", 9))
          snprintf(want[2], 32, "%ld", strtol(line + 9, NULL, 10));
      }
      fclose(f);
    }
  }
  if (kid > 0) {
    kill(kid, SIGTERM);
    waitpid(kid, NULL, 0);
  }

  int kid_row = -1, ours = 0, with_io = 0, with_delta = 0;
  for (int i = 0; i < pt.n; i++) {
    if (pt.rows[i].pid == kid)
      kid_row = i;
    if (pt.rows[i].own)
      ours++;
    if (pt.rows[i].disk_mbps >= 0)
      with_io++;
    if (pt.rows[i].cpu_pct >= 0)
      with_delta++;
  }
  printf("procs        : %d rows, %d ours, %d with an io reading, %d with a cpu delta, "
         "overflow=%d\n", pt.n, ours, with_io, with_delta, pt.overflow);
  printf("             : sum cpu %.1f%%  sum mem %.0f MiB  sweep %.1f ms wall / %.1f ms cpu "
         "(prime %.1f ms)\n",
         pt.sum_cpu_pct, pt.sum_mem_mb, sweep_ms, sweep_cpu, prime_ms);
  if (kid_row >= 0)
    printf("busy child   : pid %d '%s' cpu %.1f%%  mem %.0f MiB  <- one core busy, so ~100%%\n",
           kid, pt.rows[kid_row].name, pt.rows[kid_row].cpu_pct, pt.rows[kid_row].mem_mb);
  else
    printf("busy child   : NOT FOUND in the sweep\n");

  /* Details cross-check. Everything here is read from the same two files a
     curious human would open, so a wrong field is a wrong parse rather than a
     wrong kernel: /proc/<pid>/status for state, ppid and threads, and the
     process's own clock for the start time. */
  int dkid = -1, named = 0, with_cmd = 0, with_start = 0, with_state = 0;
  ProcRow *selfrow = NULL;
  for (int i = 0; i < dt.n; i++) {
    ProcRow *r = &dt.rows[i];
    if (r->state)
      with_state++;
    if (r->user[0])
      named++;
    if (r->cmd[0])
      with_cmd++;
    if (r->start_epoch)
      with_start++;
    if (r->pid == getpid())
      selfrow = r;
    if (r->pid == kid)
      dkid = i;
  }
  printf("details      : %d rows  %.1f ms wall / %.1f ms cpu, again %.1f ms wall / %.1f ms cpu"
         "  (plain sweep %.1f ms cpu)  state %d, user %d, cmdline %d, start %d\n",
         dt.n, detail_ms, detail_cpu, detail2_ms, detail2_cpu, sweep_cpu, with_state, named,
         with_cmd, with_start);
  if (selfrow) {
    ProcRow *r = selfrow;
    char when[32] = "unknown";
    if (r->start_epoch) {
      time_t e = (time_t)r->start_epoch;
      struct tm tm;
      if (localtime_r(&e, &tm))
        strftime(when, sizeof when, "%Y-%m-%d %H:%M:%S", &tm);
    }
    char nows[32] = "?";
    time_t nowt = time(NULL);
    struct tm tmn;
    if (localtime_r(&nowt, &tmn))
      strftime(nows, sizeof nows, "%Y-%m-%d %H:%M:%S", &tmn);
    printf("  self pid %-7d %-16.16s state %c ppid %-7d threads %-3d prio %ld nice %d\n", r->pid,
           r->name, r->state, r->ppid, r->threads, r->priority, r->nice);
    printf("             user '%s' cpu_s %.2f  start %s   now %s\n", r->user, r->cpu_s, when, nows);
    printf("             cmd '%.120s'\n", r->cmd);
  }
  if (dkid >= 0) {
    ProcRow *r = &dt.rows[dkid];
    printf("  child      state %c/%s ppid %d/%s threads %d/%s  (collector/status)\n", r->state,
           want[0], r->ppid, want[1], r->threads, want[2]);
    printf("             user '%s' cpu_s %.2f (one core for ~4 s) io %llu/%llu bytes\n", r->user,
           r->cpu_s, r->io_rd, r->io_wr);
  }
  const char *login = getenv("USER");
  if (login && *login && selfrow && selfrow->user[0])
    printf("  user       : probe says '%s', $USER says '%s'  %s\n", selfrow->user, login,
           strcmp(selfrow->user, login) == 0 ? "agree" : "DIFFER");

  for (int rank = 0; rank < 6 && rank < pt.n; rank++) {
    int best = -1;
    for (int i = 0; i < pt.n; i++)
      if (pt.rows[i].cpu_pct >= 0 && (best < 0 || pt.rows[i].cpu_pct > pt.rows[best].cpu_pct))
        best = i;
    if (best < 0)
      break;
    ProcRow *r = &pt.rows[best];
    char disk[32];
    if (r->disk_mbps < 0)
      snprintf(disk, sizeof disk, "unreadable");
    else
      snprintf(disk, sizeof disk, "%.3f MB/s", r->disk_mbps);
    printf("  #%d pid %6d %-16.16s cpu %6.1f%%  mem %8.1f MiB  disk %s\n", rank + 1, r->pid,
           r->name, r->cpu_pct, r->mem_mb, disk);
    r->cpu_pct = -9.0;
  }
  if (kid > 0)
    kill(kid, SIGKILL);

  static StartupTable st2;
  stats_scan_startup(&st2);
  printf("startup      : %d entries  on=%d off=%d other-desktop=%d  overflow=%d\n", st2.n, st2.n_on,
         st2.n_off, st2.n_other, st2.overflow);
  for (int i = 0; i < st2.n; i++) {
    StartupRow *r = &st2.rows[i];
    printf("  %-28.28s %-8s %-10s %s\n", r->name, r->user_path[0] ? "user" : "system",
           r->state == RUN_ON ? "on" : r->state == RUN_OFF ? "off" : "other-de", r->exec);
  }
  /* The Info tab reads everything once when it opens, so what matters here is the
     cost of one call, not a per-sample cost. Wall and CPU both, because the
     second is what a 45 s schedstat window would see. */
  static SysInfo inf;
  t0 = ms_now();
  c0 = cpu_ms();
  stats_collect_info(&inf);
  double info_ms = ms_now() - t0, info_cpu = cpu_ms() - c0;
  printf("info         : %.1f ms wall / %.1f ms cpu for one full collect\n", info_ms, info_cpu);
  printf("  os         : '%s'\n", inf.os);
  printf("  kernel     : '%s'  machine '%s'  host '%s'  user '%s'\n", inf.kernel, inf.machine,
         inf.hostname, inf.user);
  printf("  session    : '%s'  desktop '%s'\n", inf.session, inf.desktop);
  printf("  cpu        : '%s'  vendor '%s'  cores %d  gov '%s'  %.3f-%.3f GHz\n", inf.cpu_model,
         inf.cpu_vendor, inf.cpu_cores, inf.cpu_gov, inf.cpu_min_khz / 1e6, inf.cpu_max_khz / 1e6);
  printf("  driver     : '%s'\n", inf.cpu_driver);
  for (int i = 0; i < inf.ncache; i++)
    printf("  cache      : L%d %-11s %5d KiB\n", inf.cache[i].level, inf.cache[i].type,
           inf.cache[i].size_kb);
  printf("  mem        : %d module EEPROMs\n", inf.mem_modules);
  printf("  board      : sys '%s' prod '%s'\n", inf.sys_vendor, inf.product);
  printf("             : board '%s' '%s'\n", inf.board_vendor, inf.board_name);
  printf("             : bios '%s' '%s' '%s'  chassis %d\n", inf.bios_vendor, inf.bios_version,
         inf.bios_date, inf.chassis);
  printf("  gpu        : node '%s' driver '%s'\n", inf.gpu_node, inf.gpu_driver);
  printf("  disks      : %d\n", inf.ndisk);
  for (int i = 0; i < inf.ndisk; i++)
    printf("  %-6s %-30.30s %6.1f GiB  %s\n", inf.disk[i].name, inf.disk[i].model,
           inf.disk[i].bytes / 1073741824.0, inf.disk[i].rotational ? "rotational" : "solid");
  for (int i = 0; i < inf.ndisk; i++)
    printf("  %-6s queue '%s' %d/%d B  ra %ld KiB  rev '%s'\n", inf.disk[i].name, inf.disk[i].sched,
           inf.disk[i].logical_bs, inf.disk[i].physical_bs, inf.disk[i].read_ahead_kb,
           inf.disk[i].rev);
  printf("  vuln       : %d\n", inf.nvuln);
  for (int i = 0; i < inf.nvuln; i++)
    printf("  %-26.26s %-24.24s %s\n", inf.vuln[i].name, inf.vuln[i].state, inf.vuln[i].text);
  printf("  swap       : %d\n", inf.nswap);
  for (int i = 0; i < inf.nswap; i++)
    printf("  %-24.24s %-10s %7.1f / %7.1f MiB  prio %ld\n", inf.swap[i].file, inf.swap[i].type,
           inf.swap[i].used_kb / 1024.0, inf.swap[i].size_kb / 1024.0, inf.swap[i].prio);
  printf("  session    : init '%s'  efi %d  cgroup2 %d\n", inf.init_name, inf.efi, inf.cgroup_v2);
  printf("             : sleep '%s'  chosen '%s'\n", inf.sleep_states, inf.sleep_now);
  printf("  audio      : '%s'\n", inf.audio);
  printf("  backlight  : '%s'  %ld / %ld\n", inf.backlight, inf.bl_now, inf.bl_max);
  printf("  net        : %d\n", inf.nnet);
  for (int i = 0; i < inf.nnet; i++)
    printf("  %-10s %s\n", inf.net[i].ifn, inf.net[i].addr);
  printf("  mounts     : %d\n", inf.nfs);
  for (int i = 0; i < inf.nfs; i++)
    printf("  %-10s %-24s %-8s %7.1f / %7.1f GiB free\n", inf.fs[i].dev, inf.fs[i].mnt,
           inf.fs[i].fstype, inf.fs[i].free / 1073741824.0, inf.fs[i].total / 1073741824.0);

  static BatteryTable bt;
  t0 = ms_now();
  c0 = cpu_ms();
  stats_scan_batteries(&bt);
  printf("battery      : %.2f ms cpu for one scan  (%d rows, external %d, overflow %d)\n",
         cpu_ms() - c0, bt.n, bt.external, bt.overflow);
  for (int i = 0; i < bt.n; i++) {
    BatteryRow *r = &bt.rows[i];
    printf("  %-18s cap=%-4d level=%-8s scope='%s' status='%s'\n", r->name, r->capacity,
           r->level_word, r->scope, r->status);
    printf("                   tech='%s' vendor='%s' model='%s' cycles=%d\n", r->technology,
           r->manufacturer, r->model, r->cycles);
    printf("                   now=%lld full=%lld design=%lld cur=%lld volt=%lld uah/uv\n",
           r->now_uah, r->full_uah, r->design_uah, r->current_ua, r->voltage_uv);
  }

  return 0;
}
