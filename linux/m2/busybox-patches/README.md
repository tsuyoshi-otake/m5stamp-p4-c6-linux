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
| `0002-hush-nommu-subshell-options.patch` | A `( … )` or `$(…)` subshell, `{ … } &` and the functions and compound commands of a pipeline ran without `set -e`, `set -x` and `set -o pipefail` (#41). |

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

## 0002: options in NOMMU subshells

Without `fork()`, hush runs `( … )`, `$(…)`, `{ … } &` and the builtins,
functions and compound commands of a pipeline by executing itself again
(`re_execute_shell()`). The `-$` argument hands the new process the PIDs,
`$?`, the ignored signals and the loop depth, and further arguments pass
the variables and functions, but neither the `set -o` options nor
`G.errexit_depth` were passed. So these subshells ran without `set -e`,
while POSIX has a subshell inherit it and hush on an MMU (which forks)
does:

```sh
set -e; (false; echo SUB); echo PARENT      # printed SUB and PARENT
set -e; x=$(false; echo SUB); echo "x=$x"   # printed x=SUB
```

`set -x` and `set -o pipefail` were lost the same way. The patch adds the
options, as a bit mask, and the depth to the `-$` argument; the new process
sets them before it runs the subshell and, under `set -x`, opens the trace
descriptor as `set -x` does. With the depth, `set -e` stays off in a
subshell that runs where it does not apply, as in `if (a; b); then` or
`x=$(a; b) || …`, as it does in a forked one.

Checked on x86 builds of the board's BusyBox configuration with
`CONFIG_NOMMU=y` (hush re-executes itself as on the board) and without it
(hush forks), both with 0001:

* For 38 subshell scripts (`set -e` in `( … )`, `$(…)`, pipelines,
  functions, nested subshells, `if`, `while`, `&&`, `||` and `!` contexts,
  `trap ''`, a here-document, `set -x`, `pipefail`, `$-`), the patched
  NOMMU hush prints the same as the MMU one but in one: trace lines from
  inside `$(…)` start with `+`, where the MMU hush and bash print `++` (dash
  prints `+`). The unpatched NOMMU hush differed in 19.
* For the 60 scripts of 0001 it prints the same as the MMU hush and dash.
* BusyBox's `shell/hush_test` gives 361 ok and 18 failed with and without
  the patch, the same 18 as the MMU build.
* The MMU build compiles to the same `hush.o` with and without the patch:
  the code is NOMMU only.

### Effect on the image's scripts

`S05easystick-tmpfs` (with the `credentials.sh` and `autorun.sh` it
sources), `S85easystick-ssh`, `S10easystick-config`, `easystick-firstboot`
and `easystick-sdio-diag` keep `set -e`, and the patch does not change what
they do. Their `$(…)` and `( … )` run one command, or a pipe whose status
is its last command's either way, or `cmd || …`, or run where `set -e` is
off (an `if` condition or `cmd || true`). None uses `set -x` or `pipefail`.

### Not changed

* A background `( … ) &` or `{ … } &` runs without `set -e` in hush with or
  without an MMU: `run_list()` treats a pipe followed by `&` like one
  followed by `&&` or `||`, and the subshell inherits that. dash and bash
  apply `set -e` there. No script on these images runs such a command
  under `set -e`.
