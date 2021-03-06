/* msm8974_pwm_vibrator.c
 *
 * Copyright (C) 2009-2013 LGE, Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
 */

#include <linux/init.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/platform_device.h>
#include <linux/gpio.h>
#include <linux/of_gpio.h>
#include <linux/delay.h>
#include <linux/timer.h>
#include <linux/clk.h>
#include <linux/err.h>
#include <linux/stat.h>
#include <linux/io.h>
#include <linux/slab.h>
#include <linux/of.h>
#include <linux/regulator/consumer.h>
#include <linux/mutex.h>

#include <mach/msm_iomap.h>

#include "../staging/android/timed_output.h"

/* gpio and clock control for vibrator */
static void __iomem *virt_base;

#define MMSS_CC_GP1_BASE(x) (void __iomem *)(virt_base + (x))
#define REG_CMD_RCGR	0x00
#define REG_CFG_RCGR	0x04
#define REG_M		0x08
#define REG_N		0x0C
#define REG_D		0x10
#define REG_CBCR	0x24

#define MMSS_CC_PWM_SET		0xFD8C3450
#define MMSS_CC_PWM_SIZE	SZ_1K
#define DEVICE_NAME		"msm8974_pwm_vibrator"

#define MMSS_CC_M_DEFAULT	1
#define MMSS_CC_N_DEFAULT	128
#define MMSS_CC_D_MAX		MMSS_CC_N_DEFAULT
#define MMSS_CC_D_HALF		(MMSS_CC_N_DEFAULT >> 1)

static DEFINE_MUTEX(vib_lock);

struct timed_vibrator_data {
	struct timed_output_dev dev;
	struct hrtimer timer;
	struct mutex lock;
	int max_timeout;
	int ms_time;            /* vibrator duration */
	atomic_t vib_status;    /* on/off */
	int vibe_gain;          /* default max gain */
	int vibe_pwm;
	atomic_t gp1_clk_flag;
	int amp;
	int vibe_n_value;
	int haptic_en_gpio;
	int motor_pwm_gpio;
	int vibe_warmup_delay; /* in ms */
	struct work_struct work_vibrator_off;
	struct work_struct work_vibrator_on;
	bool use_vdd_supply;
	struct regulator *vdd_reg;
};

static struct clk *cam_gp1_clk;

static int vibrator_regulator_init(
		struct platform_device *pdev,
		struct timed_vibrator_data *vib)
{
	if (!vib->use_vdd_supply)
		return 0;

	vib->vdd_reg = devm_regulator_get(&pdev->dev, "vdd");
	if (IS_ERR(vib->vdd_reg)) {
		pr_err("%s: could not get vdd-supply\n", __func__);
		return -ENODEV;
	}

	return 0;
}

static int vibrator_set_power(int enable,
		struct timed_vibrator_data *vib)
{
	int ret = 0;
	static int vibrator_enabled = 0;

	if (!vib->use_vdd_supply)
		return 0;

	if (enable == vibrator_enabled)
		return 0;

	mutex_lock(&vib_lock);

	vibrator_enabled = enable;
	if (enable) {
		ret = regulator_enable(vib->vdd_reg);
		if (ret < 0)
			pr_err("%s: vdd_reg->enable failed\n", __func__);
	} else {
		if (regulator_is_enabled(vib->vdd_reg) > 0) {
			ret = regulator_disable(vib->vdd_reg);
			if (ret < 0)
				pr_err("%s: vdd_reg->disable failed\n",
						__func__);
		}
	}

	mutex_unlock(&vib_lock);

	return ret;
}

static int vibrator_ic_enable_set(int enable,
		struct timed_vibrator_data *vib)
{
	pr_debug("%s: enable %d\n", __func__, enable);

	if (enable)
		gpio_direction_output(vib->haptic_en_gpio, 1);
	else
		gpio_direction_output(vib->haptic_en_gpio, 0);

	return 0;
}

static int vibrator_adjust_amp(int amp)
{
	int level = 0;
	bool minus = false;

	if (amp < 0) {
		minus = true;
		amp = -amp;
	}

	level = (2 * amp * (MMSS_CC_D_HALF - 2) + 100) / (2 * 100);
	if (!level && amp)
		level = 1;

	if (minus && level)
		level = -level;

	return level;
}

static int vibrator_pwm_set(int enable, int amp, int n_value)
{
	int d_val = 0;
	int m_val = MMSS_CC_M_DEFAULT;

	pr_debug("%s: amp %d, value %d\n", __func__, amp, n_value);

	enable = !!enable;
	if (enable) {
		if (amp)
			d_val = vibrator_adjust_amp(amp) + MMSS_CC_D_HALF;

		writel((2 << 12) | /* dual edge mode */
		       (0 << 8) |  /* cxo */
		       (7 << 0),
		       MMSS_CC_GP1_BASE(REG_CFG_RCGR));
		writel(((m_val) & 0xff), MMSS_CC_GP1_BASE(REG_M));
		writel(((~(n_value - m_val)) & 0xff), MMSS_CC_GP1_BASE(REG_N));
		writel(((~(d_val << 1)) & 0xff), MMSS_CC_GP1_BASE(REG_D));
		writel(enable, MMSS_CC_GP1_BASE(REG_CMD_RCGR)); /* UPDATE */
	}
	writel(enable, MMSS_CC_GP1_BASE(REG_CBCR));

	return 0;
}

#ifdef ANDROID_VIBRATOR_USE_WORKQUEUE
static inline void vibrator_work_on(struct work_struct *work)
{
	queue_work(vibrator_workqueue, work);
}

static inline void vibrator_work_off(struct work_struct *work)
{
	if (!work_pending(work))
		queue_work(vibrator_workqueue, work);
}
#else
static inline void vibrator_work_on(struct work_struct *work)
{
	schedule_work(work);
}

static inline void vibrator_work_off(struct work_struct *work)
{
	if (!work_pending(work))
		schedule_work(work);
}
#endif

static int msm8974_pwm_vibrator_force_set(struct timed_vibrator_data *vib,
		int nForce, int n_value)
{
	/* Check the Force value with Max and Min force value */
	int vib_duration_ms = 0;

	if (vib->vibe_warmup_delay > 0) { /*warmup delay isn't used now */
		if (atomic_read(&vib->vib_status))
			usleep(vib->vibe_warmup_delay * 1000);
	}

	/* TODO: control the gain of vibrator */
	if (nForce == 0) {
		vibrator_ic_enable_set(0, vib);
		vibrator_pwm_set(0, 0, n_value);
		/* should be checked for vibrator response time */
		vibrator_set_power(0, vib);
		atomic_set(&vib->vib_status, false);

		mutex_lock(&vib_lock);
		if (atomic_read(&vib->gp1_clk_flag) == 1) {
			clk_disable_unprepare(cam_gp1_clk);
			atomic_set(&vib->gp1_clk_flag, 0);
		}
		mutex_unlock(&vib_lock);
	} else {
		mutex_lock(&vib_lock);
		if (atomic_read(&vib->gp1_clk_flag) == 0) {
			clk_prepare_enable(cam_gp1_clk);
			atomic_set(&vib->gp1_clk_flag, 1);
		}
		mutex_unlock(&vib_lock);

		if (work_pending(&vib->work_vibrator_off))
			cancel_work_sync(&vib->work_vibrator_off);
		hrtimer_cancel(&vib->timer);

		vib_duration_ms = vib->ms_time;
		/* should be checked for vibrator response time */
		vibrator_set_power(1, vib);
		vibrator_pwm_set(1, nForce, n_value);
		vibrator_ic_enable_set(1, vib);
		atomic_set(&vib->vib_status, true);

		hrtimer_start(&vib->timer,
				ns_to_ktime((u64)vib_duration_ms * NSEC_PER_MSEC),
				HRTIMER_MODE_REL);
	}

	return 0;
}

static void msm8974_pwm_vibrator_on(struct work_struct *work)
{
	struct timed_vibrator_data *vib =
		container_of(work, struct timed_vibrator_data,
				work_vibrator_on);
	int gain = vib->vibe_gain;
	int pwm = vib->vibe_pwm;
	/* suspend /resume logging test */
	pr_debug("%s: gain = %d pwm = %d\n", __func__, gain, pwm);

	msm8974_pwm_vibrator_force_set(vib, gain, pwm);
}

static void msm8974_pwm_vibrator_off(struct work_struct *work)
{
	struct timed_vibrator_data *vib =
		container_of(work, struct timed_vibrator_data,
				work_vibrator_off);

	msm8974_pwm_vibrator_force_set(vib, 0, vib->vibe_n_value);
}

static enum hrtimer_restart vibrator_timer_func(struct hrtimer *timer)
{
	struct timed_vibrator_data *vib =
		container_of(timer, struct timed_vibrator_data, timer);

	vibrator_work_off(&vib->work_vibrator_off);
	return HRTIMER_NORESTART;
}

static int vibrator_get_time(struct timed_output_dev *dev)
{
	struct timed_vibrator_data *vib =
		container_of(dev, struct timed_vibrator_data, dev);

	if (hrtimer_active(&vib->timer)) {
		ktime_t r = hrtimer_get_remaining(&vib->timer);
		return ktime_to_ms(r);
	}
	return 0;
}

static void vibrator_enable(struct timed_output_dev *dev, int value)
{
	struct timed_vibrator_data *vib =
		container_of(dev, struct timed_vibrator_data, dev);

	mutex_lock(&vib->lock);
	if (value > 0) {
		if (value > vib->max_timeout)
			value = vib->max_timeout;

		vib->ms_time = value;

		vibrator_work_on(&vib->work_vibrator_on);
	} else {
		vibrator_work_off(&vib->work_vibrator_off);
	}
	mutex_unlock(&vib->lock);
}

static int vibrator_gpio_init(struct timed_vibrator_data *vib)
{
	int rc;

	rc = gpio_request(vib->haptic_en_gpio, "motor_en");
	if (rc < 0) {
		pr_err("%s: gpio %d request failed\n", __func__,
				vib->haptic_en_gpio);
		return rc;
	}

	rc = gpio_request(vib->motor_pwm_gpio, "motor_pwm");
	if (rc < 0) {
		pr_err("%s: gpio %d request failed\n", __func__,
				vib->motor_pwm_gpio);
		gpio_free(vib->haptic_en_gpio);
		return rc;
	}

	return 0;
}

static void vibrator_gpio_deinit(struct timed_vibrator_data *vib)
{
	gpio_free(vib->motor_pwm_gpio);
	gpio_free(vib->haptic_en_gpio);
}

static int vibrator_parse_dt(struct device *dev,
		struct timed_vibrator_data *vib)
{
	int ret;
	struct device_node *np = dev->of_node;

	ret = of_get_named_gpio_flags(np, "haptic-pwr-gpio", 0, NULL);
	if (ret < 0) {
		pr_err("%s: haptic-pwr-gpio failed\n", __func__);
		return ret;
	}
	vib->haptic_en_gpio = ret;

	ret = of_get_named_gpio_flags(np, "motor-pwm-gpio", 0, NULL);
	if (ret < 0) {
		pr_err("%s: motor-pwm-gpio failed\n", __func__);
		return ret;
	}
	vib->motor_pwm_gpio = ret;

	ret = of_property_read_u32(np, "motor-amp", &vib->amp);
	if (ret < 0) {
		pr_err("%s: motor-amp failed\n", __func__);
		return ret;
	}

	ret = of_property_read_u32(np, "n-value", &vib->vibe_n_value);
	if (ret < 0) {
		pr_err("%s: n-value failed\n", __func__);
		return ret;
	}

	vib->use_vdd_supply = of_property_read_bool(np, "use-vdd-supply");

	pr_debug("%s: motor_en %d, motor_pwm %d, amp %d, n_value %d vdd %d\n",
			__func__,
			vib->haptic_en_gpio,
			vib->motor_pwm_gpio,
			vib->amp, vib->vibe_n_value,
			vib->use_vdd_supply);

	ret = of_property_read_u32(np, "vibe-warmup-delay",
			&vib->vibe_warmup_delay);
	if (!ret)
		pr_debug("%s: vibe_warmup_delay %d ms\n", __func__,
				vib->vibe_warmup_delay);

	return 0;
}

static ssize_t vibrator_amp_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct timed_output_dev *_dev = dev_get_drvdata(dev);
	struct timed_vibrator_data *vib =
		container_of(_dev, struct timed_vibrator_data, dev);
	int gain = vib->vibe_gain;

	return sprintf(buf, "%d\n", gain);
}

static ssize_t vibrator_amp_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t size)
{
	struct timed_output_dev *_dev = dev_get_drvdata(dev);
	struct timed_vibrator_data *vib =
		container_of(_dev, struct timed_vibrator_data, dev);
	int gain;

	sscanf(buf, "%d", &gain);
	vib->vibe_gain = gain;

	return size;
}

static ssize_t vibrator_pwm_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct timed_output_dev *_dev = dev_get_drvdata(dev);
	struct timed_vibrator_data *vib =
		container_of(_dev, struct timed_vibrator_data, dev);

	return sprintf(buf, "%d\n", vib->vibe_pwm);
}

static ssize_t vibrator_pwm_store(struct device *dev, struct device_attribute *attr, const char *buf, size_t size)
{
	struct timed_output_dev *_dev = dev_get_drvdata(dev);
	struct timed_vibrator_data *vib =
		container_of(_dev, struct timed_vibrator_data, dev);
	int pwm;

	sscanf(buf, "%d", &pwm);
	vib->vibe_pwm = pwm;

	return size;
}

static struct device_attribute vibrator_device_attrs[] = {
	__ATTR(amp, S_IRUGO | S_IWUSR, vibrator_amp_show, vibrator_amp_store),
	__ATTR(n_val, S_IRUGO | S_IWUSR, vibrator_pwm_show, vibrator_pwm_store),
};

static struct timed_vibrator_data msm8974_pwm_vibrator_data = {
	.dev.name = "vibrator",
	.dev.enable = vibrator_enable,
	.dev.get_time = vibrator_get_time,
	.max_timeout = 30000, /* max time for vibrator enable 30 sec. */
};

static int msm8974_pwm_vibrator_probe(struct platform_device *pdev)
{
	int i, ret = 0;
	struct timed_vibrator_data *vib;

	platform_set_drvdata(pdev, &msm8974_pwm_vibrator_data);
	vib = (struct timed_vibrator_data *)platform_get_drvdata(pdev);

	if (pdev->dev.of_node) {
		pr_debug("%s: device has of_node\n", __func__);
		ret = vibrator_parse_dt(&pdev->dev, vib);
		if (ret < 0)
			return ret;
	}

	if (vibrator_regulator_init(pdev, vib) < 0) {
		return -ENODEV;
	}

	pdev->dev.init_name = vib->dev.name;

	if (vibrator_gpio_init(vib) < 0) {
		pr_err("%s: vibrator_gpio_init failed\n", __func__);
		return -ENODEV;
	}

	virt_base = ioremap(MMSS_CC_PWM_SET, MMSS_CC_PWM_SIZE);
	if (virt_base == NULL) {
		pr_err("%s: ioremap failed\n", __func__);
		ret = -ENODEV;
		goto err_ioremap;
	}

	cam_gp1_clk = clk_get(&pdev->dev, "cam_gp1_clk");
	if (cam_gp1_clk == NULL) {
		ret = -ENODEV;
		goto err_clk_get;
	}
	clk_set_rate(cam_gp1_clk, 29268);

	vib->vibe_gain = vib->amp;
	vib->vibe_pwm = vib->vibe_n_value;
	atomic_set(&vib->vib_status, false);
	atomic_set(&vib->gp1_clk_flag, 0);

	INIT_WORK(&vib->work_vibrator_off, msm8974_pwm_vibrator_off);
	INIT_WORK(&vib->work_vibrator_on, msm8974_pwm_vibrator_on);
	hrtimer_init(&vib->timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	vib->timer.function = vibrator_timer_func;
	mutex_init(&vib->lock);

	ret = timed_output_dev_register(&vib->dev);
	if (ret < 0) {
		pr_err("%s: failed to register timed_output_dev\n", __func__);
		goto err_timed_output_dev_register;
	}

	for (i = 0; i < ARRAY_SIZE(vibrator_device_attrs); i++) {
		ret = device_create_file(vib->dev.dev,
				&vibrator_device_attrs[i]);
		if (ret < 0) {
			pr_err("%s: failed to create sysfs\n", __func__);
			goto err_sysfs;
		}
	}

	pr_info("%s: probed\n", __func__);
	return 0;

err_sysfs:
	for (; i >= 0; i--) {
		device_remove_file(vib->dev.dev,
				&vibrator_device_attrs[i]);
	}
	timed_output_dev_unregister(&vib->dev);
err_timed_output_dev_register:
	clk_put(cam_gp1_clk);
err_clk_get:
	iounmap(virt_base);
err_ioremap:
	vibrator_gpio_deinit(vib);
	return ret;
}

static int msm8974_pwm_vibrator_remove(struct platform_device *pdev)
{
	struct timed_vibrator_data *vib = platform_get_drvdata(pdev);
	int i;

	msm8974_pwm_vibrator_force_set(vib, 0, vib->vibe_n_value);
	for (i = ARRAY_SIZE(vibrator_device_attrs); i >= 0; i--) {
		device_remove_file(vib->dev.dev,
				&vibrator_device_attrs[i]);
	}
	timed_output_dev_unregister(&vib->dev);
	clk_put(cam_gp1_clk);
	iounmap(virt_base);
	vibrator_gpio_deinit(vib);

	return 0;
}

static int msm8974_pwm_vibrator_suspend(struct platform_device *pdev,
		pm_message_t state)
{
	struct timed_vibrator_data *vib = platform_get_drvdata(pdev);

	msm8974_pwm_vibrator_force_set(vib, 0, vib->vibe_n_value);

	gpio_tlmm_config(GPIO_CFG(vib->motor_pwm_gpio, 0, GPIO_CFG_OUTPUT,
		GPIO_CFG_PULL_DOWN, GPIO_CFG_2MA), GPIO_CFG_ENABLE);
	gpio_tlmm_config(GPIO_CFG(vib->haptic_en_gpio, 0, GPIO_CFG_OUTPUT,
		GPIO_CFG_PULL_DOWN, GPIO_CFG_2MA), GPIO_CFG_ENABLE);
	return 0;
}

static int msm8974_pwm_vibrator_resume(struct platform_device *pdev)
{
	struct timed_vibrator_data *vib = platform_get_drvdata(pdev);

	msm8974_pwm_vibrator_force_set(vib, 0, vib->vibe_n_value);

	gpio_tlmm_config(GPIO_CFG(vib->motor_pwm_gpio, 6, GPIO_CFG_INPUT,
		GPIO_CFG_NO_PULL, GPIO_CFG_2MA), GPIO_CFG_ENABLE);
	gpio_tlmm_config(GPIO_CFG(vib->haptic_en_gpio, 0, GPIO_CFG_OUTPUT,
		GPIO_CFG_NO_PULL, GPIO_CFG_2MA), GPIO_CFG_ENABLE);

	return 0;
}

static void msm8974_pwm_vibrator_shutdown(struct platform_device *pdev)
{
	struct timed_vibrator_data *vib;

	vib = (struct timed_vibrator_data *)platform_get_drvdata(pdev);
	msm8974_pwm_vibrator_force_set(vib, 0, vib->vibe_n_value);
}

static struct of_device_id vibrator_match_table[] = {
    { .compatible = "msm,pwm_vibrator",},
    { },
};

static struct platform_driver msm8974_pwm_vibrator_driver = {
	.probe = msm8974_pwm_vibrator_probe,
	.remove = msm8974_pwm_vibrator_remove,
	.shutdown = msm8974_pwm_vibrator_shutdown,
	.suspend = msm8974_pwm_vibrator_suspend,
	.resume = msm8974_pwm_vibrator_resume,
	.driver = {
		.name = DEVICE_NAME,
		.of_match_table = vibrator_match_table,
	},
};

static int __init msm8974_pwm_vibrator_init(void)
{
#ifdef ANDROID_VIBRATOR_USE_WORKQUEUE
	vibrator_workqueue = create_workqueue("vibrator");
	if (!vibrator_workqueue) {
		pr_err("%s: out of memory\n", __func__);
		return -ENOMEM;
	}
#endif
	return platform_driver_register(&msm8974_pwm_vibrator_driver);
}

static void __exit msm8974_pwm_vibrator_exit(void)
{
#ifdef ANDROID_VIBRATOR_USE_WORKQUEUE
	if (vibrator_workqueue)
		destroy_workqueue(vibrator_workqueue);
	vibrator_workqueue = NULL;
#endif
	platform_driver_unregister(&msm8974_pwm_vibrator_driver);
}

/* to let init lately */
late_initcall_sync(msm8974_pwm_vibrator_init);
module_exit(msm8974_pwm_vibrator_exit);

MODULE_AUTHOR("LG Electronics Inc.");
MODULE_DESCRIPTION("MSM8974 PWM Vibrator Driver");
MODULE_LICENSE("GPL");
