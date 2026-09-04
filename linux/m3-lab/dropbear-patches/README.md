# Dropbear SSH stage ledger (M2.5 / m3-lab only)

Test-only Buildroot global patches for Dropbear **2026.91**. They are staged
into `BR2_GLOBAL_PATCH_DIR/dropbear/` only when
`EASYSTICK_SSH_LEDGER=1 build-m1.sh --profile m3-lab` runs. The default
`m3-lab` image and production `--profile m3` do **not** apply them, because
the hot-path markers can perturb the NOMMU watchdog timing.

## Why

M2.5 boundary shots showed:

- `MemAvailable ≈ 15 MB`, no reboot / OOM / panic on the one-shot
- `:22` LISTEN and TCP connect can succeed while Pi still times out at
  **banner exchange** (pre-auth)
- Earlier smoke sometimes reached key-auth then hung (post-auth)

So markers must start at **DB_ENTER / RNG / IDENT**, not only at `AUTH_OK`.

The next one-shot discriminator is **`IDENT_SENT` presence/absence**:

- `IDENT_SENT` = userspace `atomicio(vwrite, …)` of the full
  `SSH-2.0-dropbear…\r\n` banner completed on `ses.sock_out`
- Pi observing the banner = separate end-to-end RX through TCP/IP →
  ESP-Hosted → SDIO → C6

Keyfix one-shot (`easystick-m25-ssh-ledger-keyfix-20260816`) hung past
banner into auth and hit **WDT**, wiping `/dev/kmsg`. Passive passive serial
raw had **no live `ES_SSH` lines before reset** (only the post-reboot
`dmesg | grep ES_SSH` command). Therefore the test-only sink is **dual**:
`/dev/kmsg` + `/dev/ttyGS1` (same open flags; write-only markers).

## Markers (passive serial and/or `dmesg | grep ES_SSH`)

Never written to stdout/stderr (inetd binds the SSH socket to stdio).
Both `/dev/kmsg` and `/dev/ttyGS1` are opened once at Dropbear start
(`O_WRONLY|O_CLOEXEC|O_NONBLOCK`); markers are fixed `write()`s with errno
save/restore. No flash/MTD/PSRAM/RTC persistence. Implementation is
`easystick-ssh-ledger.c` + `Makefile.in` object (not a multi-included static
header).

### Pre-auth / session (unchanged set)

| Tag | Meaning |
| --- | --- |
| `DB_ENTER` | `main_inetd()` entered; ledger fds opened |
| `RNG_BEGIN` / `RNG_OK` | around `seedrandom()` |
| `IDENT_BEGIN` | about to write the server banner |
| `IDENT_SENT` | full banner write to the socket completed |
| `PEER_IDENT` | client banner received |
| `KEX_OK` | first server KEX completed (`send_msg_newkeys`) |

### KEX → AUTH (public-key path fine markers)

Fixed tags only — no pubkey/base64/fingerprint in logs.

| Tag | Meaning |
| --- | --- |
| `AUTH_REQ` | `recv_msg_userauth_request` entered |
| `PUBKEY_ENTER` | `svr_auth_pubkey` entered |
| `AUTHKEY_OPEN_BEGIN` | about to open `authorized_keys` |
| `AUTHKEY_OPEN_OK` | `authorized_keys` opened |
| `AUTHKEY_PARSE_BEGIN` | start parsing key lines |
| `AUTHKEY_MATCH` | a line matched the offered key |
| `SIG_VERIFY_BEGIN` | about to `buf_verify` |
| `SIG_VERIFY_OK` / `SIG_VERIFY_FAIL` | verify result |
| `AUTH_COMMIT` | entering `send_msg_userauth_success` (before encrypt) |
| `AUTH_OK` | `authdone` set after success packet |

### Post-auth (unchanged)

| Tag | Meaning |
| --- | --- |
| `SESSION_EXEC` | channel shell/exec/subsystem request |
| `VFORK_BEGIN` | about to `vfork`/`fork` in `spawn_command` |
| `VFORK_CHILD` | child after vfork (fixed write to pre-opened fds only) |
| `VFORK_FAIL errno=N` | `vfork`/`fork` failed |
| `VFORK_PARENT_RESUME` | parent resumed after child exec/exit (NOMMU vfork) |
| `EXEC_BEGIN` | child about to call `exec_fn` |
| `EXEC_FAIL errno=N` | `execv` returned (failure); success never returns |

### Post-exec (0002 — first shot)

| Tag | Meaning |
| --- | --- |
| `DROPBEAR_PIPE_READ` | first successful `read()` of session stdout pipe in `send_msg_channel_data` |
| `DROPBEAR_SOCKET_WRITE` | first successful `write_packet` sock_out write after that channel-data packet was queued |

### Early-init path (0004 — 0024-E0 observe)

VALID reshot saw `DB_ENTER` then nothing before `RNG_*`. Between those tags
`main_inetd()` only calls `commonsetup()`, which itself calls real Dropbear
functions. Markers use those names (no invented `SIGNAL_INIT`):

```text
DB_ENTER
EARLY_COMMONSETUP_BEGIN
  EARLY_STARTSYSLOG_BEGIN/DONE   (if opts.usingsyslog)
  /* libc signal()/sigaction registration — unmarked */
  EARLY_CRYPTO_INIT_BEGIN/DONE
  EARLY_LOAD_ALL_HOSTKEYS_BEGIN/DONE
EARLY_COMMONSETUP_DONE
RNG_BEGIN
```

Exit / fatal observe (no message bodies):

| Tag | Meaning |
| --- | --- |
| `DROPBEAR_EXIT` | `svr_dropbear_exit` entered |
| `DROPBEAR_FATAL site=1` | `commonsetup` `signal(SIGINT/TERM/PIPE)` failed |
| `DROPBEAR_FATAL site=2` | `commonsetup` `sigaction(SIGCHLD)` failed |
| `DROPBEAR_FATAL site=3` | `commonsetup` `signal(SIGSEGV)` failed |
| `DROPBEAR_FATAL site=4` | `load_all_hostkeys` — no hostkeys |
| `SIGNAL_TERM` | `sigintterm_handler` (async-safe fixed write) |

### Channel / encrypt path (0003 — 0024 observe)

No payload / ciphertext bytes. Encrypt tags only for known interesting types.

| Tag | Meaning |
| --- | --- |
| `CHANNEL_DATA_BUILD chan=N len=N` | stdout bytes copied into CHANNEL_DATA payload |
| `CHANNEL_DATA_QUEUE chan=N len=N` | about to `encrypt_packet()` for that CHANNEL_DATA |
| `ENCRYPT_BEGIN ssh_seq=N type=…` | encrypt start; `type` is `CHANNEL_DATA` / `USERAUTH_SUCCESS` / `CHANNEL_OPEN_CONFIRM` / `WINDOW_ADJUST` / `CHANNEL_EOF` / `CHANNEL_CLOSE` / `CHANNEL_REQUEST` / `CHANNEL_EXT_DATA` |
| `ENCRYPT_DONE ssh_seq=N wire_len=N` | encrypt + writequeue enqueue completed |
| `SOCKET_WRITE_BEGIN/DONE ssh_seq=N wire_len=N` | first sock_out write for the pending CHANNEL_DATA wire buffer |
| `CHANNEL_EOF` / `EXIT_STATUS` / `CHANNEL_CLOSE` | once each when those messages are built |

Critical chain for `id` stdout:

```text
DROPBEAR_PIPE_READ → CHANNEL_DATA_BUILD → ENCRYPT_DONE → SOCKET_WRITE_DONE → (Pi)
```

BusyBox-side companions (`SHELL_ENTER` / `ID_ENTER` / `ID_STDOUT_DONE`) live
under `m3-lab/busybox-patches/` (hush + id; dual sink opened in the BusyBox
process). Same `ES_SSH` prefix.

There is **no `EXEC_OK`**. Exec success = `EXEC_BEGIN` present + `EXEC_FAIL`
absent + `id` stdout on the Pi.

## Classification (first missing / mismatched tag)

```text
DB_ENTERなし → inetd → exec(dropbear)
DB_ENTER + EARLY_COMMONSETUP_BEGIN [欠落] → before commonsetup mark
EARLY_COMMONSETUP_BEGIN + EARLY_STARTSYSLOG_DONEなし + CRYPTOなし → startsyslog or signal() setup
EARLY_CRYPTO_INIT_BEGIN + DONE [欠落] → crypto_init()
EARLY_LOAD_ALL_HOSTKEYS_BEGIN + DONE [欠落] → load_all_hostkeys()
EARLY_COMMONSETUP_DONE + RNG_BEGIN [欠落] → between commonsetup return and seedrandom
DROPBEAR_FATAL site=N / DROPBEAR_EXIT / SIGNAL_TERM → early abort path
DB_ENTER + RNG_BEGIN [欠落] (legacy, no 0004) → RNG / commonsetup undifferentiated
RNG_OK + IDENT_BEGIN [IDENT_SENTなし] → banner生成 / socket write
IDENT_SENT + Pi bannerなし → TCP/IP → ESP-Hosted → SDIO → C6 ⇒ 0023
PEER_IDENTあり + KEX_OKなし → KEX
KEX_OKあり + AUTH_REQなし → packet dispatch / I/O after KEX (never entered auth)
AUTH_REQあり + PUBKEY_ENTERなし → method selection (not pubkey / other method)
PUBKEY_ENTERあり + AUTHKEY_OPEN_BEGINなし → pre-open pubkey path
AUTHKEY_OPEN_BEGINあり + AUTHKEY_OPEN_OKなし → authorized_keys open/perms
AUTHKEY_OPEN_OK / PARSE_BEGINあり + AUTHKEY_MATCHなし → key parse / no match
AUTHKEY_MATCHあり + SIG_VERIFY_BEGINなし → testkey-only or pre-verify exit
SIG_VERIFY_BEGINあり + SIG_VERIFY_OK/FAILなし → hung inside verify
SIG_VERIFY_OKあり + AUTH_COMMITなし → post-verify path before success
AUTH_COMMITあり + AUTH_OKなし → hung in success encrypt / authdone
AUTH_OKあり + SESSION_EXECなし → SSH channel / exec request
VFORK_BEGINあり + VFORK_CHILDなし / VFORK_FAIL → NOMMU process creation
VFORK_CHILDあり + EXEC_BEGINなし → child setup / fd操作
EXEC_BEGINあり + EXEC_FAILあり → FLAT /bin/sh exec失敗
EXEC_BEGINあり + EXEC_FAILなし + SHELL_ENTERなし → shell startup (unexpected if PARENT_RESUME)
SHELL_ENTERあり + ID_ENTERなし → hush dispatch / 2nd vfork
ID_ENTERあり + ID_STDOUT_DONEなし → id applet / stdout
ID_STDOUT_DONEあり + DROPBEAR_PIPE_READなし → pipe
PIPE_READあり + CHANNEL_DATA_BUILDなし → Dropbear internal
CHANNEL_DATA_BUILD … SOCKET_WRITE_DONE + Pi -vvvにCHANNEL_DATAなし → SSH protocol / client
SOCKET_WRITE_DONE + Pi CHANNEL_DATAあり + idなし → higher layer
CHANNEL_DATA完了 + WDT feeds継続 → SSH/session hang without kernel lockup
CHANNEL_DATA付近 + WDT feeds即停止 → single-core stall / IRQ-off / lockup
id + rc=0 → M2.5 PASS
```
Note: WDT may glue the last marker to `ESP-ROM:` on one line — match without
requiring a trailing word boundary.

## Protocol note (server IDENT path)

Server identification bypasses `writebuf_enqueue` and writes with
`atomicio(vwrite, ses.sock_out, …)` so `IDENT_SENT` means the kernel accepted
the full banner write. Client Dropbear still uses the writequeue path. Residual
risk: `ses.sock_out` is non-blocking; `atomicio` busy-retries on `EAGAIN`
(OpenSSH-style). For a ~30-byte banner on a fresh TCP session this is expected
to complete; a stuck zero window would spin until the write fails or exits.

## M2.5 gate note

DUT loopback `dbclient` is **diagnostic-only / non-gating**. External
`Pi → DUT` key-auth + command stdout is the product path.

This patch alone does **not** complete M2 / M2.5 — rebuild, flash, and one-shot
capture are still required.
