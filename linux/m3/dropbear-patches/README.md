# Dropbear fixes for SSH images

Buildroot global patches for Dropbear **2026.91**, staged into
`BR2_GLOBAL_PATCH_DIR/dropbear/` for every SSH profile (`m3` and `m3-lab`).
They are numbered after the m3-lab ledger series
(`../../m3-lab/dropbear-patches/0001`–`0004`, applied only with
`EASYSTICK_SSH_LEDGER=1`) and apply with or without it.

| Patch | Fixes |
| --- | --- |
| `0010-easystick-vfork-child-shared-memory.patch` | A command run over SSH ended with `Bad packet length` / `Connection corrupted` instead of its exit status. |
| `0011-easystick-wtmp-rotate.patch` | `/var/log/wtmp` grew without limit on the 2 MB `/tmp` ramdisk. |

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
