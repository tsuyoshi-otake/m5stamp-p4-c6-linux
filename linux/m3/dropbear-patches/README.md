# Dropbear fixes for SSH images

Buildroot global patches for Dropbear **2026.91**, staged into
`BR2_GLOBAL_PATCH_DIR/dropbear/` for every SSH profile (`m3` and `m3-lab`).
They are numbered after the m3-lab ledger series
(`../../m3-lab/dropbear-patches/0001`–`0004`, applied only with
`SSH_LEDGER=1`) and apply with or without it.

| Patch | Fixes |
| --- | --- |
| `0010-vfork-child-shared-memory.patch` | A command run over SSH ended with `Bad packet length` / `Connection corrupted` instead of its exit status. |
| `0011-wtmp-rotate.patch` | `/var/log/wtmp` grew without limit on the 2 MB `/tmp` ramdisk. |
| `0012-vfork-child-exit.patch` | The vfork child ran libc code in the server's memory, and `free_child_envp()` freed every other element and walked past the end of the array: the server took SIGSEGV and printed `Aiee, segfault!` on the SSH socket right after the session opened. |
| `0013-ssh-pty-vfork.patch` | The PTY session's vfork child still did libc work (`signal()`, malloc, utmp, motd) in the server's memory. |
| `0014-pty-eof-hangup.patch` | A client `channel-eof` right after the exec request closed the pty master, so the hangup signalled the child while it was still inside `execve()`. |

## 0010: the vfork() child rewrote the server's memory

On NOMMU Dropbear starts the session command with `vfork()`, so the child
runs in the server's memory until it execs. uClibc's LinuxThreads `signal()`
keeps the real handler in a table in that memory and installs its own
`pthread_sighandler` in the kernel. The child's `signal(SIGCHLD, SIG_DFL)`
cleared the server's table entry, and the SIGCHLD sent when the command
exited made the server jump to address 0. Its SIGSEGV handler printed
"Aiee, segfault!" to stderr, which is the SSH socket under inetd, so the
client saw a corrupt packet.

Evidence (2026-10-15, CI v0.2.2 image, STA): a debug build whose SIGSEGV
handler saved the registers showed `pc=0`, `s1=17` (SIGCHLD) and a return
address in `pthread_sighandler`. With the patch, `ssh m5@… 'id'` exits 0 and
`ssh m5@… 'exit 7'` exits 7.

The patch also makes `svr_dropbear_exit()` leave a vfork child (exec, dup2
or chdir failed) with `_exit()` before it frees the host key and addresses
or runs `exit()`, both of which act on the server's memory.

A check for "it worked" must include the client's exit status: the remote
`echo rc=$?` prints before the crash, so `id` output plus `rc=0` alone does
not show that the session ended cleanly.

## 0011: wtmp is rotated at 32 KiB

S05 creates `/var/run/utmp` and `/var/log/wtmp` (#37) so that `who`, `w`,
`users` and `last` work. `/var/log` is `/tmp`, a 2 MB ext2 ramdisk shared with
`/home`, and nothing like logrotate runs, while every SSH login with a
terminal adds two 384-byte records. Dropbear is the only writer, so it
rotates the file itself: once the next record would take `wtmp` past 32 KiB it
is copied over `wtmp.1` (the previous `wtmp.1` is dropped) and emptied. The two
files hold about 85 logins, as `/run/messages` keeps 32 KB plus one old copy
(#26).

The first version renamed `wtmp` and created a new one, and on the board every
rename failed with `Operation not permitted`: after authentication Dropbear's
session process runs as the user (`Uid: 1000`) and switches its effective
group to `utmp` only to write the records (`svr-auth.c`). It can write the
`root:utmp 0664` files but cannot rename in the sticky `/tmp` or give a new
file to root. Copying needs only write access, and S05 creates `wtmp.1` too,
so both files stay root-owned and the user cannot edit them. Every writer
takes `flock()` on `wtmp`, so a record from another login cannot fall between
the copy and the truncation.
Commands run without a terminal (`ssh host cmd`) are not recorded, as in
OpenSSH.

## 0012: the vfork() child must not run libc teardown on exec failure

With `DROPBEAR_VFORK=1` the session child shares the server's memory until
`execve`.  If exec fails before the kernel's point of no return, `execv()`
returns `-1` in the child and the old code kept running dropbear's error
path — logging, stdio, `m_free`, `clearenv`, `putenv`, `signal()` — all in
the *parent's* address space.  uClibc's `sigaction()` writes
`sighandler[sig]` even for `SIG_DFL`, so the child scribbled the shared
handler table and the parent's next signal delivery (SIGCHLD) could jump
to a dead pointer.

0012 moves every libc operation out of the vfork window: the parent builds
the complete session `envp` (USER/LOGNAME/HOME/SHELL/PATH/LANG/TERM/
SSH_TTY/SSH_CONNECTION/SSH_CLIENT/SSH_ORIGINAL_COMMAND/SSH_PUBKEYINFO/
SSH_AUTH_SOCK) before `vfork()`, and the child performs only `chdir`,
the SIGPIPE reset, and `execve()`.  SIGPIPE matters because upstream
`commonsetup()` sets `SIG_IGN` in `svr-main.c` and the ignore survives
exec, so the child restores `SIG_DFL` with a raw `SYS_rt_sigaction`
syscall — that touches only the child's kernel sigaction (vfork has no
`CLONE_SIGHAND`) and not uClibc's shared `sighandler[]`.  Any failure exits
with `_exit(127)`; there is no `dropbear_exit()`, no logging, no heap work.

Acceptance: `ssh host 'grep SigIgn /proc/self/status'` shows the SIGPIPE
bit (0x1000) clear — same as stock.

Note: #54's observed SIGILL/SIGSEGV/SIGBUS crashes turned out to be the
ML-KEM-768 KEX overflowing Dropbear's 32 KB bFLT stack, which this patch
does not fix — that is addressed by the post-build `flthdr -s 262144` step
in `linux/m3*/post-build.sh`.  0012 remains valid hardening for the
separate exec-failure path.

### 0012, second version: `free_child_envp()` walked past the array

The first version of 0012 freed the parent-built envp with `m_free(*e++)`.
In Dropbear `m_free(X)` expands to `{ free(X); (X) = NULL; }`, so `*e++` was
evaluated twice: every other string was freed, the ones in between were
overwritten with NULL, and with an odd number of entries the loop ran one
element past the NULL terminator and called `free()` on the size word of the
next heap chunk.  The server process then took SIGSEGV just after the vfork
child had started.  Its SIGSEGV handler wrote `Aiee, segfault!` to stderr,
which is the SSH socket under inetd; the client decrypts that plain text into
garbage and reports `Bad packet length` (OpenSSH) or `Invalid packet blocking`
(paramiko) before any command output.  The loop now uses `m_free_direct()`,
which evaluates its argument once.

Whether it crashes depends on the heap layout.  On the Stamp the sessions
without a terminal survived by luck (a NULL word followed the array), and the
sessions with one died every time (Stamp #53).  On the Tab5 every session
died, with and without a terminal.

Tab5 evidence (2026-11-13, SoftAP, PC joined with a second Wi-Fi adapter):
password authentication succeeded, then the stream broke when the session
channel was opened; paramiko and OpenSSH, three different ciphers, same place.
A 440,228-byte TCP download over the same link kept its SHA-256, so the radio
path was clean, and the raw bytes captured after the channel opened were the
plain text `Aiee, segfault! `.  The syslog showed `Password auth succeeded`
but never `Exit (m5) from ...`, and `dmesg` had
`binfmt_flat: Unable to read code+data+bss, errno -4` (see 0014 for that
line).

### 0013: the PTY child does plain system calls only

`ptycommand()` still ran `signal()` (inside `pty_make_controlling_tty()`),
malloc, a utmp write and the motd code in the vfork child, all in the
server's memory.  For `DROPBEAR_VFORK` the child now does only `close`,
`setsid`, `ioctl(TIOCSCTTY)`, `dup2`, a plain `write()` of the motd and
`execve()`; the environment, the motd read and the utmp record moved to the
parent (the record also gets the real child pid).  Every child error path
ends in `_exit(127)`; the old `return DROPBEAR_FAILURE` from a vfork child
returned into the server's frames.  The fork() path is unchanged.

### 0014: a client channel-eof must not close the pty master

When the client's stdin is already at EOF (`ssh -tt host cmd </dev/null`),
OpenSSH sends `SSH_MSG_CHANNEL_EOF` right after the exec request.
`close_chan_fd()` then `m_close()`d the pty master, which was both readfd and
writefd with `bidir_fd = 0`.  That was the last master fd, so the pty driver's
`vhangup` sent SIGHUP to the new session leader, and on this target the
signal arrives while the child is still inside `execve()`: the flat loader's
killable `kernel_read` returns `-EINTR` (`binfmt_flat: Unable to read
code+data+bss, errno -4`) and the session dies.  A fast machine usually
finishes the exec first.  With `bidir_fd = 1` the eof takes the socket path
(`shutdown()` on a pty fails harmlessly, the writefd is marked closed) and the
master stays open until the channel is torn down.  The no-pty path keeps
`bidir_fd = 0`, because there writefd is a pipe and closing it is what
delivers EOF to the command.

0012 (second version), 0013 and 0014 are the #53 fixes from the
`i53-52-dropbear` branch, taken in the same form as the Tab5 repository
(6c621a7 there).  The branch's #52 work (a server-only Dropbear build) is not
part of this.

Stamp evidence without these fixes (2026-11-14, shared-fixes image, SoftAP,
an Android phone relaying port 22): the sessions without a terminal died too.
Password authentication succeeded, the stream broke as soon as the session
channel opened (paramiko `Invalid packet blocking`), and an on-board
`dbclient m5@127.0.0.1 'id'` failed the same way (`Integrity error (bad packet
size ...)`), while ICMP up to 1472 bytes over the same link came back intact.
