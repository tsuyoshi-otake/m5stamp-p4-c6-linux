# BusyBox fixes for network images

Buildroot global patches for BusyBox **1.37.0**, staged into
`BR2_GLOBAL_PATCH_DIR/busybox/` for every network profile (`m2`, `m3` and
`m3-lab`). `/bin/sh` on these images is BusyBox hush (`CONFIG_SH_IS_HUSH=y`);
ash needs `fork()` and so an MMU. The m3-lab SSH ledger markers
(`../../m3-lab/busybox-patches/`) are injected by hand and are not part of
this series.

| Patch | Fixes |
| --- | --- |
| `0001-hush-set-e-fixes.patch` | Under `set -e` a script went on after a failure once part of an `a && b \|\| c` list had been skipped, and stopped when a `while` or `until` loop ended or after `! cmd` (#41). |

## 0001: "set -e" in hush

`run_list()` runs a list of pipes and keeps `set -e` off while
`G.errexit_depth` is not zero; a pipe that `set -e` does not apply to raises
the depth while it runs. BusyBox 1.37.0 had three bugs there:

1. A pipe raised the depth before the test that skips the rest of
   `a && b` or `a || b` and lowered it only after it had run, so a skipped
   pipe that was itself followed by `&&` or `||` left it raised:

   ```sh
   set -e; false && : || :; false; echo LEAK    # printed LEAK
   ```

   At the top level of a script `set -e` stayed off to the end, and
   `set +e; set -e` did not turn it back on.
2. POSIX exempts the condition of `while` and `until` as it does that of
   `if`, but hush exempted only `if` and `elif`. A loop that ended because
   its condition became false ended the script with status 1:

   ```sh
   set -e; while read l; do :; done <file; echo after    # no "after"
   ```
3. `! pipe` was not exempt either: `set -e; ! true` exited.

The patch saves the depth once per `run_list()` call, starts every pipe from
it, adds `while`, `until` and `!` to the exempt cases, and restores the depth
after the loop, which `break`, `continue` and `return` can leave early.

Checked on an x86 build of the board's BusyBox configuration: for 49 test
scripts the patched hush prints the same as dash and `bash --posix`, where the
unpatched one differed in 17. BusyBox's own `shell/hush_test` gives 361 ok and
18 failed with and without the patch, with the same 18 failures. On the
board, the unpatched `/bin/sh` printed `LEAK`, exited with 1 at the end of a
counting `while` loop, and exited after `! true`.

### Effect on the image's scripts

* `S40network` (m3-lab) had `set -e`. Its top-level
  `[ -p "$NET_TICK_FIFO" ] || mkfifo … || true` leaked on every run after the
  first, so `apply`, `reload`, `restart` and `stop` ran without it. At boot it
  was in effect, and a wait loop that timed out (STA with a wrong PSK) ended
  the script before it printed "did not associate". The script now runs
  without `set -e` (see the comment at its top): every step handles its own
  failure, and a command that fails for want of memory must not stop a reload
  after wpa_supplicant was stopped.
* `S05easystick-tmpfs`, `S85easystick-ssh`, `easystick-firstboot` (m3) and
  `easystick-sdio-diag` keep `set -e`, and the patch does not change what
  they do: their loops run inside `$(...)` or as `cmd || …`, where `set -e`
  is off either way, and where a leak could happen it came at the end of a
  block or before commands that are exempt anyway.

### Not changed: subshells on NOMMU

Without `fork()`, hush runs `( … )`, `$(...)`, `{ … } &` and the builtins,
functions and compound commands of a pipeline by executing itself again
(`re_execute_shell()`), which passes variables and functions but no `set -o`
options. There `set -e` is off, while POSIX has a subshell inherit it.
Scripts on these images must not rely on `set -e` inside `( … )` or
`$(...)`.
