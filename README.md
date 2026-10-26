# TaskManagerOK

A small Linux task manager with the look and the data layout of the Windows 11
Task Manager: the **Performance** tab (device list on the left, one live area
chart, per-core mini charts and a row of numbers underneath), the **Processes**
tab (a sortable name / CPU / memory / disk table with the same row of numbers),
the **Startup** tab (the XDG autostart entries of this session, with a switch
that works without root) and the **Details** tab (the same processes with every
column the kernel keeps about them — state, owner, lifetime CPU and byte
counters, threads, priority, parent and command line) and the **Information**
tab (what the machine is: distribution, processor and caches, memory modules,
board and BIOS, display adapter and monitors, disks and the space left on them).
Performance also carries the power supplies: its **Battery** device lists every
one the kernel publishes — the laptop's own cells and the wireless devices beside
them — in place of the chart. A **gear** at the bottom of the rail opens
**Settings**, where each of those refresh rates and the interface language are
yours to change.

Written in plain C against GTK 3 and Cairo, with no daemons, no interpreted
runtime and no background services. The whole program is one binary.

## Status

The Performance, Processes, Startup, Details and Information tabs are
implemented, a process you own can be ended from either process table, and the
refresh rates of those tabs — plus the interface language — are set in
[Settings](#settings). The Performance tab covers CPU,
memory, disk, network, GPU, temperature where the kernel exposes sensors, and
the power supplies where `/sys/class/power_supply` has anything to say.
The Services tab is planned (see
[Roadmap](#roadmap)). The interface follows the system language (Ukrainian and
English at the moment) unless Settings says otherwise, and there is a portable
AppImage build.

Measured on the development machine (Intel Core2 T9600, 2 threads, 3.7 GiB RAM,
Linux Mint 22.3, XFCE/X11) with a desktop session running and 194 processes in the
list. Every CPU figure below is this program's own time from `/proc/<pid>/schedstat`
over a 45 s window — percent of one core, not of the machine — and was read from
the shipped build:

| | |
|---|---|
| Resident memory | 43 MiB |
| CPU, Performance tab (1 s sampling) | 0.34 % of one core (0.151 s of CPU per 45 s) |
| CPU, Processes tab (4 s refresh) | 0.85 % of one core (0.382 s per 45 s) |
| CPU, Details tab (6 s refresh) | 0.89 % of one core (0.400 s per 45 s) |
| CPU, Startup tab (scanned when the tab opens) | 0.25 % of one core (0.110 s per 45 s) |
| CPU, Information tab (read once when the tab opens) | 0.41 % of one core (0.185 s per 45 s) |
| CPU, Performance tab with the Battery device selected (one scan every 5 s) | 0.38 % of one core (0.172 s per 45 s) |
| CPU, machine saturated, Performance tab (2 busy loops + direct disk writes) | 0.22 % of one core |
| CPU, pointer swept continuously over the graph, Performance tab, 125 events/s | 0.81 % of one core |
| Startup | 0.43 s to window (best of three) |

The temperature collector is measured separately, because the machine could not be
made idle for it: another program held 59.4 % of it for the whole of one 45 s
window and 60.1 % for the next. Those two windows are quoted together, since what
is being compared is the pair and not either number on its own — the same build on
the same day, one tab with the sensor table open and one without:

| | |
|---|---|
| CPU, Temperature tab (1 s sampling, four sensors, machine 59.4 % busy) | 0.49 % of one core (0.222 s of CPU per 45 s) |
| CPU, CPU tab in the same conditions (machine 60.1 % busy) | 0.43 % of one core (0.192 s per 45 s) |
| Resident memory in the same sessions | 45.2 MiB |

All five tabs, at the cadence Settings ships with, stay inside the one per cent
of a core the program is designed not to exceed; the same page is where to see
what a faster rate would cost (see [Settings](#settings)). The refresh cadence
itself was chosen from those measurements rather than from what the collector
could bear: reading the whole list costs about 6 ms of CPU per sweep
(`/proc/<pid>/stat` for all 194 of them plus `/proc/<pid>/io` for the ones you
own), but repainting the rows that changed costs about four times that, so the
interval is what the redraw is worth. A sweep every 3 s measured 1.05 % of a
core and every 5 s 0.76 %; four seconds sits under the line with room to spare.
The Performance graph keeps its 1 s step. The Details sweep also reads the owner
and the command line of every process — 9 ms of CPU against 6 ms for the same
list without them, the spread of three `make test` runs at 199 rows — and it shows
thirteen columns instead of four, so it refreshes every 6 s instead of every 4;
nothing on that tab is a rate, so the slower step buys the budget back without
making any number on the screen less true. The Startup tab is the cheapest of the
four because nothing on it repaints on a timer: the directories are read once
when the tab opens — 0.6 ms for the 33 entries on this machine — and after that
the list only changes when something is clicked.

The whole sweep was also measured on its own, by building the collector twice —
once with the temperature pass in `stats_sample()` and once with that one call
removed — and timing a sample from each seven times in pairs: 1.8 ms of wall
clock and 1.0 ms of CPU per sample with it, 0.8 ms without. At one sample per
second that is about a tenth of one per cent of a core, which is what the
Temperature row costs on this machine before the table under it is drawn; the
pair above separates the table itself, and that is 0.06 % of a core. The absolute
values sit above the quiet-window rows because
of the load on the machine, as noted under [End task](#end-task): the same tab on
this same hardware has been seen to read 1.10 % where it costs 0.85 %.

The four tab figures and the resident memory are from one session on a freshly
booted machine; the two stress rows and the Startup time come from an earlier
session on the same machine when the list held 206 processes. Nothing in the
drawing path has changed between them. The Information row is from neither: it is
the current build in today's session, 203 processes, measured twice back to back
— the tab open and then the Processes tab — where the two came out 0.41 % and
0.40 % of a core, which is why it is quoted beside them rather than inside that
older set. The Battery row is the same build with the Battery device selected
instead of CPU: 0.38 % against the 0.34–0.43 % the other devices cost, which is
what it should be, since the scan it runs every fifth second measures 1.1 ms of
CPU on its own for the two supplies this machine publishes — nine of those scans
cost one tick of the timer.

End task was checked against those figures rather than added to them, and it is
worth saying how, because the first attempts were wrong. The machine has to be
idle while the timer runs: with something else using most of a core, the same tab
on this same build read 1.10 %, 1.28 % and 1.45 % in three back to back windows,
and none of those is the program's cost. In the quiet windows that did turn up —
`/proc/stat` under 10 % busy for the whole 45 s, 198 to 199 processes in the list —
the binary from before the feature used 0.93 % of a core on the Processes tab and
the current one 0.96 % on Details, which is the table's 0.85 % and 0.89 % with five
more rows to draw. Each of those windows is checked afterwards by reading which
rail icon carries the accent bar, because a window that has just opened does not
always take the click meant to switch it. And the comparison can only have come out
level, since nothing in end task runs on a timer: it is a menu, a dialog and two
`kill()` calls.

The Details table found the one thing in GTK that could break this budget.
`GtkTreeView` sizes its columns by default with `GROW_ONLY`, which means it
re-measures a row's height and every column's natural width each time the row
changes: writing 199 rows × 23 cells cost about 1 ms inside the call and roughly
210 ms more when the main loop got round to measuring them — a 256 ms spike every
six seconds, which is 4.8 % of a core for a tab that only reads `/proc` and paints
a table. Declaring the measurements once instead — a fixed width per column and a
fixed height per row — cut that sweep to 28 ms of table and drawing, and the tab
from 4.8 % to 0.89 %. The single row height is only an honest promise because the
collector flattens every cell to one line; see the Command line note under
[Known limitations](#known-limitations).

One full chart repaint (972x713, 60 samples, grid, gradient fill, two per-core
mini charts) costs 9 ms of CPU with software Cairo. The graph redraws once per
second, so the steady-state budget is well under 1 % of a core. Nothing in the
drawing path uses the GPU — GTK 3 and Cairo render on the CPU, so an old or
headless graphics chip does not affect the frame rate.

## Build

Dependencies — GTK 3 development files and a C compiler:

| Distribution | Command |
|---|---|
| Linux Mint / Ubuntu / Debian | `sudo apt install build-essential libgtk-3-dev` |
| Fedora / CentOS Stream | `sudo dnf install gcc gtk3-devel` |
| Arch / Manjaro | `sudo pacman -S base-devel gtk3` |
| openSUSE | `sudo zypper install gcc gtk3-devel` |

```sh
make          # builds ./taskmanagerok and the translation catalogues
make test     # builds and runs the /proc + /sys collector probe
sudo make install     # optional: /usr/local/bin + menu entry + translations
```

Verified on X11 (Mint 22.3 + XFCE). As an ordinary GTK 3 application it should
run on Wayland through the same GDK backend, but that path has not been tested
yet.

## Localisation

The interface follows the system language. English is the language of the
source strings; other languages live in `po/*.po` and are compiled to
`locale/<lang>/LC_MESSAGES/taskmanagerok.mo` by `make`. Ukrainian (`po/uk.po`)
is included.

Translations are looked up next to the binary first, so a built checkout and an
AppImage work without installing anything. To try another language without
changing your desktop settings:

```sh
LANGUAGE=uk ./taskmanagerok     # Ukrainian
LANGUAGE=en ./taskmanagerok     # English (the source strings)
```

A `language=` line in `~/.config/taskmanagerok/settings.ini` wins over the
environment — that is what the language row on the Settings page writes — and an
empty one leaves the environment in charge.

Number formatting is not hardcoded: the decimal separator comes from the
`LC_NUMERIC` locale, so a Ukrainian or Romanian locale shows `2,79 ГГц` while
`C`/English shows `2.79 GHz`.

To add a language: copy `po/uk.po`, translate it, and run `make`. After new
strings appear in the code, refresh the catalogue with `make pot` (needs
`gettext` development tools).

## Portable run (AppImage)

`tools/make-appimage.sh` produces a single file that runs without installing
anything — double-click it, or run it from a shell:

```sh
make
tools/make-appimage.sh          # -> build/appimage/TaskManagerOK-x86_64.AppImage
./TaskManagerOK.AppImage
```

The script bundles GTK, Cairo, Pango and their support libraries into the image
(9 MB compressed) and leaves the pieces every desktop Linux is expected to
provide — glibc, the X11/Wayland client libraries, GL and D-Bus — to the host.
The desktop entry and icon ship inside, so the AppImage runtime can add a menu
entry if you ask it to; installing with `sudo make install` stays available for
people who prefer it.

Requires a host with glibc 2.39 or newer (Ubuntu/Mint 24.04+, Fedora 40+, Arch
2024+) because that is what the bundled libraries were linked against. Building
on the oldest distribution you want to support is the usual way to lower that
floor.

## What each device shows

Every row of the device list opens with a 16 px glyph of the device it names,
drawn with the same Cairo strokes as the rail icons — no icon theme, no font, so
a row cannot render as a missing-glyph box.

| Device | Chart | Numbers below | Source |
|---|---|---|---|
| CPU | total utilisation, plus one mini chart per core | load, frequency, process count, uptime, core count | `/proc/stat`, `/proc/uptime`, `/proc/loadavg`, cpufreq sysfs |
| Memory | used % of installed RAM | used / total, available, cache, swap | `/proc/meminfo`, SPD EEPROM over I2C |
| Disk | read + write throughput | utilisation %, read rate, write rate, capacity | `/proc/diskstats`, `/sys/block/*` |
| Network | rx + tx throughput | rx rate, tx rate, total, link speed or Wi-Fi signal | `/sys/class/net/*`, `/proc/net/route`, `/proc/net/wireless` |
| GPU | utilisation %, or a plain statement when the driver reports none | load, temperature, dedicated memory | `/sys/class/drm/card*/device/` — `gpu_busy_percent`, `hwmon/*/temp1_input`, `mem_info_vram_used` / `_total`; name from `label` or `pci.ids` |
| Temperature | the hottest sensor in the machine, with a dashed line at the limit when the limit falls inside the drawn range | highest, hottest processor sensor, limit, how many sensors are readable | `/sys/class/hwmon/hwmon*/` — `name`, `tempN_input`, `tempN_label`, `tempN_crit` else `tempN_max` |
| Battery | none, deliberately: a bar of the level itself takes the chart's place, one per supply | level, charge state, time left or time to full, remaining capacity | `/sys/class/power_supply/*` — `capacity`, `status`, `charge_now`, `current_now`, `charge_full`, `charge_full_design`, `cycle_count` |

## Temperature

Selecting the device turns the card into a table of every sensor the kernel
exposes, each with its own value and the limit it states, so the number in the
list can be traced to the chip it came from instead of being one figure the
program picked. Until you select it, the list shows a single value: the hottest
of those sensors, because that is the one that decides whether the machine is
running hot.

The row only exists when at least one sensor is readable. A machine whose
kernel loads no hardware-monitor driver has nothing to show here, and an empty
row that says so would be a thing to read on every refresh for no information,
so the device is simply left out of the list.

Two details are worth knowing, because both were wrong before this device
existed:

- The collector enumerates `hwmon<digits>` directories and reads the driver name
  out of each one's `name` file. It does not assume a sensor is called `temp1`:
  `coretemp` numbers its inputs from `temp2` upwards, so a collector that globs
  `temp1_input` gets the ACPI board zone and reports it as the processor
  temperature — which is what this program did until the sensor table made the
  difference visible.
- A chip is treated as the processor's when its name is one of `coretemp`,
  `k10temp`, `zenpower`, `amdcpu`, `cpu_thermal`, `cpu-thermal` or
  `soc_thermal`, or when the kernel
  labels it `Package`, `Tctl`, `Tdie`, `Core ` or `CPU`. `acpitz` is deliberately
  not on that list: it is a thermal zone of the board, and on this machine it sits
  several degrees below the cores. The processor figure is reported separately
  from the highest figure precisely so that the two can disagree.

The dashed line is the lowest critical limit any sensor states — read it as the
temperature at which something in this machine objects, not as the limit of the
hottest one. It is drawn when it falls inside the plotted range and left out
when it does not: a scale stretched far enough to reach a 105 °C threshold is
what flattens the curve a person came here to read, and the limit is printed
under the table either way. The range itself follows the readings rather than
starting at zero, with the floor quantised to ten degrees so the axis does not
slide under the curve every time the room warms up.

## Processes

One row per process, sorted by CPU by default and by any column on a header
click; the arrow shows the direction. The numbers come from the same kind of
`/proc` reads as the Performance tab:

| Column | What it is | Source |
|---|---|---|
| Name | executable name as the kernel reports it | `/proc/<pid>/stat` |
| CPU | percent of **one** core — a busy process shows 100 %, one that uses both threads more | tick delta between two sweeps |
| Memory | resident set size, shared pages included | `/proc/<pid>/stat`, field 24 |
| Disk | read + write throughput | `/proc/<pid>/io` |

Rows are updated in place rather than by rebuilding the table: a process whose
values did not change is not touched at all, a new one is appended, and one the
sweep no longer sees is removed. The scroll position is restored after every
refresh, so the list does not jump while you are reading it. The footer counts
how many processes you own, and the row underneath repeats the Performance
tab's totals.

## Startup

One row per `.desktop` file in the autostart directories this session reads, in
the order the desktop itself would apply them: `~/.config/autostart` first, then
every directory in `XDG_CONFIG_DIRS` (`/etc/xdg/autostart` by default). A name
that appears in both is one row, decided by the user's copy, which is what the
"Source" column is for.

| Column | What it is | Source |
|---|---|---|
| Name | the entry's own `Name`, or the file name when it has none | `Name=` |
| State | Enabled, Disabled, or "Other desktop" | `Hidden`, `X-GNOME-Autostart-enabled`, `OnlyShowIn`, `NotShowIn` against `XDG_CURRENT_DESKTOP` |
| Source | whether the entry that decides lives in your directory or a system one | the two kinds of directory |
| Command | what will actually be run, with the field codes stripped | `Exec=` |

"Other desktop" is a real third state, not a decoration: an entry whose
`OnlyShowIn` names a different desktop is never started here, and calling that
"disabled" would be a guess about a session you are not in. `NoDisplay` is
deliberately not consulted — it hides an entry from the menus, not from the
session.

The button at the end of the list enables or disables the selected entry, and it
never needs root: it writes only into `~/.config/autostart`. Switching off a
system entry copies it there first — an override with nothing in it would delete
the autostart rather than switch it off — and then rewrites the two keys inside
`[Desktop Entry]`, leaving every other byte of the file as its author wrote it.
Both keys are written because both are in use out there: `Hidden` is what XFCE
reads, `X-GNOME-Autostart-enabled` is what GNOME, Cinnamon, MATE and LXQt read.
The file is replaced through a temporary name and a rename, so a crash cannot
leave an entry half written. An entry that belongs to another desktop, or a
directory that is not writable, greys the button out and says which in its
tooltip; a failed write is reported on the line under the table.

## Details

The same processes as on the Processes tab, with the columns the kernel reports
and that four-column table has no room for. The same collector fills them both,
one process list at a time, and this tab asks it for the extra fields: one more
file read per process and a lookup per uid.

| Column | What it is | Source |
|---|---|---|
| Name | executable name as the kernel reports it | `/proc/<pid>/stat` |
| PID | the process id | the directory name |
| Status | the kernel's state letter, spelled out: Running, Sleeping, Waiting for disk, Zombie, Stopped, Idle | `/proc/<pid>/stat`, field 3 |
| User name | the owner's login name | the uid that owns `/proc/<pid>/stat`, through `getpwuid` |
| CPU time | CPU seconds used since the process started — a total, not a rate | `/proc/<pid>/stat`, `utime` + `stime` |
| Time elapsed | how long ago it started | `/proc/<pid>/stat`, field 22, against `/proc/stat` `btime` |
| Memory | resident set size, shared pages included | `/proc/<pid>/stat`, field 24 |
| Disk read | bytes read since it started | `/proc/<pid>/io` |
| Disk write | bytes written since it started | `/proc/<pid>/io` |
| Threads | how many tasks share the address space | `/proc/<pid>/stat`, field 20 |
| Priority | `real-time n` for a task the kernel schedules ahead of everything, otherwise `nice n` | `/proc/<pid>/stat`, fields 18 and 19 |
| Parent PID | the process that started it | `/proc/<pid>/stat`, field 4 |
| Command line | argv, joined with spaces | `/proc/<pid>/cmdline` |

Nine of the thirteen cost nothing extra to read: eight are more tokens of the one
line of `/proc/<pid>/stat` the collector already opens for the Processes tab, and
the PID is the `/proc` directory name it read that line from. The other four are
the two byte counters from `/proc/<pid>/io` — already read for your own processes
— the command line, which is one more file per process and the reason this tab
refreshes every 6 s rather than every 4, and the owner's name, where the uid is
the owner of the `stat` file itself, taken from the descriptor that is already
open, and the passwd lookup happens once per uid rather than once per process.

Sorting works as it does on the Processes tab — click a header, the arrow shows
the direction — with the same rule about which way the first click goes: the
busiest process on top for the measurements (CPU time, elapsed, memory, both byte
counters, threads) and A to Z for the words, but the smallest first for the plain
counts, because a large PID or a large parent PID is a young process, not an
interesting one. Unknown cells are a dash, and a dash is not a word that sorts
before "A": it stays at the bottom whichever way the list is turned, so a column
of other users' processes sorted by Disk read still puts the ones you can read on
top.

The units follow the kind of number. Byte counters are absolute, so they read
`512 B`, `3 KiB`, `1.5 MiB`, `2.20 GiB`, `1.04 TiB` rather than as a rate. CPU time
is unbounded hours — `1:22:07` — and elapsed time carries days past 24 hours,
`3 d 04:11`, because `76:11:02` makes a reader count hours that mean nothing.

Rows are updated in place exactly as on the Processes tab, the scroll position is
restored after every refresh, and the thirteen columns — 1366 px of them — scroll
sideways as well as down. The row of numbers underneath counts the processes, your
own and their memory, how many are waiting on the disk right now, and the total
thread count.

## End task

Right-clicking a row on the Processes or the Details tab — or pressing **Menu**
while a row is selected — opens a one-item menu: **End task**. It asks first, and
then does two things with a pause between them:

1. `SIGTERM`.
2. Three seconds later, `SIGKILL` — but only if the process is still there.

Windows' End task is one hard terminate. The pause is what makes this one safe to
press on a program with unsaved work: a process that behaves itself exits on the
first signal and never sees the second. The wait is a `g_timeout_add`, not a sleep,
so the 1 s tick, the sweeps and every redraw keep running through it.

The confirmation names what is about to be killed — PID, owner and resident
memory — and its default button is Cancel, so Enter and Escape both back out and
the destructive button has to be aimed at.

Three kinds of row refuse, and each says why in the menu item's own text:

* the task manager's own process;
* a process owned by somebody else, whose owner is named — `kill` would answer
  `EPERM`, and the program never runs as root and never asks to;
* a zombie, which has already finished and takes no more notice of a signal than it
  did of the last one.

A greyed control that says nothing reads as a bug, and GTK cannot help here: an
insensitive menu item is sent no motion events at all, so it never gets a tooltip.
The reason is therefore part of the label — *End task — it is root's process*.
Right-clicking the header or the empty space under the last row opens nothing,
because there is no process there to act on.

The pid is checked again before the `SIGKILL`. Process numbers are recycled, and
three seconds is long enough for the kernel to hand one out again: the timer reads
field 22 of `/proc/<pid>/stat` (start time in clock ticks) and `btime` from
`/proc/stat`, and compares the start time they give with the one the row was built
from. A number that now belongs to a stranger gets no signal.

Anything that does not simply work out is written on the line under the table —
`“gedit” had already ended.` when the process went away while the confirmation was
open, and the system's own reason when `kill` refuses. A kill that works is
reported by the row disappearing, which is the only answer that needs no words.

What is deliberately not here: no kill-a-tree, because what belongs to a parent's
children is a decision for the person who owns them; no separate "force" item,
because the pause already is the difference between the two; no priority editing,
and nothing that needs another user's rights.

## Information

The fifth rail icon opens a page with a list of sections on the left — System,
Processor, Memory, Motherboard, Video, Disks — and two columns on the right: the
name of a fact and the value it was read out of. It answers "what is this
machine" without a terminal, so nothing on it moves and nothing on it is a
measurement window:

| Section | Rows | Source |
|---|---|---|
| System | distribution, kernel, architecture, session and desktop, host name, your user name, uptime | `/etc/os-release`, `/proc/sys/kernel/osrelease`, `uname()`, `gethostname()`, `getpwuid(geteuid())`, `XDG_SESSION_TYPE`, `XDG_CURRENT_DESKTOP`, `/proc/uptime` |
| Processor | model, vendor, physical cores, logical processors, one row per cache level, frequency range, current frequency, scaling governor | `/proc/cpuinfo`, `/sys/devices/system/cpu/cpu0/cache/index*`, `cpuinfo_min_freq` / `cpuinfo_max_freq` / `scaling_cur_freq` / `scaling_governor`, `/proc/stat` |
| Memory | installed, free, module type, module count, swap | `/proc/meminfo`, byte 3 of the SPD EEPROM at `/sys/bus/i2c/devices/*/eeprom`, `/proc/meminfo` |
| Motherboard | manufacturer, product, board, BIOS vendor and version, BIOS date, chassis type | `/sys/class/dmi/id/*` |
| Video | adapter, driver, one row per monitor with its mode | `/sys/class/drm/card*`, the `device/driver` symlink, the monitor list from GDK |
| Disks | one row per whole disk (model, SSD or HDD, capacity) and one per mounted file system (device, type, free / total) | `/sys/block/*`, `/proc/mounts` + `statvfs()` |

Three rules hold that tab together:

- **A row that cannot be read is not shown.** There is no "Unavailable" here and
  no dash standing in for a number, because a page that is mostly absentees is a
  page you learn to skip. A board that states no `board_name` loses that row, and
  a machine whose firmware says nothing about memory slots loses the module row —
  which is the honest answer, and not a claim that the memory is soldered.
- **Identifiers that are not the machine's engineering data are never read.** No
  serial number, no `product_uuid`, no MAC — the files under `/sys/class/dmi/id/`
  that hold them are not opened at all, so the tab stays as informative when it
  is shared as it is when it is not.
- **Everything is read once, when the tab opens** — 1.1 ms of wall clock and 1.1 ms
  of CPU for the whole set on this machine, and it never runs again until the tab
  is opened once more. Hardware does not change while the window is up, and a timer
  would spend the budget to repeat an answer that is already on the screen. The
  one number that does move, uptime, is taken from the sweep the other tabs already
  run. With the tab open the program measured 0.41 % of one core over a 45 s window
  — the same figure the Processes tab gives (0.40 % in the same session), because
  what is being timed is a page that draws nothing on a timer.

The chassis row translates SMBIOS type 3 rather than printing the number, since
`10` on a laptop screen tells you nothing; an unknown code keeps its number,
because inventing a word for it would be worse.

What took a bug to notice: `/proc/cpuinfo` puts a tab between the field name and
the colon on x86 (`model name\t:`) and none on ARM (`Model:`), so a prefix match
against `"model name:"` never fires and the model, vendor and core rows silently
disappeared — which is the failure mode of the whole rule above, and why the
matching is done on the trimmed name instead.



* **GPU** is listed whenever a display adapter is identified under
  `/sys/class/drm`, but *measured* only when its driver exports
  `gpu_busy_percent` — today that means AMDGPU and some hypervisor drivers.
  `i915` (the Intel chip in the development machine), `nouveau` and the
  proprietary NVIDIA driver do not, so their page names the adapter and shows
  "Unavailable" where the numbers would be, instead of a flat zero that looks
  exactly like an idle chip. The name comes from the driver's own `label`, then
  from `pci.ids` (which ships with pciutils on Mint, Debian, Fedora and Arch),
  then from the driver plus numeric ids. Without `pci.ids` the row is still
  true, just less friendly.
* **The sum of the Memory column** is larger than the installed RAM: resident
  set size counts a shared page once per process, so the column ranks processes
  well and totals badly. The Memory page reads `/proc/meminfo`, which does not
  double-count.
* **Per-process network traffic** does not exist on Linux — the kernel
  attributes no socket bytes to a process — so the Processes tab has no network
  column, and adding one would mean polling `nfconntrack` or `ss` as root.
* **Per-process disk** comes from `/proc/<pid>/io`, which the kernel marks 0400:
  readable for your own processes only. Other users' rows show a dash on both the
  Processes and the Details tabs. Running as root would fix that, and the program
  never asks to.
* **The Command line column** holds the first 199 bytes of argv with every
  control character flattened to a space and a run of them collapsed to one. A
  process can be handed a whole script as a single argument — `sh -c` with a
  here-document, `node --eval` — and Pango obeys the newlines inside it: the row
  turns into two and the list looks like it has empty rows in it. The flattening
  is also what makes one row height valid for the whole table, and one row height
  is the difference between 0.9 % and 4.8 % of a core (see the budget note
  above). Kernel threads have no command line and show a dash.
* **The Details tab has no per-process GPU column**: the kernel accounts adapter
  use per device, and the only thing that ties a process to the chip is an open
  file descriptor, which says nothing about how much of it that process uses.
* **Link speed** is not reported for Wi-Fi interfaces — the kernel does not
  expose a meaningful `speed` there. For those, the header falls back to the
  WEXT figures in `/proc/net/wireless` (signal level in dBm plus link quality),
  which is what older drivers such as `ath5k` publish. The negotiated bitrate is
  only available through `nl80211`/`iw`, which would add a dependency, so it is
  not read. Interfaces that expose neither show "link speed unavailable".
* **Memory type** (`DDR2`, `DDR4`, …) is read from byte 2 of the module's SPD
  EEPROM, which the kernel exposes as an I2C device named `spd` or `ee1004`. It
  is best-effort: soldered memory, DDR5 behind an I3C hub and distributions that
  keep the EEPROM root-only all leave it unknown, and the header then shows the
  installed size instead. The SMBIOS tables that carry the *configured* speed are
  root-only on every distribution, so speed is not shown at all rather than
  shown as the module's rated speed, which the memory controller may run lower.
* **Temperature** is listed only when a chip under `/sys/class/hwmon` exposes at
  least one readable `tempN_input`. A kernel without a hardware-monitor driver
  for this board has nothing to report, and the device is left out of the list
  rather than shown as an empty page. Two of its numbers are worth reading
  carefully: a sensor named `acpitz_0`, `acpitz_1`, … is a thermal *zone* of the
  board and not a core — the kernel gives it no label on most machines, which is
  why the row shows the chip name with an index — and the limit line is the
  lowest limit stated by *any* sensor, so on a machine whose hottest reading
  comes from a zone that states none it belongs to a different chip. Each row
  prints its own limit, or a dash, so the two can be told apart.
* **Each device row opens with a small drawn glyph** — chip, memory module, disk,
  network, monitor, thermometer — painted with the same Cairo strokes as the rail
  icons. Nothing comes from an icon theme or a font, so a row cannot degrade into a
  missing-glyph box and no desktop's icon set changes what the list looks like. The
  network glyph is chosen by the interface the row names: a Wi-Fi antenna where
  `/sys/class/net/<name>/wireless` exists, an Ethernet plug otherwise.
* Device discovery is Linux-only (`/proc`, `/sys`). Nothing is hardcoded to a
  specific disk, interface or CPU: the root device is resolved from the mount
  table, the network interface from the default route.
* The left rail has five icons for five live tabs — Processes, Performance,
  Startup, Details and Information — and a sixth, the gear, pinned to the bottom of
  the rail rather than added to the stack, so that a stray click on the last tab
  cannot open Settings. Clicking the icon of the tab you are already on folds the
  device list away and brings it back — a Performance-only gesture, so the other
  four icons only switch tab — and with the list folded away there is no keyboard
  way to change device. Switching tabs is a click: the rail is one drawing area
  and the icons are hit-tested by hand, so nothing in it takes focus. That also
  means the rail has no focusable widget and no arrow-key way to move between
  tabs; the tables themselves take the keyboard.
* **The Information tab lists what the machine states about itself**, which is
  not everything a person might want to know about it. The module count is the
  number of SPD EEPROMs the kernel binds, so memory the firmware does not expose
  that way — soldered chips, DDR5 behind an I3C hub — gives no row rather than a
  wrong one; the display rows carry the mode GDK reports, not the panel's native
  mode; and the disk rows carry the capacity `/sys/block` gives, which is the
  whole device, partitioned or not. Serial numbers and `product_uuid` are not
  read at all, by design.
* **The Startup tab covers XDG autostart only** — the `.desktop` files the
  session reads at login. Things that start another way are not in the list and
  cannot be switched off from it: systemd user units
  (`systemctl --user enable …`), `cron @reboot`, `~/.xinitrc` / `~/.xprofile`,
  display-manager session helpers and anything a desktop's own settings tool
  stores elsewhere. Reading those would mean either shelling out to
  `systemctl`/`crontab` or linking against libsystemd, and this program runs no
  external commands and takes on no libraries beyond GTK.
* End task covers one process, not a tree, and only processes you own: a program
  started by another user or by root is listed and greyed with the reason, and
  raising privileges is not something this program does.

## Battery

The last row of the Performance device list opens the supplies the kernel
publishes under `/sys/class/power_supply` — the laptop's own cells and, on a
machine with a wireless mouse or keyboard, those too. It takes the chart's place
rather than sitting under it: a battery moves by one per cent every few minutes,
so a sixty-second window of it is a straight line and a longer one is a story
about last night. What carries the information instead is a bar of the level
itself.

| Block | Rows | Source |
|---|---|---|
| The machine's own cells | name and maker, level, charge state, capacity, wear, cycles, chemistry, time left | `capacity`, `status`, `charge_full` (or `energy_full`), `charge_full_design`, `cycle_count`, `technology`, `current_now` |
| Peripherals (`scope=Device`) | model, maker, level where the driver reports one | `model_name`, `manufacturer`, `capacity`, `capacity_level`, `status` |

The rules the page is built on:

* **A number that is not there is not shown.** A mouse whose driver reports only
  `capacity_level=Unknown` gets its name and the sentence "no level is reported
  for this battery", not a bar at zero and not a dash: 0 % and "no number" are
  different answers, and the collector keeps them apart (`-1` in the struct).
  The kernel's word `Unknown` is treated the same way as a missing file.
* **The time figure appears only when there is a rate to divide by.** It is
  `charge_now` over the current the driver is reporting this second, so a cell at
  100 % on the charger — which answers `current_now=0` — shows no figure rather
  than an infinite one. It is an estimate from an instantaneous sample, which is
  what every battery gauge in the world is, but it is a rough one: a laptop that
  spends the next minute idling and the minute after that encoding video will
  disagree with it.
* **Wear** is `charge_full` against `charge_full_design`, both of which the
  firmware has to state; a cell whose firmware never re-measured the full charge
  shows no wear row at all.
* **`serial_number` is never opened.** It identifies the unit rather than the
  battery, and no row on the page needs it.
* The device rescans every fifth second while its row is selected — 1.1 ms of CPU
  per scan on this machine, against the 5 s of wall clock between scans — because a
  device can pair or a level can move while the window is up. The Information tab's
  figures do not, which is why that one reads once.
* A machine that publishes no supply — a desktop with wired input — gets no row in
  the list at all, so there is no empty page to click into. If the last supply
  disappears while the page is open, the row goes with it and the selection moves
  back to the first device.

## Settings

The gear at the bottom of the rail opens a page with five questions on it. Nothing
else in the window changes anything on disk, so this is the only place the program
writes.

| Row | Choices | Default | What it controls |
|---|---|---|---|
| Performance graph | 0.5, 1, 2, 3, 5, 10, 20, 30 s | 1 s | the timer that drives the whole window, and the width of the chart's window in seconds |
| Process list | 1, 2, 3, 4, 5, 6, 10, 15, 20, 30 s | 4 s | the sweep of `/proc/<pid>/stat` and `/proc/<pid>/io` for every process |
| Details table | the same list | 6 s | the same sweep with the extra fields and the command line |
| Battery devices | the same list | 5 s | the rescan of `/sys/class/power_supply`, while that device is on screen |
| Language | Follow the system, or any code with a catalogue beside the binary | Follow the system | which `po` catalogue the interface is drawn from |

The answers go to `~/.config/taskmanagerok/settings.ini`, written through a
temporary name and a rename, and read again on the next start:

```ini
[refresh]
performance=1
processes=4
details=6
battery=5

[interface]
language=
```

Three things about the page are deliberate:

* **Every row says what it costs.** Next to each menu is the percentage of one core
  that tab would take at that rate, worked out from the milliseconds each sweep
  measures on the reference machine — a percent of a core is those milliseconds
  divided by ten times the period in seconds, because a core offers 1000 ms in 10 s.
  The defaults are the numbers in the table at the top of
  [Status](#status), so the estimates and the measurements agree by construction;
  the row for a table counts the graph tick as well, because that keeps running
  underneath it. A line appears under the rows when a setting costs more than one
  per cent, and it is a statement about this machine, not a lock: nothing stops you
  from picking 0.5 s everywhere on a faster one.
* **The estimates are four separate answers, not a total.** Only the tab you are
  looking at is refreshed; the other three do nothing but hold their history.
* **A table never runs faster than the graph.** The countdowns are counted in ticks
  of the one timer the window has, so a 4 s list on a 3 s graph refreshes every
  3 s. The estimate is computed from what actually happens, not from the number you
  asked for, and the label on the Processes and Details headers prints the same
  figure.

Changing the language writes the file and starts a second copy of the program,
which reads it, and then this one closes: the catalogue is chosen before GTK is
initialised, so there is no in-place way to do it. A running AppImage restarts
itself from its own path; a plain binary from `/proc/self/exe`. If the start fails
the app says so and stays open, with the choice already saved. A language with no
`.mo` next to the binary is not offered at all, because a menu entry that changes
nothing is the kind of control this program does not have.

The model behind those percentages was re-checked by pairing runs of the same tab
back to back, because the machine could not be made idle for it: the session that
wrote this held 59–62 % of the machine for every window, so the absolute figures
are not comparable with the quiet rows above and only the differences are quoted.
Each window is 25 s of the same build, one config at a time, and the difference
divided by the extra sweeps it bought:

| Paired at | | Machine | Cost of one more sweep |
|---|---|---|---|
| Performance tab, 1 s tick → 0.5 s | 0.45 % → 0.93 % of a core | 59–60 % busy | 4.8 ms (the card's tick: 3.4 ms) |
| Processes tab, 4 s → 1 s | 0.99 % → 2.67 % | 59–60 % busy | 22 ms (the card's process sweep: 21 ms) |
| Details tab, 6 s → 1 s | 1.19 % → 5.58 % | 61–62 % busy | 53 ms (the card's details sweep: 34 ms) |

So the arithmetic is right where the program spends most of its time — the tick
and the process sweep come out within the noise of the figures the card uses — and
the Details row is the one place the estimate is a floor rather than an answer:
thirteen columns of table repainted once a second are limited by memory traffic,
which a neighbour on the same core makes worse faster than it makes the reading
worse. A Details tab set to 1 s on this machine measured 5.58 % of a core where the
card says 3.74 %. The estimate is left as the quiet figure, because that is the
condition every other number in this file was read in.

What is deliberately not here: no theme or colour picker (that is the next item on
the roadmap), no per-column visibility, and nothing that would need a second
process, a setting written anywhere else, or a privilege this program does not have.

## Roadmap

1. Services tab
2. Per-user colour themes and a light variant
3. Flatpak, on top of the AppImage

## Layout

```
src/stats.c     /proc, /sys and XDG autostart readers, no GUI dependency
src/stats.h     SysStats snapshot, ProcTable and StartupTable
src/settings.c  the settings file and the catalogue discovery, no GUI dependency
src/settings.h  Settings struct and the four functions around it
src/main.c      GTK 3 window, Cairo charts, gettext strings
po/             translation catalogues (uk included)
tools/test_stats.c   headless probe that prints every collected value
tools/make-appimage.sh  portable single-file build
data/           .desktop entry and icon
```

## License

MIT — see [LICENSE](LICENSE).
