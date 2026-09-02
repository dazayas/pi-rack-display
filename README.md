# pi-rack-display

Home Assistant add-on repository for the LCD on a **UCTRONICS RM0004** Raspberry Pi
rack module.

A fork of [UCTRONICS/UCTRONICS_RM0004_HA](https://github.com/UCTRONICS/UCTRONICS_RM0004_HA),
which is the original and where the credit belongs — the display driver, the fonts
and the addon packaging are all theirs. This fork exists to fix bugs found while
running the same source outside the Home Assistant add-on container, on bare-metal
Ubuntu under systemd.

## Install

Home Assistant → Settings → Add-ons → Add-on Store → ⋮ → Repositories, then add:

```
https://github.com/dazayas/pi-rack-display
```

## What differs from upstream

| Fix | Why it mattered |
|---|---|
| **CPU load read from `/proc/stat`** | It shelled out to `top -bn1 \| awk '/^CPU:/...'`, parsing **busybox** `top`. Correct inside the Alpine addon container; on Debian/Ubuntu procps prints `%Cpu(s): … 98.3 id,` with no `idle` token, so the value was always 0 and the bar never moved. Also removes a `popen` from the slowest screen. |
| **IP address is detected across all interfaces** | `get_ip_address()` returned a literal address with the real lookup commented out beside it — a workaround for the container, where `eth0` is a 172.x bridge address rather than the host's. `host_network: true` makes detection correct in the addon too. Detection now asks the kernel which source address it would use to reach the internet, via a UDP `connect()` that sends nothing. Interface names are not portable (`eth0` on Ubuntu, `end0` on Raspberry Pi kernels, `enp`-style elsewhere) and picking the first non-loopback address finds `docker0` on any host running Docker. `UCTRONICS_IP_ADDRESS` still overrides. |
| **Fixed screen dwell** | The loop slept a flat 2s *after* drawing, so the rotation sped up whenever drawing did — raising the I2C bus to 400 kHz made identical trays cycle at visibly different rates. The dwell is now timed from the end of the draw, so every screen is readable for the same duration. Configurable via `dwell_seconds`. |
| **Cycles aligned to the wall clock** | Four Pis in one rack each start when their service happens to start, so their page transitions land at random offsets from one another — every display correct, the rack looking broken. Each host now waits for the next boundary on a shared wall-clock grid and runs a cycle of exactly `cycle_seconds`, so all four transition together with no communication between them. See *Keeping four displays in step*. |
| **Non-zero exit on init failure** | `lcd_begin()` failure returned 0, so failing to open the I2C bus exited *successfully*. Under systemd the journal reads "Deactivated successfully" while the unit restarts forever. |
| **`build.yaml` added** | Supervisor supplies `BUILD_FROM` from this file. Without it the build fails at `FROM $BUILD_FROM` with *base name should not be blank*. Older Supervisor releases had implicit defaults, so the addon built once and then stopped without anything about it changing. |
| **No more log spam** | The loop printed `lcd display` with no newline on every iteration, producing one ever-growing line in the addon log. |

Two of these are only visible outside the addon container, which is why they survived
upstream: the busybox/procps difference and the container-versus-host `eth0`.

## Keeping four displays in step

Four trays in one rack, four copies of this display, four different service start
times — so the pages turn at four different moments. Nothing is wrong with any of
them; the rack still looks broken.

The fix is **align, don't coordinate**. There is no leader, no broker and no shared
state between the hosts: each one aligns independently against a reference they
already share, the wall clock. A host waits for the next boundary on the grid
(`:00`, `:12`, `:24`, `:36`, `:48`), then draws each page at an absolute instant computed
from the clock — so every host lands on the same boundaries forever after, and one
that reboots rejoins within a cycle. While it waits it shows its own hostname
rather than a blank panel.

Four things this depends on, in case they ever need re-deriving:

- **The target is recomputed every iteration, never `sleep(3)`.** A relative
  sleep accumulates each draw's duration and the scheduler's jitter, so the hosts
  drift apart over hours.
- **A draw that overruns loses its slot rather than extending it.** The next target
  is always the boundary strictly after *now*, so a slow refresh drops one page and
  stays on the grid. One dropped page beats a permanent offset.
- **The clock has to be real first.** These Pis have no RTC, so the display waits
  for `timedatectl show -p NTPSynchronized --value` to say `yes` before aligning to
  anything. If it never does — no network yet — the display free-runs on
  `dwell_seconds` and retries every 30 s rather than sitting on the placeholder.
  Inside the add-on container there is no systemd to ask; it logs that once and
  aligns anyway.
- **Residual offset is NTP quality**, single-digit milliseconds on a LAN, well under
  the tens of milliseconds at which the eye reads two transitions as separate.
  Nothing tries to improve on it.

To check the rack is in step, watch it: all four should wipe at the same instant and
then sit still. In the log, a draw starting more than `UCTRONICS_LATE_WARN_MS` after
its boundary is reported — a host under load, or one whose time sync is broken.

## Options

| Option | Default | Meaning |
|---|---|---|
| `ip_address` | `""` | Empty detects from the routing table. Set it only if detection is wrong. |
| `cycle_seconds` | `12` | One full rotation of all four screens, split evenly — so a round 3 s per screen. Clamped to 2–3600. |
| `align_seconds` | `12` | The wall-clock grid a cycle starts on. `12` gives five boundaries a minute. Any value dividing 60 works. `0` disables alignment and free-runs on `dwell_seconds`. Should be a whole number of `cycle_seconds`, and the log says so if it is not. |
| `align_offset_seconds` | `0` | How far this host sits off that grid. `0` is all four transitioning together; set it to a fraction of the cycle per rack position for a deliberate wave instead. |
| `dwell_seconds` | `3` | Seconds each screen stays readable **when not aligned**. Clamped to 0.5–60. |

All of them are also read from the environment (`UCTRONICS_IP_ADDRESS`,
`UCTRONICS_CYCLE_SECONDS`, `UCTRONICS_ALIGN_SECONDS`,
`UCTRONICS_ALIGN_OFFSET_SECONDS`, `UCTRONICS_DWELL_SECONDS`) for running the binary
directly under systemd rather than as an add-on.

Three more are environment-only, being diagnostics rather than things to tune:

| Variable | Default | Meaning |
|---|---|---|
| `UCTRONICS_LATE_WARN_MS` | `200` | Log a draw that starts this long after its boundary. `0` silences it. |
| `UCTRONICS_NTP_WAIT_SECONDS` | `120` | How long to wait for the clock before free-running instead. |
| `UCTRONICS_HOSTNAME` | *(hostname)* | Overrides the name on the placeholder screen. Needed in the add-on, where the container's own hostname is not the machine's. |

## Licence

Upstream publishes no licence, so no licence is asserted here either. This is a
GitHub fork of a public repository and nothing more; all original copyright remains
with UCTRONICS / Arducam.
