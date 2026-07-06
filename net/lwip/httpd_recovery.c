// SPDX-License-Identifier: GPL-2.0+
/*
 * Minimal HTTP upload recovery server using lwIP httpd
 * Serves a tiny upload page at / and handles POST to /upload/{firmware|uboot}
 */

#include <dm.h>
#include <dm/ofnode.h>
#include <blk.h>
#include <env.h>
#include <image.h>
#include <log.h>
#include <malloc.h>
#include <memalign.h>
#include <command.h>
#include <mtd.h>
#include <net-lwip.h>
#include <net.h>
#include <part.h>
#include <initcall.h>
#include <ubi_uboot.h>
#include <watchdog.h>
#include <linux/delay.h>
#include <linux/errno.h>
#include <linux/string.h>
#include <linux/libfdt.h>
#include <console.h>
#include <asm/gpio.h>
#include <asm/global_data.h>
#include <dt-bindings/gpio/gpio.h>

#ifdef crc32
#undef crc32
#endif
#include <u-boot/crc.h>

#include <lwip/netif.h>
#include <lwip/pbuf.h>
#include <lwip/ip.h>
#include <lwip/timeouts.h>
#include <lwip/udp.h>
#include <lwip/apps/httpd.h>
#include <lwip/apps/fs.h>
#include <lwip/prot/dhcp.h>
#include <lwip/prot/iana.h>
#include <version.h>
#include <timestamp.h>
#include <limits.h>
#include <miiphy.h>
#include <linux/mii.h>
#include <mtd/ubi-user.h>
#include <timer.h>
#include <asm/io.h>
#include "../../drivers/mtd/ubi/ubi.h"

DECLARE_GLOBAL_DATA_PTR;

__weak int xr1710g_sync_factory(void)
{
	return 0;
}

__weak void recovery_board_watchdog_kick(void)
{
}

__weak void recovery_board_http_acl(bool enable)
{
}

static void recovery_watchdog_poll(void)
{
	recovery_board_watchdog_kick();
	WATCHDOG_RESET();
}

static bool recovery_debug_enabled(void)
{
	return env_get_yesno("recovery_debug") == 1;
}

static bool recovery_board_is_sbe1v1k(void)
{
	return ofnode_device_is_compatible(ofnode_root(), "askey,sbe1v1k");
}

/*
 * Upload buffer
 * Use env 'recovery_addr' if set, otherwise fall back to U-Boot 'loadaddr'.
 * Avoid hard-coding a RAM address which may overlap U-Boot/lwIP memory.
 */
/* Default maximum upload size in bytes (override with env 'recovery_max') */
#define RECOVERY_UPLOAD_MAX    (32 * 1024 * 1024UL)
#define RECOVERY_MIN_FIRMWARE_SIZE (1 * 1024 * 1024UL)
#define RECOVERY_MAX_UBOOT_SIZE    (1 * 1024 * 1024UL)
#define RECOVERY_MAX_UBOOT_SIZE_MMC (4 * 1024 * 1024UL)
#define RECOVERY_KERNEL_PAD_SIZE    (7000 * 1024UL)
#define RECOVERY_ROOTFS_DATA_CLEAR  (4 * 1024 * 1024UL)
#define RECOVERY_MMC_WRITE_CHUNK    (1024 * 1024UL)
#define RECOVERY_REPARTITION_MAX    64UL

/* Delay before reboot after flashing completes, to let browser finish reads */
#define REBOOT_DELAY_MS        3000
#define FLASH_START_DELAY_MS   3000
#define RECOVERY_STATIC_IPADDR           "192.168.255.1"
#define RECOVERY_STATIC_NETMASK          "255.255.255.0"
#define RECOVERY_STATIC_GATEWAY          "0.0.0.0"
#define RECOVERY_DHCP_CLIENT_IPADDR      "192.168.255.2"
#define RECOVERY_DHCP_BROADCAST_IPADDR   "192.168.255.255"
#define RECOVERY_DHCP_LEASE_SECS         86400U
#define RECOVERY_DHCP_MAX_MSG_LEN        1500
#define RECOVERY_LED_PORTS     2
#define RECOVERY_LED_POLL_MS   100
#define RECOVERY_STATUS_LED_MAX           8
#define RECOVERY_STATUS_PWM_PERIOD_MS     20
#define RECOVERY_STATUS_BREATHE_PERIOD_MS 1800
#define RECOVERY_STATUS_BREATHE_HALF_MS   (RECOVERY_STATUS_BREATHE_PERIOD_MS / 2)
#define RECOVERY_STATUS_SWEEP_STEP_MS     700
#define RECOVERY_STATUS_OVERLAP_FP        (2 * 256)
#define RECOVERY_STATUS_BRIGHTNESS_FP     256
#define RECOVERY_STATUS_BREATHE_MIN_FP    64

#define RECOVERY_GPIO_SYSCTL_BASE      0x1fbf0200
#define RECOVERY_CHIP_SCU_BASE         0x1fa20000
#define RECOVERY_REG_GPIO_DATA         0x0004
#define RECOVERY_REG_GPIO_OE           0x0014
#define RECOVERY_REG_GPIO_CTRL         0x0000
#define RECOVERY_REG_GPIO_CTRL1        0x0020
#define RECOVERY_REG_GPIO_FLASH_MODE_CFG 0x0034
#define RECOVERY_REG_GPIO_CTRL2        0x0060
#define RECOVERY_REG_GPIO_CTRL3        0x0064
#define RECOVERY_REG_GPIO_DATA1        0x0070
#define RECOVERY_REG_GPIO_OE1          0x0078
#define RECOVERY_REG_GPIO_2ND_I2C_MODE 0x0214
#define RECOVERY_REG_GPIO_FLASH_MODE_CFG_EXT 0x0068

#define RECOVERY_GPIO_LAN0_LED0_MODE_MASK BIT(3)
#define RECOVERY_GPIO_LAN0_LED1_MODE_MASK BIT(4)
#define RECOVERY_GPIO_LAN1_LED0_MODE_MASK BIT(5)
#define RECOVERY_GPIO_LAN1_LED1_MODE_MASK BIT(6)
#define RECOVERY_GPIO43_FLASH_MODE_CFG BIT(23)
#define RECOVERY_GPIO44_FLASH_MODE_CFG BIT(24)
#define RECOVERY_UBOOTENV_SIZE (1 * 1024 * 1024UL)
#define RECOVERY_FACTORY_SIZE  (1 * 1024 * 1024UL)
#define RECOVERY_UBI_WRITE_CHUNK (1024 * 1024U)
#define RECOVERY_APPSBLENV_PART "0#0:APPSBLENV"
#define RECOVERY_APPSBLENV_SIZE (256 * 1024UL)
#define RECOVERY_ENV_CRC_SIZE   4
#define RECOVERY_SBE1V1K_REPARTITION_TOKEN "SBE1V1K_REPARTITION"
#define RECOVERY_SBE1V1K_CHAINLOADER_PART "0#chainloader"

static u8 *recv_base;
static u32 recv_off;
static u32 recv_total;
static int post_ok;
static int upload_done;
static int flash_request;
static volatile int reboot_request;
/* Progress for /status polling */
static volatile u32 prog_total; /* combined total for backward compat */
static volatile u32 prog_done;  /* combined done for backward compat */
static volatile u32 prog_erase_total;
static volatile u32 prog_erase_done;
static volatile u32 prog_write_total;
static volatile u32 prog_write_done;
static volatile int prog_phase; /* 0 idle, 1 erase, 2 write, 3 done, -1 error */
static volatile int prog_reboot;
static unsigned long long prog_erase_volume_base;
static unsigned long long prog_erase_volume_bytes;
static struct recovery_status_led_ctrl *prog_status_leds;

static void post_delay_cb(void *arg)
{
    (void)arg;
    flash_request = 1;
}

static void reboot_delay_cb(void *arg)
{
    (void)arg;
    reboot_request = 1;
}

static void recovery_prepare_static_network(void)
{
	env_set("ipaddr", RECOVERY_STATIC_IPADDR);
	env_set("netmask", RECOVERY_STATIC_NETMASK);
	env_set("gatewayip", RECOVERY_STATIC_GATEWAY);
}

enum upload_target {
	TARGET_FIRMWARE = 0,
	TARGET_UBOOT,
	TARGET_REPARTITION,
};
static enum upload_target current_target = TARGET_FIRMWARE;
static bool current_force_recreate;

enum recovery_backend {
	RECOVERY_BACKEND_MTD = 0,
	RECOVERY_BACKEND_UBI,
	RECOVERY_BACKEND_MMC,
};

struct recovery_target {
	enum recovery_backend backend;
	const char *name;
	const char *ubi_part;
	struct mtd_info *mtd;
	struct blk_desc *blk;
	struct disk_partition part;
	loff_t ofs;
	unsigned long long cur_size;
	unsigned long long limit;
	bool ubi_needs_format;
};

struct recovery_led_ctrl {
	struct udevice *mdio_dev;
	ulong last_poll;
};

struct recovery_gpio_pin {
	struct gpio_desc desc;
	ofnode node;
	u8 gpio;
	bool active_low;
	bool valid;
};

struct recovery_status_led_ctrl {
	struct recovery_gpio_pin leds[RECOVERY_STATUS_LED_MAX];
	int led_count;
	ulong start_ms;
};

struct recovery_dhcp_server {
	struct udp_pcb *pcb;
	struct netif *netif;
	ip4_addr_t server_ip;
	ip4_addr_t client_ip;
	ip4_addr_t netmask;
	ip4_addr_t router;
	ip4_addr_t broadcast;
	ip4_addr_t dns;
};

static const int recovery_led_phy_addrs[RECOVERY_LED_PORTS] = { 9, 10 };
static const u8 recovery_green_led_gpios[RECOVERY_LED_PORTS] = { 43, 44 };
static const u8 recovery_yellow_led_gpios[RECOVERY_LED_PORTS] = { 33, 34 };

static void recovery_led_ctrl_free(struct recovery_led_ctrl *ctrl)
{
	memset(ctrl, 0, sizeof(*ctrl));
}

static bool recovery_gpio_flash_mode_bit(u8 gpio, uintptr_t *reg, u32 *mask)
{
	if (gpio <= 15) {
		*reg = RECOVERY_GPIO_SYSCTL_BASE + RECOVERY_REG_GPIO_FLASH_MODE_CFG;
		*mask = BIT(gpio);
		return true;
	}

	if (gpio >= 16 && gpio <= 31) {
		*reg = RECOVERY_GPIO_SYSCTL_BASE + RECOVERY_REG_GPIO_FLASH_MODE_CFG_EXT;
		*mask = BIT(gpio - 16);
		return true;
	}

	if (gpio >= 36 && gpio <= 51) {
		*reg = RECOVERY_GPIO_SYSCTL_BASE + RECOVERY_REG_GPIO_FLASH_MODE_CFG_EXT;
		*mask = BIT(gpio - 20);
		return true;
	}

	return false;
}

static void recovery_clrsetbits_le32(uintptr_t addr, u32 clear, u32 set)
{
	u32 val = readl((void __iomem *)addr);

	val &= ~clear;
	val |= set;
	writel(val, (void __iomem *)addr);
}

static uintptr_t recovery_gpio_data_reg(u8 gpio)
{
	return RECOVERY_GPIO_SYSCTL_BASE +
	       (gpio < 32 ? RECOVERY_REG_GPIO_DATA : RECOVERY_REG_GPIO_DATA1);
}

static uintptr_t recovery_gpio_oe_reg(u8 gpio)
{
	return RECOVERY_GPIO_SYSCTL_BASE +
	       (gpio < 32 ? RECOVERY_REG_GPIO_OE : RECOVERY_REG_GPIO_OE1);
}

static uintptr_t recovery_gpio_dir_reg(u8 gpio)
{
	static const u16 dir_regs[] = {
		RECOVERY_REG_GPIO_CTRL,
		RECOVERY_REG_GPIO_CTRL1,
		RECOVERY_REG_GPIO_CTRL2,
		RECOVERY_REG_GPIO_CTRL3,
	};

	return RECOVERY_GPIO_SYSCTL_BASE + dir_regs[gpio / 16];
}

static void recovery_gpio_direction_output(u8 gpio)
{
	u32 bank_bit = BIT(gpio % 32);
	u32 dir_bit = BIT(2 * (gpio % 16));

	recovery_clrsetbits_le32(recovery_gpio_oe_reg(gpio), 0, bank_bit);
	recovery_clrsetbits_le32(recovery_gpio_dir_reg(gpio), 0, dir_bit);
}

static void recovery_gpio_prepare_output(u8 gpio)
{
	uintptr_t reg;
	u32 mask;

	if (recovery_gpio_flash_mode_bit(gpio, &reg, &mask))
		recovery_clrsetbits_le32(reg, mask, 0);

	recovery_gpio_direction_output(gpio);
}

static void recovery_gpio_set_value(u8 gpio, bool active_low, int active)
{
	u32 bit = BIT(gpio % 32);
	uintptr_t reg = recovery_gpio_data_reg(gpio);
	u32 set = active_low ? (active ? 0 : bit) : (active ? bit : 0);

	recovery_clrsetbits_le32(reg, bit, set);
}

static void recovery_led_set_pin(u8 gpio, int on)
{
	recovery_gpio_set_value(gpio, true, on);
}

static void recovery_status_led_set(const struct recovery_gpio_pin *pin, int on)
{
	if (!pin->valid)
		return;

	recovery_gpio_set_value(pin->gpio, pin->active_low, on);
}

static void recovery_gpio_pin_release(struct recovery_gpio_pin *pin)
{
	if (!pin->valid)
		return;

	memset(pin, 0, sizeof(*pin));
}

static ofnode recovery_led_alias_node(const char *alias)
{
	ofnode aliases;
	const char *path;

	aliases = ofnode_path("/aliases");
	if (!ofnode_valid(aliases))
		return ofnode_null();

	path = ofnode_read_string(aliases, alias);
	if (!path)
		return ofnode_null();

	return ofnode_path(path);
}

static int recovery_led_node_to_gpio(ofnode node, struct recovery_gpio_pin *pin)
{
	struct ofnode_phandle_args args;
	int ret;

	memset(pin, 0, sizeof(*pin));

	if (!ofnode_valid(node))
		return -ENOENT;

	ret = ofnode_parse_phandle_with_args(node, "gpios", "#gpio-cells", 0, 0,
					     &args);
	if (ret)
		return ret;

	if (args.args_count < 1)
		return -EINVAL;

	pin->gpio = args.args[0];
	pin->active_low = args.args_count > 1 &&
			  (args.args[1] & GPIO_ACTIVE_LOW);
	pin->node = node;
	pin->valid = true;

	return 0;
}

static void recovery_status_led_add(struct recovery_status_led_ctrl *ctrl,
				    const struct recovery_gpio_pin *pin)
{
	int i;

	if (!pin->valid)
		return;

	for (i = 0; i < ctrl->led_count; i++) {
		if (ctrl->leds[i].gpio == pin->gpio)
			return;
	}

	if (ctrl->led_count >= ARRAY_SIZE(ctrl->leds))
		return;

	ctrl->leds[ctrl->led_count++] = *pin;
}

static void recovery_status_led_collect_alias(struct recovery_status_led_ctrl *ctrl,
					      const char *alias)
{
	struct recovery_gpio_pin pin;

	if (!recovery_led_node_to_gpio(recovery_led_alias_node(alias), &pin))
		recovery_status_led_add(ctrl, &pin);
}

static int recovery_status_led_request(struct recovery_gpio_pin *pin)
{
	recovery_gpio_prepare_output(pin->gpio);
	recovery_gpio_set_value(pin->gpio, pin->active_low, 0);

	return 0;
}

static void recovery_status_led_release(struct recovery_status_led_ctrl *ctrl)
{
	int i;

	for (i = 0; i < ctrl->led_count; i++)
		recovery_gpio_pin_release(&ctrl->leds[i]);

	memset(ctrl, 0, sizeof(*ctrl));
}

static int recovery_status_led_init(struct recovery_status_led_ctrl *ctrl)
{
	static const char *const status_aliases[] = {
		"led-boot",
		"led-running",
		"led-failsafe",
		"led-upgrade",
	};
	ofnode leds, node;
	int i, keep = 0;

	memset(ctrl, 0, sizeof(*ctrl));

	leds = ofnode_path("/leds");
	if (ofnode_valid(leds)) {
		ofnode_for_each_subnode(node, leds) {
			const char *function;
			struct recovery_gpio_pin pin;

			function = ofnode_read_string(node, "function");
			if (!function || strcmp(function, "status"))
				continue;

			if (!recovery_led_node_to_gpio(node, &pin))
				recovery_status_led_add(ctrl, &pin);
		}
	}

	for (i = 0; i < ARRAY_SIZE(status_aliases); i++)
		recovery_status_led_collect_alias(ctrl, status_aliases[i]);

	if (!ctrl->led_count)
		return -ENOENT;

	for (i = 0; i < ctrl->led_count; i++) {
		int ret;

		ret = recovery_status_led_request(&ctrl->leds[i]);
		if (ret) {
			printf("Recovery status LED gpio%u request failed: %d\n",
			       ctrl->leds[i].gpio, ret);
			recovery_gpio_pin_release(&ctrl->leds[i]);
			continue;
		}

		if (keep != i) {
			ctrl->leds[keep] = ctrl->leds[i];
			memset(&ctrl->leds[i], 0, sizeof(ctrl->leds[i]));
		}
		keep++;
	}

	ctrl->led_count = keep;

	if (!ctrl->led_count) {
		recovery_status_led_release(ctrl);
		return -ENODEV;
	}

	ctrl->start_ms = get_timer(0);

	printf("Recovery status LEDs:");
	for (i = 0; i < ctrl->led_count; i++)
		printf(" gpio%u%s", ctrl->leds[i].gpio,
		       ctrl->leds[i].active_low ? "(L)" : "");
	printf("\n");

	return 0;
}

static u32 recovery_status_led_breath_fp(ulong elapsed)
{
	ulong phase, ramp;
	u32 delta;

	phase = elapsed % RECOVERY_STATUS_BREATHE_PERIOD_MS;
	ramp = phase < RECOVERY_STATUS_BREATHE_HALF_MS ?
	       phase : RECOVERY_STATUS_BREATHE_PERIOD_MS - phase;
	delta = RECOVERY_STATUS_BRIGHTNESS_FP - RECOVERY_STATUS_BREATHE_MIN_FP;

	return RECOVERY_STATUS_BREATHE_MIN_FP +
	       (delta * ramp * ramp +
		(RECOVERY_STATUS_BREATHE_HALF_MS *
		 RECOVERY_STATUS_BREATHE_HALF_MS) / 2) /
	       (RECOVERY_STATUS_BREATHE_HALF_MS *
		RECOVERY_STATUS_BREATHE_HALF_MS);
}

static ulong recovery_status_led_position_fp(struct recovery_status_led_ctrl *ctrl,
					     ulong elapsed)
{
	ulong range_fp, phase;
	int span;

	if (ctrl->led_count <= 1)
		return 0;

	span = ctrl->led_count - 1;
	range_fp = span * RECOVERY_STATUS_BRIGHTNESS_FP;
	phase = elapsed % (2 * span * RECOVERY_STATUS_SWEEP_STEP_MS);

	if (phase <= span * RECOVERY_STATUS_SWEEP_STEP_MS)
		return phase * RECOVERY_STATUS_BRIGHTNESS_FP /
		       RECOVERY_STATUS_SWEEP_STEP_MS;

	phase -= span * RECOVERY_STATUS_SWEEP_STEP_MS;

	return range_fp - (phase * RECOVERY_STATUS_BRIGHTNESS_FP /
			   RECOVERY_STATUS_SWEEP_STEP_MS);
}

static int recovery_status_led_duty_ms(struct recovery_status_led_ctrl *ctrl,
				       int idx, ulong elapsed)
{
	ulong center_fp, pwm_phase;
	u32 breath_fp, weight_fp, dist_fp;
	ulong led_pos_fp;

	center_fp = recovery_status_led_position_fp(ctrl, elapsed);
	led_pos_fp = idx * RECOVERY_STATUS_BRIGHTNESS_FP;
	dist_fp = center_fp > led_pos_fp ? center_fp - led_pos_fp :
					  led_pos_fp - center_fp;
	if (dist_fp >= RECOVERY_STATUS_OVERLAP_FP)
		return 0;

	weight_fp = (RECOVERY_STATUS_OVERLAP_FP - dist_fp) *
		    RECOVERY_STATUS_BRIGHTNESS_FP / RECOVERY_STATUS_OVERLAP_FP;
	weight_fp = weight_fp * weight_fp / RECOVERY_STATUS_BRIGHTNESS_FP;
	breath_fp = recovery_status_led_breath_fp(elapsed);
	pwm_phase = weight_fp * breath_fp;
	pwm_phase = (pwm_phase + RECOVERY_STATUS_BRIGHTNESS_FP / 2) /
		    RECOVERY_STATUS_BRIGHTNESS_FP;

	return (pwm_phase * RECOVERY_STATUS_PWM_PERIOD_MS +
		RECOVERY_STATUS_BRIGHTNESS_FP / 2) /
	       RECOVERY_STATUS_BRIGHTNESS_FP;
}

static void recovery_status_led_poll(struct recovery_status_led_ctrl *ctrl)
{
	ulong elapsed, now, pwm_phase;
	int duty_ms;
	int i;

	if (!ctrl->led_count)
		return;

	now = get_timer(0);
	elapsed = now - ctrl->start_ms;
	pwm_phase = elapsed % RECOVERY_STATUS_PWM_PERIOD_MS;

	for (i = 0; i < ctrl->led_count; i++) {
		duty_ms = recovery_status_led_duty_ms(ctrl, i, elapsed);
		recovery_status_led_set(&ctrl->leds[i],
					duty_ms && pwm_phase < duty_ms);
	}
}

static void recovery_status_led_stop(struct recovery_status_led_ctrl *ctrl)
{
	int i;

	if (!ctrl->led_count)
		return;

	for (i = 0; i < ctrl->led_count; i++)
		recovery_status_led_set(&ctrl->leds[i], 0);
}

enum recovery_dhcp_request_verdict {
	RECOVERY_DHCP_REQUEST_ACK = 0,
	RECOVERY_DHCP_REQUEST_NAK,
	RECOVERY_DHCP_REQUEST_IGNORE,
};

static int recovery_dhcp_get_option(const u8 *pkt, int pkt_len, u8 code,
				    const u8 **value, u8 *value_len)
{
	int off = DHCP_OPTIONS_OFS;

	while (off < pkt_len) {
		u8 opt, opt_len;

		opt = pkt[off++];
		if (opt == DHCP_OPTION_PAD)
			continue;
		if (opt == DHCP_OPTION_END)
			return -ENOENT;
		if (off >= pkt_len)
			break;

		opt_len = pkt[off++];
		if (off + opt_len > pkt_len)
			break;

		if (opt == code) {
			if (value)
				*value = pkt + off;
			if (value_len)
				*value_len = opt_len;
			return 0;
		}

		off += opt_len;
	}

	return -EINVAL;
}

static int recovery_dhcp_get_u8_option(const u8 *pkt, int pkt_len, u8 code,
				       u8 *value)
{
	const u8 *opt;
	u8 opt_len;
	int ret;

	ret = recovery_dhcp_get_option(pkt, pkt_len, code, &opt, &opt_len);
	if (ret)
		return ret;
	if (opt_len != sizeof(*value))
		return -EINVAL;

	*value = opt[0];
	return 0;
}

static int recovery_dhcp_get_ip4_option(const u8 *pkt, int pkt_len, u8 code,
					ip4_addr_t *addr)
{
	const u8 *opt;
	u8 opt_len;
	int ret;

	ret = recovery_dhcp_get_option(pkt, pkt_len, code, &opt, &opt_len);
	if (ret)
		return ret;
	if (opt_len != sizeof(addr->addr))
		return -EINVAL;

	memcpy(&addr->addr, opt, sizeof(addr->addr));
	return 0;
}

static int recovery_dhcp_put_option_head(u8 *options, int off, u8 code, u8 len)
{
	if (off < 0 || off + 2 + len > DHCP_OPTIONS_LEN)
		return -ENOSPC;

	options[off++] = code;
	options[off++] = len;

	return off;
}

static int recovery_dhcp_put_u8_option(u8 *options, int off, u8 code, u8 value)
{
	off = recovery_dhcp_put_option_head(options, off, code, sizeof(value));
	if (off < 0)
		return off;

	options[off++] = value;
	return off;
}

static int recovery_dhcp_put_u32_option(u8 *options, int off, u8 code, u32 value)
{
	u32 be_value = lwip_htonl(value);

	off = recovery_dhcp_put_option_head(options, off, code,
					    sizeof(be_value));
	if (off < 0)
		return off;

	memcpy(options + off, &be_value, sizeof(be_value));
	return off + sizeof(be_value);
}

static int recovery_dhcp_put_ip4_option(u8 *options, int off, u8 code,
					const ip4_addr_t *addr)
{
	off = recovery_dhcp_put_option_head(options, off, code,
					    sizeof(addr->addr));
	if (off < 0)
		return off;

	memcpy(options + off, &addr->addr, sizeof(addr->addr));
	return off + sizeof(addr->addr);
}

static void recovery_dhcp_finalize_options(struct pbuf *p, struct dhcp_msg *msg,
					   int opt_len)
{
	msg->options[opt_len++] = DHCP_OPTION_END;

	while (((opt_len < DHCP_MIN_OPTIONS_LEN) || (opt_len & 3)) &&
	       opt_len < DHCP_OPTIONS_LEN)
		msg->options[opt_len++] = DHCP_OPTION_PAD;

	pbuf_realloc(p, sizeof(*msg) - DHCP_OPTIONS_LEN + opt_len);
}

static enum recovery_dhcp_request_verdict
recovery_dhcp_classify_request(struct recovery_dhcp_server *srv,
			       const struct dhcp_msg *req,
			       const u8 *pkt, int pkt_len)
{
	ip4_addr_t option_ip, ciaddr;
	bool has_server_id, has_requested_ip;

	has_server_id = !recovery_dhcp_get_ip4_option(pkt, pkt_len,
						      DHCP_OPTION_SERVER_ID,
						      &option_ip);
	if (has_server_id) {
		if (!ip4_addr_eq(&option_ip, &srv->server_ip))
			return RECOVERY_DHCP_REQUEST_IGNORE;
	}

	has_requested_ip = !recovery_dhcp_get_ip4_option(pkt, pkt_len,
							 DHCP_OPTION_REQUESTED_IP,
							 &option_ip);
	if (has_requested_ip) {
		return ip4_addr_eq(&option_ip, &srv->client_ip) ?
			RECOVERY_DHCP_REQUEST_ACK :
			RECOVERY_DHCP_REQUEST_NAK;
	}

	ciaddr.addr = req->ciaddr.addr;
	if (!ip4_addr_isany(&ciaddr)) {
		return ip4_addr_eq(&ciaddr, &srv->client_ip) ?
			RECOVERY_DHCP_REQUEST_ACK :
			RECOVERY_DHCP_REQUEST_NAK;
	}

	return RECOVERY_DHCP_REQUEST_ACK;
}

static int recovery_dhcp_send_reply(struct recovery_dhcp_server *srv,
				    const struct dhcp_msg *req,
				    u8 message_type)
{
	struct pbuf *p;
	struct dhcp_msg *reply;
	ip_addr_t src_addr;
	ip_addr_t reply_addr;
	int opt_len;
	err_t err;

	static u32 dhcp_tx_log_count;

	p = pbuf_alloc(PBUF_TRANSPORT, sizeof(*reply), PBUF_RAM);
	if (!p)
		return -ENOMEM;

	reply = p->payload;
	memset(reply, 0, sizeof(*reply));

	reply->op = DHCP_BOOTREPLY;
	reply->htype = req->htype;
	reply->hlen = req->hlen;
	reply->hops = req->hops;
	reply->xid = req->xid;
	reply->secs = req->secs;
	reply->flags = req->flags | lwip_htons(0x8000);
	if (message_type != DHCP_NAK)
		reply->yiaddr.addr = srv->client_ip.addr;
	reply->siaddr.addr = 0;
	reply->giaddr = req->giaddr;
	memcpy(reply->chaddr, req->chaddr, DHCP_CHADDR_LEN);
	reply->cookie = PP_HTONL(DHCP_MAGIC_COOKIE);

	opt_len = 0;
	opt_len = recovery_dhcp_put_u8_option(reply->options, opt_len,
					      DHCP_OPTION_MESSAGE_TYPE,
					      message_type);
	opt_len = recovery_dhcp_put_ip4_option(reply->options, opt_len,
					       DHCP_OPTION_SERVER_ID,
					       &srv->server_ip);
	if (message_type != DHCP_NAK) {
		opt_len = recovery_dhcp_put_u32_option(reply->options, opt_len,
						       DHCP_OPTION_LEASE_TIME,
						       RECOVERY_DHCP_LEASE_SECS);
		opt_len = recovery_dhcp_put_ip4_option(reply->options, opt_len,
						       DHCP_OPTION_SUBNET_MASK,
						       &srv->netmask);
		opt_len = recovery_dhcp_put_ip4_option(reply->options, opt_len,
						       DHCP_OPTION_ROUTER,
						       &srv->router);
		opt_len = recovery_dhcp_put_ip4_option(reply->options, opt_len,
						       DHCP_OPTION_DNS_SERVER,
						       &srv->dns);
		opt_len = recovery_dhcp_put_ip4_option(reply->options, opt_len,
						       DHCP_OPTION_BROADCAST,
						       &srv->broadcast);
	}
	if (opt_len < 0) {
		pbuf_free(p);
		return opt_len;
	}

	recovery_dhcp_finalize_options(p, reply, opt_len);

	reply_addr = *IP_ADDR_BROADCAST;

	ip_addr_copy_from_ip4(src_addr, srv->server_ip);
	err = udp_sendto_if_src(srv->pcb, p, &reply_addr,
				LWIP_IANA_PORT_DHCP_CLIENT, srv->netif,
				&src_addr);

	if (recovery_debug_enabled() && dhcp_tx_log_count < 16) {
		printf("DHCP TX type=%u xid=0x%08x to %s rc=%d\n",
		       message_type, lwip_ntohl(req->xid),
		       ip4addr_ntoa(&srv->client_ip), err);
		dhcp_tx_log_count++;
	}

	pbuf_free(p);

	return err == ERR_OK ? 0 : -EIO;
}

static void recovery_dhcp_recv(void *arg, struct udp_pcb *pcb, struct pbuf *p,
			       const ip_addr_t *addr, u16_t port)
{
	struct recovery_dhcp_server *srv = arg;
	const struct dhcp_msg *req;
	u8 pkt[RECOVERY_DHCP_MAX_MSG_LEN];
	u8 message_type;
	int copy_len, pkt_len;
	static u32 dhcp_rx_log_count;

	(void)pcb;

	if (!p)
		return;

	if (port != LWIP_IANA_PORT_DHCP_CLIENT)
		goto out;

	copy_len = p->tot_len;
	if (copy_len > sizeof(pkt))
		copy_len = sizeof(pkt);

	pkt_len = pbuf_copy_partial(p, pkt, copy_len, 0);
	if (pkt_len < DHCP_OPTIONS_OFS)
		goto out;

	req = (const struct dhcp_msg *)pkt;
	if (req->op != DHCP_BOOTREQUEST ||
	    req->htype != LWIP_IANA_HWTYPE_ETHERNET ||
	    req->hlen != ARP_HLEN ||
	    req->cookie != PP_HTONL(DHCP_MAGIC_COOKIE))
		goto out;

	if (recovery_dhcp_get_u8_option(pkt, pkt_len, DHCP_OPTION_MESSAGE_TYPE,
					&message_type))
		goto out;

	if (recovery_debug_enabled() && dhcp_rx_log_count < 16) {
		printf("DHCP RX type=%u xid=0x%08x from %s:%u len=%d\n",
		       message_type, lwip_ntohl(req->xid), ipaddr_ntoa(addr),
		       port, pkt_len);
		dhcp_rx_log_count++;
	}

	switch (message_type) {
	case DHCP_DISCOVER:
		recovery_dhcp_send_reply(srv, req, DHCP_OFFER);
		break;
	case DHCP_REQUEST:
		switch (recovery_dhcp_classify_request(srv, req, pkt, pkt_len)) {
		case RECOVERY_DHCP_REQUEST_ACK:
			recovery_dhcp_send_reply(srv, req, DHCP_ACK);
			break;
		case RECOVERY_DHCP_REQUEST_NAK:
			recovery_dhcp_send_reply(srv, req, DHCP_NAK);
			break;
		default:
			break;
		}
		break;
	default:
		break;
	}

out:
	pbuf_free(p);
}

static int recovery_dhcp_server_init(struct recovery_dhcp_server *srv,
				     struct netif *netif)
{
	char server_ip[IP4ADDR_STRLEN_MAX];
	char client_ip[IP4ADDR_STRLEN_MAX];
	char netmask[IP4ADDR_STRLEN_MAX];
	char router[IP4ADDR_STRLEN_MAX];
	char broadcast[IP4ADDR_STRLEN_MAX];
	err_t err;

	memset(srv, 0, sizeof(*srv));

	srv->netif = netif;
	ip4_addr_copy(srv->server_ip, *netif_ip4_addr(netif));
	ip4_addr_copy(srv->netmask, *netif_ip4_netmask(netif));
	ip4_addr_copy(srv->router, *netif_ip4_gw(netif));
	ip4_addr_copy(srv->dns, *netif_ip4_addr(netif));

	if (ip4_addr_isany(&srv->router))
		ip4_addr_copy(srv->router, srv->server_ip);

	if (!ip4addr_aton(RECOVERY_DHCP_CLIENT_IPADDR, &srv->client_ip))
		return -EINVAL;

	if (ip4_addr_isany(&srv->netmask)) {
		if (!ip4addr_aton(RECOVERY_DHCP_BROADCAST_IPADDR, &srv->broadcast))
			return -EINVAL;
	} else {
		srv->broadcast.addr = (srv->server_ip.addr & srv->netmask.addr) |
				      ~srv->netmask.addr;
	}

	srv->pcb = udp_new();
	if (!srv->pcb)
		return -ENOMEM;

	ip_set_option(srv->pcb, SOF_BROADCAST);

	err = udp_bind(srv->pcb, IP4_ADDR_ANY, LWIP_IANA_PORT_DHCP_SERVER);
	if (err != ERR_OK) {
		udp_remove(srv->pcb);
		srv->pcb = NULL;
		return -EIO;
	}

	udp_bind_netif(srv->pcb, netif);
	udp_recv(srv->pcb, recovery_dhcp_recv, srv);

	printf("DHCP recovery server: %s/67 -> offer %s mask %s gw %s bcast %s\n",
	       ip4addr_ntoa_r(&srv->server_ip, server_ip, sizeof(server_ip)),
	       ip4addr_ntoa_r(&srv->client_ip, client_ip, sizeof(client_ip)),
	       ip4addr_ntoa_r(&srv->netmask, netmask, sizeof(netmask)),
	       ip4addr_ntoa_r(&srv->router, router, sizeof(router)),
	       ip4addr_ntoa_r(&srv->broadcast, broadcast,
			      sizeof(broadcast)));

	return 0;
}

static void recovery_dhcp_server_stop(struct recovery_dhcp_server *srv)
{
	if (srv->pcb)
		udp_remove(srv->pcb);

	memset(srv, 0, sizeof(*srv));
}

static int recovery_led_init(struct recovery_led_ctrl *ctrl)
{
	ofnode mdio_node;
	int i, ret;

	memset(ctrl, 0, sizeof(*ctrl));

	/* Make sure PHY LED mux is disabled so software can own the lines. */
	recovery_clrsetbits_le32(RECOVERY_CHIP_SCU_BASE + RECOVERY_REG_GPIO_2ND_I2C_MODE,
				 RECOVERY_GPIO_LAN0_LED0_MODE_MASK |
				 RECOVERY_GPIO_LAN0_LED1_MODE_MASK |
				 RECOVERY_GPIO_LAN1_LED0_MODE_MASK |
				 RECOVERY_GPIO_LAN1_LED1_MODE_MASK, 0);

	for (i = 0; i < RECOVERY_LED_PORTS; i++) {
		recovery_gpio_prepare_output(recovery_green_led_gpios[i]);
		recovery_gpio_prepare_output(recovery_yellow_led_gpios[i]);
		recovery_led_set_pin(recovery_green_led_gpios[i], 0);
		recovery_led_set_pin(recovery_yellow_led_gpios[i], 0);
	}

	mdio_node = ofnode_path("/soc/switch@1fb58000/mdio");
	if (!ofnode_valid(mdio_node))
		return 0;

	ret = uclass_get_device_by_ofnode(UCLASS_MDIO, mdio_node, &ctrl->mdio_dev);
	if (ret)
		ctrl->mdio_dev = NULL;

	return 0;
}

static int recovery_led_phy_speed(struct recovery_led_ctrl *ctrl, int idx)
{
	int bmcr, bmsr, stat1000, ctrl1000, lpa;

	if (!ctrl->mdio_dev || idx >= ARRAY_SIZE(recovery_led_phy_addrs))
		return 0;

	bmsr = dm_mdio_read(ctrl->mdio_dev, recovery_led_phy_addrs[idx],
			    MDIO_DEVAD_NONE, MII_BMSR);
	if (bmsr < 0)
		return 0;

	bmsr = dm_mdio_read(ctrl->mdio_dev, recovery_led_phy_addrs[idx],
			    MDIO_DEVAD_NONE, MII_BMSR);
	if (bmsr < 0)
		return 0;

	if (!(bmsr & BMSR_LSTATUS))
		return 0;

	bmcr = dm_mdio_read(ctrl->mdio_dev, recovery_led_phy_addrs[idx],
			    MDIO_DEVAD_NONE, MII_BMCR);
	if (bmcr < 0)
		return 0;

	if (!(bmcr & BMCR_ANENABLE)) {
		if (bmcr & BMCR_SPEED1000)
			return SPEED_1000;
		if (bmcr & BMCR_SPEED100)
			return SPEED_100;

		return SPEED_10;
	}

	stat1000 = dm_mdio_read(ctrl->mdio_dev, recovery_led_phy_addrs[idx],
				MDIO_DEVAD_NONE, MII_STAT1000);
	ctrl1000 = dm_mdio_read(ctrl->mdio_dev, recovery_led_phy_addrs[idx],
				MDIO_DEVAD_NONE, MII_CTRL1000);
	if (stat1000 >= 0 && ctrl1000 >= 0) {
		stat1000 &= ctrl1000 << 2;
		if (stat1000 & (PHY_1000BTSR_1000FD | PHY_1000BTSR_1000HD))
			return SPEED_1000;
	}

	lpa = dm_mdio_read(ctrl->mdio_dev, recovery_led_phy_addrs[idx],
			   MDIO_DEVAD_NONE, MII_ADVERTISE);
	if (lpa < 0)
		return SPEED_10;

	bmsr = dm_mdio_read(ctrl->mdio_dev, recovery_led_phy_addrs[idx],
			    MDIO_DEVAD_NONE, MII_LPA);
	if (bmsr < 0)
		return SPEED_10;

	lpa &= bmsr;
	if (lpa & (LPA_100FULL | LPA_100HALF))
		return SPEED_100;

	return SPEED_10;
}

static void recovery_led_poll(struct recovery_led_ctrl *ctrl)
{
	ulong now;
	int i, speed;

	now = get_timer(0);
	if (ctrl->last_poll && now - ctrl->last_poll < RECOVERY_LED_POLL_MS)
		return;

	ctrl->last_poll = now;

	for (i = 0; i < RECOVERY_LED_PORTS; i++) {
		speed = recovery_led_phy_speed(ctrl, i);
		recovery_led_set_pin(recovery_green_led_gpios[i],
				     speed == SPEED_1000);
		recovery_led_set_pin(recovery_yellow_led_gpios[i],
				     speed == SPEED_10 || speed == SPEED_100);
	}
}

static void recovery_service_runtime(struct recovery_status_led_ctrl *status_leds);

static const char *recovery_default_target(enum upload_target tgt)
{
	switch (tgt) {
	case TARGET_FIRMWARE:
		return "fit";
	case TARGET_UBOOT:
		return "uboot";
	case TARGET_REPARTITION:
		return "repartition";
	}

	return "fit";
}

static const char *recovery_target_env(enum upload_target tgt)
{
	switch (tgt) {
	case TARGET_FIRMWARE:
		return "recovery_mtd";
	case TARGET_UBOOT:
		return "recovery_mtd_uboot";
	case TARGET_REPARTITION:
		return "recovery_mtd";
	}

	return "recovery_mtd";
}

static const char *recovery_raw_env(enum upload_target tgt)
{
	switch (tgt) {
	case TARGET_FIRMWARE:
		return "recovery_dev";
	case TARGET_UBOOT:
		return "recovery_dev_uboot";
	case TARGET_REPARTITION:
		return "recovery_dev";
	}

	return "recovery_dev";
}

static const char *recovery_size_env(enum upload_target tgt)
{
	switch (tgt) {
	case TARGET_FIRMWARE:
		return "recovery_size";
	case TARGET_UBOOT:
		return "recovery_size_uboot";
	case TARGET_REPARTITION:
		return "recovery_size";
	}

	return "recovery_size";
}

static ulong recovery_raw_offset(enum upload_target tgt)
{
	switch (tgt) {
	case TARGET_FIRMWARE:
		return env_get_hex("recovery_ofs", 0x050000);
	case TARGET_UBOOT:
		return env_get_hex("uboot_ofs", 0x0);
	case TARGET_REPARTITION:
		return 0;
	}

	return 0;
}

static bool recovery_backend_is_mmc(void)
{
	const char *backend = env_get("recovery_backend");

	if (recovery_board_is_sbe1v1k())
		return true;

	return backend && (!strcasecmp(backend, "mmc") ||
			   !strcasecmp(backend, "emmc"));
}

static const char *recovery_mmcdev(void)
{
	const char *dev = env_get("recovery_mmcdev");

	if (!dev)
		dev = env_get("mmcdev");

	return dev ?: "0";
}

static const char *recovery_mmc_part_env(enum upload_target tgt)
{
	switch (tgt) {
	case TARGET_FIRMWARE:
		return "recovery_part_kernel";
	case TARGET_UBOOT:
		return "recovery_part_uboot";
	case TARGET_REPARTITION:
		return "recovery_part_uboot";
	}

	return "recovery_part_kernel";
}

static unsigned long recovery_uboot_limit(void)
{
	unsigned long def = recovery_backend_is_mmc() ?
			    RECOVERY_MAX_UBOOT_SIZE_MMC :
			    RECOVERY_MAX_UBOOT_SIZE;

	return env_get_hex("recovery_uboot_max", def);
}

static bool recovery_string_is_uint(const char *s)
{
	if (!s || !*s)
		return false;

	while (*s) {
		if (*s < '0' || *s > '9')
			return false;
		s++;
	}

	return true;
}

static int recovery_format_mmc_part_spec(const char *spec, char *buf,
					 size_t buf_len)
{
	const char *sep;
	size_t dev_len;
	int len;

	if (!spec || !*spec)
		return -EINVAL;

	if (strchr(spec, '#')) {
		strlcpy(buf, spec, buf_len);
		return 0;
	}

	sep = strchr(spec, ':');
	if (sep && !recovery_string_is_uint(sep + 1)) {
		dev_len = sep - spec;
		if (dev_len >= buf_len)
			return -ENOSPC;

		memcpy(buf, spec, dev_len);
		buf[dev_len] = '#';
		strlcpy(buf + dev_len + 1, sep + 1, buf_len - dev_len - 1);
		return 0;
	}

	if (sep) {
		strlcpy(buf, spec, buf_len);
		return 0;
	}

	len = snprintf(buf, buf_len, "%s#%s", recovery_mmcdev(), spec);
	if (len < 0 || len >= buf_len)
		return -ENOSPC;

	return 0;
}

static int recovery_get_mmc_part(const char *spec, struct blk_desc **desc,
				 struct disk_partition *part)
{
	char part_spec[96];
	int ret;

	ret = recovery_format_mmc_part_spec(spec, part_spec, sizeof(part_spec));
	if (ret)
		return ret;

	ret = part_get_info_by_dev_and_name_or_num("mmc", part_spec, desc,
						   part, false);
	if (ret < 0)
		printf("Failed to resolve eMMC partition '%s': %d\n",
		       part_spec, ret);

	return ret < 0 ? ret : 0;
}

static unsigned long long
recovery_mmc_part_bytes(const struct disk_partition *part)
{
	return (unsigned long long)part->size *
	       (unsigned long long)part->blksz;
}

static bool recovery_data_range_ok(const void *base, size_t total,
				   const void *data, size_t size)
{
	uintptr_t b = (uintptr_t)base;
	uintptr_t d = (uintptr_t)data;

	if (d < b)
		return false;
	if (size > total)
		return false;
	if (d - b > total - size)
		return false;

	return true;
}

struct recovery_image_part {
	const void *data;
	size_t size;
};

struct recovery_firmware_image {
	struct recovery_image_part kernel;
	struct recovery_image_part rootfs;
};

static bool recovery_name_has_prefix(const char *name, const char *prefix)
{
	size_t plen;

	if (!name || !prefix)
		return false;

	plen = strlen(prefix);
	return !strncasecmp(name, prefix, plen);
}

static const char *recovery_basename(const char *name)
{
	const char *base, *p;

	if (!name)
		return "";

	base = name;
	for (p = name; *p; p++) {
		if (*p == '/')
			base = p + 1;
	}

	return base;
}

static bool recovery_is_kernel_name(const char *name)
{
	const char *base = recovery_basename(name);

	return recovery_name_has_prefix(base, "hlos") ||
	       recovery_name_has_prefix(base, "kernel");
}

static bool recovery_is_rootfs_name(const char *name)
{
	const char *base = recovery_basename(name);

	return recovery_name_has_prefix(base, "rootfs") ||
	       !strcasecmp(base, "root") ||
	       !strcasecmp(base, "fs") ||
	       recovery_name_has_prefix(base, "fs-");
}

static int recovery_fit_extract_firmware(const void *fit, size_t fit_size,
					 struct recovery_firmware_image *image)
{
	int images, node, ret;

	memset(image, 0, sizeof(*image));

	ret = fit_check_format(fit, fit_size);
	if (ret)
		return ret;

	images = fdt_path_offset(fit, FIT_IMAGES_PATH);
	if (images < 0)
		return images;

	fdt_for_each_subnode(node, fit, images) {
		const char *node_name;
		const char *desc;
		const char *type;
		const void *data;
		size_t size;

		if (fit_image_get_data(fit, node, &data, &size))
			continue;

		if (!recovery_data_range_ok(fit, fit_size, data, size))
			return -EINVAL;

		node_name = fdt_get_name(fit, node, NULL);
		desc = fdt_getprop(fit, node, FIT_DESC_PROP, NULL);
		type = fdt_getprop(fit, node, FIT_TYPE_PROP, NULL);

		if (!image->kernel.data &&
		    (recovery_is_kernel_name(node_name) ||
		     recovery_is_kernel_name(desc) ||
		     (type && !strcasecmp(type, "kernel")))) {
			image->kernel.data = data;
			image->kernel.size = size;
			continue;
		}

		if (!image->rootfs.data &&
		    (recovery_is_rootfs_name(node_name) ||
		     recovery_is_rootfs_name(desc) ||
		     (type && (!strcasecmp(type, "filesystem") ||
			       !strcasecmp(type, "rootfs"))))) {
			image->rootfs.data = data;
			image->rootfs.size = size;
		}
	}

	return image->kernel.data || image->rootfs.data ? 0 : -ENOENT;
}

struct recovery_tar_header {
	char name[100];
	char mode[8];
	char uid[8];
	char gid[8];
	char size[12];
	char mtime[12];
	char chksum[8];
	char typeflag;
	char linkname[100];
	char magic[6];
	char version[2];
	char uname[32];
	char gname[32];
	char devmajor[8];
	char devminor[8];
	char prefix[155];
};

static bool recovery_tar_header_empty(const struct recovery_tar_header *hdr)
{
	const u8 *p = (const u8 *)hdr;
	int i;

	for (i = 0; i < 512; i++) {
		if (p[i])
			return false;
	}

	return true;
}

static int recovery_tar_octal(const char *field, size_t len, size_t *value)
{
	size_t v = 0;
	size_t i;

	for (i = 0; i < len; i++) {
		char c = field[i];

		if (c == '\0' || c == ' ')
			break;
		if (c < '0' || c > '7')
			return -EINVAL;
		v = (v << 3) + c - '0';
	}

	*value = v;
	return 0;
}

static void recovery_tar_name(const struct recovery_tar_header *hdr,
			      char *name, size_t name_len)
{
	size_t prefix_len = strnlen(hdr->prefix, sizeof(hdr->prefix));
	size_t file_len = strnlen(hdr->name, sizeof(hdr->name));
	size_t off = 0;

	if (!name_len)
		return;

	if (prefix_len) {
		off = prefix_len >= name_len ? name_len - 1 : prefix_len;
		memcpy(name, hdr->prefix, off);
		if (off < name_len - 1)
			name[off++] = '/';
	}

	if (off < name_len - 1) {
		size_t copy = file_len;

		if (copy > name_len - 1 - off)
			copy = name_len - 1 - off;
		memcpy(name + off, hdr->name, copy);
		off += copy;
	}

	name[off] = '\0';
}

static int recovery_tar_extract_firmware(const void *tar, size_t tar_size,
					 struct recovery_firmware_image *image)
{
	const u8 *base = tar;
	size_t off = 0;

	memset(image, 0, sizeof(*image));

	while (off + 512 <= tar_size) {
		const struct recovery_tar_header *hdr =
			(const struct recovery_tar_header *)(base + off);
		char name[256];
		size_t size;

		if (recovery_tar_header_empty(hdr))
			break;

		if (memcmp(hdr->magic, "ustar", 5))
			return -EINVAL;

		if (recovery_tar_octal(hdr->size, sizeof(hdr->size), &size))
			return -EINVAL;

		if (off + 512 > tar_size || size > tar_size - off - 512)
			return -EINVAL;

		recovery_tar_name(hdr, name, sizeof(name));

		if ((hdr->typeflag == '\0' || hdr->typeflag == '0') &&
		    !image->kernel.data && recovery_is_kernel_name(name)) {
			image->kernel.data = base + off + 512;
			image->kernel.size = size;
		} else if ((hdr->typeflag == '\0' || hdr->typeflag == '0') &&
			   !image->rootfs.data && recovery_is_rootfs_name(name)) {
			image->rootfs.data = base + off + 512;
			image->rootfs.size = size;
		}

		off += 512 + ALIGN(size, 512);
	}

	return image->kernel.data || image->rootfs.data ? 0 : -ENOENT;
}

static int recovery_raw_extract_firmware(const void *raw, size_t raw_size,
					 struct recovery_firmware_image *image)
{
	size_t kernel_pad = env_get_hex("recovery_kernel_pad",
				       RECOVERY_KERNEL_PAD_SIZE);
	size_t kernel_size = kernel_pad;

	memset(image, 0, sizeof(*image));

	if (raw_size <= kernel_pad)
		return -ENOENT;

	if (!fit_check_format(raw, raw_size)) {
		size_t fit_size = fdt_totalsize(raw);

		if (fit_size > 0 && fit_size <= kernel_pad)
			kernel_size = fit_size;
	}

	image->kernel.data = raw;
	image->kernel.size = kernel_size;
	image->rootfs.data = (const u8 *)raw + kernel_pad;
	image->rootfs.size = raw_size - kernel_pad;

	return 0;
}

static int recovery_extract_firmware(const void *data, size_t size,
				     struct recovery_firmware_image *image)
{
	int ret;

	ret = recovery_fit_extract_firmware(data, size, image);
	if (!ret && image->kernel.data && image->rootfs.data) {
		printf("Firmware image: FIT sections kernel=%lu rootfs=%lu\n",
		       (ulong)image->kernel.size, (ulong)image->rootfs.size);
		return 0;
	}

	ret = recovery_tar_extract_firmware(data, size, image);
	if (!ret && image->kernel.data && image->rootfs.data) {
		printf("Firmware image: sysupgrade tar kernel=%lu rootfs=%lu\n",
		       (ulong)image->kernel.size, (ulong)image->rootfs.size);
		return 0;
	}

	ret = recovery_raw_extract_firmware(data, size, image);
	if (!ret && image->kernel.data && image->rootfs.data) {
		printf("Firmware image: raw kernel/rootfs split kernel=%lu rootfs=%lu\n",
		       (ulong)image->kernel.size, (ulong)image->rootfs.size);
		return 0;
	}

	printf("Firmware image must contain both kernel/hlos and root/rootfs/fs payloads\n");
	return -EINVAL;
}

static int recovery_mmc_write_part(const char *spec,
				   struct recovery_status_led_ctrl *status_leds,
				   const void *data, size_t size,
				   size_t progress_base)
{
	struct disk_partition part;
	struct blk_desc *desc;
	unsigned long long capacity;
	ulong blksz, chunk_size;
	u8 *chunk_buf;
	size_t written = 0;
	int ret;

	ret = recovery_get_mmc_part(spec, &desc, &part);
	if (ret)
		return ret;

	blksz = part.blksz ?: desc->blksz;
	capacity = recovery_mmc_part_bytes(&part);
	if (size > capacity) {
		printf("Image size %lu exceeds eMMC partition '%s' size %llu\n",
		       (ulong)size, spec, capacity);
		return -EFBIG;
	}

	chunk_size = ALIGN(RECOVERY_MMC_WRITE_CHUNK, blksz);
	if (chunk_size < blksz)
		chunk_size = blksz;

	chunk_buf = memalign(ARCH_DMA_MINALIGN, chunk_size);
	if (!chunk_buf)
		return -ENOMEM;

	while (written < size) {
		size_t todo = size - written;
		size_t write_len;
		lbaint_t blk;
		lbaint_t blkcnt;

		if (todo > chunk_size)
			todo = chunk_size;

		write_len = ALIGN(todo, blksz);
		memset(chunk_buf, 0, write_len);
		memcpy(chunk_buf, (const u8 *)data + written, todo);

		blk = part.start + written / blksz;
		blkcnt = write_len / blksz;
		if (blk_dwrite(desc, blk, blkcnt, chunk_buf) != blkcnt) {
			printf("eMMC write failed at partition '%s' block " LBAF "\n",
			       spec, blk);
			free(chunk_buf);
			return -EIO;
		}

		written += todo;
		prog_write_done = progress_base + written;
		prog_done = prog_erase_done + prog_write_done;
		recovery_service_runtime(status_leds);
	}

	free(chunk_buf);
	return 0;
}

static int recovery_mmc_clear_part(const char *spec,
				   struct recovery_status_led_ctrl *status_leds,
				   size_t clear_size)
{
	struct disk_partition part;
	struct blk_desc *desc;
	unsigned long long capacity;
	ulong blksz, chunk_size;
	u8 *chunk_buf;
	size_t cleared = 0;
	int ret;

	if (!spec || !*spec || !clear_size)
		return 0;

	ret = recovery_get_mmc_part(spec, &desc, &part);
	if (ret)
		return ret;

	blksz = part.blksz ?: desc->blksz;
	capacity = recovery_mmc_part_bytes(&part);
	if (clear_size > capacity)
		clear_size = capacity;

	clear_size = ALIGN(clear_size, blksz);
	chunk_size = ALIGN(RECOVERY_MMC_WRITE_CHUNK, blksz);
	if (chunk_size < blksz)
		chunk_size = blksz;

	chunk_buf = memalign(ARCH_DMA_MINALIGN, chunk_size);
	if (!chunk_buf)
		return -ENOMEM;

	memset(chunk_buf, 0, chunk_size);

	while (cleared < clear_size) {
		size_t todo = clear_size - cleared;
		lbaint_t blk, blkcnt;

		if (todo > chunk_size)
			todo = chunk_size;

		blk = part.start + cleared / blksz;
		blkcnt = todo / blksz;
		if (blk_dwrite(desc, blk, blkcnt, chunk_buf) != blkcnt) {
			printf("eMMC clear failed at partition '%s' block " LBAF "\n",
			       spec, blk);
			free(chunk_buf);
			return -EIO;
		}

		cleared += todo;
		recovery_service_runtime(status_leds);
	}

	free(chunk_buf);
	return 0;
}

struct recovery_factory_part {
	const char *spec;
	lbaint_t start;
	lbaint_t size;
};

static const struct recovery_factory_part sbe1v1k_factory_parts[] = {
	{ "0#0:SBL1", 34, 2048 },
	{ "0#0:WIFIFW_1", 61474, 20480 },
	{ "0#0:HLOS", 81954, 14336 },
	{ "0#0:HLOS_1", 96290, 14336 },
	{ "0#rootfs", 110626, 249856 },
	{ "0#rootfs_1", 360482, 249856 },
	{ "0#rootfs_data", 610338, 1048576 },
	{ "0#rootfs_data_1", 1658914, 1048576 },
	{ "0#rsvd_2", 5201954, 65536 },
	{ "0#ASKEYMFC", 15249408, 20480 },
};

static const char sbe1v1k_openwrt_gpt[] =
	"uuid_disk=98101B32-BBE2-4BF2-A06E-2BB33D000C20;"
	"name=0:SBL1,start=0x4400,size=0x100000,type=DEA0BA2C-CBDD-4805-B4F9-F428251C3E98,uuid=8A52D344-343A-E549-ED9A-B264DE413D55;"
	"name=0:SBL1_1,start=0x104400,size=0x100000,type=7A3DF1A3-A31A-454D-BD78-DF259ED486BE,uuid=A49B7547-7E90-6DB7-F276-06E754650644;"
	"name=0:BOOTCONFIG,start=0x204400,size=0x80000,type=2B7D04FF-31F0-4E6A-BE9A-DA50314DAD58,uuid=025187DC-4C24-21A6-D491-D58A0F0435BD;"
	"name=0:BOOTCONFIG1,start=0x284400,size=0x80000,type=7BD25378-5C39-11E5-8A77-40A8F05F1418,uuid=F1F736F9-BF18-2C53-8F1D-69215C962CEA;"
	"name=0:QSEE,start=0x304400,size=0x300000,type=A053AA7F-40B8-4B1C-BA08-2F68AC71A4F4,uuid=3961AB4B-A7C9-7CAB-DC73-C7D4941C3903;"
	"name=0:QSEE_1,start=0x604400,size=0x300000,type=A6DD74A1-C8BF-4DBC-AE39-62B8E78C4038,uuid=A52286FC-F827-D89C-1477-6B52F206828E;"
	"name=0:DEVCFG,start=0x904400,size=0x80000,type=F65D4B16-343D-4E25-AAFC-BE99B6556A6D,uuid=687EA2EE-1821-0010-761B-02DB14BEEC22;"
	"name=0:DEVCFG_1,start=0x984400,size=0x80000,type=48BFA451-9443-46F7-B400-892A6B1BFC16,uuid=FDEC5365-B2C0-D094-1228-A39958709F8E;"
	"name=0:APDP,start=0xa04400,size=0x80000,type=318B7C50-A113-0FC1-F3A2-59E76A68F3F7,uuid=1AA0D675-A37F-4535-E72D-04F404D859FC;"
	"name=0:APDP_1,start=0xa84400,size=0x80000,type=BC4386C7-B38D-BA64-A4EC-127D57C3F24B,uuid=4FAC8501-E709-EF23-AAF3-E5EF62BC61CA;"
	"name=0:TME,start=0xb04400,size=0x80000,type=0833175E-AF8F-BACC-F2A2-5B70354B1A0A,uuid=3F5C7D97-610B-9F02-FBD0-487CE616B38C;"
	"name=0:TME_1,start=0xb84400,size=0x80000,type=66A49874-13D3-66EA-B0B7-3F61C7E405CD,uuid=ED1D73AD-604C-6838-F9C5-A0EDD4DBFFCF;"
	"name=0:RPM,start=0xc04400,size=0x80000,type=098DF793-D712-413D-9D4E-89D711772228,uuid=1EB4925F-885B-D6ED-8765-382281B80334;"
	"name=0:RPM_1,start=0xc84400,size=0x80000,type=2D2BE762-890B-11E5-AAF3-40A8F05F1418,uuid=001588BF-1DB6-1D47-6FFC-ABFC1A131725;"
	"name=0:CDT,start=0xd04400,size=0x80000,type=A19F205F-CCD8-4B6D-8F1E-2D9BC24CFFB1,uuid=AE2F5404-53F5-D041-E55E-D2FA3993216F;"
	"name=0:CDT_1,start=0xd84400,size=0x80000,type=7A795379-C250-4282-A2C7-FC4E13F4A43D,uuid=EA5E685D-7172-398E-3A8D-F5BB64D46929;"
	"name=0:APPSBLENV,start=0xe04400,size=0x40000,type=300FFDCD-22E0-47E7-9A23-F16ED9382387,uuid=FB5FF3E9-FF90-D2F8-D453-9A0BF004A4E9;"
	"name=0:APPSBL,start=0xe44400,size=0x200000,type=400FFDCD-22E0-47E7-9A23-F16ED9382388,uuid=8BE81F01-FAE2-9AFA-9DC1-1AD01F5B3ADE;"
	"name=0:APPSBL_1,start=0x1044400,size=0x200000,type=C126787D-3EEF-444C-9E43-FEFF3F103E22,uuid=E7988837-5B97-DE5E-1354-4A0D190B878F;"
	"name=0:ART,start=0x1244400,size=0x100000,type=A72E50C1-D37C-429D-9620-35FCA612B9A8,uuid=E4871F79-1E23-8018-ABFD-3EE3AD567B10;"
	"name=0:ETHPHYFW,start=0x1344400,size=0x80000,type=C1DC4CAB-430B-4CDC-A8C5-7115912B74FE,uuid=65EF33C8-6506-5583-5171-24A2A56C7EAC;"
	"name=0:LICENSE,start=0x13c4400,size=0x40000,type=A7A81A61-66CE-C908-F4A2-82D39D2AFF57,uuid=38A446C9-37EC-230A-9F64-603D3E5F98A5;"
	"name=0:WIFIFW,start=0x1404400,size=0xa00000,type=888D8069-8D27-40A8-95A9-6006E1CE9B3B,uuid=4044C5A9-1C7A-ABEB-C3C4-8F46644DCD06;"
	"name=0:WIFIFW_1,start=0x1e04400,size=0xa00000,type=981476F5-5CD7-42DB-9CE9-87B3A31AADBD,uuid=7A86C4EE-D900-D817-BEBC-5CF07BF59CE1;"
	"name=0:HLOS,start=0x2804400,size=0x700000,type=B51F2982-3EBE-46DE-8721-EE641E1F9997,uuid=BB5169B0-2E1C-0EE4-AFA7-D9613B5605AC;"
	"name=0:HLOS_1,start=0x2f04400,size=0x700000,type=A71DA577-7F81-4626-B4A2-E377F9174525,uuid=F81FBCEB-AB17-2544-4762-C5206AC116AE;"
	"name=chainloader,start=0x3604400,size=0x400000,type=EBD0A0A2-B9E5-4433-87C0-68B6B72699C7;"
	"name=kernel,start=0x3a04400,size=0x2000000,type=EBD0A0A2-B9E5-4433-87C0-68B6B72699C7;"
	"name=rootfs,start=0x5a04400,size=0x40000000,type=0FC63DAF-8483-4772-8E79-3D69D8477DE4;"
	"name=rootfs_data,start=0x45a04400,size=-,type=0FC63DAF-8483-4772-8E79-3D69D8477DE4;";

static int recovery_verify_factory_gpt(struct recovery_status_led_ctrl *status_leds)
{
	size_t i;

	prog_phase = 1;
	prog_erase_done = 0;
	prog_erase_total = ARRAY_SIZE(sbe1v1k_factory_parts);
	prog_write_done = 0;
	prog_write_total = 0;
	prog_total = prog_erase_total;
	prog_done = 0;

	for (i = 0; i < ARRAY_SIZE(sbe1v1k_factory_parts); i++) {
		const struct recovery_factory_part *expect =
			&sbe1v1k_factory_parts[i];
		struct disk_partition part;
		struct blk_desc *desc;
		int ret;

		ret = recovery_get_mmc_part(expect->spec, &desc, &part);
		if (ret)
			return ret;

		if (part.start != expect->start || part.size != expect->size) {
			printf("Factory GPT check failed for '%s': start " LBAF
			       " size " LBAF ", expected start " LBAF
			       " size " LBAF "\n",
			       expect->spec, part.start, part.size,
			       expect->start, expect->size);
			return -EINVAL;
		}

		prog_erase_done = i + 1;
		prog_done = prog_erase_done;
		recovery_service_runtime(status_leds);
	}

	return 0;
}

static int recovery_verify_openwrt_gpt(void)
{
	struct disk_partition part;
	struct blk_desc *desc;
	int ret;

	ret = recovery_get_mmc_part("0#0:HLOS", &desc, &part);
	if (ret || part.start != 81954 || part.size != 14336)
		return ret ?: -EINVAL;

	ret = recovery_get_mmc_part("0#0:HLOS_1", &desc, &part);
	if (ret || part.start != 96290 || part.size != 14336)
		return ret ?: -EINVAL;

	ret = recovery_get_mmc_part(RECOVERY_SBE1V1K_CHAINLOADER_PART,
				    &desc, &part);
	if (ret || part.start != 110626 || part.size != 8192)
		return ret ?: -EINVAL;

	ret = recovery_get_mmc_part("0#kernel", &desc, &part);
	if (ret || part.start != 118818 || part.size != 65536)
		return ret ?: -EINVAL;

	ret = recovery_get_mmc_part("0#rootfs", &desc, &part);
	if (ret || part.start != 184354 || part.size != 2097152)
		return ret ?: -EINVAL;

	ret = recovery_get_mmc_part("0#rootfs_data", &desc, &part);
	if (ret || part.start != 2281506 || part.size < 1048576)
		return ret ?: -EINVAL;

	return 0;
}

static bool recovery_fit_has_image_node(const void *fit, const char *name)
{
	int images;

	images = fdt_path_offset(fit, FIT_IMAGES_PATH);
	if (images < 0)
		return false;

	return fdt_subnode_offset(fit, images, name) >= 0;
}

static bool recovery_is_sbe1v1k_chainloader_fit(const void *fit,
						size_t fit_size)
{
	const char *desc;

	if (fit_check_format(fit, fit_size))
		return false;

	desc = fdt_getprop(fit, 0, FIT_DESC_PROP, NULL);
	if (!desc || !strstr(desc, "SBE1V1K") || !strstr(desc, "chainloader"))
		return false;

	return recovery_fit_has_image_node(fit, "kernel-1") &&
	       recovery_fit_has_image_node(fit, "uboot-1");
}

static int recovery_find_running_chainloader_fit(const void **fitp,
						 size_t *fit_sizep)
{
	ulong candidates[] = {
		0x80000000UL,
	};
	unsigned long max = recovery_uboot_limit();
	size_t i, j;

	for (i = 0; i < ARRAY_SIZE(candidates); i++) {
		const void *fit;
		size_t fit_size;
		ulong addr = candidates[i];

		if (!addr)
			continue;

		for (j = 0; j < i; j++) {
			if (candidates[j] == addr)
				break;
		}
		if (j != i)
			continue;

		fit = (const void *)addr;
		if (fit_check_format(fit, IMAGE_SIZE_INVAL))
			continue;

		fit_size = fit_get_size(fit);
		if (!fit_size || fit_size > max)
			continue;

		if (!recovery_is_sbe1v1k_chainloader_fit(fit, fit_size))
			continue;

		*fitp = fit;
		*fit_sizep = fit_size;
		printf("Found running SBE1V1K chainloader FIT at 0x%08lx (%lu bytes)\n",
		       addr, (ulong)fit_size);
		return 0;
	}

	printf("Cannot find the running SBE1V1K chainloader FIT in RAM\n");
	return -ENOENT;
}

static int recovery_load_appsblenv(u8 **bufp, size_t *env_bytesp,
				   struct blk_desc **descp,
				   struct disk_partition *partp)
{
	struct disk_partition part;
	struct blk_desc *desc;
	unsigned long long capacity;
	u8 *buf;
	int ret;

	ret = recovery_get_mmc_part(RECOVERY_APPSBLENV_PART, &desc, &part);
	if (ret)
		return ret;

	capacity = recovery_mmc_part_bytes(&part);
	if (capacity != RECOVERY_APPSBLENV_SIZE) {
		printf("Unexpected APPSBLENV size %llu, expected %lu\n",
		       capacity, RECOVERY_APPSBLENV_SIZE);
		return -EINVAL;
	}

	buf = memalign(ARCH_DMA_MINALIGN, RECOVERY_APPSBLENV_SIZE);
	if (!buf)
		return -ENOMEM;

	if (blk_dread(desc, part.start, part.size, buf) != part.size) {
		printf("Failed to read APPSBLENV from eMMC\n");
		free(buf);
		return -EIO;
	}

	*bufp = buf;
	*env_bytesp = RECOVERY_APPSBLENV_SIZE;
	if (descp)
		*descp = desc;
	if (partp)
		*partp = part;

	return 0;
}

static int recovery_verify_appsblenv_crc(const u8 *env_buf, size_t env_bytes)
{
	u32 stored_crc;
	u32 calc_crc;

	if (env_bytes <= RECOVERY_ENV_CRC_SIZE)
		return -EINVAL;

	memcpy(&stored_crc, env_buf, sizeof(stored_crc));
	calc_crc = crc32(0, env_buf + RECOVERY_ENV_CRC_SIZE,
			 env_bytes - RECOVERY_ENV_CRC_SIZE);
	if (stored_crc != calc_crc) {
		printf("APPSBLENV CRC mismatch: stored 0x%08x calculated 0x%08x\n",
		       stored_crc, calc_crc);
		return -EINVAL;
	}

	return 0;
}

static int recovery_check_appsblenv(void)
{
	size_t env_bytes;
	u8 *env_buf;
	int ret;

	ret = recovery_load_appsblenv(&env_buf, &env_bytes, NULL, NULL);
	if (ret)
		return ret;

	ret = recovery_verify_appsblenv_crc(env_buf, env_bytes);
	free(env_buf);

	return ret;
}

struct recovery_env_update {
	const char *key;
	const char *value;
};

static bool recovery_env_entry_is_key(const char *entry, const char *key)
{
	size_t key_len = strlen(key);

	return !strncmp(entry, key, key_len) && entry[key_len] == '=';
}

static bool recovery_env_entry_replaced(const char *entry,
					const struct recovery_env_update *updates,
					size_t update_count)
{
	size_t i;

	for (i = 0; i < update_count; i++) {
		if (recovery_env_entry_is_key(entry, updates[i].key))
			return true;
	}

	return false;
}

static int recovery_env_append(char *data, size_t data_size, size_t *offp,
			       const char *key, const char *value)
{
	int len;

	if (*offp >= data_size)
		return -ENOSPC;

	len = snprintf(data + *offp, data_size - *offp, "%s=%s", key, value);
	if (len < 0)
		return len;

	if (*offp + len + 2 > data_size)
		return -ENOSPC;

	*offp += len + 1;
	return 0;
}

static int recovery_update_env_data(u8 *env_data, size_t data_size,
				    const struct recovery_env_update *updates,
				    size_t update_count)
{
	char *new_data;
	size_t old_off = 0;
	size_t new_off = 0;
	size_t i;
	int ret = 0;

	new_data = malloc(data_size);
	if (!new_data)
		return -ENOMEM;
	memset(new_data, 0, data_size);

	while (old_off < data_size && env_data[old_off]) {
		const char *entry = (const char *)env_data + old_off;
		size_t remain = data_size - old_off;
		size_t entry_len = strnlen(entry, remain);

		if (entry_len == remain) {
			ret = -EINVAL;
			goto out;
		}

		if (!recovery_env_entry_replaced(entry, updates, update_count)) {
			if (new_off + entry_len + 2 > data_size) {
				ret = -ENOSPC;
				goto out;
			}
			memcpy(new_data + new_off, entry, entry_len + 1);
			new_off += entry_len + 1;
		}

		old_off += entry_len + 1;
	}

	for (i = 0; i < update_count; i++) {
		ret = recovery_env_append(new_data, data_size, &new_off,
					  updates[i].key, updates[i].value);
		if (ret)
			goto out;
	}

	memcpy(env_data, new_data, data_size);

out:
	free(new_data);
	return ret;
}

static int recovery_write_appsblenv(struct recovery_status_led_ctrl *status_leds,
				    size_t progress_base)
{
	static const struct recovery_env_update updates[] = {
		{
			"bootargs",
			"console=ttyMSM0,115200n8 rootwait root=PARTLABEL=rootfs"
		},
		{
			"boot_chainloader",
			"mmc read 0x44000000 0x0001b022 0x2000; bootm 0x44000000"
		},
		{ "do_boot", "run boot_chainloader" },
		{
			"bootcmd",
			"echo \"Hit ctrl+c for shell...\"; if sleep 3; then run do_boot; else true; fi;"
		},
	};
	struct disk_partition part;
	struct blk_desc *desc;
	size_t env_bytes;
	u8 *env_buf;
	u32 crc;
	int ret;

	ret = recovery_load_appsblenv(&env_buf, &env_bytes, &desc, &part);
	if (ret)
		return ret;

	ret = recovery_verify_appsblenv_crc(env_buf, env_bytes);
	if (ret)
		goto out;

	ret = recovery_update_env_data(env_buf + RECOVERY_ENV_CRC_SIZE,
				       env_bytes - RECOVERY_ENV_CRC_SIZE,
				       updates, ARRAY_SIZE(updates));
	if (ret)
		goto out;

	crc = crc32(0, env_buf + RECOVERY_ENV_CRC_SIZE,
		    env_bytes - RECOVERY_ENV_CRC_SIZE);
	memcpy(env_buf, &crc, sizeof(crc));

	if (blk_dwrite(desc, part.start, part.size, env_buf) != part.size) {
		printf("Failed to write updated APPSBLENV to eMMC\n");
		ret = -EIO;
		goto out;
	}

	prog_write_done = progress_base + env_bytes;
	prog_done = prog_erase_done + prog_write_done;
	recovery_service_runtime(status_leds);

out:
	free(env_buf);
	return ret;
}

static int recovery_repartition_factory(struct recovery_status_led_ctrl *status_leds)
{
	const void *chainloader_fit;
	size_t chainloader_size;
	int ret;

	if (recv_off != strlen(RECOVERY_SBE1V1K_REPARTITION_TOKEN) ||
	    memcmp(recv_base, RECOVERY_SBE1V1K_REPARTITION_TOKEN,
		   strlen(RECOVERY_SBE1V1K_REPARTITION_TOKEN))) {
		printf("Invalid repartition confirmation token\n");
		return -EINVAL;
	}

	if (!recovery_backend_is_mmc()) {
		printf("Factory repartition requires eMMC recovery backend\n");
		return -EINVAL;
	}

	ret = recovery_verify_factory_gpt(status_leds);
	if (ret)
		return ret;

	ret = recovery_find_running_chainloader_fit(&chainloader_fit,
						    &chainloader_size);
	if (ret)
		return ret;

	ret = recovery_check_appsblenv();
	if (ret)
		return ret;

	printf("Factory GPT verified. Writing OpenWrt GPT layout...\n");
	prog_phase = 2;
	prog_erase_done = prog_erase_total;
	prog_write_done = 0;
	prog_write_total = 2 + chainloader_size + RECOVERY_APPSBLENV_SIZE;
	prog_total = prog_erase_total + prog_write_total;
	prog_done = prog_erase_done;
	recovery_service_runtime(status_leds);

	ret = env_set("sbe1v1k_repartition_gpt", sbe1v1k_openwrt_gpt);
	if (ret)
		return ret;
	prog_write_done = 1;
	prog_done = prog_erase_done + prog_write_done;
	recovery_service_runtime(status_leds);

	ret = run_commandf("mmc dev %s", recovery_mmcdev());
	if (ret) {
		env_set("sbe1v1k_repartition_gpt", NULL);
		return ret;
	}

	ret = run_commandf("gpt write mmc %s ${sbe1v1k_repartition_gpt}",
			   recovery_mmcdev());
	env_set("sbe1v1k_repartition_gpt", NULL);
	if (ret)
		return ret;
	prog_write_done = 2;
	prog_done = prog_erase_done + prog_write_done;
	recovery_service_runtime(status_leds);

	ret = recovery_verify_openwrt_gpt();
	if (ret) {
		printf("OpenWrt GPT verification failed after write: %d\n", ret);
		return ret;
	}
	prog_write_done = 2;
	prog_done = prog_erase_done + prog_write_done;
	recovery_service_runtime(status_leds);

	printf("Writing running chainloader FIT to '%s'...\n",
	       RECOVERY_SBE1V1K_CHAINLOADER_PART);
	ret = recovery_mmc_write_part(RECOVERY_SBE1V1K_CHAINLOADER_PART,
				      status_leds, chainloader_fit,
				      chainloader_size, 2);
	if (ret)
		return ret;

	printf("Updating factory U-Boot environment in APPSBLENV...\n");
	ret = recovery_write_appsblenv(status_leds, 2 + chainloader_size);
	if (ret)
		return ret;

	prog_write_done = prog_write_total;
	prog_done = prog_erase_done + prog_write_done;
	recovery_service_runtime(status_leds);

	printf("OpenWrt GPT layout written, chainloader installed, APPSBLENV updated. Upload firmware before reboot.\n");
	return 0;
}

static int recovery_flash_mmc_firmware(struct recovery_status_led_ctrl *status_leds)
{
	const char *kernel_part = env_get("recovery_part_kernel") ?: "0#kernel";
	const char *rootfs_part = env_get("recovery_part_rootfs") ?: "0#rootfs";
	const char *data_part = env_get("recovery_part_data") ?: "0#rootfs_data";
	struct recovery_firmware_image image;
	size_t clear_size;
	int ret;

	ret = recovery_extract_firmware(recv_base, recv_off, &image);
	if (ret)
		return ret;

	prog_phase = 2;
	prog_done = 0;
	prog_erase_done = 0;
	prog_erase_total = 0;
	prog_write_done = 0;
	prog_write_total = image.kernel.size + image.rootfs.size;
	prog_total = prog_write_total;

	printf("Writing kernel payload to eMMC partition '%s'...\n",
	       kernel_part);
	ret = recovery_mmc_write_part(kernel_part, status_leds,
				      image.kernel.data, image.kernel.size, 0);
	if (ret)
		return ret;

	printf("Writing rootfs payload to eMMC partition '%s'...\n",
	       rootfs_part);
	ret = recovery_mmc_write_part(rootfs_part, status_leds,
				      image.rootfs.data, image.rootfs.size,
				      image.kernel.size);
	if (ret)
		return ret;

	clear_size = env_get_hex("recovery_data_clear",
				 RECOVERY_ROOTFS_DATA_CLEAR);
	if (clear_size) {
		printf("Clearing %lu bytes of eMMC data partition '%s'...\n",
		       (ulong)clear_size, data_part);
		ret = recovery_mmc_clear_part(data_part, status_leds, clear_size);
		if (ret)
			return ret;
	}

	return 0;
}

static int recovery_flash_mmc_uboot(struct recovery_status_led_ctrl *status_leds)
{
	const char *uboot_part = env_get("recovery_part_uboot");
	const char *uboot_alt_part = env_get("recovery_part_uboot_alt");
	unsigned long max = recovery_uboot_limit();
	int ret;

	if (!uboot_part || !*uboot_part) {
		printf("Chainloader eMMC target is not configured. Set recovery_part_uboot explicitly.\n");
		return -EINVAL;
	}

	if (recv_off > max) {
		printf("Chainloader image size %u exceeds limit %lu\n",
		       recv_off, max);
		return -EFBIG;
	}

	ret = fit_check_format(recv_base, recv_off);
	if (ret) {
		printf("Chainloader upload must be a raw FIT image (.itb): %d\n",
		       ret);
		return ret;
	}

	prog_phase = 2;
	prog_done = 0;
	prog_erase_done = 0;
	prog_erase_total = 0;
	prog_write_done = 0;
	prog_write_total = recv_off;
	prog_total = prog_write_total;

	printf("Writing chainloader FIT to eMMC partition '%s'...\n",
	       uboot_part);
	ret = recovery_mmc_write_part(uboot_part, status_leds,
				      recv_base, recv_off, 0);
	if (ret)
		return ret;

	if (uboot_alt_part && *uboot_alt_part) {
		printf("Writing chainloader FIT to alternate eMMC partition '%s'...\n",
		       uboot_alt_part);
		ret = recovery_mmc_write_part(uboot_alt_part, status_leds,
					      recv_base, recv_off, 0);
		if (ret)
			return ret;
	}

	return 0;
}

static int recovery_flash_mmc_target(enum upload_target tgt,
				     struct recovery_status_led_ctrl *status_leds)
{
	switch (tgt) {
	case TARGET_FIRMWARE:
		return recovery_flash_mmc_firmware(status_leds);
	case TARGET_UBOOT:
		return recovery_flash_mmc_uboot(status_leds);
	case TARGET_REPARTITION:
		return recovery_repartition_factory(status_leds);
	}

	return -EINVAL;
}

static __maybe_unused const char *recovery_ubi_part(enum upload_target tgt)
{
	const char *part;

	switch (tgt) {
	case TARGET_UBOOT:
		part = env_get("recovery_ubi_part_uboot");
		break;
	case TARGET_FIRMWARE:
	default:
		part = env_get("recovery_ubi_part");
		break;
	}

	if (!part)
		part = env_get("recovery_ubi_part");

	return part ?: "ubi";
}

static int recovery_try_ubi_target(enum upload_target tgt,
				       struct recovery_target *target)
{
#if IS_ENABLED(CONFIG_CMD_UBI) && IS_ENABLED(CONFIG_MTD_UBI)
	struct ubi_volume_desc *desc;
	struct ubi_device *ubi;
	struct mtd_info *mtd;
	const char *volume = env_get(recovery_target_env(tgt));
	const char *part = recovery_ubi_part(tgt);

	if (!volume)
		volume = recovery_default_target(tgt);

	if (ubi_part((char *)part, NULL)) {
		mtd_probe_devices();
		mtd = get_mtd_device_nm(part);
		if (IS_ERR_OR_NULL(mtd))
			return -ENODEV;

		target->backend = RECOVERY_BACKEND_UBI;
		target->name = volume;
		target->ubi_part = part;
		target->mtd = mtd;
		target->cur_size = 0;
		target->limit = mtd->size;
		target->ubi_needs_format = true;
		return 0;
	}

	desc = ubi_open_volume_nm(0, volume, UBI_READWRITE);
	if (IS_ERR_OR_NULL(desc)) {
		ubi = ubi_get_device(0);
		if (!ubi)
			return -ENODEV;

		target->backend = RECOVERY_BACKEND_UBI;
		target->name = volume;
		target->ubi_part = part;
		target->ubi_needs_format = false;
		target->cur_size = 0;
		target->limit = (unsigned long long)ubi->avail_pebs *
			(unsigned long long)ubi->leb_size;
		ubi_put_device(ubi);
		return 0;
	}

	target->backend = RECOVERY_BACKEND_UBI;
	target->name = volume;
	target->ubi_part = part;
	target->ubi_needs_format = false;
	target->cur_size = (unsigned long long)desc->vol->reserved_pebs *
			   (unsigned long long)desc->vol->usable_leb_size;
	target->limit = (unsigned long long)(desc->vol->reserved_pebs +
					     desc->vol->ubi->avail_pebs) *
			(unsigned long long)desc->vol->usable_leb_size;

	ubi_close_volume(desc);
	return 0;
#else
	return -ENODEV;
#endif
}

static int recovery_resolve_target(enum upload_target tgt,
				   struct recovery_target *target)
{
	const char *name = env_get(recovery_target_env(tgt));
	const char *raw;
	struct mtd_info *mtd;
	ulong ofs;

	memset(target, 0, sizeof(*target));

	if (!name)
		name = recovery_default_target(tgt);

	if (recovery_backend_is_mmc()) {
		struct disk_partition kernel_part, rootfs_part;
		struct blk_desc *desc;
		const char *part;
		int ret;

		target->backend = RECOVERY_BACKEND_MMC;

		if (tgt == TARGET_UBOOT) {
			part = env_get(recovery_mmc_part_env(tgt));
			if (!part || !*part) {
				printf("Chainloader eMMC target is not configured. Set recovery_part_uboot explicitly.\n");
				return -ENODEV;
			}

			ret = recovery_get_mmc_part(part, &target->blk,
						    &target->part);
			if (ret)
				return ret;

			target->name = part;
			target->limit = recovery_mmc_part_bytes(&target->part);
			if (target->limit > recovery_uboot_limit())
				target->limit = recovery_uboot_limit();
			target->cur_size = target->limit;
			return 0;
		}

		part = env_get("recovery_part_kernel") ?: "0#kernel";
		ret = recovery_get_mmc_part(part, &desc, &kernel_part);
		if (ret)
			return ret;

		part = env_get("recovery_part_rootfs") ?: "0#rootfs";
		ret = recovery_get_mmc_part(part, &desc, &rootfs_part);
		if (ret)
			return ret;

		target->name = "mmc-sysupgrade";
		target->blk = desc;
		target->limit = recovery_mmc_part_bytes(&kernel_part) +
				recovery_mmc_part_bytes(&rootfs_part);
		target->cur_size = target->limit;
		return 0;
	}

	mtd_probe_devices();

	raw = env_get(recovery_raw_env(tgt));
	if (raw && *raw) {
		mtd = get_mtd_device_nm(raw);
		if (!IS_ERR_OR_NULL(mtd)) {
			ulong size_cap = env_get_hex(recovery_size_env(tgt), 0);

			ofs = recovery_raw_offset(tgt);
			target->backend = RECOVERY_BACKEND_MTD;
			target->name = raw;
			target->mtd = mtd;
			target->ofs = ofs;
			target->limit = mtd->size;
			if (ofs && target->limit > ofs)
				target->limit -= ofs;
			if (size_cap && target->limit > size_cap)
				target->limit = size_cap;
			target->cur_size = target->limit;
			return 0;
		}
	}

	mtd = get_mtd_device_nm(name);
	if (!IS_ERR_OR_NULL(mtd)) {
		target->backend = RECOVERY_BACKEND_MTD;
		target->name = name;
		target->mtd = mtd;
		target->limit = mtd->size;
		target->cur_size = target->limit;
		return 0;
	}

	if (!recovery_try_ubi_target(tgt, target))
		return 0;

	if (!raw) {
		switch (tgt) {
		case TARGET_FIRMWARE:
			raw = "nor0";
			break;
		case TARGET_UBOOT:
			raw = env_get("recovery_dev");
			if (!raw)
				raw = "nor0";
			break;
		case TARGET_REPARTITION:
			raw = "nor0";
			break;
		}
	}

	mtd = get_mtd_device_nm(raw);
	if (IS_ERR_OR_NULL(mtd))
		return -ENODEV;

	ofs = recovery_raw_offset(tgt);
	target->backend = RECOVERY_BACKEND_MTD;
	target->name = raw;
	target->mtd = mtd;
	target->ofs = ofs;
	target->limit = mtd->size;
	if (ofs && target->limit > ofs)
		target->limit -= ofs;
	{
		ulong size_cap = env_get_hex(recovery_size_env(tgt), 0);

		if (size_cap && target->limit > size_cap)
			target->limit = size_cap;
	}
	target->cur_size = target->limit;

	return 0;
}

static void recovery_release_target(struct recovery_target *target)
{
	if (target->mtd)
		put_mtd_device(target->mtd);

	target->mtd = NULL;
}

static void recovery_service_runtime(struct recovery_status_led_ctrl *status_leds)
{
	struct udevice *udev = eth_get_current();
	struct netif *netif = net_lwip_get_netif();

	if (udev && eth_is_active(udev) && netif)
		net_lwip_rx(udev, netif);
	recovery_status_led_poll(status_leds);
	WATCHDOG_RESET();
}

static __maybe_unused void recovery_ubi_progress(struct ubi_volume *vol, int done, int total)
{
	unsigned long long bytes_done = prog_erase_volume_base;

	(void)vol;

	if (total > 0)
		bytes_done += (prog_erase_volume_bytes * (unsigned long long)done) /
			      (unsigned long long)total;

	if (bytes_done > prog_erase_total)
		bytes_done = prog_erase_total;

	prog_erase_done = bytes_done;
	prog_done = prog_erase_done + prog_write_done;

	if (prog_status_leds)
		recovery_service_runtime(prog_status_leds);
}

static int recovery_erase_mtd_region(struct mtd_info *mtd, loff_t ofs,
				     size_t len,
				     struct recovery_status_led_ctrl *status_leds)
{
	struct erase_info ei = { 0 };
	loff_t erase_len;
	int ret;

	if (!mtd || !len)
		return 0;

	erase_len = ALIGN(len, mtd->erasesize);
	ei.addr = ofs;
	ei.len = erase_len;

	ret = mtd_unlock(mtd, ei.addr, ei.len);
	if (ret && ret != -EOPNOTSUPP) {
		printf("Warning: initial mtd_unlock 0x%llx..+0x%llx failed: %d\n",
		       (unsigned long long)ei.addr,
		       (unsigned long long)ei.len, ret);
	}

	for (loff_t addr = 0; addr < erase_len; addr += mtd->erasesize) {
		struct erase_info e = {
			.addr = ofs + addr,
			.len = mtd->erasesize,
		};
		int tries = 0;

		do {
			ret = mtd_erase(mtd, &e);
			if (!ret)
				break;
			if (ret == -EROFS || ret == -EACCES)
				mtd_unlock(mtd, e.addr, e.len);
			else
				break;
		} while (++tries < 2);

		if (ret)
			return ret;

		prog_erase_done = addr + mtd->erasesize;
		if (prog_erase_done > prog_erase_total)
			prog_erase_done = prog_erase_total;
		prog_done = prog_erase_done + prog_write_done;
		recovery_service_runtime(status_leds);
	}

	return 0;
}

static __maybe_unused bool recovery_preserve_ubi_volume(const char *name)
{
	if (!name || !*name)
		return true;

	if (!strcmp(name, "ubootenv") || !strcmp(name, "ubootenv2"))
		return true;

	if (of_machine_is_compatible("econet,xr1710g") ||
	    of_machine_is_compatible("econet,xr1710g-ubi") ||
	    of_machine_is_compatible("gemtek,xr1710g") ||
	    of_machine_is_compatible("gemtek,xr1710g-ubi"))
		return !strcmp(name, "factory");

	if (of_machine_is_compatible("gemtek,w1700k") ||
	    of_machine_is_compatible("gemtek,w1700k-ubi"))
		return !strcmp(name, "factory");

	return false;
}

static unsigned long long
recovery_calc_forced_ubi_limit(struct recovery_target *target)
{
#if IS_ENABLED(CONFIG_CMD_UBI) && IS_ENABLED(CONFIG_MTD_UBI)
	struct ubi_device *ubi;
	unsigned long long limit;
	int i;

	if (target->backend != RECOVERY_BACKEND_UBI)
		return target->limit;

	if (ubi_part((char *)target->ubi_part, NULL))
		return target->limit;

	ubi = ubi_get_device(0);
	if (!ubi)
		return target->limit;

	limit = (unsigned long long)ubi->avail_pebs *
		(unsigned long long)ubi->leb_size;

	for (i = 0; i < ubi->vtbl_slots; i++) {
		struct ubi_volume *vol = ubi->volumes[i];

		if (!vol || vol->vol_id >= UBI_INTERNAL_VOL_START)
			continue;
		if (recovery_preserve_ubi_volume(vol->name))
			continue;

		limit += (unsigned long long)vol->reserved_pebs *
			 (unsigned long long)vol->usable_leb_size;
	}

	ubi_put_device(ubi);
	return limit;
#else
	return target->limit;
#endif
}

static __maybe_unused int recovery_create_ubi_volume(const char *name, size_t size, int vol_type)
{
#if IS_ENABLED(CONFIG_CMD_UBI) && IS_ENABLED(CONFIG_MTD_UBI)
	struct ubi_mkvol_req req;
	struct ubi_device *ubi;
	int ret;

	if (!name || !*name)
		return -EINVAL;

	ubi = ubi_get_device(0);
	if (!ubi)
		return -ENODEV;

	if (!size)
		size = (size_t)ubi->avail_pebs * (size_t)ubi->leb_size;

	memset(&req, 0, sizeof(req));
	req.vol_id = UBI_VOL_NUM_AUTO;
	req.alignment = 1;
	req.bytes = size;
	req.vol_type = vol_type;
	req.name_len = strlen(name);
	if (req.name_len > UBI_VOL_NAME_MAX) {
		ubi_put_device(ubi);
		return -ENAMETOOLONG;
	}
	memcpy(req.name, name, req.name_len);
	req.name[req.name_len] = '\0';

	mutex_lock(&ubi->device_mutex);
	ret = ubi_create_volume(ubi, &req);
	mutex_unlock(&ubi->device_mutex);
	ubi_put_device(ubi);

	return ret;
#else
	return -ENODEV;
#endif
}

static __maybe_unused int recovery_remove_ubi_volume(const char *name)
{
#if IS_ENABLED(CONFIG_CMD_UBI) && IS_ENABLED(CONFIG_MTD_UBI)
	struct ubi_volume_desc *desc;
	int ret;

	desc = ubi_open_volume_nm(0, name, UBI_EXCLUSIVE);
	if (IS_ERR_OR_NULL(desc))
		return IS_ERR(desc) ? PTR_ERR(desc) : -ENODEV;

	mutex_lock(&desc->vol->ubi->device_mutex);
	ret = ubi_remove_volume(desc, 0);
	mutex_unlock(&desc->vol->ubi->device_mutex);
	ubi_close_volume(desc);

	return ret;
#else
	return -ENODEV;
#endif
}

static int recovery_ensure_rootfs_data(struct recovery_target *target)
{
#if IS_ENABLED(CONFIG_CMD_UBI) && IS_ENABLED(CONFIG_MTD_UBI)
	struct ubi_volume_desc *desc;
	int ret;

	if (target->backend != RECOVERY_BACKEND_UBI)
		return 0;

	if (ubi_part((char *)target->ubi_part, NULL))
		return -ENODEV;

	desc = ubi_open_volume_nm(0, "rootfs_data", UBI_READWRITE);
	if (!IS_ERR_OR_NULL(desc)) {
		ubi_close_volume(desc);
		return 0;
	}

	ret = recovery_create_ubi_volume("rootfs_data", 0, UBI_DYNAMIC_VOLUME);
	if (ret && ret != -EEXIST) {
		printf("Failed to create UBI volume 'rootfs_data': %d\n", ret);
		return ret;
	}

	return 0;
#else
	return -ENODEV;
#endif
}

static int recovery_prepare_ubi_target(struct recovery_target *target,
				       struct recovery_status_led_ctrl *status_leds,
				       size_t image_size, bool *reformatted)
{
#if IS_ENABLED(CONFIG_CMD_UBI) && IS_ENABLED(CONFIG_MTD_UBI)
	struct ubi_device *ubi;
	loff_t erase_len;
	int ret;

	if (reformatted)
		*reformatted = false;

	if (target->backend != RECOVERY_BACKEND_UBI)
		return -EINVAL;

	if (!target->ubi_needs_format)
		return ubi_part((char *)target->ubi_part, NULL) ? -ENODEV : 0;

	if (!target->mtd)
		return -ENODEV;

	erase_len = ALIGN(target->mtd->size, target->mtd->erasesize);
	prog_phase = 1;
	prog_done = 0;
	prog_erase_done = 0;
	prog_erase_total = erase_len;
	prog_write_done = 0;
	prog_write_total = image_size;
	prog_total = prog_erase_total + prog_write_total;

	printf("UBI partition '%s' is invalid, erasing it to recreate layout...\n",
	       target->ubi_part);
	ret = recovery_erase_mtd_region(target->mtd, 0, target->mtd->size,
					status_leds);
	if (ret)
		return ret;

	ret = ubi_part((char *)target->ubi_part, NULL);
	if (ret)
		return ret;

	ubi = ubi_get_device(0);
	if (ubi) {
		target->limit = (unsigned long long)ubi->avail_pebs *
				(unsigned long long)ubi->leb_size;
		ubi_put_device(ubi);
	}

	target->cur_size = 0;
	target->ubi_needs_format = false;
	if (reformatted)
		*reformatted = true;

	return 0;
#else
	return -ENODEV;
#endif
}

static int recovery_ensure_ubi_volume_named(const char *name, size_t size,
					    int vol_type)
{
#if IS_ENABLED(CONFIG_CMD_UBI) && IS_ENABLED(CONFIG_MTD_UBI)
	struct ubi_volume_desc *desc;
	int ret;

	if (!name || !*name)
		return -EINVAL;

	desc = ubi_open_volume_nm(0, name, UBI_READWRITE);
	if (!IS_ERR_OR_NULL(desc)) {
		ubi_close_volume(desc);
		return 0;
	}

	ret = recovery_create_ubi_volume(name, size, vol_type);
	if (ret && ret != -EEXIST) {
		printf("Failed to create UBI volume '%s': %d\n", name, ret);
		return ret;
	}

	return 0;
#else
	return -ENODEV;
#endif
}

static int recovery_ensure_preserved_ubi_volumes(struct recovery_target *target)
{
	int ret;

	if (target->backend != RECOVERY_BACKEND_UBI)
		return 0;

	if (ubi_part((char *)target->ubi_part, NULL))
		return -ENODEV;

	ret = recovery_ensure_ubi_volume_named("ubootenv",
					       RECOVERY_UBOOTENV_SIZE,
					       UBI_DYNAMIC_VOLUME);
	if (ret)
		return ret;

	ret = recovery_ensure_ubi_volume_named("ubootenv2",
					       RECOVERY_UBOOTENV_SIZE,
					       UBI_DYNAMIC_VOLUME);
	if (ret)
		return ret;

	if (of_machine_is_compatible("econet,xr1710g") ||
	    of_machine_is_compatible("econet,xr1710g-ubi") ||
	    of_machine_is_compatible("gemtek,xr1710g") ||
	    of_machine_is_compatible("gemtek,xr1710g-ubi") ||
	    of_machine_is_compatible("gemtek,w1700k") ||
	    of_machine_is_compatible("gemtek,w1700k-ubi")) {
		ret = recovery_ensure_ubi_volume_named("factory",
						       RECOVERY_FACTORY_SIZE,
						       UBI_STATIC_VOLUME);
		if (ret)
			return ret;
	}

	return 0;
}

static int recovery_cleanup_ubi_firmware(struct recovery_target *target,
					 struct recovery_status_led_ctrl *status_leds,
					 size_t image_size)
{
#if IS_ENABLED(CONFIG_CMD_UBI) && IS_ENABLED(CONFIG_MTD_UBI)
	struct ubi_device *ubi;
	unsigned long long erase_total = 0;
	unsigned long long erase_done = 0;
	int ret = 0;
	int i;

	if (target->backend != RECOVERY_BACKEND_UBI)
		return -EINVAL;

	if (ubi_part((char *)target->ubi_part, NULL))
		return -ENODEV;

	ubi = ubi_get_device(0);
	if (!ubi)
		return -ENODEV;

	for (i = 0; i < ubi->vtbl_slots; i++) {
		struct ubi_volume *vol = ubi->volumes[i];

		if (!vol || vol->vol_id >= UBI_INTERNAL_VOL_START)
			continue;
		if (recovery_preserve_ubi_volume(vol->name))
			continue;

		erase_total += (unsigned long long)vol->reserved_pebs *
			       (unsigned long long)vol->usable_leb_size;
	}

	prog_phase = 1;
	prog_done = 0;
	prog_erase_done = 0;
	prog_erase_total = erase_total > UINT_MAX ? UINT_MAX : erase_total;
	prog_write_done = 0;
	prog_write_total = image_size;
	prog_total = prog_erase_total + prog_write_total;
	prog_status_leds = status_leds;
	ubi_set_progress_callback(recovery_ubi_progress);

	for (i = 0; i < ubi->vtbl_slots; i++) {
		struct ubi_volume *vol = ubi->volumes[i];
		unsigned long long vol_size;
		char name[UBI_VOL_NAME_MAX + 1];

		if (!vol || vol->vol_id >= UBI_INTERNAL_VOL_START)
			continue;
		if (recovery_preserve_ubi_volume(vol->name))
			continue;

		vol_size = (unsigned long long)vol->reserved_pebs *
			   (unsigned long long)vol->usable_leb_size;
		strlcpy(name, vol->name, sizeof(name));
		prog_erase_volume_base = erase_done;
		prog_erase_volume_bytes = vol_size;

		printf("Removing UBI volume '%s' before flashing '%s'...\n",
		       name, target->name);
		ret = recovery_remove_ubi_volume(name);
		if (ret) {
			printf("Failed to remove UBI volume '%s': %d\n", name, ret);
			break;
		}

		erase_done += vol_size;
		prog_erase_done = erase_done > prog_erase_total ?
				  prog_erase_total : erase_done;
		prog_done = prog_erase_done + prog_write_done;
		recovery_service_runtime(status_leds);
	}

	ubi_set_progress_callback(NULL);
	prog_status_leds = NULL;
	ubi_put_device(ubi);
	if (ret)
		return ret;

	return recovery_try_ubi_target(current_target, target);
#else
	return -ENODEV;
#endif
}

static int recovery_write_ubi_target(struct recovery_target *target,
				      struct recovery_status_led_ctrl *status_leds,
				      const void *image, size_t image_size)
{
#if IS_ENABLED(CONFIG_CMD_UBI) && IS_ENABLED(CONFIG_MTD_UBI)
	struct ubi_volume_desc *desc;
	struct ubi_volume *vol;
	struct ubi_device *ubi;
	const u8 *src = image;
	size_t reserved_bytes;
	u32 written = 0;
	int ret;

	if (target->backend != RECOVERY_BACKEND_UBI)
		return -EINVAL;

	if (ubi_part((char *)target->ubi_part, NULL))
		return -ENODEV;

	desc = ubi_open_volume_nm(0, target->name, UBI_READWRITE);
	if (IS_ERR_OR_NULL(desc))
		return IS_ERR(desc) ? PTR_ERR(desc) : -ENODEV;

	vol = desc->vol;
	ubi = vol->ubi;
	reserved_bytes = (size_t)vol->reserved_pebs *
			 (size_t)(ubi->leb_size - vol->data_pad);
	if (image_size > reserved_bytes) {
		ret = -EFBIG;
		goto out_close;
	}

	ret = ubi_start_update(ubi, vol, image_size);
	if (ret)
		goto out_close;

	/* Flush one runtime cycle so the browser can observe phase 2 before
	 * the actual UBI data write loop starts.
	 */
	recovery_service_runtime(status_leds);

	while (written < image_size) {
		u32 chunk = image_size - written;

		if (chunk > RECOVERY_UBI_WRITE_CHUNK)
			chunk = RECOVERY_UBI_WRITE_CHUNK;

		ret = ubi_more_update_data(ubi, vol, src + written, chunk);
		if (ret < 0)
			goto out_close;

		written += chunk;
		prog_write_done = written > prog_write_total ?
				  prog_write_total : written;
		prog_done = prog_erase_done + prog_write_done;
		recovery_service_runtime(status_leds);
	}

	ret = ubi_check_volume(ubi, vol->vol_id);
	if (ret < 0) {
		ret = -ret;
		goto out_close;
	}

	if (ret) {
		ubi_warn(ubi, "volume %d on UBI device %d is corrupt",
			 vol->vol_id, ubi->ubi_num);
		vol->corrupted = 1;
	}

	vol->checked = 1;
	ubi_gluebi_updated(vol);
	ret = 0;

out_close:
	ubi_close_volume(desc);
	return ret;
#else
	return -ENODEV;
#endif
}

static int recovery_resize_ubi_target(struct recovery_target *target,
				      size_t new_size)
{
#if IS_ENABLED(CONFIG_CMD_UBI) && IS_ENABLED(CONFIG_MTD_UBI)
	struct ubi_volume_desc *desc;
	struct ubi_volume *vol;
	int needed_pebs;
	int ret;

	if (target->backend != RECOVERY_BACKEND_UBI)
		return -EINVAL;

	if (ubi_part((char *)target->ubi_part, NULL))
		return -ENODEV;

	desc = ubi_open_volume_nm(0, target->name, UBI_EXCLUSIVE);
	if (IS_ERR_OR_NULL(desc))
		return IS_ERR(desc) ? PTR_ERR(desc) : -ENODEV;

	vol = desc->vol;
	needed_pebs = DIV_ROUND_UP(new_size, vol->usable_leb_size);

	mutex_lock(&vol->ubi->device_mutex);
	ret = ubi_resize_volume(desc, needed_pebs);
	mutex_unlock(&vol->ubi->device_mutex);

	if (!ret) {
		target->cur_size = (unsigned long long)needed_pebs *
				   (unsigned long long)vol->usable_leb_size;
		target->limit = (unsigned long long)(needed_pebs +
						     vol->ubi->avail_pebs) *
			(unsigned long long)vol->usable_leb_size;
	}

	ubi_close_volume(desc);
	return ret;
#else
	return -ENODEV;
#endif
}

static int recovery_create_ubi_target(struct recovery_target *target,
				      size_t new_size)
{
#if IS_ENABLED(CONFIG_CMD_UBI) && IS_ENABLED(CONFIG_MTD_UBI)
	struct ubi_mkvol_req req;
	struct ubi_device *ubi;
	int ret;

	if (target->backend != RECOVERY_BACKEND_UBI)
		return -EINVAL;

	if (ubi_part((char *)target->ubi_part, NULL))
		return -ENODEV;

	ubi = ubi_get_device(0);
	if (!ubi)
		return -ENODEV;

	memset(&req, 0, sizeof(req));
	req.vol_id = UBI_VOL_NUM_AUTO;
	req.alignment = 1;
	req.bytes = new_size;
	req.vol_type = UBI_STATIC_VOLUME;
	req.name_len = strlen(target->name);
	if (req.name_len > UBI_VOL_NAME_MAX) {
		ubi_put_device(ubi);
		return -ENAMETOOLONG;
	}
	memcpy(req.name, target->name, req.name_len);
	req.name[req.name_len] = '\0';

	mutex_lock(&ubi->device_mutex);
	ret = ubi_create_volume(ubi, &req);
	mutex_unlock(&ubi->device_mutex);
	ubi_put_device(ubi);
	if (ret)
		return ret;

	return recovery_try_ubi_target(current_target, target);
#else
	return -ENODEV;
#endif
}

/* Determine maximum payload size based on selected target and DTS-defined MTD
 * layout. Returns 0 on error. */
static unsigned long recovery_calc_target_max(enum upload_target tgt,
					      bool force_recreate, loff_t *p_ofs)
{
	struct recovery_target target;
	unsigned long limit = 0;
	unsigned long long effective_limit;

	if (recovery_resolve_target(tgt, &target))
		return 0;

	if (p_ofs)
		*p_ofs = target.ofs;

	effective_limit = target.limit;
	if (force_recreate && tgt == TARGET_FIRMWARE)
		effective_limit = recovery_calc_forced_ubi_limit(&target);

	limit = (effective_limit > ULONG_MAX) ? ULONG_MAX :
		(unsigned long)effective_limit;
	recovery_release_target(&target);

	return limit;
}

/* Only dynamic endpoints here; static files come from fsdata */

/* lwIP httpd custom file hooks: serve only dynamic endpoints; static files via fsdata */
int fs_open_custom(struct fs_file *file, const char *name)
{
    const char *p;
    if (!file || !name)
        return 0;

    /* Normalize leading slash to make httpd default filenames work */
    p = name;
    if (*p == '/')
        p++;


	    if (!strcmp(p, "status")) {
        static char page_status[512];
        char json[256];
        int ok = (prog_phase == 3);
        int err = (prog_phase == -1);
        int json_len = snprintf(json, sizeof(json),
                                "{\"in_progress\":%d,\"done\":%u,\"total\":%u,\"erase_done\":%u,\"erase_total\":%u,\"write_done\":%u,\"write_total\":%u,\"ok\":%d,\"error\":%d,\"phase\":%d,\"reboot\":%d}\n",
                                prog_phase > 0 && prog_phase < 3,
                                (unsigned)prog_done, (unsigned)prog_total,
                                (unsigned)prog_erase_done, (unsigned)prog_erase_total,
                                (unsigned)prog_write_done, (unsigned)prog_write_total,
                                ok, err, prog_phase, prog_reboot);
        if (json_len < 0)
            return 0;
        if (json_len >= (int)sizeof(json))
            json_len = (int)sizeof(json) - 1;

        /* Build header with Content-Length for robustness */
        int hdr_len = snprintf(page_status, sizeof(page_status),
                               "HTTP/1.0 200 OK\r\nContent-Type: application/json\r\nCache-Control: no-store\r\nContent-Length: %d\r\nConnection: close\r\n\r\n",
                               json_len);
        if (hdr_len < 0)
            return 0;
        if ((size_t)hdr_len >= sizeof(page_status))
            hdr_len = sizeof(page_status) - 1;
        /* Append body */
        size_t space = sizeof(page_status) - hdr_len - 1;
        size_t body_len = (json_len > (int)space) ? space : (size_t)json_len;
        memcpy(page_status + hdr_len, json, body_len);
        page_status[hdr_len + body_len] = '\0';

        file->data = page_status;
        file->len = hdr_len + body_len; /* actual header + body written */
        file->index = 0;
	        file->flags = FS_FILE_FLAGS_HEADER_INCLUDED; /* header already included */
	        return 1;
	    }
	    else if (!strcmp(p, "ok")) {
	        static const char page_ok[] =
	            "HTTP/1.0 200 OK\r\n"
	            "Content-Type: text/plain\r\n"
	            "Cache-Control: no-store\r\n"
	            "Content-Length: 2\r\n"
	            "Connection: close\r\n"
	            "\r\n"
	            "OK";

	        file->data = page_ok;
	        file->len = sizeof(page_ok) - 1;
	        file->index = 0;
	        file->flags = FS_FILE_FLAGS_HEADER_INCLUDED;
	        return 1;
	    }
	    else if (!strcmp(p, "about")) {
        static char page_about[384];
        char json[256];
        int json_len =
#ifdef U_BOOT_DATE
            snprintf(json, sizeof(json),
                     "{\"u_boot\":\"%s (%s - %s %s)\"}\n",
                     U_BOOT_VERSION, U_BOOT_DATE, U_BOOT_TIME, U_BOOT_TZ);
#else
            snprintf(json, sizeof(json),
                     "{\"u_boot\":\"%s\"}\n",
                     U_BOOT_VERSION);
#endif
        if (json_len < 0)
            return 0;
        if (json_len >= (int)sizeof(json))
            json_len = (int)sizeof(json) - 1;

        int hdr_len = snprintf(page_about, sizeof(page_about),
                               "HTTP/1.0 200 OK\r\nContent-Type: application/json\r\nCache-Control: no-store\r\nContent-Length: %d\r\nConnection: close\r\n\r\n",
                               json_len);
        if (hdr_len < 0)
            return 0;
        if ((size_t)hdr_len >= sizeof(page_about))
            hdr_len = sizeof(page_about) - 1;
        size_t space = sizeof(page_about) - hdr_len - 1;
        size_t body_len = (json_len > (int)space) ? space : (size_t)json_len;
        memcpy(page_about + hdr_len, json, body_len);
        page_about[hdr_len + body_len] = '\0';

        file->data = page_about;
        file->len = hdr_len + body_len;
        file->index = 0;
        file->flags = FS_FILE_FLAGS_HEADER_INCLUDED;
        return 1;
    }
    /* Do not intercept favicon/index/ok/fail: served by fsdata */
    /* let fsdata handle others */
    return 0;
}

void fs_close_custom(struct fs_file *file)
{
    (void)file;
}

int fs_read_custom(struct fs_file *file, char *buffer, int count)
{
    u32_t left;
    if (!file || !buffer || count <= 0)
        return FS_READ_EOF;
    left = file->len - file->index;
    if (left <= 0)
        return FS_READ_EOF;
    if ((u32_t)count > left)
        count = left;
    memcpy(buffer, file->data + file->index, count);
    file->index += count;
    return count;
}

/* bytes_left for custom files is handled by fs_read_custom + file->index; */

/* HTTP POST handlers */
err_t httpd_post_begin(void *connection, const char *uri, const char *http_request,
                       u16_t http_request_len, int content_len, char *response_uri,
                       u16_t response_uri_len, u8_t *post_auto_wnd)
{
    (void)http_request; (void)http_request_len; (void)connection;
    /*
     * Throttle large uploads explicitly: XR1710G recovery accepts firmware
     * images that are much larger than a typical lwIP POST body and manual
     * window updates avoid over-optimistic receive windows.
     */
    if (post_auto_wnd)
        *post_auto_wnd = 0;

    post_ok = 0;
    upload_done = 0;
    recv_off = 0;
    recv_total = 0;
    /* Reset progress state. */
    prog_phase = 0;
    prog_done = 0;
    prog_total = 0;
    prog_erase_done = 0;
	prog_erase_total = 0;
	prog_write_done = 0;
	prog_write_total = 0;
	prog_reboot = 0;
	current_force_recreate = false;

	    /* Accept optional query parameters after the target path. */
    if (!strncmp(uri, "/upload/firmware", 16) && (uri[16] == '\0' || uri[16] == '?'))
        current_target = TARGET_FIRMWARE;
    else if (!strncmp(uri, "/upload/uboot", 13) && (uri[13] == '\0' || uri[13] == '?'))
        current_target = TARGET_UBOOT;
    else if (!strncmp(uri, "/action/repartition", 19) && (uri[19] == '\0' || uri[19] == '?'))
        current_target = TARGET_REPARTITION;
    else if (!strncmp(uri, "/upload", 7) && (uri[7] == '\0' || uri[7] == '?'))
        current_target = TARGET_FIRMWARE;
    else {
        prog_phase = -1;
        strlcpy(response_uri, "/fail.html", response_uri_len);
        return ERR_ARG;
    }

    if (current_target == TARGET_FIRMWARE &&
        (strstr(uri, "?recreate=1") || strstr(uri, "&recreate=1") ||
         strstr(uri, "?force_recreate=1") || strstr(uri, "&force_recreate=1")))
        current_force_recreate = true;

    if (current_target == TARGET_REPARTITION) {
        if (content_len <= 0 || (ulong)content_len > RECOVERY_REPARTITION_MAX) {
            prog_phase = -1;
            printf("httpd: invalid repartition request length %d\n",
                   content_len);
            strlcpy(response_uri, "/fail.html", response_uri_len);
            return ERR_ARG;
        }
    } else {
        ulong min = 0;
        ulong env_max = env_get_hex("recovery_max", 0);
        loff_t tmpofs = 0;
        ulong dts_max = recovery_calc_target_max(current_target,
                                                 current_force_recreate,
                                                 &tmpofs);
        ulong max = dts_max ? dts_max : RECOVERY_UPLOAD_MAX;

        if (current_target == TARGET_FIRMWARE)
            min = RECOVERY_MIN_FIRMWARE_SIZE;
        else if (current_target == TARGET_UBOOT &&
                 max > recovery_uboot_limit())
            max = recovery_uboot_limit();

        if (env_max && env_max < max)
            max = env_max; /* allow env to further cap */
        if (content_len <= 0 || (ulong)content_len > max ||
            (min && (ulong)content_len < min)) {
            prog_phase = -1;
            if (min && (ulong)content_len < min) {
                printf("httpd: content_len %d below allowed min %lu for target %d\n",
                       content_len, min, current_target);
            } else {
                printf("httpd: content_len %d exceeds allowed max %lu (target limit %lu, ofs 0x%llx)\n",
                       content_len, max, dts_max, (unsigned long long)tmpofs);
            }
            strlcpy(response_uri, "/fail.html", response_uri_len);
            return ERR_ARG;
        }
    }

    recv_total = content_len;

    /*
     * Pick a stable upload buffer:
     * recovery_addr -> loadaddr -> CONFIG_SYS_LOAD_ADDR -> RAM fallback.
     */
    {
        ulong ram_start = (ulong)gd->ram_base;
        ulong ram_end = (ulong)gd->ram_base + (ulong)gd->ram_size;
        ulong base = env_get_hex("recovery_addr", 0);
        if (!base)
            base = env_get_hex("loadaddr", 0);
        if (!base)
            base = CONFIG_SYS_LOAD_ADDR;

        /* Keep the buffer inside usable RAM. */
        if (base < ram_start || (base + recv_total) > ram_end) {
            ulong fallback = ram_start + 0x01000000UL;
            if ((fallback >= ram_start) && ((fallback + recv_total) <= ram_end))
                base = fallback;
            else if ((ram_end - recv_total) > ram_start)
                base = ram_end - recv_total;
            else {
                prog_phase = -1;
                printf("httpd: no sufficient RAM for upload (%u bytes)\n", recv_total);
                strlcpy(response_uri, "/fail.html", response_uri_len);
                return ERR_MEM;
            }
        }
        recv_base = (u8 *)base;
    }

    post_ok = 1;
    /* Leave response_uri untouched here so the POST can complete normally. */
    const char *tname = current_target == TARGET_FIRMWARE ? "firmware" :
                        current_target == TARGET_UBOOT ? "uboot" :
                        "repartition";
    printf("httpd: accepting upload of %u bytes for %s to 0x%08lx%s\n",
           recv_total, tname, (ulong)recv_base,
           current_force_recreate ? " (force recreate)" : "");
    return ERR_OK;
}

err_t httpd_post_receive_data(void *connection, struct pbuf *p)
{
	struct pbuf *q;
	u16_t recved = 0;

	if (!post_ok) {
		pbuf_free(p);
		return ERR_ARG;
	}

	/* Copy request payload into RAM and defer flash work to the main loop. */
	for (q = p; q != NULL; q = q->next) {
        size_t avail = recv_total - recv_off;
        size_t clen = q->len;
	        if (clen > avail)
	            clen = avail;
	        memcpy(recv_base + recv_off, q->payload, clen);
	        recv_off += clen;
    }
    recved = p->tot_len;
    pbuf_free(p);

    if (recv_off >= recv_total) {
        upload_done = 1;
    }

#if LWIP_HTTPD_POST_MANUAL_WND
    if (recved)
        httpd_post_data_recved(connection, recved);
#endif

    return ERR_OK;
}

void httpd_post_finished(void *connection, char *response_uri, u16_t response_uri_len)
{
    (void)connection;
    printf("httpd: post finished, %u/%u bytes received\n", recv_off, recv_total);
    /* Tell httpd which page to return after POST (keep user on main page) */
    if (post_ok && recv_total && (recv_off >= recv_total))
        strlcpy(response_uri, "/ok", response_uri_len);
    else
        strlcpy(response_uri, "/fail.html", response_uri_len);

    /*
     * Delay flashing slightly so the browser can finish receiving the POST
     * response before erase/write work blocks the network loop.
     */
    if (post_ok && recv_total && (recv_off >= recv_total))
        sys_timeout(FLASH_START_DELAY_MS, post_delay_cb, NULL);
}

static int flash_image(struct recovery_status_led_ctrl *status_leds)
{
	struct recovery_target target;
	size_t retlen;
	int ret;

	if (!recv_off) {
		printf("No data received to flash\n");
		prog_phase = -1;
		return -EINVAL;
	}

	if (current_target == TARGET_REPARTITION) {
		prog_reboot = 0;
		ret = recovery_repartition_factory(status_leds);
		if (ret) {
			printf("Factory repartition failed: %d\n", ret);
			prog_phase = -1;
			return ret;
		}

		prog_phase = 3;
		printf("Factory repartition complete.\n");
		return 0;
	}

	ret = recovery_resolve_target(current_target, &target);
	if (ret) {
		printf("No flash target found for upload type %d\n", current_target);
		prog_phase = -1;
		return ret;
	}

	if (current_force_recreate && current_target == TARGET_FIRMWARE &&
	    target.backend == RECOVERY_BACKEND_UBI)
		target.limit = recovery_calc_forced_ubi_limit(&target);

	if (recv_off > target.limit) {
		printf("Image size %u exceeds target size %llu\n",
		       recv_off, target.limit);
		recovery_release_target(&target);
		prog_phase = -1;
		return -EFBIG;
	}

	if (target.backend == RECOVERY_BACKEND_MMC) {
		ret = recovery_flash_mmc_target(current_target, status_leds);
		recovery_release_target(&target);
		if (ret) {
			printf("eMMC flash failed: %d\n", ret);
			prog_phase = -1;
			return ret;
		}

		prog_phase = 3;
		prog_reboot = 1;
		printf("Flashing complete.\n");
		return 0;
	}

	if (target.backend == RECOVERY_BACKEND_UBI) {
		bool reformatted = false;

		if (current_force_recreate && current_target == TARGET_FIRMWARE)
			printf("Force recreate requested: removing non-preserved UBI volumes before flashing.\n");

		ret = recovery_prepare_ubi_target(&target, status_leds, recv_off,
						      &reformatted);
		if (ret) {
			printf("Failed to prepare UBI target '%s' on '%s': %d\n",
			       target.name, target.ubi_part, ret);
			recovery_release_target(&target);
			prog_phase = -1;
			return ret;
		}

		if (!reformatted) {
			ret = recovery_cleanup_ubi_firmware(&target, status_leds,
							    recv_off);
			if (ret) {
				printf("Failed to clean UBI firmware volumes for '%s': %d\n",
				       target.name, ret);
				recovery_release_target(&target);
				prog_phase = -1;
				return ret;
			}
		}

		if (!target.cur_size) {
			printf("Creating missing UBI volume '%s' for %u bytes...\n",
			       target.name, recv_off);
			ret = recovery_create_ubi_target(&target, recv_off);
			if (ret) {
				printf("ubi_create_volume failed for '%s': %d\n",
				       target.name, ret);
				prog_phase = -1;
				return ret;
			}
		} else if (recv_off > target.cur_size) {
			printf("Resizing UBI volume '%s' from %llu to fit %u bytes...\n",
			       target.name, target.cur_size, recv_off);
			ret = recovery_resize_ubi_target(&target, recv_off);
			if (ret) {
				printf("ubi_resize_volume failed for '%s': %d\n",
				       target.name, ret);
				prog_phase = -1;
				return ret;
			}
		}

		ret = recovery_ensure_preserved_ubi_volumes(&target);
		if (ret) {
			recovery_release_target(&target);
			prog_phase = -1;
			return ret;
		}

		ret = recovery_ensure_rootfs_data(&target);
		if (ret) {
			recovery_release_target(&target);
			prog_phase = -1;
			return ret;
		}

			printf("Writing %u bytes to UBI volume '%s' on '%s'...\n",
			       recv_off, target.name, target.ubi_part);
			prog_phase = 2;
			prog_done = prog_erase_total;
			prog_write_done = 0;
			recovery_service_runtime(status_leds);
			ret = recovery_write_ubi_target(&target, status_leds,
							recv_base, recv_off);
			if (ret) {
				printf("UBI write failed for '%s': %d\n",
				       target.name, ret);
				recovery_release_target(&target);
				prog_phase = -1;
			return ret;
		}

		prog_write_done = recv_off;
		prog_done = prog_erase_total + recv_off;
		recovery_release_target(&target);
		prog_phase = 3;
		prog_reboot = 1;
		if (current_target == TARGET_FIRMWARE)
			xr1710g_sync_factory();
		printf("Flashing complete.\n");
		return 0;
	}

	{
		struct mtd_info *mtd = target.mtd;
		loff_t ofs = target.ofs;
		loff_t erase_len = ALIGN(target.limit, mtd->erasesize);

		prog_phase = 1;
		prog_done = 0;
		prog_erase_done = 0;
		prog_erase_total = erase_len;
		prog_write_done = 0;
		prog_write_total = recv_off;
		prog_total = prog_erase_total + prog_write_total;

		printf("Erasing entire target '%s' (%llu bytes) and writing %u bytes...\n",
		       target.name, (unsigned long long)target.limit, recv_off);
		ret = recovery_erase_mtd_region(mtd, ofs, target.limit,
						status_leds);
		if (ret) {
			printf("mtd_erase failed: %d\n", ret);
			recovery_release_target(&target);
			prog_phase = -1;
			return ret;
		}

		prog_phase = 2;
		for (u32 written = 0; written < recv_off; ) {
			u32 remain = recv_off - written;
			u32 chunk = remain > (64 * 1024) ? (64 * 1024) : remain;

			ret = mtd_write(mtd, ofs + written, chunk, &retlen,
					recv_base + written);
			if (ret) {
				printf("mtd_write failed: ret=%d at 0x%llx\n",
				       ret, (unsigned long long)(ofs + written));
				recovery_release_target(&target);
				prog_phase = -1;
				return ret;
			}
			if (!retlen) {
				printf("mtd_write made no progress at 0x%llx\n",
				       (unsigned long long)(ofs + written));
				recovery_release_target(&target);
				prog_phase = -1;
				return -EIO;
			}

			written += retlen;
			prog_write_done = written;
			prog_done = prog_erase_done + prog_write_done;
			recovery_service_runtime(status_leds);
		}
	}

	recovery_release_target(&target);
	prog_phase = 3;
	prog_reboot = 1;
	if (current_target == TARGET_FIRMWARE)
		xr1710g_sync_factory();
	printf("Flashing complete.\n");
	return 0;
}

int run_http_recovery(void)
{
	struct udevice *udev = NULL;
	struct netif *netif = NULL;
	struct recovery_led_ctrl leds;
	struct recovery_status_led_ctrl status_leds;
	struct recovery_dhcp_server dhcp;
	bool use_status_leds = false;
	bool use_link_leds = false;
	bool eth_started = false;
	const char *old_allow_no_link;
	char *saved_allow_no_link = NULL;
	ulong timeout_ms;
	ulong start_ms;
	int rc;

	old_allow_no_link = env_get("eth_allow_no_link");
	if (old_allow_no_link) {
		saved_allow_no_link = strdup(old_allow_no_link);
		if (!saved_allow_no_link)
			return -ENOMEM;
	}
	env_set("eth_allow_no_link", "1");
	recovery_board_http_acl(true);

	recv_off = recv_total = 0;
	post_ok = 0;
	upload_done = 0;
	flash_request = 0;
	reboot_request = 0;
	memset(&leds, 0, sizeof(leds));
	memset(&status_leds, 0, sizeof(status_leds));
	memset(&dhcp, 0, sizeof(dhcp));

	printf("HTTP recovery: preparing board runtime\n");
	recovery_watchdog_poll();
	if (recovery_board_is_sbe1v1k()) {
		printf("Recovery LEDs disabled on SBE1V1K\n");
	} else {
		rc = recovery_status_led_init(&status_leds);
		if (!rc) {
			use_status_leds = true;
		} else {
			printf("Recovery status LEDs unavailable (%d)\n", rc);
			printf("Fallback to link LEDs\n");
			recovery_led_init(&leds);
			use_link_leds = true;
		}
	}
	recovery_prepare_static_network();

	printf("HTTP recovery: starting Ethernet\n");
	recovery_watchdog_poll();
	rc = net_lwip_eth_start();
	if (rc < 0) {
		printf("Failed to start Ethernet: %d\n", rc);
		goto out;
	}
	eth_started = true;
	recovery_watchdog_poll();

	udev = eth_get_current();
	if (!udev || !eth_is_active(udev)) {
		printf("No active net device\n");
		rc = -ENODEV;
		goto out;
	}

	printf("HTTP recovery: creating lwIP netif\n");
	netif = net_lwip_new_netif(udev);
	if (!netif) {
		rc = -ENODEV;
		goto out;
	}
	recovery_watchdog_poll();

	printf("HTTP recovery: starting DHCP helper\n");
	rc = recovery_dhcp_server_init(&dhcp, netif);
	if (rc)
		printf("Failed to start recovery DHCP server: %d\n", rc);
	else
		net_lwip_set_recovery_dhcp_hook(recovery_dhcp_recv, &dhcp);

	printf("HTTP recovery: starting HTTP server\n");
	httpd_init();
	printf("HTTP recovery server listening on http://%s/\n",
	       ip4addr_ntoa(netif_ip4_addr(netif)));

	timeout_ms = env_get_ulong("recovery_timeout", 10, 0) * 1000;
	start_ms = get_timer(0);

	while (1) {
		if (tstc()) {
			int c = getchar();

			if (c == 0x03) { /* Ctrl-C */
				printf("Abort by user\n");
				break;
			}
		}
		/* net_lwip_rx() already runs sys_check_timeouts(). */
		net_lwip_rx(udev, netif);
		if (use_status_leds)
			recovery_status_led_poll(&status_leds);
		else if (use_link_leds)
			recovery_led_poll(&leds);
		if (flash_request) {
			flash_request = 0;
			printf("Upload done, flashing...\n");
			rc = flash_image(&status_leds);
			if (!rc) {
				if (prog_reboot) {
					printf("Flashing complete. Rebooting in %dms...\n",
					       REBOOT_DELAY_MS);
					reboot_request = 0;
					sys_timeout(REBOOT_DELAY_MS, reboot_delay_cb, NULL);
				} else {
					printf("Action complete. Keeping recovery server running.\n");
				}
			} else {
				printf("Flashing failed: %d. Keeping server running.\n",
				       rc);
			}
		}
		if (reboot_request)
			do_reset(NULL, 0, 0, NULL);
		if (timeout_ms && !post_ok && !upload_done && !flash_request &&
		    !reboot_request && prog_phase == 0 &&
			get_timer(start_ms) >= timeout_ms) {
			printf("HTTP recovery timeout after %lums\n", timeout_ms);
			break;
		}
		recovery_watchdog_poll();
	}

	rc = 0;

out:
	recovery_dhcp_server_stop(&dhcp);
	net_lwip_set_recovery_dhcp_hook(NULL, NULL);
	if (netif)
		net_lwip_remove_netif(netif);
	if (eth_started)
		eth_halt();
	recovery_status_led_stop(&status_leds);
	recovery_status_led_release(&status_leds);
	recovery_led_ctrl_free(&leds);
	recovery_board_http_acl(false);
	env_set("eth_allow_no_link", saved_allow_no_link);
	free(saved_allow_no_link);

	return rc;
}
