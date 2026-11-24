#!/bin/sh
# esp-hosted-ng 0049: a command completes only with its own answer.  A live
# switch from SoftAP to station left wlan0 with an all-zero MAC because
# CMD_GET_MAC took another command's answer (an unsolicited CMD_MGMT_TX
# tx-done, or the late answer of a command whose wait a signal had ended).
# The vendor source is not checked out here, so this checks the patch text:
# the response match (with the firmware's GET_REG_DOMAIN quirk), the
# uninterruptible wait and its wakers, and the MAC guards.
set -u

repo_root=$(cd -- "$(dirname -- "$0")/../.." && pwd)
pkg="$repo_root/linux/buildroot-external/package/esp-hosted-ng"
patch="$pkg/0049-cmd-response-match.patch"

fails=0
ok() { echo "ok: $1"; }
bad() { echo "FAIL: $1"; fails=$((fails + 1)); }

[ -f "$patch" ] || { echo "FAIL: $patch is missing"; exit 1; }

added() { grep -E '^\+' "$patch" | grep -Fq -- "$1"; }
removed() { grep -E '^-' "$patch" | grep -Fq -- "$1"; }

added 'static bool esp_resp_matches(u8 resp, u8 cmd)' &&
	ok "adds the response matcher" || bad "no esp_resp_matches()"
added '(cmd == CMD_GET_REG_DOMAIN && resp == CMD_SET_REG_DOMAIN)' &&
	ok "GET_REG_DOMAIN accepts the firmware's SET_REG_DOMAIN answer" ||
	bad "the GET_REG_DOMAIN exception is missing (it would time out)"
added 'if (!esp_resp_matches(((struct command_header *) skb->data)->cmd_code,' &&
	ok "process_cmd_resp drops a foreign answer" ||
	bad "process_cmd_resp does not check the answer's code"
added 'if (skb->len < sizeof(struct command_header)) {' &&
	ok "a response shorter than its header is dropped" ||
	bad "no length check before reading the header"

removed 'ret = wait_event_interruptible_timeout(adapter->wait_for_cmd_resp,' &&
	added 'ret = wait_event_timeout(adapter->wait_for_cmd_resp,' &&
	ok "the command wait is uninterruptible" ||
	bad "the command wait can still be ended by a signal"
# An uninterruptible sleeper is not woken by wake_up_interruptible*().
removed 'wake_up_interruptible(&adapter->wait_for_cmd_resp);' &&
	added 'wake_up(&adapter->wait_for_cmd_resp);' &&
	ok "the response waker reaches an uninterruptible sleeper" ||
	bad "process_cmd_resp still uses wake_up_interruptible"
removed 'wake_up_interruptible_all(&adapter->wait_for_cmd_resp);' &&
	added 'wake_up_all(&adapter->wait_for_cmd_resp);' &&
	ok "the teardown waker (0047) reaches an uninterruptible sleeper" ||
	bad "the teardown still uses wake_up_interruptible_all"

added 'if (cmd_get_mac(priv) == 0 &&' &&
	added 'is_valid_ether_addr(priv->mac_address))' &&
	ok "change_iface sets only a MAC the ESP really gave" ||
	bad "change_iface may still set an all-zero MAC"
removed 'memcpy(priv->mac_address, header->mac_addr, MAC_ADDR_LEN);' &&
	added 'if (cmd_node->resp_skb->len < sizeof(*header)) {' &&
	ok "decode_mac_addr copies only a full, successful answer" ||
	bad "decode_mac_addr still copies on failure"

# No later patch may bring the interruptible wait back.
for p in "$pkg"/*.patch; do
	case "$(basename "$p")" in 0049-*) continue ;; esac
	n=${p##*/}; n=${n%%-*}
	[ "$n" -gt 49 ] 2>/dev/null || continue
	if grep -E '^\+' "$p" | grep -Fq 'wait_event_interruptible_timeout(adapter->wait_for_cmd_resp'; then
		bad "$(basename "$p") brings the interruptible command wait back"
	fi
done

if [ "$fails" -eq 0 ]; then
	echo "PASS: esp-hosted commands complete only with their own answer"
else
	echo "FAIL: $fails checks failed" >&2
	exit 1
fi
