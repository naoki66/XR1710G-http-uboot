// SPDX-License-Identifier: GPL-2.0+
/*
 * Qualcomm IPQ PWM controller
 *
 * Based on the Linux/QSDK pwm-ipq driver register model.
 */

#include <clk.h>
#include <dm.h>
#include <pwm.h>
#include <linux/bitfield.h>
#include <linux/bitops.h>
#include <linux/errno.h>
#include <linux/math64.h>
#include <linux/time.h>
#include <asm/io.h>

#define IPQ_PWM_DEFAULT_CLK_RATE	100000000UL
#define IPQ_PWM_MAX_PERIOD_NS		((u64)NSEC_PER_SEC)
#define IPQ_PWM_MAX_DIV			0xffff
#define IPQ_PWM_MAX_CHANNELS		4

/*
 * Two 32-bit registers for each PWM: REG0 and REG1.
 * Base offset for PWM #i is at 8 * #i.
 */
#define IPQ_PWM_REG0(ch)		(8 * (ch))
#define IPQ_PWM_REG0_PWM_DIV		GENMASK(15, 0)
#define IPQ_PWM_REG0_HI_DURATION	GENMASK(31, 16)

#define IPQ_PWM_REG1(ch)		(8 * (ch) + 4)
#define IPQ_PWM_REG1_PRE_DIV		GENMASK(15, 0)
#define IPQ_PWM_REG1_UPDATE		BIT(30)
#define IPQ_PWM_REG1_ENABLE		BIT(31)

struct ipq_pwm_channel {
	uint period_ns;
	uint duty_ns;
	bool configured;
	bool enabled;
	bool inverted;
};

struct ipq_pwm_priv {
	void __iomem *base;
	struct clk clk;
	ulong rate;
	uint npwm;
	struct ipq_pwm_channel channels[IPQ_PWM_MAX_CHANNELS];
};

static void ipq_pwm_write(struct ipq_pwm_priv *priv, uint channel, uint reg,
			  u32 val)
{
	writel(val, priv->base + (8 * channel) + reg);
}

static u32 ipq_pwm_read(struct ipq_pwm_priv *priv, uint channel, uint reg)
{
	return readl(priv->base + (8 * channel) + reg);
}

static void ipq_pwm_config_div_and_duty(struct ipq_pwm_priv *priv,
					uint channel, uint pre_div,
					uint pwm_div, u64 duty_ns,
					bool enable)
{
	u64 hi_dur;
	u32 val;

	hi_dur = div64_u64(duty_ns * priv->rate,
			   (u64)(pre_div + 1) * NSEC_PER_SEC);
	if (hi_dur > pwm_div + 1)
		hi_dur = pwm_div + 1;

	val = FIELD_PREP(IPQ_PWM_REG0_HI_DURATION, hi_dur) |
	      FIELD_PREP(IPQ_PWM_REG0_PWM_DIV, pwm_div);
	ipq_pwm_write(priv, channel, 0, val);

	val = FIELD_PREP(IPQ_PWM_REG1_PRE_DIV, pre_div);
	ipq_pwm_write(priv, channel, 4, val);

	val |= IPQ_PWM_REG1_UPDATE;
	if (enable)
		val |= IPQ_PWM_REG1_ENABLE;
	ipq_pwm_write(priv, channel, 4, val);
}

static int ipq_pwm_set_config(struct udevice *dev, uint channel,
			      uint period_ns, uint duty_ns)
{
	struct ipq_pwm_priv *priv = dev_get_priv(dev);
	struct ipq_pwm_channel *ch;
	uint pre_div, pwm_div, best_pre_div, best_pwm_div;
	uint requested_duty_ns;
	u64 period_rate, min_diff;

	if (channel >= priv->npwm)
		return -EINVAL;
	if (!period_ns || period_ns > IPQ_PWM_MAX_PERIOD_NS)
		return -ERANGE;
	if (!priv->rate)
		return -EINVAL;
	if (period_ns < DIV64_U64_ROUND_UP(NSEC_PER_SEC, priv->rate))
		return -ERANGE;

	ch = &priv->channels[channel];
	if (duty_ns > period_ns)
		duty_ns = period_ns;
	requested_duty_ns = duty_ns;
	if (ch->inverted)
		duty_ns = period_ns - duty_ns;

	period_rate = (u64)period_ns * priv->rate;
	best_pre_div = IPQ_PWM_MAX_DIV;
	best_pwm_div = IPQ_PWM_MAX_DIV;
	min_diff = period_rate;

	pre_div = div64_u64(period_rate,
			   (u64)NSEC_PER_SEC * (IPQ_PWM_MAX_DIV + 1));

	for (; pre_div <= IPQ_PWM_MAX_DIV; pre_div++) {
		u64 remainder, ticks;

		ticks = div64_u64_rem(period_rate,
				      (u64)NSEC_PER_SEC * (pre_div + 1),
				      &remainder);
		if (!ticks)
			continue;

		pwm_div = ticks - 1;
		if (pre_div > pwm_div)
			break;
		if (pwm_div > IPQ_PWM_MAX_DIV - 1)
			continue;

		if (remainder < min_diff) {
			best_pre_div = pre_div;
			best_pwm_div = pwm_div;
			min_diff = remainder;
			if (!min_diff)
				break;
		}
	}

	if (best_pre_div == IPQ_PWM_MAX_DIV &&
	    best_pwm_div == IPQ_PWM_MAX_DIV)
		return -ERANGE;

	ch->period_ns = period_ns;
	ch->duty_ns = requested_duty_ns;
	ch->configured = true;
	ipq_pwm_config_div_and_duty(priv, channel, best_pre_div, best_pwm_div,
				    duty_ns, ch->enabled);

	return 0;
}

static int ipq_pwm_set_enable(struct udevice *dev, uint channel, bool enable)
{
	struct ipq_pwm_priv *priv = dev_get_priv(dev);
	struct ipq_pwm_channel *ch;
	u32 val;

	if (channel >= priv->npwm)
		return -EINVAL;

	ch = &priv->channels[channel];
	if (!ch->configured)
		return -EINVAL;

	val = ipq_pwm_read(priv, channel, 4);
	if (enable)
		val |= IPQ_PWM_REG1_ENABLE;
	else
		val &= ~IPQ_PWM_REG1_ENABLE;
	val |= IPQ_PWM_REG1_UPDATE;
	ipq_pwm_write(priv, channel, 4, val);

	ch->enabled = enable;

	return 0;
}

static int ipq_pwm_set_invert(struct udevice *dev, uint channel, bool polarity)
{
	struct ipq_pwm_priv *priv = dev_get_priv(dev);
	struct ipq_pwm_channel *ch;

	if (channel >= priv->npwm)
		return -EINVAL;

	ch = &priv->channels[channel];
	ch->inverted = polarity;

	if (!ch->configured)
		return 0;

	return ipq_pwm_set_config(dev, channel, ch->period_ns, ch->duty_ns);
}

static int ipq_pwm_probe(struct udevice *dev)
{
	struct ipq_pwm_priv *priv = dev_get_priv(dev);
	ulong rate;
	int ret;

	priv->base = dev_read_addr_ptr(dev);
	if (!priv->base)
		return -EINVAL;

	priv->npwm = dev_read_u32_default(dev, "pwm-npwm", IPQ_PWM_MAX_CHANNELS);
	if (!priv->npwm || priv->npwm > IPQ_PWM_MAX_CHANNELS)
		return -EINVAL;

	ret = clk_get_by_index(dev, 0, &priv->clk);
	if (ret)
		return ret;

	rate = dev_read_u32_default(dev, "assigned-clock-rates",
				    IPQ_PWM_DEFAULT_CLK_RATE);
	rate = clk_set_rate(&priv->clk, rate);
	if ((long)rate < 0)
		return (int)rate;
	if (!rate)
		rate = IPQ_PWM_DEFAULT_CLK_RATE;
	priv->rate = rate;

	ret = clk_enable(&priv->clk);
	if (ret)
		return ret;

	return 0;
}

static int ipq_pwm_remove(struct udevice *dev)
{
	struct ipq_pwm_priv *priv = dev_get_priv(dev);
	uint i;

	for (i = 0; i < priv->npwm; i++) {
		if (priv->channels[i].configured)
			ipq_pwm_set_enable(dev, i, false);
	}

	return 0;
}

static const struct pwm_ops ipq_pwm_ops = {
	.set_config = ipq_pwm_set_config,
	.set_enable = ipq_pwm_set_enable,
	.set_invert = ipq_pwm_set_invert,
};

static const struct udevice_id ipq_pwm_ids[] = {
	{ .compatible = "qcom,ipq9574-pwm" },
	{ .compatible = "qcom,ipq6018-pwm" },
	{ }
};

U_BOOT_DRIVER(ipq_pwm) = {
	.name = "ipq_pwm",
	.id = UCLASS_PWM,
	.of_match = ipq_pwm_ids,
	.ops = &ipq_pwm_ops,
	.probe = ipq_pwm_probe,
	.remove = ipq_pwm_remove,
	.priv_auto = sizeof(struct ipq_pwm_priv),
};
