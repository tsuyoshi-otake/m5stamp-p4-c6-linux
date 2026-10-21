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


def pool_stats(b):
    return kv(b.send('cat /sys/kernel/easystick/nommu_pool'))


def meminfo(b):
    txt = b.send("grep -E 'MemFree|MemAvailable|Buffers|Cached|Slab|SReclaim' /proc/meminfo")
    out = {}
    for k, v in kv(txt).items():
        m = re.match(r'(\d+)', v)
        if m:
            out[k.rstrip(':')] = int(m.group(1))
    return out


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
    c = ['ssh', '-o', 'BatchMode=yes', '-o', 'StrictHostKeyChecking=accept-new',
         '-o', 'ConnectTimeout=10', '-o', 'UserKnownHostsFile=NUL' if sys.platform == 'win32' else '/dev/null']
    if key:
        c += ['-i', key]
    return c + ['root@' + host, remote]


def ensure_ssh(b, host, pubkey):
    """Make key auth work; installs pubkey over serial if needed."""
    priv = pubkey[:-4] if pubkey.endswith('.pub') else None
    try:
        subprocess.run(ssh_cmd(host, 'true', priv), timeout=15,
                       capture_output=True, check=True)
        return True
    except Exception:
        pass
    key_txt = open(pubkey).read().strip()
    b.send('mkdir -p /root/.ssh')
    b.send("echo '%s' >> /root/.ssh/authorized_keys" % key_txt)
    try:
        subprocess.run(ssh_cmd(host, 'true', priv), timeout=15,
                       capture_output=True, check=True)
        return True
    except Exception:
        return False


def open_sessions(host, n, key=None):
    """Open n `ssh ... sleep 600` sessions; return (procs, opened_count)."""
    procs = []
    for _ in range(n):
        try:
            p = subprocess.Popen(ssh_cmd(host, 'sleep 600', key),
                                 stdout=subprocess.DEVNULL,
                                 stderr=subprocess.DEVNULL)
            procs.append(p)
        except Exception:
            break
    time.sleep(4)
    return procs, sum(1 for p in procs if p.poll() is None)


def close_sessions(procs):
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


def scenario_wget(b, url):
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


def scenario_ssh(b, host, n, key):
    procs, opened = open_sessions(host, n, key)
    time.sleep(3)
    st, mi = pool_stats(b), meminfo(b)
    dbs = b.send("ps | grep -c 'dropbear -i'").strip().splitlines()
    print('### %d SSH sessions held (of %d attempted)\n' % (opened, n))
    print(md_table(['key', 'value'],
                   [['dropbear sessions on board', dbs[-1] if dbs else '-'],
                    ['used', st.get('used', '-')],
                    ['largest_free', st.get('largest_free', '-')],
                    ['alloc_fail', st.get('alloc_fail', '-')],
                    ['fallback', st.get('fallback', '-')],
                    ['MemFree', str(mi.get('MemFree', '-')) + ' kB'],
                    ['MemAvailable', str(mi.get('MemAvailable', '-')) + ' kB']]))
    print()
    return procs, opened


def net_up(b):
    return ' inet ' in b.send('ip -4 addr show wlan0', 4)


def scenario_a(b, host, n, key):
    before = pool_stats(b)
    fail0 = int(before.get('alloc_fail', 0))
    lf_min = int(before.get('largest_free', 0))
    procs, opened = open_sessions(host, n, key)
    # pipelines run concurrently with the restarts: backgrounded on the
    # console shell (a small extra shell is part of the scenario load).
    b.spawn('i=0; while [ $i -lt 10 ]; do %s; i=$((i+1)); done &' % PIPELINE)
    recov = []
    for rnd in range(5):
        b.spawn('/etc/init.d/S40network restart &')
        t0 = time.time()
        ok = False
        while time.time() - t0 < 90:
            time.sleep(5)
            st = pool_stats(b)
            lf_min = min(lf_min, int(st.get('largest_free', lf_min)))
            if net_up(b):
                ok = True
                break
        recov.append(round(time.time() - t0, 1) if ok else 'TIMEOUT')
    b.send('wait', 30)  # let the pipeline loop drain
    after = pool_stats(b)
    live = sum(1 for p in procs if p.poll() is None)
    print('### scenario A: %d SSH + 10 pipelines + 5 restarts\n' % opened)
    print(md_table(['metric', 'value'],
                   [['recovery times (s)', ', '.join(map(str, recov))],
                    ['alloc_fail delta', int(after.get('alloc_fail', 0)) - fail0],
                    ['largest_free min', lf_min],
                    ['largest_free after', after.get('largest_free', '-')],
                    ['ssh sessions still alive', live],
                    ['wlan up after', net_up(b)]]))
    print()
    close_sessions(procs)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--port', default='COM10')
    ap.add_argument('--scenario', default='all',
                    choices=['all', 'idle', 'wget', 'ssh', 'scenarioA', 'tables'])
    ap.add_argument('--n', type=int, default=6)
    ap.add_argument('--key', default=None, help='ssh private key (pub inferred)')
    ap.add_argument('--label', default='unnamed')
    ap.add_argument('--wget-url', default='http://speedtest.tele2.net/10MB.zip')
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

    host = board_ip(b)
    need_ssh = a.scenario in ('all', 'ssh', 'scenarioA')
    if need_ssh:
        if not host:
            sys.exit('no wlan0 IPv4 on board')
        if pub:
            ok = ensure_ssh(b, host, pub)
            print('ssh key auth: %s\n' % ('ok' if ok else 'FAILED'))
            if not ok:
                sys.exit('ssh unavailable')

    if a.scenario in ('all', 'idle', 'tables'):
        scenario_idle(b, regions)
    if a.scenario in ('all', 'wget'):
        scenario_wget(b, a.wget_url)
    if a.scenario in ('all', 'ssh'):
        procs, _ = scenario_ssh(b, host, a.n, a.key)
        close_sessions(procs)
        time.sleep(2)
        st = pool_stats(b)
        print('post-ssh: used=%s largest_free=%s\n'
              % (st.get('used'), st.get('largest_free')))
    if a.scenario in ('all', 'scenarioA'):
        scenario_a(b, host, a.n, a.key)
    if a.scenario == 'tables':
        pass
    b.close()


if __name__ == '__main__':
    main()
