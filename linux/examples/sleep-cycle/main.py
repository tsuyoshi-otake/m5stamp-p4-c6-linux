# Deep Sleep cycle for measuring the current (#56): wake, wait for Wi-Fi
# (the C6 joins the network and DHCP gives an address), then sleep again.
#
# Copy this file to the STAMP_P4 drive as main.py.  Autorun starts it at
# every boot, as user m5.  Each cycle is about SLEEP_S asleep plus the boot
# and the Wi-Fi join awake (roughly 30-40 s).
#
# The cycle count and the last few results are kept in the sleep memory, so
# they survive the sleeps:
#   cat /sys/kernel/board/sleep_memory      (over SSH or the serial console)
#
# To stop it: hold BOOT while the board wakes (autorun does not start and
# deepsleep refuses for that boot), then delete main.py from the drive.
import deepsleep
import time

SLEEP_S = 60           # time asleep per cycle
WIFI_TIMEOUT_S = 60    # sleep anyway when Wi-Fi is not up by then
STAY_S = 5             # stay awake this long once Wi-Fi is up
HISTORY = 20           # results kept in the sleep memory


def wifi_up():
    # wlan0 has a default route once DHCP has finished.
    try:
        with open("/proc/net/route") as f:
            for line in f.read().split("\n")[1:]:
                p = line.split()
                if len(p) > 2 and p[0] == "wlan0" and p[1] == "00000000":
                    return True
    except OSError:
        pass
    return False


def uptime():
    with open("/proc/uptime") as f:
        return float(f.read().split()[0])


# Sleep memory: "count\nresult\nresult..." with the newest result last.
lines = (deepsleep.memory() or b"").decode().split("\n")
try:
    count = int(lines[0]) + 1
except ValueError:
    count, lines = 1, ["0"]
history = [l for l in lines[1:] if l]

reason = deepsleep.wake_reason()
start = uptime()
while not wifi_up() and uptime() - start < WIFI_TIMEOUT_S:
    time.sleep(0.5)
t = time.localtime()
# Seconds since Linux started; the shim and the wake add about 3 s before it.
wifi = "wifi=%.1fs" % uptime() if wifi_up() else "wifi=timeout"
result = "%d %02d:%02d:%02d %s %s" % (count, t[3], t[4], t[5], reason, wifi)
print("cycle", result)

history = (history + [result])[-HISTORY:]
while True:
    data = ("\n".join([str(count)] + history) + "\n").encode()
    if len(data) <= 1024:
        break
    history = history[1:]
deepsleep.memory(data)

time.sleep(STAY_S)
deepsleep.sleep(SLEEP_S * 1000)
