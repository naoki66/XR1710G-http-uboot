// SPDX-License-Identifier: GPL-2.0+
/*
 * Askey SBE1V1K chainloader board support.
 */

#include <asm/io.h>

#define SBE1V1K_APSS_WDT_BASE		0x0b017000
#define SBE1V1K_APSS_WDT_RST		0x04
#define SBE1V1K_APSS_WDT_EN		0x08
#define SBE1V1K_APSS_WDT_BARK_TIME	0x10
#define SBE1V1K_APSS_WDT_BITE_TIME	0x14

static void sbe1v1k_watchdog_kick(void)
{
	void __iomem *wdt = (void __iomem *)SBE1V1K_APSS_WDT_BASE;

	writel(1, wdt + SBE1V1K_APSS_WDT_RST);
	readl(wdt + SBE1V1K_APSS_WDT_RST);
}

static void sbe1v1k_disable_watchdog(void)
{
	void __iomem *wdt = (void __iomem *)SBE1V1K_APSS_WDT_BASE;

	writel(0, wdt + SBE1V1K_APSS_WDT_EN);
	sbe1v1k_watchdog_kick();
	writel(0, wdt + SBE1V1K_APSS_WDT_BARK_TIME);
	writel(0, wdt + SBE1V1K_APSS_WDT_BITE_TIME);
	readl(wdt + SBE1V1K_APSS_WDT_EN);
}

void recovery_board_watchdog_kick(void)
{
	sbe1v1k_watchdog_kick();
}

void board_net_phy_watchdog_kick(void)
{
	sbe1v1k_watchdog_kick();
}

int board_early_init_f(void)
{
	sbe1v1k_disable_watchdog();

	return 0;
}

void qcom_board_init(void)
{
	sbe1v1k_disable_watchdog();
}
