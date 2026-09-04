/*
 * EasyStick Stamp-P4 P1 control application.
 *
 * P1 intentionally stops at the ESP-Hosted-MCU Wi-Fi path.  SSH, NTP, SMP,
 * and Bluetooth are not part of this image's acceptance claim.
 */

#include <errno.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/util.h>

#include <zephyr/net/icmp.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_ip.h>
#include <zephyr/net/net_mgmt.h>
#include <zephyr/net/wifi_mgmt.h>

LOG_MODULE_REGISTER(easystick_p1, LOG_LEVEL_INF);

#define P1_WIFI_EVENTS                                                                    \
	(NET_EVENT_WIFI_SCAN_RESULT | NET_EVENT_WIFI_SCAN_DONE |                          \
	 NET_EVENT_WIFI_CONNECT_RESULT | NET_EVENT_WIFI_DISCONNECT_RESULT)

static struct net_if *sta_iface;
static struct net_mgmt_event_callback wifi_cb;
static struct net_mgmt_event_callback ipv4_cb;

static uint32_t scan_result_count;
static bool scan_pending;
static bool connect_pending;
static bool associated;
static bool ipv4_ready;

static K_SEM_DEFINE(ping_reply_sem, 0, 1);
static struct net_icmp_ctx ping_ctx;

static int wifi_event_status(const struct net_mgmt_event_callback *cb)
{
	if (cb->info != NULL && cb->info_length >= sizeof(struct wifi_status)) {
		const struct wifi_status *status = cb->info;

		return status->status;
	}

	return -EIO;
}

static void p1_wifi_event_handler(struct net_mgmt_event_callback *cb,
				  uint64_t mgmt_event, struct net_if *iface)
{
	if (iface != sta_iface) {
		return;
	}

	switch (mgmt_event) {
	case NET_EVENT_WIFI_SCAN_RESULT: {
		const struct wifi_scan_result *result = cb->info;

		if (result == NULL ||
		    cb->info_length < sizeof(struct wifi_scan_result)) {
			LOG_ERR("EASYSTICK_P1 WIFI_SCAN_RESULT FAIL malformed");
			return;
		}

		scan_result_count++;
		LOG_INF("EASYSTICK_P1 WIFI_SCAN_RESULT index=%u ssid=%.*s channel=%u rssi=%d",
			scan_result_count, result->ssid_length,
			(const char *)result->ssid, result->channel, result->rssi);
		break;
	}

	case NET_EVENT_WIFI_SCAN_DONE: {
		int status = wifi_event_status(cb);

		scan_pending = false;
		if (status == 0) {
			LOG_INF("EASYSTICK_P1 STEP 09 WIFI_SCAN PASS results=%u",
				scan_result_count);
		} else {
			LOG_ERR("EASYSTICK_P1 STEP 09 WIFI_SCAN FAIL status=%d results=%u",
				status, scan_result_count);
		}
		break;
	}

	case NET_EVENT_WIFI_CONNECT_RESULT: {
		int status = wifi_event_status(cb);

		connect_pending = false;
		associated = (status == 0);
		ipv4_ready = false;
		if (associated) {
			LOG_INF("EASYSTICK_P1 STEP 10 ASSOCIATION PASS");
		} else {
			LOG_ERR("EASYSTICK_P1 STEP 10 ASSOCIATION FAIL status=%d", status);
		}
		break;
	}

	case NET_EVENT_WIFI_DISCONNECT_RESULT:
		associated = false;
		ipv4_ready = false;
		LOG_INF("EASYSTICK_P1 WIFI_DISCONNECT");
		break;

	default:
		break;
	}
}

static void p1_ipv4_event_handler(struct net_mgmt_event_callback *cb,
				  uint64_t mgmt_event, struct net_if *iface)
{
	char address[NET_IPV4_ADDR_LEN];
	struct net_in_addr *addr;

	ARG_UNUSED(cb);

	if (mgmt_event != NET_EVENT_IPV4_ADDR_ADD || iface != sta_iface) {
		return;
	}

	addr = net_if_ipv4_get_global_addr(iface, NET_ADDR_PREFERRED);
	if (addr == NULL) {
		LOG_ERR("EASYSTICK_P1 IPV4 FAIL no_preferred_address");
		return;
	}

	ipv4_ready = true;
	LOG_INF("EASYSTICK_P1 IPV4 PASS address=%s",
		net_addr_ntop(NET_AF_INET, addr, address, sizeof(address)));
}

static int cmd_p1_scan(const struct shell *sh, size_t argc, char **argv)
{
	struct wifi_scan_params params = {0};
	int ret;

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	if (sta_iface == NULL) {
		shell_error(sh, "Wi-Fi station interface is not ready");
		return -ENODEV;
	}
	if (scan_pending) {
		shell_error(sh, "scan already pending");
		return -EBUSY;
	}

	scan_result_count = 0;
	scan_pending = true;
	params.scan_type = WIFI_SCAN_TYPE_ACTIVE;
	params.bands = BIT(WIFI_FREQ_BAND_2_4_GHZ);
	params.max_bss_cnt = 0;

	LOG_INF("EASYSTICK_P1 STEP 09 WIFI_SCAN BEGIN");
	ret = net_mgmt(NET_REQUEST_WIFI_SCAN, sta_iface, &params, sizeof(params));
	if (ret != 0) {
		scan_pending = false;
		LOG_ERR("EASYSTICK_P1 STEP 09 WIFI_SCAN FAIL request_errno=%d", ret);
		shell_error(sh, "scan request failed: %d", ret);
		return ret;
	}

	shell_print(sh, "scan requested");
	return 0;
}

static int cmd_p1_connect(const struct shell *sh, size_t argc, char **argv)
{
	struct wifi_connect_req_params params = {0};
	size_t ssid_len = strlen(argv[1]);
	size_t psk_len = argc >= 3 ? strlen(argv[2]) : 0;
	int ret;

	if (sta_iface == NULL) {
		shell_error(sh, "Wi-Fi station interface is not ready");
		return -ENODEV;
	}
	if (ssid_len == 0 || ssid_len > WIFI_SSID_MAX_LEN) {
		shell_error(sh, "SSID length must be 1..%u", WIFI_SSID_MAX_LEN);
		return -EINVAL;
	}
	if (psk_len > 64) {
		shell_error(sh, "PSK length must be <= 64");
		return -EINVAL;
	}
	if (connect_pending) {
		shell_error(sh, "connect already pending");
		return -EBUSY;
	}

	params.ssid = (const uint8_t *)argv[1];
	params.ssid_length = (uint8_t)ssid_len;
	params.psk = psk_len ? (const uint8_t *)argv[2] : NULL;
	params.psk_length = (uint8_t)psk_len;
	params.security = psk_len ? WIFI_SECURITY_TYPE_PSK : WIFI_SECURITY_TYPE_NONE;
	params.band = WIFI_FREQ_BAND_2_4_GHZ;
	params.channel = WIFI_CHANNEL_ANY;
	params.timeout = 30;

	connect_pending = true;
	LOG_INF("EASYSTICK_P1 STEP 10 ASSOCIATION BEGIN ssid_length=%u",
		(unsigned int)ssid_len);
	ret = net_mgmt(NET_REQUEST_WIFI_CONNECT, sta_iface, &params, sizeof(params));
	if (ret != 0) {
		connect_pending = false;
		LOG_ERR("EASYSTICK_P1 STEP 10 ASSOCIATION FAIL request_errno=%d", ret);
		shell_error(sh, "connect request failed: %d", ret);
		return ret;
	}

	shell_print(sh, "connect requested; wait for ASSOCIATION and IPV4 markers");
	return 0;
}

static enum net_verdict p1_ping_handler(struct net_icmp_ctx *ctx,
					struct net_pkt *pkt,
					struct net_icmp_ip_hdr *ip_hdr,
					struct net_icmp_hdr *icmp_hdr,
					void *user_data)
{
	ARG_UNUSED(ctx);
	ARG_UNUSED(pkt);
	ARG_UNUSED(ip_hdr);
	ARG_UNUSED(icmp_hdr);
	ARG_UNUSED(user_data);

	k_sem_give(&ping_reply_sem);
	return NET_OK;
}

static int cmd_p1_ping(const struct shell *sh, size_t argc, char **argv)
{
	struct net_sockaddr_in destination = {0};
	struct net_icmp_ping_params params = {0};
	int ret;

	ARG_UNUSED(argc);

	if (!associated || !ipv4_ready) {
		shell_error(sh, "association and DHCPv4 address are required first");
		LOG_ERR("EASYSTICK_P1 STEP 11 PING FAIL ipv4_not_ready");
		return -ENETDOWN;
	}

	destination.sin_family = NET_AF_INET;
	ret = net_addr_pton(NET_AF_INET, argv[1], &destination.sin_addr);
	if (ret != 0) {
		shell_error(sh, "invalid IPv4 address: %s", argv[1]);
		LOG_ERR("EASYSTICK_P1 STEP 11 PING FAIL invalid_target=%s", argv[1]);
		return -EINVAL;
	}

	params.identifier = 0x4553;
	params.sequence = 1;
	params.data_size = 16;
	k_sem_reset(&ping_reply_sem);

	LOG_INF("EASYSTICK_P1 STEP 11 PING BEGIN target=%s", argv[1]);
	ret = net_icmp_send_echo_request(&ping_ctx, sta_iface,
					 (struct net_sockaddr *)&destination,
					 &params, NULL);
	if (ret != 0) {
		LOG_ERR("EASYSTICK_P1 STEP 11 PING FAIL send_errno=%d", ret);
		shell_error(sh, "ping send failed: %d", ret);
		return ret;
	}

	if (k_sem_take(&ping_reply_sem, K_SECONDS(5)) != 0) {
		LOG_ERR("EASYSTICK_P1 STEP 11 PING FAIL timeout");
		shell_error(sh, "ping timeout");
		return -ETIMEDOUT;
	}

	LOG_INF("EASYSTICK_P1 STEP 11 PING PASS target=%s", argv[1]);
	shell_print(sh, "ping reply received");
	return 0;
}

static int cmd_p1_status(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	shell_print(sh, "sta=%s associated=%s ipv4=%s scan_pending=%s connect_pending=%s",
		    sta_iface != NULL ? "ready" : "missing",
		    associated ? "yes" : "no", ipv4_ready ? "yes" : "no",
		    scan_pending ? "yes" : "no", connect_pending ? "yes" : "no");
	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(p1_commands,
	SHELL_CMD_ARG(scan, NULL, "Scan 2.4 GHz Wi-Fi", cmd_p1_scan, 1, 0),
	SHELL_CMD_ARG(connect, NULL, "Connect: p1 connect <ssid> [psk]",
		      cmd_p1_connect, 2, 1),
	SHELL_CMD_ARG(ping, NULL, "ICMPv4 ping: p1 ping <ipv4>",
		      cmd_p1_ping, 2, 1),
	SHELL_CMD_ARG(status, NULL, "Show P1 state", cmd_p1_status, 1, 0),
	SHELL_SUBCMD_SET_END
);

SHELL_CMD_REGISTER(p1, &p1_commands, "EasyStick P1 controls", NULL);

int main(void)
{
	int ret;

	sta_iface = net_if_get_wifi_sta();
	if (sta_iface == NULL) {
		LOG_ERR("EASYSTICK_P1 READY FAIL wifi_sta_interface_missing");
		return 0;
	}

	net_mgmt_init_event_callback(&wifi_cb, p1_wifi_event_handler, P1_WIFI_EVENTS);
	net_mgmt_add_event_callback(&wifi_cb);
	net_mgmt_init_event_callback(&ipv4_cb, p1_ipv4_event_handler,
				     NET_EVENT_IPV4_ADDR_ADD);
	net_mgmt_add_event_callback(&ipv4_cb);

	ret = net_icmp_init_ctx(&ping_ctx, NET_AF_INET,
				NET_ICMPV4_ECHO_REPLY, 0, p1_ping_handler);
	if (ret != 0) {
		LOG_ERR("EASYSTICK_P1 READY FAIL icmp_init_errno=%d", ret);
		return 0;
	}

	LOG_INF("EASYSTICK_P1 READY host=zephyr wifi=esp-hosted-mcu");
	LOG_INF("EASYSTICK_P1 COMMANDS p1 scan | p1 connect <ssid> [psk] | p1 ping <ipv4>");
	return 0;
}
