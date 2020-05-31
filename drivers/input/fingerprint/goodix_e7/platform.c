#include <linux/delay.h>
#include <linux/workqueue.h>
#include <linux/of_gpio.h>
#include <linux/gpio.h>
#include <linux/regulator/consumer.h>
#include <linux/timer.h>
#include <linux/err.h>

#include "gf_spi.h"

#include <linux/platform_device.h>

#define gf_dbg(fmt, args...) do { \
		pr_debug("gf:" fmt, ##args);\
} while (0)

static int gf3208_request_named_gpio(struct gf_dev *gf_dev,
			const char *label, int *gpio)
{
	struct device *dev = &gf_dev->spi->dev;
	struct device_node *np = dev->of_node;
	int rc = of_get_named_gpio(np, label, 0);

	if (rc < 0) {
		dev_err(dev, "failed to get '%s'\n", label);
		return rc;
	}
	*gpio = rc;

	rc = devm_gpio_request(dev, *gpio, label);
	if (rc) {
		dev_err(dev, "failed to request gpio %d\n", *gpio);
		return rc;
	}

	dev_err(dev, "%s %d\n", label, *gpio);
	return 0;
}

static int select_pin_ctl(struct gf_dev *gf_dev, const char *name)
{
	size_t i;
	int rc;
	struct device *dev = &gf_dev->spi->dev;

	for (i = 0; i < ARRAY_SIZE(gf_dev->pinctrl_state); i++) {
		const char *n = pctl_names[i];
		if (!strncmp(n, name, strlen(n))) {
			rc = pinctrl_select_state(gf_dev->fingerprint_pinctrl,gf_dev->pinctrl_state[i]);

			if (rc)
				dev_err(dev, "cannot select '%s'\n", name);
			else
				dev_err(dev, "Selected '%s'\n", name);

			goto exit;
		}
	}
	rc = -EINVAL;
	dev_err(dev, "%s:'%s' not found\n", __func__, name);
exit:
	return rc;
}

/*GPIO pins reference.*/
int gf_parse_dts(struct gf_dev *gf_dev)
{
 	int rc = 0;
 	int i = 0;

    /*get reset resource*/
	rc = gf3208_request_named_gpio(gf_dev,"goodix,gpio_reset",
			&gf_dev->reset_gpio);
    if (rc) {
		gf_dbg("Failed to request RESET GPIO. rc = %d\n", rc);
		return -EPERM;
    }

    /*get irq resource*/
    rc = gf3208_request_named_gpio(gf_dev,"goodix,gpio_irq",
			&gf_dev->irq_gpio);
    if (rc) {
		gf_dbg("Failed to request IRQ GPIO. rc = %d\n", rc);
		return -EPERM;
    }

    gf_dev->fingerprint_pinctrl = devm_pinctrl_get(&gf_dev->spi->dev);
   	for (i = 0; i < ARRAY_SIZE(gf_dev->pinctrl_state); i++) {
		const char *n = pctl_names[i];
		struct pinctrl_state *state =
			pinctrl_lookup_state(gf_dev->fingerprint_pinctrl, n);
		if (IS_ERR(state)) {
			pr_err("cannot find '%s'\n", n);
			rc = -EINVAL;
		}
		gf_dev->pinctrl_state[i] = state;
	}

	rc = select_pin_ctl(gf_dev, "goodixfp_reset_active");
	if (rc)
		goto exit;

	rc = select_pin_ctl(gf_dev, "goodixfp_irq_active");
	if (rc)
		goto exit;

exit:
	 return rc;
}

void gf_cleanup(struct gf_dev	* gf_dev)
{
    gf_dbg("[info]  enter%s\n",__func__);

    if (gpio_is_valid(gf_dev->irq_gpio)) {
        devm_gpio_free(&gf_dev->spi->dev,gf_dev->irq_gpio);
        gf_dbg("remove irq_gpio success\n");
    }

    if (gpio_is_valid(gf_dev->reset_gpio)) {
        devm_gpio_free(&gf_dev->spi->dev,gf_dev->reset_gpio);
        gf_dbg("remove reset_gpio success\n");
    }

	if (gf_dev->fingerprint_pinctrl != NULL) {
  	  	devm_pinctrl_put(gf_dev->fingerprint_pinctrl);
		gf_dev->fingerprint_pinctrl = NULL;
		gf_dbg("gx  fingerprint_pinctrl  release success\n");
	}
}

/*power management*/
int gf_power_on(struct gf_dev *gf_dev)
{
    int rc = 0;

    msleep(10);

    return rc;
}

int gf_power_off(struct gf_dev *gf_dev)
{
    int rc = 0;

    return rc;
}

static int hw_reset(struct  gf_dev *gf_dev)
{
	int irq_gpio;
	struct device *dev = &gf_dev->spi->dev;
	int rc ;

	rc = select_pin_ctl(gf_dev, "goodixfp_reset_reset");
	if (rc)
		goto exit;
	mdelay(3);

	rc = select_pin_ctl(gf_dev, "goodixfp_reset_active");
	if (rc)
		goto exit;

	irq_gpio = gpio_get_value(gf_dev->irq_gpio);
	dev_info(dev, "IRQ after reset %d\n", irq_gpio);
exit:
	return rc;
}

/********************************************************************
 *CPU output low level in RST pin to reset GF. This is the MUST action for GF.
 *Take care of this function. IO Pin driver strength / glitch and so on.
 ********************************************************************/
int gf_hw_reset(struct gf_dev *gf_dev, unsigned int delay_ms)
{
    if (gf_dev == NULL)
        return -EPERM;
    hw_reset(gf_dev);
	
    mdelay(delay_ms);
    return 0;
}

int gf_irq_num(struct gf_dev *gf_dev)
{
    if (gf_dev == NULL)
        return -EPERM;
    else
        return gpio_to_irq(gf_dev->irq_gpio);
}

