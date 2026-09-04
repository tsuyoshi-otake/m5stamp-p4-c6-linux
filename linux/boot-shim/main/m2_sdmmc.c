// SPDX-License-Identifier: Apache-2.0 OR GPL-2.0-only
/*
 * ESP32-P4 Slot 1 GPIO-matrix/clock bootstrap for Stamp-AddOn C6.
 *
 * Linux's board port does not yet have an upstream P4 pinctrl/clock
 * controller, so the boot shim leaves the controller clocked and routes the
 * six SDIO signals before jumping to the kernel.  The constants mirror the
 * locked ESP-IDF 5.5.3 ESP32-P4 v1 register definitions.  This is an M2
 * experiment: it must be validated on the assembled carrier at 20 MHz before
 * any higher-speed or DMA setting is attempted.
 */

#include <stdint.h>
#include <stdbool.h>

#include "esp_log.h"
#include "esp_rom_sys.h"

static const char *TAG = "easystick-m2-sdio";

#define HP_SYS_CLKRST_BASE 0x500e6000u
#define HP_SOC_CLK_CTRL1 (HP_SYS_CLKRST_BASE + 0x18u)
#define HP_PERI_CLK_CTRL01 (HP_SYS_CLKRST_BASE + 0x34u)
#define HP_PERI_CLK_CTRL02 (HP_SYS_CLKRST_BASE + 0x38u)
#define LP_CLKRST_BASE 0x50111000u
#define LP_SDMMC_RESET (LP_CLKRST_BASE + 0x4cu)

#define GPIO_BASE 0x500e0000u
#define GPIO_OUT1_W1TS (GPIO_BASE + 0x14u)
#define GPIO_OUT1_W1TC (GPIO_BASE + 0x18u)
#define GPIO_ENABLE1_W1TS (GPIO_BASE + 0x30u)
#define GPIO_ENABLE1_W1TC (GPIO_BASE + 0x34u)
#define GPIO_FUNC_IN_SEL_BASE (GPIO_BASE + 0x158u)
#define GPIO_FUNC_OUT_SEL_BASE (GPIO_BASE + 0x558u)
#define IO_MUX_BASE 0x500e1000u

#define SDMMC_SYS_CLK_EN (1u << 14)
#define SDIO_HS_MODE (1u << 22)
#define SDIO_LS_CLK_SRC_SEL (1u << 23)
#define SDIO_LS_CLK_EN (1u << 24)
#define SDIO_EDGE_CFG_UPDATE (1u << 8)
#define SDIO_EDGE_L_SHIFT 9
#define SDIO_EDGE_H_SHIFT 13
#define SDIO_EDGE_N_SHIFT 17
#define SDIO_SLF_EDGE_SEL_SHIFT 21
#define SDIO_DRV_EDGE_SEL_SHIFT 23
#define SDIO_SAM_EDGE_SEL_SHIFT 25
#define SDIO_SLF_CLK_EN (1u << 27)
#define SDIO_DRV_CLK_EN (1u << 28)
#define SDIO_SAM_CLK_EN (1u << 29)
#define SDMMC_RESET_EN (1u << 28)

/* M5Stack's Stamp-P4 ESP-Hosted configuration waits 1500 ms after the
 * active-high C6 EN/reset sequence before starting SDIO card init.  Keep the
 * same board-proven readiness window in the Linux handoff. */
#define C6_RESET_ASSERT_US 10000u
#define C6_RESET_INACTIVE_US 10000u
#define C6_RESET_READY_DELAY_US 1500000u

#define IOMUX_FUN_PD (1u << 7)
#define IOMUX_FUN_PU (1u << 8)
#define IOMUX_FUN_IE (1u << 9)
#define IOMUX_FUN_DRV_MASK (3u << 10)
#define IOMUX_MCU_SEL_MASK (7u << 12)
#define PIN_FUNC_GPIO 1u

static inline volatile uint32_t *reg32(uint32_t addr)
{
	return (volatile uint32_t *)(uintptr_t)addr;
}

static inline void set_bits(uint32_t addr, uint32_t mask)
{
	*reg32(addr) |= mask;
}

static inline void clear_set_bits(uint32_t addr, uint32_t clear, uint32_t set)
{
	uint32_t value = *reg32(addr);
	*reg32(addr) = (value & ~clear) | set;
}

static void configure_iomux_gpio(uint32_t gpio, bool pullup)
{
	/*
	 * ESP32-P4's IO_MUX register map is not gpio_number * 4 throughout:
	 * GPIO26 starts at offset 0x6c, so the Stamp SDIO pins 42..48 occupy
	 * offsets 0xac..0xc4.  Using gpio * 4 here configured the preceding
	 * pad (GPIO43 wrote GPIO42's register), leaving the data path unreliable.
	 * Keep the small board-specific mapping explicit until this bootstrap can
	 * use the ESP-IDF GPIO helpers directly.
	 */
	uint32_t offset;
	switch (gpio) {
	case 42u: offset = 0xacu; break;
	case 43u: offset = 0xb0u; break;
	case 44u: offset = 0xb4u; break;
	case 45u: offset = 0xb8u; break;
	case 46u: offset = 0xbcu; break;
	case 47u: offset = 0xc0u; break;
	case 48u: offset = 0xc4u; break;
	default: return;
	}
	uint32_t clear = IOMUX_FUN_PD | IOMUX_FUN_PU | IOMUX_FUN_IE |
			 IOMUX_FUN_DRV_MASK | IOMUX_MCU_SEL_MASK;
	uint32_t set = IOMUX_FUN_IE | (3u << 10) | (PIN_FUNC_GPIO << 12);
	if (pullup)
		set |= IOMUX_FUN_PU;
	clear_set_bits(IO_MUX_BASE + offset, clear, set);
}

static void route_output(uint32_t gpio, uint32_t signal)
{
	/* OEN_SEL=0 delegates output-enable to the SDMMC peripheral. */
	clear_set_bits(GPIO_FUNC_OUT_SEL_BASE + (gpio * 4u),
			       0x1ffu | (1u << 10) | (1u << 11), signal & 0x1ffu);
}

static void route_input(uint32_t signal, uint32_t gpio)
{
	/* SIG_IN_SEL=1 selects the GPIO matrix rather than the direct bypass. */
	*reg32(GPIO_FUNC_IN_SEL_BASE + (signal * 4u)) = (gpio & 0x3fu) | (1u << 7);
}

static void pulse_c6_reset(void)
{
	const uint32_t bit = 1u << (42u - 32u);

	configure_iomux_gpio(42u, false);
	*reg32(GPIO_ENABLE1_W1TS) = bit;
	*reg32(GPIO_OUT1_W1TS) = bit;
	esp_rom_delay_us(C6_RESET_ASSERT_US);
	*reg32(GPIO_OUT1_W1TC) = bit;
	esp_rom_delay_us(C6_RESET_INACTIVE_US);
	*reg32(GPIO_OUT1_W1TS) = bit;
	esp_rom_delay_us(C6_RESET_READY_DELAY_US);
	ESP_LOGI(TAG, "C6 reset GPIO42 released");
}

static void enable_sdmmc_clock(void)
{
	set_bits(HP_SOC_CLK_CTRL1, SDMMC_SYS_CLK_EN);
	set_bits(LP_SDMMC_RESET, SDMMC_RESET_EN);
	clear_set_bits(LP_SDMMC_RESET, SDMMC_RESET_EN, 0);

	/* PLL_F160M / host divider 4 gives a conservative 40 MHz LS clock. */
	clear_set_bits(HP_PERI_CLK_CTRL01,
			       SDIO_LS_CLK_SRC_SEL | SDIO_HS_MODE, SDIO_LS_CLK_EN);

	uint32_t clear = (0xfu << SDIO_EDGE_L_SHIFT) |
			 (0xfu << SDIO_EDGE_H_SHIFT) |
			 (0xfu << SDIO_EDGE_N_SHIFT) |
			 (0x3u << SDIO_SLF_EDGE_SEL_SHIFT) |
			 (0x3u << SDIO_DRV_EDGE_SEL_SHIFT) |
			 (0x3u << SDIO_SAM_EDGE_SEL_SHIFT);
	uint32_t set = (3u << SDIO_EDGE_L_SHIFT) |
			 (1u << SDIO_EDGE_H_SHIFT) |
			 (3u << SDIO_EDGE_N_SHIFT) |
			 (1u << SDIO_DRV_EDGE_SEL_SHIFT) |
			 SDIO_SLF_CLK_EN | SDIO_DRV_CLK_EN | SDIO_SAM_CLK_EN;
	clear_set_bits(HP_PERI_CLK_CTRL02, clear, set);
	set_bits(HP_PERI_CLK_CTRL02, SDIO_EDGE_CFG_UPDATE);
	clear_set_bits(HP_PERI_CLK_CTRL02, SDIO_EDGE_CFG_UPDATE, 0);
}

void easystick_m2_sdmmc_init(void)
{
	enable_sdmmc_clock();

	/* Slot 1: CLK=43, CMD=44, DAT0..3=45..48. */
	const uint32_t pins[] = { 43u, 44u, 45u, 46u, 47u, 48u };
	for (uint32_t i = 0; i < 6u; ++i)
		configure_iomux_gpio(pins[i], i != 0u);

	route_output(43u, 0u); /* SD_CARD_CCLK_2_PAD_OUT_IDX */
	route_output(44u, 1u); /* SD_CARD_CCMD_2_PAD_OUT_IDX */
	route_output(45u, 2u);
	route_output(46u, 3u);
	route_output(47u, 4u);
	route_output(48u, 5u);
	route_input(1u, 44u); /* CMD */
	route_input(2u, 45u); /* DAT0 */
	route_input(3u, 46u); /* DAT1 / SDIO interrupt */
	route_input(4u, 47u); /* DAT2 */
	route_input(5u, 48u); /* DAT3 */
	/*
	 * P4 gates the in-band DAT1 interrupt with a separate per-slot
	 * card_int matrix input. Slot 1 has no separate pin on Stamp-P4, so
	 * match ESP-IDF's SDMMC host setup and hold that input high.
	 */
	route_input(129u, 0x3fu); /* SD_CARD_INT_N_2_PAD_IN_IDX <- constant one */

	/*
	 * Reset before Linux enumerates the card. ESP-IDF's working SDIO host does
	 * this, whereas the Linux ESP-Hosted module cannot drive GPIO42 through its
	 * current legacy gpiolib mapping. This avoids resetting an enumerated card.
	 * M5Stack's official Stamp-P4 ESP-Hosted configuration waits 1500 ms after
	 * a 10 ms active-high / 10 ms inactive / active-high sequence; keep the
	 * same readiness window before handoff.
	 */
	pulse_c6_reset();
	ESP_LOGI(TAG, "Slot 1 SDIO GPIO matrix ready; C6 reset before Linux handoff");
}
