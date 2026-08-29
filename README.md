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
| **Non-zero exit on init failure** | `lcd_begin()` failure returned 0, so failing to open the I2C bus exited *successfully*. Under systemd the journal reads "Deactivated successfully" while the unit restarts forever. |
| **`build.yaml` added** | Supervisor supplies `BUILD_FROM` from this file. Without it the build fails at `FROM $BUILD_FROM` with *base name should not be blank*. Older Supervisor releases had implicit defaults, so the addon built once and then stopped without anything about it changing. |
| **No more log spam** | The loop printed `lcd display` with no newline on every iteration, producing one ever-growing line in the addon log. |

Two of these are only visible outside the addon container, which is why they survived
upstream: the busybox/procps difference and the container-versus-host `eth0`.

## Options

| Option | Default | Meaning |
|---|---|---|
| `ip_address` | `""` | Empty detects from `eth0`/`wlan0`. Set it only if detection is wrong. |
| `dwell_seconds` | `3` | Seconds each screen stays readable. Clamped to 0.5–60. |

Both are also read from the environment (`UCTRONICS_IP_ADDRESS`,
`UCTRONICS_DWELL_SECONDS`) for running the binary directly under systemd rather than
as an add-on.

## Licence

Upstream publishes no licence, so no licence is asserted here either. This is a
GitHub fork of a public repository and nothing more; all original copyright remains
with UCTRONICS / Arducam.
