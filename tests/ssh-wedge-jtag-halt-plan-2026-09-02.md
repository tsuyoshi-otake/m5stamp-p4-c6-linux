# SSH wedge / JTAG halt plan

Date: 2026-09-02  
DUT: Stamp-P4 module USB-C = USB-Serial/JTAG, VID:PID `303A:1001` (COM10).  
Linux console is the UART fixture, not this port. The running Image has no
USB-Serial/JTAG console driver, so OpenOCD can attach the JTAG function
without sharing the kernel console path.

This shot **breaks** the "no write to COM10 after A" rule. Run it as its
own delay110, never on the same capture as a beat/capsule interpretation.

## Discriminant

| Halt result | Class |
|---|---|
| `halt` completes; `pc` / `ra` / `mstatus` / `mcause` / `sp` readable | 2 (IRQ-off or sitting in a higher-priority handler). `mstatus.MIE` and `pc` name the site. |
| `halt` does not complete (debug module no response) | 3 (core or APB/SDIO bus hung waiting for a reply) |

vmlinux is on the flashed Image's matching build tree; GDB can backtrace
once halt works.

## Host tools (this machine)

```
C:\Users\developer\AppData\Local\Arduino15\packages\esp32\tools\openocd-esp32\v0.12.0-esp32-20240318\bin\openocd.exe
  -s ...\share\openocd\scripts
  -f board/esp32p4-builtin.cfg
```

`interface/esp_usb_jtag.cfg` already matches `vid_pid 0x303a 0x1001`.
Windows must present the JTAG interface as WinUSB (Zadig) if OpenOCD
cannot open the device. CDC ACM (COM10) can stay enumerated; do not
open COM10 as a serial port during attach.

## Protocol (separate shot)

1. Flash a known Image (current `60a71fe3` is enough; no new instrument).
2. Serial A `id` on the UART fixture only. Do not touch COM10 after A.
3. Idle to reset+110 s. Paramiko `id` (same delay110 as P3).
4. Wait until the SSH probe logs `EXIT_TIMEOUT` (~exec+12 s).
5. Attach OpenOCD. Do **not** wait for `rst:0x7` (exec+73.5 without 0054,
   exec+163.7 with 0054). Window: exec+13 s to well before those resets.
6. `halt`. Record whether it returns.
7. If it returns:

```
reg pc
reg ra
reg sp
reg mstatus
reg mcause
```

`mstatus` bit 3 is MIE. Then GDB: `target extended-remote :3333`,
`file vmlinux`, `bt`.

8. Do not `reset` from OpenOCD unless the shot is finished. After
   registers, the capture may continue to the hardware WDT if left halted
   the feed also stops — treat post-halt `rst:0x7` as contaminated.

## Result (2026-09-02, Image `b4575e14`)

Shot: `C:\Users\developer\tmp\p4-ssh-wedge-stacktrace-20260829\c-bb-jtag-halt-delay110-20260902.json`

- Serial A `id` passed. COM10 closed. SSH `id` at reset+111.1 s. `exit_ready_before_halt=false`. Halt at exec+13.0 s.
- **Class 2**: `halt` completed. Not a core/APB hang.
- First sample was `task_tick_fair` / `mcause` IRQ 17 (`SYSTIMER0`). After `resume`, 5/5 samples were `arch_cpu_idle` immediately after `wfi`, `ra=default_idle_call`, `mstatus.MIE=0`.
- Named site: **idle/WFI**. SSH exec is not on the CPU. This is not the IRQ-off `while(1)` injector (mode 2) and not a debug-module-dead hang.
- vmlinux load `0x48000000` from inject volume `linux-custom/vmlinux` 2026-09-02T06:47Z.

## Task walk (2026-09-02, same Image, no resume)

Shot: `C:\Users\developer\tmp\p4-ssh-wedge-stacktrace-20260829\c-bb-jtag-tasks-delay110-20260902.json`

- Halt exec+13.0 s, `exit_ready=false`, `pc=arch_cpu_idle` post-`wfi`, `tp=swapper/0`.
- 31 tasks. `watchdogd` and `es-beat/0` are `I`. The only non-idle `D` is `esp_TX` pid 56.
- `esp_TX` stack is `tx_process` → `msleep(1)` in `esp32_sdio.ko` (text `0x48f40000`), the empty HIGH/MID/LOW queue path (`esp_sdio.c`). It is not sitting in `sdio_memcpy_*`.
- No `dropbear`/`sshd`/`id` on the `tasks` list. `inetd` children list is empty.
- This names the CPU and the hosted TX thread. It does not yet name why SSH exec never returns or why PRETIMEOUT stays silent.

## Earlier halt (2026-09-02)

`plus1s` JSON: OpenOCD started *after* the 1 s sleep, so the actual freeze is ~exec+3 s. Still no `dropbear`. Syslog in RAM: `dropbear[157]: Child connection from 10.255.` then the line is overwritten. `inetd` children empty. `last_feed_age_jiffies=12050`.

`exec0` JSON: OpenOCD pre-armed, halt at exec+0.094 s.

- 33 tasks. **`dropbear` pid 144 state `R`**. **`sh` pid 145 `dead`**.
- dropbear stack is `poll_freewait` / `hrtimer_wakeup` / `schedule_hrtimeout` — poll wait just woken, not on CPU (`is_current=false`).
- Hart regs: `pc=0`, `tp=0`, `mcause=0x38000001` (instruction access fault), `mstatus=0x11880`. This is a new sample class versus the later idle/`wfi` sits. Repeat before treating jump-to-0 as named.

## Pre-arm register artifact (2026-09-02)

Four pre-armed halts (`exec0`, `exec0-r2`, `exec08`, `exec2`) all reported the same
`pc=0 ra=0x49b7ce84 sp=0x49bc4720`. That is not the wedge site.

`exec2-resample`: after `resume` 50 ms + rehalt, HP is `arch_cpu_idle` /
`swapper/0` / `mcause=0x88000016`. First-halt `pc=0` is a pre-arm OpenOCD view.

Task walk is still usable:

| exec+ | dropbear |
|---|---|
| 0.09 s | `R` (poll wakeup) |
| 0.80 s | `R` |
| 2.00 s | `R`; child `sh` already `dead` |
| ~3 s (OpenOCD started after sleep) | gone |

Cause still unnamed. Next useful check is `on_rq` for that `R` dropbear, not another `pc=0` shot.

## CFS / on_rq (2026-09-02, exec+2s)

Image `b4575e14`. Pre-arm memory is trustworthy; first-halt `pc=0` is not.

- dropbear `R` `on_rq=1` `prio=120` `policy=0`. Child `sh` already `dead`.
- `rq->curr` = dropbear. `nr_running=1` (one shot had `ksoftirqd` also `R`).
- `TIF_NEED_RESCHED` clear on idle and dropbear. Not a lost-enqueue.
- 2 ms resume: scheduler alive (`es-beat` / `set_next_task_fair`). dropbear still `R`. `esp_TX` can wake to `R`.
- 8 PC samples after that halt: first is dropbear in `update_process_times` (tick), then 7× `arch_cpu_idle`. Interrupted user/kernel PC was not recorded (`mepc` missing).
- By ~exec+3 s without holding halt, dropbear is gone. Paramiko `Invalid packet blocking`.
- `last_feed_age_jiffies` ≈ 11500 (HZ=250 → ~46 s) even while `es-beat` can run. Software WDT stamp is already stale before exec.
- printk scan of the first 8 KiB of `log_buf`: no dropbear/oops hit (ring format may hide it).

Not CFS-starvation. dropbear is current, then one tick later the CPU is idle and the SSH session dies. Next: `mepc` of that tick (the interrupted dropbear PC) and `jiffies` vs `timer_bases.next_expiry`.

## What this cannot see

- C6 / SDIO slave registers unless the bus still answers.
- Whether a higher-priority CLIC handler is the sit vs `MIE=0` in a
  thread: `pc` plus `mcause` have to say.
- A halt that works only because attach itself woke a stuck bus.
