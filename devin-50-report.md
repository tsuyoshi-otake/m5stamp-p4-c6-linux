# #49/#50/#51/#54 memory work report

Merge candidate: kernel patches 0089 (gen_pool best-fit), 0090 (protected
user-allocation reserve — 1.2 MB contiguous / 2 MB total), 0091 (explicit
system marking via `PF_EASYSTICK_SYSTEM` / `easystick-sys`), plus Dropbear
patches 0010, 0011, 0012 and the m3/m3-lab post-build stack fix below.

## Round 7 E — criterion E on mem50r5

| Check | Result |
| --- | --- |
| boot-log checks | 13/13 PASS |
| host tests | PASS |
| USB update | PASS |
| configuration application | PASS |
| STA | PASS |
| AP | board-side PASS (host join not run — off-limits) |
| SSH / scp | PASS |
| autorun | PASS |
| 30-min soak | PASS — uptime 2120 s, `alloc_fail=0`, `system_alloc_fail=0` |

DEVIN_50R7E_DONE

## Round 7 — Dropbear #54: root cause, fix, validation

### Corrected root cause

The crash is **not** a half-loaded child executing corrupted text, and it is
not primarily the vfork child. The direct cause is **ML-KEM-768 hybrid KEX
stack overflow in the bFLT Dropbear binary**.

Evidence chain:

- With `kernel.print-fatal-signals=1`, every SSH attempt to stock or patched
  images crashed during KEX (marker `P30`), before userauth — not in session
  exec.
- Two crash families, both signature `s1=0x11`/SIGCHLD (or 0x0b/SIGSEGV) with
  `ra` immediately after `jalr a5` inside uClibc's
  `pthread_sighandler`/`pthread_sighandler_rt`: the process dispatched a
  pending signal through `sighandler[]`, which held garbage (0, 0xffff,
  0x2ffff). One SIGBUS (`setusershell`, `epc=0`, badaddr=0x29af3c8b) showed a
  corrupted `shells` static pointer — same .bss splash zone.
- Primary fault with SIGSEGV catch disabled: `epc=0x495baca4`, cause=5
  (store), inside `kexpqhybrid_comb_key`, writing via a corrupted crypto
  descriptor function pointer.
- Forcing `curve25519-sha256` KEX made SSH work end-to-end on the same binary;
  default ML-KEM-768 crashed deterministically.
- The shipped Dropbear bFLT header had `Stack Size: 0x8000` (32 KB); the
  elf2flt default is 4 KB. ML-KEM-768 needs >32 KB. `flthdr -s 262144`
  (256 KB) on the same binary made default KEX work completely (session exec,
  clean disconnect, no crash).
- Mechanism: on NOMMU the fixed-size stack lives at the top of the flat
  memory chunk (`end_brk = memp + memp_size - stack_len`); overflow grows the
  heap boundary down into .bss — `sighandler[]`, uClibc `getXXbyYY` static
  buffers, `shellp/shells/shellb`, `__environ` all sit in the splash zone —
  and corrupted objects then dispatch/jump wild. Best-fit packing (0089)
  makes a victim chunk sit deterministically adjacent.

### Fix

1. `linux/m3-lab/post-build.sh`: `usr/sbin/dropbear` and `usr/bin/dbclient`
   now get `flthdr -s 262144` (256 KB) with a build-fail check for
   `0x40000`; other network daemons keep 32 KB; the ≥16 KB floor check is
   unchanged.
2. `linux/m3/post-build.sh`: gained the same flthdr block (the profile had
   none — shipped Dropbear there ran the 4 KB default).
3. `linux/m3/dropbear-patches/0012-easystick-vfork-child-exit.patch` kept as
   hardening (pre-existing defect, proven by register evidence, but not the
   ML-KEM trigger): under `DROPBEAR_VFORK` the vfork child does only
   `chdir`/`execve` with a parent-built envp and `_exit(127)` on any failure;
   SIGPIPE is restored for the exec'd program with a raw
   `SYS_rt_sigaction(SIGPIPE, SIG_DFL)` syscall so uClibc's shared
   `sighandler[13]` is untouched. This is required because upstream
   `commonsetup()` sets `SIGPIPE` to `SIG_IGN` and the ignore survives
   `execve` — without the reset every SSH shell inherits ignored SIGPIPE
   (pipelines like `yes | head` change behaviour).
   Acceptance check: `ssh host 'grep SigIgn /proc/self/status'` shows bit
   0x1000 clear — verified PASS with the 256 KB stack.
4. Kernel patches 0089/0090/0091 unchanged — Round 6 exonerated them (crash
   reproduced on stock).

### Validation

Acceptance check (`ssh host 'grep SigIgn /proc/self/status'`): **PASS** —
`SigIgn: 0000000000000000`, the SIGPIPE bit (0x1000) is clear in the exec'd
shell, same as stock.

Shipped header verified: `usr/sbin/dropbear` reports
`Stack Size: 0x40000` in the build target.

Crash scenario (N churn sessions → 5× `S40network restart` → 20 sequential
SSH probes), mem50r8 on the M1 board:

| run | churn | probes ok | post-auth closes | dmesg crashes | freeze |
| --- | --- | --- | --- | --- | --- |
| 1 | 4 | 20/20 | 0 | 0 | none |
| 2 | 4 | 20/20 | 0 | 0 | none |
| 3 | 4 | 20/20 | 0 | 0 | none |

Run 3 shows one probe with `auth=no` on the board-log counter but `ok` on the
session — a `/run/messages` read race, not a failure.

SIGILL/SIGSEGV/SIGBUS observed: **0** across all three runs. Clean ENOMEM
closes: **0**.

Note: the `nommu-mem-harness.py` `net_up()` helper gained an IPv4-lease gate —
operstate recovers ~10 s after a restart but udhcpc can still be acquiring,
and probes launched in that window fail at TCP connect (this hid behind
serial-open board resets in the earlier rounds' logs).
