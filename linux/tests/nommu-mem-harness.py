#!/usr/bin/env python3
"""nommu-mem-harness.py - #50 measurement harness for the easystick NOMMU pool.

Runs on the build host (Windows or Linux). Drives the board over the serial
console for every measurement command (zero extra pool footprint) and uses
SSH only for the session-count scenarios, where SSH is the subject.

Scenarios:
  idle      - one snapshot
  wget      - wget 10MB to /dev/null, meminfo sampled during the transfer
  ssh N     - N concurrent SSH sessions held open, snapshot while held
  scenarioA - N SSH sessions held, then (via serial) a 5-stage pipeline x10
              and 5x S40network restart; records recovery times and
              alloc_fail deltas (epic #49 criterion A)

Output: Markdown tables on stdout. Nothing that identifies the network
(IPs, MACs, SSIDs) is ever printed; the board address is discovered from
the console and kept in memory only.

Usage:
  py nommu-mem-harness.py [--port COM10] [--scenario all|idle|wget|ssh|scenarioA]
      [--n 6] [--key ~/.ssh/id_ed25519.pub] [--label NAME]
"""
import argparse, os, re, subprocess, sys, time

try:
    import serial
except ImportError:
    sys.exit("pyserial required: pip install pyserial")

PROMPT = b'~ # '
MARK = '@@E'


class Board:
    """Serial console session. send() returns text between the echoed
    command and the next prompt; async log noise is tolerated via the
    end marker."""

    def __init__(self, port, baud=115200):
        self.s = serial.Serial()
        self.s.port, self.s.baudrate, self.s.timeout = port, baud, 0.1
        self.s.dtr = self.s.rts = False
        self.s.open()
        self.sync()
        # echo off: busybox line-editing wraps long command echoes and breaks
        # marker matching.  stty -echo leaves output clean.
        self.s.write(b'stty -echo\r')
        self._until(PROMPT, 2)
        self.s.read(4096)

    def _until(self, tok, t):
        end, buf = time.time() + t, b''
        while time.time() < end:
            buf += self.s.read(4096)
            if buf.endswith(tok):
                break
        return buf

    def sync(self):
        self.s.write(b'\x03')
        self._until(PROMPT, 2)
        self.s.write(b'\r')
        self._until(PROMPT, 2)

    def send(self, cmd, wait=8):
        """Single-line cmd; echoes nothing (stty -echo), output ends at the
        MARK line."""
        self.s.write(cmd.encode() + b'; echo ' + MARK.encode() + b'\r')
        self.s.flush()
        out = self._until(MARK.encode() + b'\r\n', wait)
        out += self._until(PROMPT, 2)
        txt = out.decode('utf-8', 'replace').replace('\r', '')
        return txt.split(MARK)[0]

    def spawn(self, cmd):
        """Fire-and-forget: start a backgrounded command, no sync."""
        self.s.write(cmd.encode() + b'\r')
        self.s.flush()
        time.sleep(0.2)

    def close(self):
        try:
            self.s.write(b'stty echo\r')
            time.sleep(0.2)
        except Exception:
            pass
        self.s.close()


def kv(text):
    """'key value' lines -> dict."""
    d = {}
    for ln in text.splitlines():
        p = ln.split(None, 1)
        if len(p) == 2:
            d[p[0]] = p[1].strip()
    return d


# Under pool exhaustion even `cat`/`grep` cannot exec (binfmt_flat needs a
# contiguous pool block), so the hot-path readers below use only shell
# builtins.  `while read ... done < file` does NOT work here -- busybox read
# drains the whole seq_file on the first call and discards past the newline.
# Holding the fd with `exec 3<` and issuing sequential `read <&3` advances
# line by line (verified on the board).
BUILTIN_POOL = ('exec 3< /sys/kernel/easystick/nommu_pool; '
                'read a <&3; read b <&3; read c <&3; read d <&3; read e <&3; '
                'read f <&3; read g <&3; read h <&3; exec 3<&-; '
                'echo "$a"; echo "$b"; echo "$c"; echo "$d"; '
                'echo "$e"; echo "$f"; echo "$g"; echo "$h"')
BUILTIN_MEMINFO = ('exec 3< /proc/meminfo; i=0; while [ $i -lt 45 ]; do '
                   'read L <&3 || break; case "$L" in '
                   'MemFree:*|MemAvailable:*|Buffers:*|Cached:*|Slab:*|'
                   'SReclaimable:*) echo "$L";; esac; i=$((i+1)); done; '
                   'exec 3<&-')


def _kv_ints(txt):
    out = {}
    for k, v in kv(txt).items():
        m = re.match(r'(\d+)', v)
        if m:
            out[k.rstrip(':')] = int(m.group(1))
    return out


def pool_stats(b):
    d = _kv_ints(b.send(BUILTIN_POOL, 6))
    if d:
        return {str(k): str(v) for k, v in d.items()}
    return kv(b.send('cat /sys/kernel/easystick/nommu_pool'))


def meminfo(b):
    # Regex, not kv(): a pending prompt or job notification can merge onto
    # the first output line ('~ # MemFree: 6688 kB'), which would lose the
    # key entirely under key-based parsing.
    out = {}
    for m in re.finditer(
            r'(MemFree|MemAvailable|Buffers|Cached|Slab|SReclaimable)'
            r':\s*(\d+)', b.send(BUILTIN_MEMINFO, 8)):
        out[m.group(1)] = int(m.group(2))
    return out


# --- alloc_fail attribution (#50 R2 item 1) ------------------------------
# The kernel logs 'nommu: Allocation of length N from process PID (comm)
# failed' at the nommu.c enomem path, reached only when BOTH the userspace
# pool and the buddy fallback failed -> every line is a real ENOMEM.
# Sides are resolved by PID ancestry in classify_pids() (ttyGS1 ctty or
# dropbear ancestor => user; init chain => system), never by name lists.
FAIL_RE = re.compile(
    r'Allocation of length (\d+) from process (\d+) \(([^)]+)\) failed')


def dmesg_len(b):
    t = b.send('dmesg | wc -l', 8)
    for ln in t.splitlines():
        if ln.strip().isdigit():
            return int(ln.strip())
    return 0


def fail_tally(b, start_line):
    """Tally alloc failures in dmesg lines > start_line.

    'Allocation of length N from process P (comm) failed' is printed at
    the nommu.c enomem path, which is reached only when BOTH the pool
    and the buddy fallback failed -> every line is a real ENOMEM (the
    mmap/exec actually failed).  Returns {pid: [comm, fails, max_len]}.
    """
    txt = b.send('dmesg | tail -n +%d' % (start_line + 1), 15)
    tally = {}
    for ln in txt.splitlines():
        m = FAIL_RE.search(ln)
        if m:
            e = tally.setdefault(int(m.group(2)), [m.group(3), 0, 0])
            e[1] += 1
            e[2] = max(e[2], int(m.group(1)))
    return tally


def classify_pids(b, pids):
    """Classify pids by ancestry via /proc/*/stat ppid chains: a task is
    'user' when its controlling tty is ttyGS1 or any ancestor comm is
    dropbear (same rule as the 0090 kernel mark); reaching init means
    'system'; an unreadable /proc/<pid>/stat means the process (or an
    ancestor) already exited -> 'exited'."""
    out = {}
    if not pids:
        return out
    # Field 7 after the comm ')' is task->flags; PF_EASYSTICK_SYSTEM
    # (0x40000000, #50 R4) wins over ancestry.  Leaf-only: the flag is
    # fork-inherited, and a per-connection dropbear deliberately clears it,
    # so an ancestor's flag must not re-mark that child.
    cmd = ("read dv < /sys/class/tty/ttyGS1/dev; maj=${dv%%:*}; "
           "min=${dv##*:}; "
           "TGS=$(( (maj << 8) | (min & 255) | ((min & ~255) << 12) )); "
           "for pid in %s; do p=$pid; cls=''; "
           "read -r s < /proc/$p/stat 2>/dev/null || cls=exited; "
           "if [ -z \"$cls\" ]; then r=${s##*) }; set -- $r; "
           "[ $(( $7 & 1073741824 )) -ne 0 ] && cls=sysflag; fi; "
           "while [ -z \"$cls\" ] && [ $p -gt 1 ]; do "
           "read -r s < /proc/$p/stat 2>/dev/null || { cls=exited; break; }; "
           "r=${s##*) }; set -- $r; c=${s#*\\(}; c=${c%%\\)*}; "
           "if [ \"$5\" = \"$TGS\" ] || [ \"$c\" = dropbear ]; then "
           "cls=user; break; fi; p=$2; done; "
           "[ -z \"$cls\" ] && cls=system; echo C$pid=$cls; done"
           % ' '.join(map(str, sorted(pids))))
    txt = b.send(cmd, 20)
    for ln in txt.splitlines():
        m = re.match(r'C(\d+)=(\w+)', ln.strip())
        if m:
            cls = 'system(flag)' if m.group(2) == 'sysflag' else m.group(2)
            out[int(m.group(1))] = cls
    return out


def print_attrib(tally, title, cls=None):
    print('### %s -- real ENOMEM attribution (dmesg)\n' % title)
    print('(each line = mmap/exec actually failed; the nommu print fires '
          'only when pool AND buddy both fail)\n')
    if not tally:
        print('(no allocation failures)\n')
        return
    if cls is None:
        cls = {}
    by = {}
    for pid, (comm, n, mx) in tally.items():
        by.setdefault((comm, cls.get(pid, '?')), [0, 0])
        e = by[(comm, cls.get(pid, '?'))]
        e[0] += n
        e[1] = max(e[1], mx)
    rows = [[c, s, n, mx]
            for (c, s), (n, mx) in sorted(by.items(), key=lambda kv: -kv[1][0])]
    print(md_table(['comm', 'side', 'fails', 'max len (B)'], rows))
    sys_n = usr_n = ext_n = 0
    for pid, (comm, n, _) in tally.items():
        s = cls.get(pid, '?')
        if s in ('system', 'system(flag)'):
            sys_n += n
        elif s == 'user':
            usr_n += n
        elif s == 'exited':
            ext_n += n
    print('\nsystem-side: %d   user-side: %d   exited-before-read: %d\n'
          % (sys_n, usr_n, ext_n))
    print('sides resolved by /proc ancestry plus PF_EASYSTICK_SYSTEM '
          '(flag => system; else ttyGS1 ctty or dropbear ancestor => user; '
          'init chain => system)\n')


def pid_set(b):
    txt = b.send('ls -d /proc/[0-9]*', 6)
    return frozenset(re.findall(r'/proc/(\d+)', txt))


def wait_settled(b, min_up=45, tries=10):
    """Idle snapshot must follow a stable process set (#50 R2 item 2):
    same pid set twice 10 s apart and uptime >= min_up."""
    for _ in range(tries):
        u = b.send('read u < /proc/uptime; echo U=$u', 5)
        m = re.search(r'U=(\d+)', u)
        up = int(m.group(1)) if m else 0
        s1 = pid_set(b)
        time.sleep(10)
        if pid_set(b) == s1 and up >= min_up:
            print('settled: uptime=%ds pids=%d\n' % (up, len(s1)))
            return
        time.sleep(3)
    print('settled: TIMEOUT (uptime=%ds pids=%d) -- snapshot may be unstable\n'
          % (up, len(s1)))


def pool_regions(b):
    """Parse 'NOMMU user mmap' dmesg lines -> [(base,size)] (hex ints)."""
    txt = b.send("dmesg | grep -i 'user mmap'", 15)
    regs = []
    for m in re.finditer(r'(\d+) KiB at (0x[0-9a-f]+)', txt):
        r = (int(m.group(2), 16), int(m.group(1)) * 1024)
        if r not in regs:
            regs.append(r)
    return regs


def proc_pool_table(b, regions):
    """Pull all /proc/*/maps over serial; sum per-pid bytes inside regions.
    Returns {pid: (name, bytes)} sorted by bytes desc."""
    raw = b.send('for p in /proc/[0-9]*; do echo "P ${p#/proc/} $(cat $p/comm)"; '
                 'cat $p/maps; done', 60)
    per_pid, cur = {}, None
    rng = re.compile(r'^([0-9a-f]+)-([0-9a-f]+)')
    for ln in raw.splitlines():
        if ln.startswith('P '):
            parts = ln.split()
            if len(parts) >= 3:
                cur = parts[1]
                per_pid[cur] = [parts[2], 0]
            continue
        m = rng.match(ln.strip())
        if m and cur:
            lo, hi = int(m.group(1), 16), int(m.group(2), 16)
            if any(r0 <= lo < r0 + sz for r0, sz in regions):
                per_pid[cur][1] += hi - lo
    return {p: tuple(v) for p, v in sorted(
        per_pid.items(), key=lambda kv: -kv[1][1]) if v[1] > 0}


def board_ip(b):
    txt = b.send('ip -4 addr show wlan0')
    m = re.search(r'inet (\d+\.\d+\.\d+\.\d+)', txt)
    return m.group(1) if m else None


def ssh_cmd(host, remote, key=None):
    # dropbear regenerates its host key every boot (/run is volatile), so
    # never consult a known_hosts file -- 'no' accepts whatever is present.
    c = ['ssh', '-o', 'BatchMode=yes', '-o', 'StrictHostKeyChecking=no',
         '-o', 'UserKnownHostsFile=/dev/null', '-o', 'ConnectTimeout=10']
    if key:
        c += ['-i', key]
    return c + ['root@' + host, remote]


def _ssh_probe(host, priv):
    """The lab network closes the TCP flow ~1 s in, so a successful exec
    still exits 255.  Judge by the marker output instead."""
    try:
        r = subprocess.run(ssh_cmd(host, 'echo SSHPROBE_OK', priv),
                           timeout=15, capture_output=True, text=True)
        return 'SSHPROBE_OK' in (r.stdout or '')
    except Exception:
        return False


def ensure_ssh(b, host, pubkey):
    """Make key auth work; installs pubkey over serial if needed."""
    priv = pubkey[:-4] if pubkey.endswith('.pub') else None
    if _ssh_probe(host, priv):
        return True
    key_txt = open(pubkey).read().strip()
    b.send('mkdir -p /root/.ssh')
    b.send("grep -q '%s' /root/.ssh/authorized_keys 2>/dev/null || "
           "echo '%s' >> /root/.ssh/authorized_keys"
           % (key_txt[:32], key_txt))
    return _ssh_probe(host, priv)


LBKEY = '/tmp/.lbk'


def setup_loopback_key(b):
    """Throwaway ed25519 keypair on the board for dbclient->127.0.0.1.
    Stays in /tmp (RAM), never leaves the board, regenerated per boot."""
    if 'lbk' not in b.send('ls %s 2>/dev/null' % LBKEY):
        b.send('dropbearkey -t ed25519 -f %s >/dev/null 2>&1' % LBKEY, 30)
        pub = b.send('dropbearkey -y -f %s | grep ^ssh-' % LBKEY, 10)
        key = pub.strip().splitlines()[-1] if pub.strip() else ''
        if not key.startswith('ssh-'):
            return False
        b.send('mkdir -p /root/.ssh')
        b.send("grep -q '%s' /root/.ssh/authorized_keys 2>/dev/null || "
               "echo '%s' >> /root/.ssh/authorized_keys" % (key[:40], key))
    return True


def proc_count(b, pat):
    """Count /proc/*/cmdline matches via builtins only (works when execs
    fail under pool exhaustion).  pat is a shell case glob."""
    txt = b.send('i=0; for p in /proc/[0-9]*/cmdline; do '
                 'read -r c < $p 2>/dev/null; case "$c" in %s) '
                 'i=$((i+1));; esac; done; echo N=$i' % pat, 15)
    for ln in txt.splitlines():
        if ln.startswith('N='):
            return int(ln[2:])
    return 0


def dropbear_count(b):
    """dropbear -i children on the board."""
    return proc_count(b, '*dropbear*-i*')


def open_sessions(b, host, n, key=None, loopback=False):
    """Fire n `sleep 90` SSH sessions; return (procs, opened_count).

    loopback: dbclient on the board to 127.0.0.1 -- inetd forks the same
    dropbear -i children with the same pool cost, no LAN needed.
    LAN: the client TCP flow is closed externally ~1 s in on this lab
    network, but the server-side dropbear keeps the session (and its
    ~1 MB of pool) for the remote command's lifetime -- the board-side
    count is the source of truth either way."""
    base = dropbear_count(b)
    procs = []
    if loopback:
        b.spawn('i=0; while [ $i -lt %d ]; do '
                "dbclient -i %s -y root@127.0.0.1 'sleep 90' "
                '>/dev/null 2>&1 & i=$((i+1)); done' % (n, LBKEY))
    else:
        for _ in range(n):
            try:
                procs.append(subprocess.Popen(
                    ssh_cmd(host, 'sleep 90', key),
                    stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL))
            except Exception:
                break
            time.sleep(0.5)
    time.sleep(6)
    return procs, dropbear_count(b) - base


def close_sessions(b, procs):
    for p in procs:
        try:
            p.terminate()
        except Exception:
            pass
    for p in procs:
        try:
            p.wait(5)
        except Exception:
            pass
    # server-side dropbears outlive the client flow; kill them and wait for
    # the pool to drain so the next scenario starts clean
    if b is not None:
        b.send('for p in $(ls /proc | grep "^[0-9]"); do '
               'grep -q "dropbear -i" /proc/$p/cmdline 2>/dev/null && '
               'kill $p; done', 15)
        end = time.time() + 120
        while time.time() < end and dropbear_count(b):
            time.sleep(5)


def md_table(headers, rows):
    out = ['| ' + ' | '.join(headers) + ' |',
           '|' + '|'.join(['---'] * len(headers)) + '|']
    out += ['| ' + ' | '.join(str(c) for c in r) + ' |' for r in rows]
    return '\n'.join(out)


PIPELINE = "cat /etc/inittab | grep -v '^$' | sort | head -6 | wc -l"


def scenario_idle(b, regions):
    st, mi = pool_stats(b), meminfo(b)
    print('### idle snapshot\n')
    print(md_table(['key', 'value'],
                   [[k, st.get(k, '-')] for k in
                    ('total', 'used', 'largest_free', 'alloc_fail', 'fallback')]))
    print()
    print(md_table(['meminfo (kB)', 'value'],
                   [[k, mi.get(k, '-')] for k in
                    ('MemFree', 'MemAvailable', 'Slab', 'SReclaimable')]))
    print()
    tbl = proc_pool_table(b, regions)
    print(md_table(['pid', 'name', 'pool bytes'],
                   [[p, n, bts] for p, (n, bts) in tbl.items()]))
    print()
    return st, mi


def scenario_wget(b, url, loopback=False):
    if loopback:
        # no LAN: serve 10MB of zeros over loopback HTTP.  loopback does
        # not exercise the SDIO/esp-hosted path -- kernel TCP + userspace
        # only -- noted in the record.
        b.spawn("nc -l -p 8080 -e sh -c 'printf \"HTTP/1.0 200 OK\\\\r\\\\n"
                "Content-Length: 10485760\\\\r\\\\n\\\\r\\\\n\"; "
                "dd if=/dev/zero bs=1048576 count=10 2>/dev/null' &")
        time.sleep(1)
        url = 'http://127.0.0.1:8080/'
    before = pool_stats(b)
    b.spawn('wget -q -O /dev/null %s &' % url)
    lo = {'MemFree': 1 << 30, 'MemAvailable': 1 << 30}
    hi = {}
    done = False
    for _ in range(90):
        time.sleep(2)
        mi = meminfo(b)
        for k in lo:
            if mi.get(k):
                lo[k] = min(lo[k], mi[k])
        for k in ('Slab',):
            hi[k] = max(hi.get(k, 0), mi.get(k, 0))
        ps = b.send('ps | grep -c "[w]get -q"')
        if '0' in ps.split():
            done = True
            break
    after = pool_stats(b)
    print('### wget 10MB -> /dev/null (meminfo sampled every 2 s)\n')
    print(md_table(['metric', 'value'],
                   [['MemFree min', lo['MemFree']],
                    ['MemAvailable min', lo['MemAvailable']],
                    ['Slab max', hi.get('Slab', '-')],
                    ['alloc_fail delta',
                     int(after.get('alloc_fail', 0)) - int(before.get('alloc_fail', 0))],
                    ['largest_free after', after.get('largest_free', '-')],
                    ['completed', done]]))
    print()


def scenario_ssh(b, host, n, key, loopback=False):
    dl = dmesg_len(b)
    procs, opened = open_sessions(b, host, n, key, loopback)
    time.sleep(3)
    st, mi = pool_stats(b), meminfo(b)
    print('### %d SSH sessions held (of %d attempted)\n' % (opened, n))
    print(md_table(['key', 'value'],
                   [['transport', 'loopback dbclient' if loopback else 'lan ssh'],
                    ['dropbear sessions on board', dropbear_count(b)],
                    ['dbclient procs (loopback only)',
                     proc_count(b, '*dbclient*') if loopback else 0],
                    ['used', st.get('used', '-')],
                    ['largest_free', st.get('largest_free', '-')],
                    ['alloc_fail', st.get('alloc_fail', '-')],
                    ['fallback', st.get('fallback', '-')],
                    ['MemFree', str(mi.get('MemFree', '-')) + ' kB'],
                    ['MemAvailable', str(mi.get('MemAvailable', '-')) + ' kB']]))
    print()
    tally = fail_tally(b, dl)
    print_attrib(tally, 'ssh-open window', classify_pids(b, tally))
    return procs, opened


def net_up(b):
    # builtin-only: `ip` cannot exec under pool pressure.  This board rests
    # at operstate `dormant` (no STA link), so recovered = left `down`.
    s = b.send('read s < /sys/class/net/wlan0/operstate; echo S=$s', 4)
    return 'S=up' in s or 'S=dormant' in s


def scenario_a(b, host, n, key, loopback=False, trigger='user'):
    # trigger='user' (console ancestry, the R3 scenario) vs 'system': the
    # restart request goes through /run/.es-netreq, which init-ancestry
    # easystick-bootsync picks up -- the same classification a real
    # config-import apply gets, and no flash writes (#50 round 4).
    if trigger == 'system':
        def fire():
            b.spawn('echo restart > /run/.es-netreq')
    else:
        def fire():
            b.spawn('/etc/init.d/S40network restart &')
    dl = dmesg_len(b)
    before = pool_stats(b)
    fail0 = int(before.get('alloc_fail', 0))
    denied0 = int(before.get('user_denied', 0))
    sysfail0 = int(before.get('system_alloc_fail', 0))
    lf_min = int(before.get('largest_free', 0))
    mem_lo = {'MemFree': 10**9, 'MemAvailable': 10**9}
    procs, opened = open_sessions(b, host, n, key, loopback)
    # pipelines run concurrently with the restarts: backgrounded on the
    # console shell (a small extra shell is part of the scenario load).
    b.spawn('i=0; while [ $i -lt 10 ]; do %s; i=$((i+1)); done &' % PIPELINE)
    recov = []
    for rnd in range(5):
        fire()
        t0 = time.time()
        time.sleep(4)  # skip the not-yet-down window (~2 s dip seen on board)
        ok = False
        while time.time() - t0 < 90:
            st = pool_stats(b)
            lf_min = min(lf_min, int(st.get('largest_free', lf_min)))
            mi = meminfo(b)
            for k in mem_lo:
                if mi.get(k):
                    mem_lo[k] = min(mem_lo[k], mi[k])
            if net_up(b):
                ok = True
                break
            time.sleep(2)
        recov.append(round(time.time() - t0, 1) if ok else 'TIMEOUT')
    b.send('wait', 30)  # let the pipeline loop drain
    after = pool_stats(b)
    live = dropbear_count(b)
    delta = int(after.get('alloc_fail', fail0)) - fail0
    if delta < 0:  # counter resets if the value read mid-restart was lost
        delta = 'reset mid-run (before=%d after=%s)' % (
            fail0, after.get('alloc_fail', '?'))
    rows = [['recovery times (s)', ', '.join(map(str, recov))],
            ['pool-miss counter delta (alloc_fail)', delta],
            ['largest_free min', lf_min],
            ['largest_free after', after.get('largest_free', '-')],
            ['MemFree min', mem_lo['MemFree']
             if mem_lo['MemFree'] < 10**9 else '-'],
            ['MemAvailable min', mem_lo['MemAvailable']
             if mem_lo['MemAvailable'] < 10**9 else '-'],
            ['ssh sessions still alive', live],
            ['wlan up after', net_up(b)]]
    if 'user_denied' in after or 'system_alloc_fail' in after:
        rows.insert(2, ['user_denied delta',
                        int(after.get('user_denied', denied0)) - denied0])
        rows.insert(3, ['system_alloc_fail delta',
                        int(after.get('system_alloc_fail', sysfail0))
                        - sysfail0])
    print('### scenario A-%s: %d SSH + 10 pipelines + 5 restarts\n'
          % (trigger, opened))
    print(md_table(['metric', 'value'], rows))
    print()
    tally = fail_tally(b, dl)
    print_attrib(tally, 'scenario A window', classify_pids(b, tally))
    close_sessions(b, procs)


# Board-side samplers for scenario_syspeak (#50 round 5): builtin-only ash
# loops, so they cost ~0 pool and keep sampling while execs are pressured.
# Each line is uploaded via `echo 'line' >> /tmp/...` (no single quotes or
# backslashes inside, so serial echo is faithful).  The used sampler polls
# nommu_pool at ~12 Hz; pacing is a /proc/uptime centisecond busy-wait
# (busybox `read -t` has no fractions and `usleep` would exec a ~0.5 MB
# busybox per call, polluting `used` itself).  The pid sampler re-does the
# maps math on-board (busybox ash does 0x arithmetic) at ~2-4 s per sweep.
USED_SAMPLER = [
    'tick() {',
    'read u < /proc/uptime',
    'u=${u%% *}',
    'd=${u%.*}${u#*.}',
    'e=$((d + $1))',
    'while :; do',
    'read u < /proc/uptime',
    'u=${u%% *}',
    'w=${u%.*}${u#*.}',
    '[ $w -ge $e ] && break',
    'done',
    '}',
    'n=0',
    'while [ $n -lt 650 ]; do',
    'exec 3< /sys/kernel/easystick/nommu_pool',
    'read a <&3',
    'read b <&3',
    'read c <&3',
    'exec 3<&-',
    'echo "U $n ${b#used } ${c#largest_free }"',
    'n=$((n+1))',
    'tick 8',
    'done',
]
# lo bound of the first pool region (0x495af000) -- `49*-*` prefix-matches
# both regions, the compare drops the 0x49000000-0x495af000 tail.
PID_SAMPLER = [
    'n=0',
    'while [ $n -lt 18 ]; do',
    'echo "S $n"',
    'for p in /proc/[0-9]*; do',
    'c=x',
    'read -r c < $p/comm 2>/dev/null || continue',
    'f=0',
    'if read -r st < $p/stat 2>/dev/null; then',
    'r=${st##*) }',
    'set -- $r',
    'f=$7',
    'fi',
    's=0',
    'while read -r a b; do',
    'case $a in',
    '49*-*) lo=${a%%-*}',
    '[ $((0x$lo)) -ge 1230688256 ] && s=$((s + 0x${a#*-} - 0x$lo)) ;;',
    'esac',
    'done < $p/maps',
    '[ $s -gt 0 ] && echo "${p#/proc/} $f $c $s"',
    'done',
    'n=$((n+1))',
    'done',
]


def _upload_sampler(b, name, lines):
    b.send('rm -f /tmp/%s.sh' % name)
    for ln in lines:
        b.send("echo '%s' >> /tmp/%s.sh" % (ln, name), 6)


def scenario_syspeak(b, regions):
    """System transient pool demand at the restart peak (#50 round 5).

    A-system trigger (netreq -> bootsync -> S40network, init ancestry) with
    NO SSH load.  Serial proc_pool_table takes ~60 s per snapshot -- far too
    slow for the restart peak -- so two builtin-only samplers run on the
    board instead: phase A polls `used`/`largest_free` at ~10 Hz through one
    restart; phase B sweeps per-pid pool bytes through a second restart and
    tags each pid's PF_EASYSTICK_SYSTEM flag.  A flagged pid absent from the
    settled baseline (or grown since) is transient demand the reserve must
    hold free for; unflagged transients (this sampler's own shell) are
    reported separately."""
    def fire():
        b.spawn('echo restart > /run/.es-netreq')
    base_st = pool_stats(b)
    base_tbl = proc_pool_table(b, regions)
    print('### syspeak: baseline (settled, no SSH)\n')
    print(md_table(['key', 'value'],
                   [[k, base_st.get(k, '-')] for k in
                    ('used', 'largest_free', 'alloc_fail')]))
    print()
    print(md_table(['pid', 'name', 'pool bytes'],
                   [[p, n, bts] for p, (n, bts) in base_tbl.items()]))
    print()
    _upload_sampler(b, 'u', USED_SAMPLER)
    _upload_sampler(b, 'p', PID_SAMPLER)
    dl = dmesg_len(b)
    b.spawn('sh /tmp/u.sh > /tmp/u.log 2>&1 &')
    time.sleep(1)
    fire()
    # poll until the sampler finishes (last sweep) or output plateaus
    ulog, last_n = '', -2
    for _ in range(24):
        time.sleep(10)
        ulog = b.send('cat /tmp/u.log', 40)
        n = ulog.count('\nU ')
        if 'U 649' in ulog or (n == last_n and n > 50):
            break
        last_n = n
    series = []
    for ln in ulog.splitlines():
        m = re.match(r'U (\d+) (\d+) (\d+)', ln.strip())
        if m:
            series.append((int(m.group(1)), int(m.group(2)), int(m.group(3))))
    peak = max(series, key=lambda s: s[1]) if series else (0, 0, 0)
    floor = min(series, key=lambda s: s[1]) if series else (0, 0, 0)
    # compress: one row per value run (sweep range) so the log stays small
    runs = []
    for i, u, lf in series:
        if runs and runs[-1][2] == u and runs[-1][3] == lf:
            runs[-1][1] = i
        else:
            runs.append([i, i, u, lf])
    print('### syspeak phase A: `used` timeline (~12 Hz, 1st restart)\n')
    print(md_table(['sweep first', 'sweep last', 'used (B)',
                    'largest_free (B)'], runs))
    print('\npeak used: %d B at sweep %d; min used: %d B at sweep %d; '
          'baseline used %d B\n'
          % (peak[1], peak[0], floor[1], floor[0],
             int(base_st.get('used', 0))))
    # settle before the second restart
    end = time.time() + 90
    while time.time() < end and not net_up(b):
        time.sleep(3)
    time.sleep(8)
    b.spawn('sh /tmp/p.sh > /tmp/p.log 2>&1 &')
    time.sleep(1)
    fire()
    plog, last_n = '', -2
    for _ in range(24):
        time.sleep(10)
        plog = b.send('cat /tmp/p.log', 60)
        n = len(re.findall(r'(?m)^S \d+', plog))
        if 'S 17' in plog or (n == last_n and n > 4):
            break
        last_n = n
    sweeps, cur = [], None
    for ln in plog.splitlines():
        m = re.match(r'S (\d+)', ln.strip())
        if m:
            cur = []
            sweeps.append(cur)
            continue
        m = re.match(r'(\d+) (\d+) (\S+) (\d+)', ln.strip())
        if m and cur is not None:
            cur.append((m.group(1), int(m.group(2)), m.group(3),
                        int(m.group(4))))
    rows = []
    for i, sw in enumerate(sweeps):
        systr = usrtr = 0
        det = []
        for pid, fl, comm, bts in sw:
            base = base_tbl.get(pid)
            delta = bts if (base is None or base[0] != comm) \
                else max(0, bts - base[1])
            if not delta:
                continue
            if fl & 0x40000000:
                systr += delta
                det.append('%s(%d)' % (comm, delta))
            else:
                usrtr += delta
        rows.append((i, len(sw), systr, usrtr, ' '.join(det)))
    pk = max(range(len(rows)), key=lambda i: rows[i][2]) if rows else -1
    print('### syspeak phase B: per-pid sweeps (2nd restart)\n')
    print(md_table(['sweep', 'pids seen', 'flagged transient (B)',
                    'unflagged (B)', 'flagged transients'],
                   [[r[0], r[1], r[2], r[3], r[4]] for r in rows]))
    if pk >= 0:
        print('\npeak flagged transient: %d B at sweep %d '
              '-- table at that sweep:\n' % (rows[pk][2], pk))
        out = []
        for pid, fl, comm, bts in sweeps[pk]:
            base = base_tbl.get(pid)
            vs = 'new' if base is None or base[0] != comm \
                else '%+d' % (bts - base[1])
            out.append([pid, comm, '0x%x' % fl, bts, vs])
        print(md_table(['pid', 'name', 'flags', 'pool bytes',
                        'vs baseline'], out))
        print()
    tally = fail_tally(b, dl)
    print_attrib(tally, 'syspeak window (both restarts)',
                 classify_pids(b, tally))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--port', default='COM10')
    ap.add_argument('--scenario', default='all',
                    choices=['all', 'idle', 'wget', 'ssh', 'scenarioA',
                             'syspeak', 'tables'])
    ap.add_argument('--n', type=int, default=6)
    ap.add_argument('--key', default='~/.ssh/id_ed25519',
                    help='ssh private key (pub inferred)')
    ap.add_argument('--label', default='unnamed')
    ap.add_argument('--wget-url', default='http://speedtest.tele2.net/10MB.zip')
    ap.add_argument('--transport', default='auto',
                    choices=['auto', 'lan', 'loopback'],
                    help='SSH session transport; auto falls back to loopback '
                         'dbclient when the board has no LAN route')
    ap.add_argument('--a-trigger', default='user',
                    choices=['user', 'system'],
                    help='scenario A restart origin: console shell (user '
                         'ancestry) or bootsync netreq file (init ancestry)')
    ap.add_argument('--rcontig', type=int, default=None,
                    help='write /sys/kernel/easystick/user_reserve_contig '
                         '(bytes) before the scenarios (0090 images only)')
    ap.add_argument('--rtotal', type=int, default=None,
                    help='write /sys/kernel/easystick/user_reserve_total '
                         '(bytes) before the scenarios (0090 images only)')
    a = ap.parse_args()

    if a.key:
        a.key = os.path.expanduser(a.key)
    pub = (a.key + '.pub') if a.key else None

    print('<!-- harness label: %s port:%s scenario:%s -->\n'
          % (a.label, a.port, a.scenario))
    b = Board(a.port)
    regions = pool_regions(b)
    print('regions: ' + ', '.join('0x%x+%dKiB' % (r, s // 1024)
                                for r, s in regions) + '\n')

    # pre-clean: kill leftover test sessions so baselines are comparable
    b.send('for p in $(ls /proc | grep "^[0-9]"); do '
           'grep -qE "dropbear -i|dbclient" /proc/$p/cmdline 2>/dev/null && '
           'kill $p; done', 20)
    time.sleep(3)

    if a.rcontig is not None:
        r = b.send('echo %d > /sys/kernel/easystick/user_reserve_contig '
                   '2>/dev/null; read v < '
                   '/sys/kernel/easystick/user_reserve_contig; echo RC=$v'
                   % a.rcontig, 6)
        m = re.search(r'RC=(\d+)', r)
        print('user_reserve_contig := %s\n'
              % (m.group(1) if m else 'UNSUPPORTED'))
    if a.rtotal is not None:
        r = b.send('echo %d > /sys/kernel/easystick/user_reserve_total '
                   '2>/dev/null; read v < '
                   '/sys/kernel/easystick/user_reserve_total; echo RT=$v'
                   % a.rtotal, 6)
        m = re.search(r'RT=(\d+)', r)
        print('user_reserve_total := %s\n'
              % (m.group(1) if m else 'UNSUPPORTED'))

    host = board_ip(b)
    need_ssh = a.scenario in ('all', 'ssh', 'scenarioA')
    loopback = False
    if need_ssh:
        if a.transport == 'loopback':
            loopback = True
        elif a.transport == 'lan':
            loopback = False
        elif not host or not pub or not _ssh_probe(host, a.key):
            loopback = True
        if loopback:
            if not setup_loopback_key(b):
                sys.exit('loopback key setup failed')
            print('transport: loopback dbclient (no LAN path)\n')
        else:
            ok = ensure_ssh(b, host, pub)
            print('transport: lan ssh (%s)\n' % ('ok' if ok else 'FAILED'))
            if not ok:
                sys.exit('ssh unavailable')

    if a.scenario in ('all', 'idle', 'tables'):
        wait_settled(b)
        scenario_idle(b, regions)
    if a.scenario in ('all', 'wget'):
        scenario_wget(b, a.wget_url, loopback)
    if a.scenario in ('all', 'ssh'):
        procs, _ = scenario_ssh(b, host, a.n, a.key, loopback)
        close_sessions(b, procs)
        time.sleep(2)
        st = pool_stats(b)
        print('post-ssh: used=%s largest_free=%s\n'
              % (st.get('used'), st.get('largest_free')))
    if a.scenario in ('all', 'scenarioA'):
        scenario_a(b, host, a.n, a.key, loopback, trigger=a.a_trigger)
    if a.scenario == 'syspeak':
        scenario_syspeak(b, regions)
    if a.scenario == 'tables':
        pass
    b.close()


if __name__ == '__main__':
    main()
