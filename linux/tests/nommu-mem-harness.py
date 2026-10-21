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
    return _kv_ints(b.send(BUILTIN_MEMINFO, 8))


# --- alloc_fail attribution (#50 R2 item 1) ------------------------------
# The kernel logs 'nommu: Allocation of length N from process PID (comm)
# failed' on every pool failure; the following binfmt_flat line carries no
# comm, so attribution is by the nommu line.  Sides per the #50 review:
# user = dropbear/dbclient sessions, their children, the console shell and
# the harness's own probes; everything else is system.
FAIL_RE = re.compile(
    r'Allocation of length (\d+) from process (\d+) \(([^)]+)\) failed')
SYSTEM_COMMS = frozenset((
    'wpa_supplicant', 'udhcpc', 'udhcpd', 'hostapd', 'inetd', 'init',
    'syslogd', 'klogd', 'easystick-boots', 'easystick-mdns',
    'easystick-respa', 'easystick-watch', 'S40network', 'S05easyst',
    'bootsync', 'swclock', 'ntpd', 'easystick-bootsync',
    'easystick-boot-commit', 'mdev', 'crond', 'getty'))
USER_COMMS = frozenset((
    'dropbear', 'dbclient', 'sleep', 'sort', 'head', 'wc', 'cat', 'grep',
    'cut', 'tail', 'nc', 'dd', 'wget', 'ssh', 'sh', 'printf', 'tr', 'awk',
    'sed', 'ls', 'basename', 'date', 'ip', 'ps', 'kill', 'ash'))


def dmesg_len(b):
    t = b.send('dmesg | wc -l', 8)
    for ln in t.splitlines():
        if ln.strip().isdigit():
            return int(ln.strip())
    return 0


def fail_tally(b, start_line):
    """Tally pool alloc failures per comm in dmesg lines > start_line."""
    txt = b.send('dmesg | tail -n +%d' % (start_line + 1), 15)
    tally = {}
    for ln in txt.splitlines():
        m = FAIL_RE.search(ln)
        if m:
            e = tally.setdefault(m.group(3), [0, 0])
            e[0] += 1
            e[1] = max(e[1], int(m.group(1)))
    return tally


def side_of(comm):
    if comm in SYSTEM_COMMS:
        return 'system'
    if comm in USER_COMMS:
        return 'user*'
    return '?'


def print_attrib(tally, title):
    print('### %s -- alloc_fail attribution (dmesg)\n' % title)
    if not tally:
        print('(no pool allocation failures)\n')
        return
    rows = [[c, side_of(c), n, mx]
            for c, (n, mx) in sorted(tally.items(), key=lambda kv: -kv[1][0])]
    print(md_table(['comm', 'side', 'fails', 'max len (B)'], rows))
    sys_n = sum(n for c, (n, _) in tally.items() if side_of(c) == 'system')
    usr_n = sum(n for c, (n, _) in tally.items() if side_of(c) == 'user*')
    print('\nsystem-side: %d   user-side: %d   unclassified: %d\n'
          % (sys_n, usr_n, sum(n for c, (n, _) in tally.items())
             - sys_n - usr_n))
    print('*sh counts as user (console/session shells); S40network scripts '
          'also exec as sh -- see comm table\n')


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
    print(md_table(['metric', 'min kB' if 'Mem' in 'x' else 'value'],
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
    print_attrib(fail_tally(b, dl), 'ssh-open window')
    return procs, opened


def net_up(b):
    # builtin-only: `ip` cannot exec under pool pressure.  This board rests
    # at operstate `dormant` (no STA link), so recovered = left `down`.
    s = b.send('read s < /sys/class/net/wlan0/operstate; echo S=$s', 4)
    return 'S=up' in s or 'S=dormant' in s


def scenario_a(b, host, n, key, loopback=False):
    dl = dmesg_len(b)
    before = pool_stats(b)
    fail0 = int(before.get('alloc_fail', 0))
    lf_min = int(before.get('largest_free', 0))
    procs, opened = open_sessions(b, host, n, key, loopback)
    # pipelines run concurrently with the restarts: backgrounded on the
    # console shell (a small extra shell is part of the scenario load).
    b.spawn('i=0; while [ $i -lt 10 ]; do %s; i=$((i+1)); done &' % PIPELINE)
    recov = []
    for rnd in range(5):
        b.spawn('/etc/init.d/S40network restart &')
        t0 = time.time()
        time.sleep(4)  # skip the not-yet-down window (~2 s dip seen on board)
        ok = False
        while time.time() - t0 < 90:
            st = pool_stats(b)
            lf_min = min(lf_min, int(st.get('largest_free', lf_min)))
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
    print('### scenario A: %d SSH + 10 pipelines + 5 restarts\n' % opened)
    print(md_table(['metric', 'value'],
                   [['recovery times (s)', ', '.join(map(str, recov))],
                    ['alloc_fail delta', delta],
                    ['largest_free min', lf_min],
                    ['largest_free after', after.get('largest_free', '-')],
                    ['ssh sessions still alive', live],
                    ['wlan up after', net_up(b)]]))
    print()
    print_attrib(fail_tally(b, dl), 'scenario A window')
    close_sessions(b, procs)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--port', default='COM10')
    ap.add_argument('--scenario', default='all',
                    choices=['all', 'idle', 'wget', 'ssh', 'scenarioA', 'tables'])
    ap.add_argument('--n', type=int, default=6)
    ap.add_argument('--key', default='~/.ssh/id_ed25519',
                    help='ssh private key (pub inferred)')
    ap.add_argument('--label', default='unnamed')
    ap.add_argument('--wget-url', default='http://speedtest.tele2.net/10MB.zip')
    ap.add_argument('--transport', default='auto',
                    choices=['auto', 'lan', 'loopback'],
                    help='SSH session transport; auto falls back to loopback '
                         'dbclient when the board has no LAN route')
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
        scenario_a(b, host, a.n, a.key, loopback)
    if a.scenario == 'tables':
        pass
    b.close()


if __name__ == '__main__':
    main()
