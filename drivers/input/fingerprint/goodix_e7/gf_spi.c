/*Simple synchronous userspace interface to SPI devices
 *
 * Copyright (C) 2006 SWAPP
 *     Andrea Paterniani <a.paterniani@swapp-eng.it>
 * Copyright (C) 2007 David Brownell (simplification, cleanup)
 * Copyright (C) 2018 XiaoMi, Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */
#include <linux/init.h>
#include <linux/module.h>
#include <linux/ioctl.h>
#include <linux/fs.h>
#include <linux/device.h>
#include <linux/input.h>
#include <linux/clk.h>
#include <linux/err.h>
#include <linux/list.h>
#include <linux/errno.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/compat.h>
#include <linux/delay.h>
#include <asm/uaccess.h>
#include <linux/ktime.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/gpio.h>
#include <linux/regulator/consumer.h>
#include <linux/of_gpio.h>
#include <linux/timer.h>
#include <linux/notifier.h>
#include <linux/fb.h>
#include <linux/pm_qos.h>
#include <linux/cpufreq.h>
#include "gf_spi.h"

#include <linux/platform_device.h>

#define GF_SPIDEV_NAME     "goodix,fingerprint"
/*device name after register in charater*/
#define GF_DEV_NAME            "goodix_fp"
#define	GF_INPUT_NAME	    "uinput-goodix"	/*"goodix_fp" */

#define	CHRD_DRIVER_NAME	"goodix_fp_spi"
#define	CLASS_NAME		    "goodix_fp"
#define SPIDEV_MAJOR		225	/* assigned */
#define N_SPI_MINORS		32	/* ... up to 256 */

#define GF_INPUT_HOME_KEY KEY_HOMEPAGE /* KEY_HOME */
#define GF_INPUT_MENU_KEY  KEY_MENU
#define GF_INPUT_BACK_KEY  KEY_BACK
#define GF_INPUT_FF_KEY  KEY_POWER
#define GF_INPUT_CAMERA_KEY  KEY_CAMERA
#define GF_INPUT_OTHER_KEY KEY_VOLUMEDOWN  /* temporary key value for capture use */
#define GF_NAV_UP_KEY  KEY_GESTURE_NAV_UP
#define GF_NAV_DOWN_KEY  KEY_GESTURE_NAV_DOWN
#define GF_NAV_LEFT_KEY  KEY_GESTURE_NAV_LEFT
#define GF_NAV_RIGHT_KEY  KEY_GESTURE_NAV_RIGHT

#define GF_CLICK_KEY  114
#define GF_DOUBLE_CLICK_KEY  115
#define GF_LONG_PRESS_KEY  217

struct gf_key_map key_map[] = {
    { "POWER", KEY_POWER }, { "HOME", KEY_HOME },
    {"MENU", KEY_MENU }, { "BACK", KEY_BACK },
    { "UP", KEY_UP }, { "DOWN", KEY_DOWN },
    { "LEFT", KEY_LEFT }, { "RIGHT", KEY_RIGHT },
    { "FORCE", KEY_F9 }, { "CLICK", KEY_F19 },
};

/**************************debug******************************/
/*Global variables*/
/*static MODE g_mode = GF_IMAGE_MODE;*/
static DECLARE_BITMAP(minors, N_SPI_MINORS);
static LIST_HEAD(device_list);
static DEFINE_MUTEX(device_list_lock);
static struct gf_dev gf;
static struct wakeup_source fp_wakelock;
static int driver_init_partial(struct gf_dev *gf_dev);
static void nav_event_input(struct gf_dev *gf_dev, gf_nav_event_t nav_event);

static void gf_enable_irq(struct gf_dev *gf_dev)
{
    if (gf_dev->irq_enabled) {
        pr_warn("IRQ has been enabled.\n");
    } else {
        enable_irq_wake(gf_dev->irq);
        gf_dev->irq_enabled = 1;
    }
}

static void gf_disable_irq(struct gf_dev *gf_dev)
{
    if (gf_dev->irq_enabled) {
        gf_dev->irq_enabled = 0;
        disable_irq_wake(gf_dev->irq);
    } else {
        pr_warn("IRQ has been disabled.\n");
    }
}

static long gf_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
    struct gf_dev *gf_dev = &gf;
    struct gf_key gf_key;
    int retval = 0;
    u8 netlink_route = NETLINK_TEST;
    struct gf_ioc_chip_info info;
    uint32_t key_event;
    gf_nav_event_t nav_event = GF_NAV_NONE;

    if (_IOC_TYPE(cmd) != GF_IOC_MAGIC)
        return -ENODEV;

    if (_IOC_DIR(cmd) & _IOC_READ)
        retval = !access_ok(VERIFY_WRITE, (void __user *) arg, _IOC_SIZE(cmd));

    if ((retval == 0) && (_IOC_DIR(cmd) & _IOC_WRITE))
        retval = !access_ok(VERIFY_READ, (void __user *) arg, _IOC_SIZE(cmd));
        
    if (retval)
        return -EFAULT;

    if (gf_dev->device_available == 0)
        if (!((cmd == GF_IOC_ENABLE_POWER) ||
                (cmd == GF_IOC_DISABLE_POWER)))
            return -ENODEV;

    switch (cmd) {
        case GF_IOC_INIT:
            if (copy_to_user((void __user *) arg,
                (void *) &netlink_route, sizeof(u8))) {
                retval = -EFAULT;
                break;
            }
            break;

        case GF_IOC_DISABLE_IRQ:
            gf_disable_irq(gf_dev);
            break;

        case GF_IOC_ENABLE_IRQ:
            gf_enable_irq(gf_dev);
            break;

        case GF_IOC_RESET:
            gf_hw_reset(gf_dev, 3);
            break;

        case GF_IOC_ENABLE_GPIO:
            driver_init_partial(gf_dev);
            break;

        case GF_IOC_RELEASE_GPIO:
            gf_disable_irq(gf_dev);
            devm_free_irq(&gf_dev->spi->dev,gf_dev->irq,gf_dev);
            gf_cleanup(gf_dev);
            break;

        case GF_IOC_INPUT_KEY_EVENT:
            if (copy_from_user(&gf_key, (struct gf_key *) arg,
                    sizeof(struct gf_key))) {
                retval = -EFAULT;
                break;
            }

            if (GF_KEY_HOME == gf_key.key)
                key_event = KEY_SELECT;
            else if (GF_KEY_POWER == gf_key.key)
                key_event = GF_INPUT_FF_KEY;
            else if (GF_KEY_CAPTURE == gf_key.key)
                key_event = GF_INPUT_CAMERA_KEY;
            else if (GF_KEY_LONG_PRESS == gf_key.key)
                key_event = GF_LONG_PRESS_KEY;
            else if (GF_KEY_DOUBLE_TAP == gf_key.key)
                key_event = GF_DOUBLE_CLICK_KEY;
            else if (GF_KEY_TAP == gf_key.key)
                key_event = GF_CLICK_KEY;
            else
                key_event = gf_key.key;

            if ((GF_KEY_POWER == gf_key.key || GF_KEY_CAPTURE == gf_key.key)
                    && (gf_key.value == 1)) {
                input_report_key(gf_dev->input, key_event, 1);
                input_sync(gf_dev->input);
                input_report_key(gf_dev->input, key_event, 0);
                input_sync(gf_dev->input);
            } else if (GF_KEY_UP == gf_key.key) {
                input_report_key(gf_dev->input, GF_NAV_UP_KEY, 1);
                input_sync(gf_dev->input);
                input_report_key(gf_dev->input, GF_NAV_UP_KEY, 0);
                input_sync(gf_dev->input);
            } else if (GF_KEY_DOWN == gf_key.key) {
                input_report_key(gf_dev->input, GF_NAV_DOWN_KEY, 1);
                input_sync(gf_dev->input);
                input_report_key(gf_dev->input, GF_NAV_DOWN_KEY, 0);
                input_sync(gf_dev->input);
            } else if (GF_KEY_RIGHT == gf_key.key) {
                input_report_key(gf_dev->input, GF_NAV_RIGHT_KEY, 1);
                input_sync(gf_dev->input);
                input_report_key(gf_dev->input, GF_NAV_RIGHT_KEY, 0);
                input_sync(gf_dev->input);
            } else if (GF_KEY_LEFT == gf_key.key) {
                input_report_key(gf_dev->input, GF_NAV_LEFT_KEY, 1);
                input_sync(gf_dev->input);
                input_report_key(gf_dev->input, GF_NAV_LEFT_KEY, 0);
                input_sync(gf_dev->input);
            } else if ((GF_KEY_POWER != gf_key.key)
                    && (GF_KEY_CAPTURE != gf_key.key)) {
                input_report_key(gf_dev->input, key_event, gf_key.value);
                input_sync(gf_dev->input);
            }
            break;

        case GF_IOC_ENABLE_POWER:
            if (gf_dev->device_available != 1)
                gf_power_on(gf_dev);
            gf_dev->device_available = 1;
            break;

        case GF_IOC_DISABLE_POWER:
            if (gf_dev->device_available != 0)
                gf_power_off(gf_dev);
            gf_dev->device_available = 0;
            break;

        case GF_IOC_CHIP_INFO:
            if (copy_from_user(&info, (struct gf_ioc_chip_info *) arg,
                    sizeof(struct gf_ioc_chip_info))) {
                retval = -EFAULT;
                break;
            }
            break;

        case GF_IOC_NAV_EVENT:
            if (copy_from_user(&nav_event, (gf_nav_event_t *)arg,
                    sizeof(gf_nav_event_t))) {
                retval = -EFAULT;
                break;
            }
            nav_event_input(gf_dev, nav_event);
            break;

        default:
            break;
    }
    return retval;
}

static void nav_event_input(struct gf_dev *gf_dev, gf_nav_event_t nav_event)
{
	uint32_t nav_input = 0;

	switch (nav_event) {
	    case GF_NAV_DOWN:
            input_report_key(gf_dev->input, GF_NAV_UP_KEY, 1);
            input_sync(gf_dev->input);
            input_report_key(gf_dev->input, GF_NAV_UP_KEY, 0);
            input_sync(gf_dev->input);
            break;

	    case GF_NAV_UP:
            input_report_key(gf_dev->input, GF_NAV_DOWN_KEY, 1);
                input_sync(gf_dev->input);
                input_report_key(gf_dev->input, GF_NAV_DOWN_KEY, 0);
                input_sync(gf_dev->input);
            break;

	    case GF_NAV_LEFT:
            input_report_key(gf_dev->input, GF_NAV_RIGHT_KEY, 1);
                input_sync(gf_dev->input);
                input_report_key(gf_dev->input, GF_NAV_RIGHT_KEY, 0);
                input_sync(gf_dev->input);
            break;

	    case GF_NAV_RIGHT:
            input_report_key(gf_dev->input, GF_NAV_LEFT_KEY, 1);
                input_sync(gf_dev->input);
                input_report_key(gf_dev->input, GF_NAV_LEFT_KEY, 0);
                input_sync(gf_dev->input);
            break;

        case GF_NAV_HEAVY:
            nav_input = GF_NAV_INPUT_HEAVY;
            break;

        case GF_NAV_LONG_PRESS:
            nav_input = GF_NAV_INPUT_LONG_PRESS;
            break;

        case GF_NAV_DOUBLE_CLICK:
            nav_input = GF_NAV_INPUT_DOUBLE_CLICK;
            break;

        default:
            break;
        }
}

#ifdef CONFIG_COMPAT
static long
gf_compat_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
    return gf_ioctl(filp, cmd, (unsigned long)compat_ptr(arg));
}
#endif

static irqreturn_t gf_irq(int irq, void *handle)
{
    char temp = GF_NET_EVENT_IRQ;
    __pm_wakeup_event(&fp_wakelock, msecs_to_jiffies(1000));
    sendnlmsg(&temp);

    return IRQ_HANDLED;
}

static int gf_open(struct inode *inode, struct file *filp)
{
    struct gf_dev *gf_dev;
    int status = -ENXIO;

    mutex_lock(&device_list_lock);

    list_for_each_entry(gf_dev, &device_list, device_entry)
    {
        if (gf_dev->devt == inode->i_rdev) {
            pr_info("Found\n");
            status = 0;
            break;
        }
    }

    if (status == 0) {
        if (status == 0) {
            gf_dev->users++;
            filp->private_data = gf_dev;
            nonseekable_open(inode, filp);
            gf_dev->device_available = 1;
        }
    }
    mutex_unlock(&device_list_lock);

    return status;
}

static int gf_release(struct inode *inode, struct file *filp)
{
    struct gf_dev *gf_dev;
    int status = 0;

    mutex_lock(&device_list_lock);
    gf_dev = filp->private_data;
    filp->private_data = NULL;

    /*last close?? */
    gf_dev->users--;
    if (!gf_dev->users) {
        gf_disable_irq(gf_dev);
        devm_free_irq(&gf_dev->spi->dev,gf_dev->irq,gf_dev);
        gf_cleanup(gf_dev);
        /*power off the sensor*/
        gf_dev->device_available = 0;
        gf_power_off(gf_dev);
    }
    mutex_unlock(&device_list_lock);
    return status;
}

static const struct file_operations gf_fops = {
    .owner = THIS_MODULE,
    .unlocked_ioctl = gf_ioctl,
#ifdef CONFIG_COMPAT
    .compat_ioctl = gf_compat_ioctl,
#endif
    .open = gf_open, .release = gf_release,
};

static int goodix_fb_state_chg_callback(struct notifier_block *nb,
        unsigned long val, void *data)
{
    struct gf_dev *gf_dev;
    struct fb_event *evdata = data;
    unsigned int blank;
    char temp = 0;

    if (val != FB_EARLY_EVENT_BLANK)
        return 0;

    gf_dev = container_of(nb, struct gf_dev, notifier);

    if (evdata && evdata->data && val ==
            FB_EARLY_EVENT_BLANK && gf_dev) {
        blank = *(int *) (evdata->data);

        switch (blank) {
            case FB_BLANK_POWERDOWN:
                if (gf_dev->device_available == 1) {
                    gf_dev->fb_black = 1;
                    temp = GF_NET_EVENT_FB_BLACK;
                    sendnlmsg(&temp);
                }
                break;

            case FB_BLANK_UNBLANK:
                if (gf_dev->device_available == 1) {
                    gf_dev->fb_black = 0;
                    temp = GF_NET_EVENT_FB_UNBLACK;
                    sendnlmsg(&temp);
                }
                break;

            default:
                break;
            }
    }
    return NOTIFY_OK;
}

static struct notifier_block goodix_noti_block = {
    .notifier_call =
    goodix_fb_state_chg_callback,
};

static void gf_reg_key_kernel(struct gf_dev *gf_dev)
{
    int i;

    set_bit(EV_KEY, gf_dev->input->evbit);

    for (i = 0; i < ARRAY_SIZE(key_map); i++)
        set_bit(key_map[i].val, gf_dev->input->keybit);

    set_bit(KEY_SELECT, gf_dev->input->keybit);

    gf_dev->input->name = GF_INPUT_NAME;
    if (input_register_device(gf_dev->input))
        pr_warn("Failed to register GF as input device.\n");
}

static int driver_init_partial(struct gf_dev *gf_dev)
{
	int ret = 0;

	gf_dev->device_available = 1;

	if (gf_parse_dts(gf_dev))
		goto error;

	gf_dev->irq = gf_irq_num(gf_dev);
	ret = devm_request_threaded_irq(&gf_dev->spi->dev,
					gf_dev->irq,
					NULL,
					gf_irq,
					IRQF_TRIGGER_RISING | IRQF_ONESHOT,
					"gf",gf_dev);
	
    if (ret){
		pr_err("Could not request irq %d\n",gpio_to_irq(gf_dev->irq_gpio));
		goto error;
	}

	if (!ret) {
		gf_enable_irq(gf_dev);
		gf_disable_irq(gf_dev);
	}

    gf_hw_reset(gf_dev, 360);

	return 0;

error:
	gf_cleanup(gf_dev);
	gf_dev->device_available = 0;
	return -EPERM;
}

static struct class *gf_class;

static int gf_probe(struct platform_device *pdev)
{
    struct gf_dev *gf_dev = &gf;
    int status = -EINVAL;
    unsigned long minor;

    /* Initialize the driver data */
    INIT_LIST_HEAD(&gf_dev->device_entry);
    gf_dev->spi = pdev;
    gf_dev->irq_gpio = -EINVAL;
    gf_dev->reset_gpio = -EINVAL;
    gf_dev->pwr_gpio = -EINVAL;
    gf_dev->device_available = 0;
    gf_dev->fb_black = 0;
    gf_dev->irq_enabled = 0;
    gf_dev->fingerprint_pinctrl = NULL;

    mutex_lock(&device_list_lock);

    minor = find_first_zero_bit(minors, N_SPI_MINORS);
    if (minor < N_SPI_MINORS) {
        struct device *dev;

        gf_dev->devt = MKDEV(SPIDEV_MAJOR, minor);
        dev = device_create(gf_class, &gf_dev->spi->dev, gf_dev->devt, gf_dev,
        GF_DEV_NAME);
        status = IS_ERR(dev) ? PTR_ERR(dev) : 0;
    } else {
        dev_dbg(&gf_dev->spi->dev, "no minor number available!\n");
        status = -ENODEV;
    }

    if (status == 0) {
        set_bit(minor, minors);
        list_add(&gf_dev->device_entry, &device_list);
    } else {
        gf_dev->devt = 0;
    }
    mutex_unlock(&device_list_lock);

    if (status == 0) {
        gf_dev->input = input_allocate_device();

        if (gf_dev->input == NULL) {
            status = -ENOMEM;
	        goto error;
        }

        __set_bit(EV_KEY, gf_dev->input->evbit);
        __set_bit(GF_INPUT_HOME_KEY, gf_dev->input->keybit);
        __set_bit(GF_INPUT_MENU_KEY, gf_dev->input->keybit);
        __set_bit(GF_INPUT_BACK_KEY, gf_dev->input->keybit);
        __set_bit(GF_INPUT_FF_KEY, gf_dev->input->keybit);
        __set_bit(GF_NAV_UP_KEY, gf_dev->input->keybit);
        __set_bit(GF_NAV_DOWN_KEY, gf_dev->input->keybit);
        __set_bit(GF_NAV_RIGHT_KEY, gf_dev->input->keybit);
        __set_bit(GF_NAV_LEFT_KEY, gf_dev->input->keybit);
        __set_bit(GF_INPUT_CAMERA_KEY, gf_dev->input->keybit);
        __set_bit(GF_CLICK_KEY, gf_dev->input->keybit);
        __set_bit(GF_DOUBLE_CLICK_KEY, gf_dev->input->keybit);
        __set_bit(GF_LONG_PRESS_KEY, gf_dev->input->keybit);
    }

    gf_dev->notifier = goodix_noti_block;
    fb_register_client(&gf_dev->notifier);
    gf_reg_key_kernel(gf_dev);

    wakeup_source_init(&fp_wakelock, "fp_wakelock");

    return status;

error:
    gf_cleanup(gf_dev);
    gf_dev->device_available = 0;

    if (gf_dev->devt != 0) {
        pr_info("Err: status = %d\n", status);
        mutex_lock(&device_list_lock);
        list_del(&gf_dev->device_entry);
        device_destroy(gf_class, gf_dev->devt);
        clear_bit(MINOR(gf_dev->devt), minors);
        mutex_unlock(&device_list_lock);

        if (gf_dev->input != NULL)
            input_unregister_device(gf_dev->input);
    }

    return status;
}

static int gf_remove(struct platform_device *pdev)
{
    struct gf_dev *gf_dev = &gf;

    /* make sure ops on existing fds can abort cleanly */
    if (gf_dev->irq)
        free_irq(gf_dev->irq, gf_dev);

    if (gf_dev->input != NULL)
        input_unregister_device(gf_dev->input);
    input_free_device(gf_dev->input);

    /* prevent new opens */
    mutex_lock(&device_list_lock);
    list_del(&gf_dev->device_entry);
    device_destroy(gf_class, gf_dev->devt);
    clear_bit(MINOR(gf_dev->devt), minors);
    if (gf_dev->users == 0)
        gf_cleanup(gf_dev);

    fb_unregister_client(&gf_dev->notifier);
    mutex_unlock(&device_list_lock);
    wakeup_source_trash(&fp_wakelock);
    return 0;
}

static int gf_suspend(struct platform_device *pdev,
            pm_message_t state)
{
    return 0;
}

static int gf_resume(struct platform_device *pdev)
{
    return 0;
}

static struct of_device_id gx_match_table[] = {
    { .compatible = GF_SPIDEV_NAME, },
    { },
};

static struct platform_driver gf_driver = {
    .driver = {
        .name = GF_DEV_NAME, .owner = THIS_MODULE,
        .of_match_table = gx_match_table,
    },
    .probe = gf_probe,
    .remove = gf_remove,
    .suspend = gf_suspend,
    .resume = gf_resume,
};

static int __init gf_init(void)
{
    int status;

    BUILD_BUG_ON(N_SPI_MINORS > 256);

    status = register_chrdev(SPIDEV_MAJOR, CHRD_DRIVER_NAME, &gf_fops);
    if (status < 0) {
        pr_warn("Failed to register char device!\n");
        return status;
    }

    gf_class = class_create(THIS_MODULE, CLASS_NAME);
    if (IS_ERR(gf_class)) {
        unregister_chrdev(SPIDEV_MAJOR, gf_driver.driver.name);
        pr_warn("Failed to create class.\n");
        return PTR_ERR(gf_class);
    }

    status = platform_driver_register(&gf_driver);
    if (status < 0) {
        class_destroy(gf_class);
        unregister_chrdev(SPIDEV_MAJOR, gf_driver.driver.name);
        pr_warn("Failed to register SPI driver.\n");
    }

    netlink_init();

    return 0;
}

module_init(gf_init);

static void __exit gf_exit(void)
{
    netlink_exit();
    platform_driver_unregister(&gf_driver);
    class_destroy(gf_class);
    unregister_chrdev(SPIDEV_MAJOR, gf_driver.driver.name);
}

module_exit(gf_exit);

MODULE_AUTHOR("Jiangtao Yi, <yijiangtao@goodix.com>");
MODULE_DESCRIPTION("User mode SPI device interface");
MODULE_LICENSE("GPL");
MODULE_ALIAS("spi:gf-spi");
