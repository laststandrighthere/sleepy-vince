/*
* Copyright (C) 2014  Peel Technologies Inc
* Copyright (C) 2018 XiaoMi, Inc.
*/

#include <linux/init.h>
#include <linux/module.h>
#include <linux/fs.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/list.h>
#include <linux/errno.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/compat.h>
#include <linux/delay.h>
#include <linux/kthread.h>

#include <linux/mm.h>
#include <linux/slab.h>

#ifdef CONFIG_OF
#include <linux/regulator/consumer.h>
#include <linux/of_device.h>
#include <linux/of.h>
#endif

#include <linux/gpio.h>
#include <linux/miscdevice.h>
#include <linux/spi/spi.h>
#include "peelir.h"

#include <asm/uaccess.h>
#include <asm/delay.h>

#define SPI_MODE_MASK (SPI_CPHA | SPI_CPOL | SPI_CS_HIGH \
				| SPI_LSB_FIRST | SPI_3WIRE | SPI_LOOP \
				| SPI_NO_CS | SPI_READY)

#ifndef CONFIG_OF
#define LR_EN		73
#endif

#define TRACE printk("@@@@ %s, %d\n", __func__, __LINE__);

#define USES_MMAP
struct peelir_data {
	dev_t			devt;
	struct spi_device	*spi;
	struct mutex		buf_lock;
	spinlock_t		spi_lock;
	unsigned		users;
	u8			*buffer;
};

static unsigned int npages = 150;
static unsigned bufsiz;
u32 is_gpio_used;

#ifndef CONFIG_OF
static int mode = 0, bpw = 32, spi_clk_freq = 960000;
#endif

static int lr_en, in_use , rcount;
static int prev_tx_status;
static u32 field;
static const char  *reg_id;
static struct regulator *ir_reg;
u8 *p_buf;
struct peelir_data *peel_data_g;

#ifdef USES_MMAP
static void *kmalloc_ptr;
static int *kmalloc_area;
#endif

static struct spi_transfer t;

static int ir_regulator_set(bool enable)
{
	int rc = 0;

#ifdef CONFIG_OF
	if (ir_reg) {
		if (enable)
			rc = regulator_enable(ir_reg);
		else
			rc = regulator_disable(ir_reg);
	}
#endif

	return rc;
}

static inline int
peelir_read(struct peelir_data *peelir, size_t len)
{
	struct spi_message	m;

	t.rx_buf = peelir->buffer;
	t.len = len;
	t.tx_buf = NULL;

	memset(peelir->buffer, 0, len); TRACE
	spi_message_init(&m);
	spi_message_add_tail(&t, &m);

	return spi_sync(peelir->spi, &m);
}

static int peelir_read_message(struct peelir_data *peelir,
		struct spi_ioc_transfer *u_xfers)
{
	u8 *buf;

	memset(peelir->buffer, 0, bufsiz);

	buf = peelir->buffer; TRACE
	if (u_xfers->len > bufsiz) {
		return -EMSGSIZE;
	}

	peelir_read(peelir, bufsiz); TRACE

	if (u_xfers->rx_buf) {
		if (__copy_to_user((u8 __user *)
				(uintptr_t) u_xfers->rx_buf, buf, u_xfers->len))
			return -EFAULT;
	}

	return 0;
}

static inline int
peelir_write(struct peelir_data *peelir, size_t len)
{
	struct spi_message m;

	t.tx_buf = peelir->buffer;
	t.len = len;
	t.bits_per_word	= peelir->spi->bits_per_word;

	spi_message_init(&m); TRACE
	spi_message_add_tail(&t, &m);

	return spi_sync(peelir->spi, &m);
}

static int peelir_write_message(struct peelir_data *peelir,
		struct spi_ioc_transfer *u_xfers)
{
	u8 *buf;
	int status = -EFAULT;

	buf = peelir->buffer; TRACE

	if (u_xfers->len > bufsiz)
		status = -EMSGSIZE;

	if (u_xfers->tx_buf)
		if (copy_from_user(buf, (const u8 __user *)
					(uintptr_t) u_xfers->tx_buf,
					u_xfers->len))

	peelir->spi->bits_per_word = u_xfers->bits_per_word;

	status = peelir_write(peelir, u_xfers->len); TRACE
	return status;
}

static long
peelir_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
	int retval = 0;
	struct peelir_data	*peelir;
	struct spi_ioc_transfer	*ioc;
	struct strIds *id;
	int rc = 0;

	peelir = filp->private_data; TRACE

	mutex_lock(&peelir->buf_lock);

	switch (cmd) {
		case SPI_IOC_WR_MSG:
			ioc = kmalloc(sizeof(struct spi_ioc_transfer), GFP_KERNEL);
			if (!ioc) {
				retval = -ENOMEM;
				break;
			}

			if (__copy_from_user(ioc, (void __user *)arg,
					sizeof(struct spi_ioc_transfer))) {
				kfree(ioc);
				retval = -EFAULT;
				break;
			}

			rc = ir_regulator_set(1);
			if (!rc) {
				retval = peelir_write_message(peelir, ioc);
			}

			if (retval > 0)
				prev_tx_status = 1;
			else
				prev_tx_status = 0;

			ir_regulator_set(0);
			kfree(ioc);

			break;

		case SPI_IOC_RD_MSG:
			if (is_gpio_used)
				gpio_set_value(lr_en, 1);

			ioc = kmalloc(sizeof(struct spi_ioc_transfer), GFP_KERNEL);
			if (!ioc) {
				pr_err("%s: No memory for ioc. Exiting\n", __func__);
				retval = -ENOMEM;
				break;
			}

			if (__copy_from_user(ioc, (void __user *)arg,
					sizeof(struct spi_ioc_transfer))) {
				pr_err("%s: Error performing copy from user of ioc\n",
						__func__);
				kfree(ioc);
				retval = -EFAULT;
				break;
			}

			rc = ir_regulator_set(1);
			if (!rc) {
				retval = peelir_read_message(peelir, ioc);
			}

			ir_regulator_set(0);
			if (is_gpio_used)
				gpio_set_value(lr_en, 0);
			break;

		case SPI_IOC_RD_IDS:
			id = kmalloc(sizeof(struct strIds), GFP_KERNEL);
			if (!id) {
				retval = -ENOMEM;
				break;
			}
			id->u32ID1 = 0xad1a4100;
			id->u32ID2 = 0x3c03d40;
			id->u32ID3 = 0xb5300000;

			if (__copy_to_user((void __user *)arg , id,
					sizeof(struct strIds))) {
				kfree(id);
				retval = -EFAULT;
				break;
			}
			break;
	}

	mutex_unlock(&peelir->buf_lock);

	return retval;
}

static int peelir_open(struct inode *inode, struct file *filp)
{
	struct peelir_data *peelir;
	int	status = 0;

	peelir = peel_data_g; TRACE
	if (in_use) {
		dev_err(&peelir->spi->dev, "%s: Device in use. users = %d\n",
			__func__, in_use);
		return -EBUSY;
	}

	peelir->buffer = p_buf;
	if (!peelir->buffer) {
		if (!peelir->buffer) {
			dev_dbg(&peelir->spi->dev, "open/ENOMEM\n");
			status = -ENOMEM;
		}
	}
	if (status == 0) {
		peelir->users++;
		filp->private_data = peel_data_g;
		nonseekable_open(inode, filp);
	}
	rcount = 0;

	return status;
}

static int peelir_release(struct inode *inode, struct file *filp)
{
	int	status = 0;

	in_use = 0; TRACE
	peel_data_g->users = 0;
	filp->private_data = NULL;
	rcount = 0;

	return status;
}

#ifdef USES_MMAP
int peelir_mmap(struct file *filp, struct vm_area_struct *vma)
{
	int ret;
	struct peelir_data *peelir;
	long length = vma->vm_end - vma->vm_start;

	peelir = (struct peelir_data *)filp->private_data; TRACE
	if (length > bufsiz)
		return -EIO;

	ret = remap_pfn_range(vma, vma->vm_start,
				virt_to_phys((void *)kmalloc_area) >> PAGE_SHIFT,
				length,
				vma->vm_page_prot);
	if (ret < 0)
		return ret;

	return 0;
}
#endif

static ssize_t ir_tx_status(struct device *dev,
			struct device_attribute *attr, char *buf)
{TRACE
	return snprintf(buf, strlen(buf) + 1, "%d\n", prev_tx_status);
}

static ssize_t field_show(struct device *dev,
			struct device_attribute *attr, char *buf)
{TRACE
	return snprintf(buf, strlen(buf) + 1, "%x\n", field);
}

static ssize_t field_store(struct device *dev,
			struct device_attribute *attr, const char *buf, size_t count)
{TRACE
	sscanf(buf, "%x", &field);
	return count;
}

static DEVICE_ATTR(txstat, S_IRUGO, ir_tx_status, NULL);
static DEVICE_ATTR(field, S_IRUGO | S_IWUSR, field_show, field_store);

static struct attribute *peel_attributes[] = {
	&dev_attr_txstat.attr,
	&dev_attr_field.attr,
	NULL,
};

static struct attribute_group attr_group = {
	.attrs = peel_attributes,
};

static const struct file_operations peel_dev_fops = {
	.owner = THIS_MODULE,
	.open = peelir_open,
	.release = peelir_release,
	.unlocked_ioctl	= peelir_ioctl,
	.compat_ioctl = peelir_ioctl,
#ifdef USES_MMAP
	.mmap = peelir_mmap,
#endif
};

static struct miscdevice peel_dev_drv = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "peel_ir",
	.fops = &peel_dev_fops,
	.nodename = "peel_ir",
	.mode = 0666
};

static int peelir_probe(struct spi_device *spi)
{
	struct peelir_data *peelir;
	int	status;
	struct device_node *np = spi->dev.of_node;
	#ifdef CONFIG_OF
	u32 bpw, mode; TRACE
	#endif

	peelir = kzalloc(sizeof(*peelir), GFP_KERNEL);
	if (!peelir)
		return -ENOMEM;

	peelir->spi = spi;
	spin_lock_init(&peelir->spi_lock);
	mutex_init(&peelir->buf_lock);
	spi_set_drvdata(spi, peelir);
	peel_data_g = peelir;
	in_use = 0;

	#ifdef CONFIG_OF
	of_property_read_u32(np, "peel_ir,spi-bpw", &bpw); TRACE
	of_property_read_u32(np, "peel_ir,spi-clk-speed", &spi->max_speed_hz);
	of_property_read_u32(np, "peel_ir,spi-mode", &mode);
	of_property_read_u32(np, "peel_ir,lr-gpio-valid", &is_gpio_used);
	of_property_read_u32(np, "peel_ir,peel-field", &field);
	of_property_read_u32(np, "peel_ir,lr-gpio", &lr_en);
	of_property_read_string(np, "peel_ir,reg-id", &reg_id);
	if (reg_id) {
		ir_reg = regulator_get(&(spi->dev), reg_id);
		if (IS_ERR(ir_reg)) {
			printk(KERN_ERR "ir regulator_get fail.\n");
			return PTR_ERR(ir_reg);
		}
	}
	spi->bits_per_word = (u8)bpw;
	spi->mode = (u8)mode;
	#else
	lr_en = LR_EN; TRACE
	spi->bits_per_word = bpw;
	spi->max_speed_hz = spi_clk_freq;
	is_gpio_used = 1;
	#endif

	if (is_gpio_used) {
		if (gpio_is_valid(lr_en)) {
			status = gpio_request(lr_en, "lr_enable");
			if (status) {
				pr_debug("unable to request gpio [%d]: %d\n",
					 lr_en, status);
			}
			status = gpio_direction_output(lr_en, 0);
			if (status) {
				pr_debug("unable to set direction for gpio [%d]: %d\n",
					lr_en, status);
			}
			gpio_set_value(lr_en, 0);
		} else {
			pr_debug("gpio %d is not valid \n", lr_en);
		}
	}
	misc_register(&peel_dev_drv);
	status = sysfs_create_group(&spi->dev.kobj, &attr_group);
	if (status)
		dev_dbg(&spi->dev, " Error creating sysfs entry ");

	return status;
}

static int peelir_remove(struct spi_device *spi)
{
	struct peelir_data	*peelir = spi_get_drvdata(spi);

	sysfs_remove_group(&spi->dev.kobj, &attr_group); TRACE

	spin_lock_irq(&peelir->spi_lock);
	peelir->spi = NULL;
	spi_set_drvdata(spi, NULL);
	spin_unlock_irq(&peelir->spi_lock);

	if (peelir->users == 0) {
		kfree(peelir);
		kfree(p_buf);
	} else {
		return -EBUSY;
	}

	return 0;
}
#ifdef CONFIG_OF
static const struct of_device_id peel_of_match[] = {
	{.compatible = "peel_ir"},
};
MODULE_DEVICE_TABLE(of, peel_of_match);
#endif

static struct spi_driver peelir_spi_driver = {
	.driver = {
		.name = "peel_ir",
		.owner = THIS_MODULE,
		#ifdef CONFIG_OF
		.of_match_table = peel_of_match,
		#endif
	},
	.probe = peelir_probe,
	.remove = peelir_remove,
};

static int __init peelir_init(void)
{
	int status;

	bufsiz = npages * PAGE_SIZE;
	if (bufsiz % PAGE_SIZE) {
		return -EINVAL;
	}

	p_buf = kzalloc(bufsiz, GFP_KERNEL|GFP_ATOMIC);
	if (p_buf == NULL)
		return -ENOMEM;

#ifdef USES_MMAP
	kmalloc_ptr = p_buf;
	kmalloc_area = (int *)((((unsigned long)kmalloc_ptr) +
			PAGE_SIZE - 1) & PAGE_MASK);
#endif

	status = spi_register_driver(&peelir_spi_driver);
	if (status < 0 || p_buf == NULL) {
		printk("%s: Error registerign peel driver\n", __func__);
		return -ENODEV;
	}

	return status;
}
module_init(peelir_init);

static void __exit peelir_exit(void)
{
	spi_unregister_driver(&peelir_spi_driver);
	misc_deregister(&peel_dev_drv);
}
module_exit(peelir_exit);

MODULE_DESCRIPTION("Peel IR SPI driver");
MODULE_LICENSE("GPL");
MODULE_ALIAS("PEEL_IR");
MODULE_AUTHOR("Preetam S Reddy <preetam.reddy@peel.com>");

