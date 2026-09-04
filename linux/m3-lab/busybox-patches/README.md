# BusyBox post-exec SSH ledger (M2.5 / m3-lab only)

Test-only BusyBox markers for the post-`VFORK_PARENT_RESUME` path.
Surgical inject into the extracted BusyBox 1.37.0 tree (same workflow as
Dropbear ledger). Production `m3` must not inherit these.

## Important: shell is hush, not ash

This image has `CONFIG_SH_IS_HUSH=y`. `/bin/sh` → BusyBox hush.
`SHELL_ENTER` is therefore in `shell/hush.c` `hush_main()`, not ash.

## Files

| Path | Role |
| --- | --- |
| `easystick_bb_ledger.c` / `.h` | Dual sink (`/dev/kmsg` + `/dev/ttyGS1`), once-open, errno save/restore |
| Inject: `shell/hush.c` | `ES_SSH SHELL_ENTER` at `hush_main` after `INIT_G()` |
| Inject: `coreutils/id.c` | `ES_SSH ID_ENTER` / `ES_SSH ID_STDOUT_DONE` |

BusyBox child **cannot** use Dropbear's open fds after exec — it opens its own
sinks. Tag prefix stays `ES_SSH` so one passive serial grep covers both.

## Markers

| Tag | Meaning |
| --- | --- |
| `SHELL_ENTER` | `hush_main` entered |
| `ID_ENTER` | `id_main` entered |
| `ID_STDOUT_DONE` | `id` finished printing; about to `fflush_stdout_and_exit` |

## Surgical rebuild

```text
# inside easystick-p4-build with out volume mounted
# copy ledger into busybox-1.37.0/{libbb,include}
# patch hush.c + id.c; append lib-y += easystick_bb_ledger.o to libbb/Kbuild
make O=/out/buildroot busybox-rebuild
# force stamp clear if needed
```

See evidence dir `easystick-m25-ssh-ledger-postexec-20260816` for inject scripts.
