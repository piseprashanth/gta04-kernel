/*
 * TWL4030/TPS65950 BCI (Battery Charger Interface) driver
 *
 * Copyright (C) 2010 Gražvydas Ignotas <notasas@gmail.com>
 *
 * based on twl4030_bci_battery.c by TI
 * Copyright (C) 2008 Texas Instruments, Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include <linux/init.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/err.h>
#include <linux/platform_device.h>
#include <linux/interrupt.h>
#include <linux/i2c/twl.h>
#include <linux/power_supply.h>
#include <linux/notifier.h>
#include <linux/usb/otg.h>
#include <linux/regulator/machine.h>

#define TWL4030_BCIMDEN		0x00
#define TWL4030_BCIMDKEY	0x01
#define TWL4030_BCIMSTATEC	0x02
#define TWL4030_BCIICHG		0x08
#define TWL4030_BCIVAC		0x0a
#define TWL4030_BCIVBUS		0x0c
#define TWL4030_BCIMFSTS4	0x10
#define TWL4030_BCICTL1		0x23
#define TWL4030_BB_CFG		0x12
#define TWL4030_BCIIREF1	0x27
#define TWL4030_BCIIREF2	0x28
#define TWL4030_BCIMFKEY	0x11
#define TWL4030_BCIMFEN3	0x14
#define TWL4030_BCIWDKEY	0x21



#define TWL4030_BCIAUTOWEN	BIT(5)
#define TWL4030_CONFIG_DONE	BIT(4)
#define TWL4030_BCIAUTOUSB	BIT(1)
#define TWL4030_BCIAUTOAC	BIT(0)
#define TWL4030_CGAIN		BIT(5)
#define TWL4030_USBFASTMCHG	BIT(2)
#define TWL4030_USBSLOWMCHG     BIT(1)
#define TWL4030_STS_VBUS	BIT(7)
#define TWL4030_STS_USB_ID	BIT(2)
#define TWL4030_BBCHEN		BIT(4)
#define TWL4030_BBSEL_MASK	0x0c
#define TWL4030_BBSEL_2V5	0x00
#define TWL4030_BBSEL_3V0	0x04
#define TWL4030_BBSEL_3V1	0x08
#define TWL4030_BBSEL_3V2	0x0c
#define TWL4030_BBISEL_MASK	0x03
#define TWL4030_BBISEL_25uA	0x00
#define TWL4030_BBISEL_150uA	0x01
#define TWL4030_BBISEL_500uA	0x02
#define TWL4030_BBISEL_1000uA	0x03

/* BCI interrupts */
#define TWL4030_WOVF		BIT(0) /* Watchdog overflow */
#define TWL4030_TMOVF		BIT(1) /* Timer overflow */
#define TWL4030_ICHGHIGH	BIT(2) /* Battery charge current high */
#define TWL4030_ICHGLOW		BIT(3) /* Battery cc. low / FSM state change */
#define TWL4030_ICHGEOC		BIT(4) /* Battery current end-of-charge */
#define TWL4030_TBATOR2		BIT(5) /* Battery temperature out of range 2 */
#define TWL4030_TBATOR1		BIT(6) /* Battery temperature out of range 1 */
#define TWL4030_BATSTS		BIT(7) /* Battery status */

#define TWL4030_VBATLVL		BIT(0) /* VBAT level */
#define TWL4030_VBATOV		BIT(1) /* VBAT overvoltage */
#define TWL4030_VBUSOV		BIT(2) /* VBUS overvoltage */
#define TWL4030_ACCHGOV		BIT(3) /* Ac charger overvoltage */

#define TWL4030_MSTATEC_USB		BIT(4)
#define TWL4030_MSTATEC_AC		BIT(5)
#define TWL4030_MSTATEC_MASK		0x0f
#define TWL4030_MSTATEC_QUICK1		0x02
#define TWL4030_MSTATEC_QUICK7		0x07
#define TWL4030_MSTATEC_COMPLETE1	0x0b
#define TWL4030_MSTATEC_COMPLETE4	0x0e

static bool allow_usb;
module_param(allow_usb, bool, 0644);
MODULE_PARM_DESC(allow_usb, "Allow USB charge drawing default current");

static int do_lin;
static int default_usb_current = 100000;
module_param(default_usb_current, int, 0644);
MODULE_PARM_DESC(default_usb_current, "Default usb current for newly connected devices in uA");
struct twl4030_bci {
	struct device		*dev;
	struct power_supply	ac;
	struct power_supply	usb;
	struct usb_phy		*transceiver;
	struct notifier_block	usb_nb;
	struct work_struct	work;
	int			irq_chg;
	int			irq_bci;
	struct regulator	*usb_reg;
	int			usb_enabled;

	unsigned long		event;
};

static struct twl4030_bci *bci_glob;


/*
 * clear and set bits on an given register on a given module
 */
static int twl4030_clear_set(u8 mod_no, u8 clear, u8 set, u8 reg)
{
	u8 val = 0;
	int ret;

	ret = twl_i2c_read_u8(mod_no, &val, reg);
	if (ret)
		return ret;

	val &= ~clear;
	val |= set;

	return twl_i2c_write_u8(mod_no, val, reg);
}

static int twl4030_bci_read(u8 reg, u8 *val)
{
	return twl_i2c_read_u8(TWL_MODULE_MAIN_CHARGE, val, reg);
}

static int twl4030_clear_set_boot_bci(u8 clear, u8 set)
{
	return twl4030_clear_set(TWL_MODULE_PM_MASTER, clear,
			TWL4030_CONFIG_DONE | TWL4030_BCIAUTOWEN | set,
			TWL4030_PM_MASTER_BOOT_BCI);
}

static int twl4030bci_read_adc_val(u8 reg)
{
	int ret, temp;
	u8 val;

	/* read MSB */
	ret = twl4030_bci_read(reg + 1, &val);
	if (ret)
		return ret;

	temp = (int)(val & 0x03) << 8;

	/* read LSB */
	ret = twl4030_bci_read(reg, &val);
	if (ret)
		return ret;

	return temp | val;
}

/*
 * Check if VBUS power is present
 */
static int twl4030_bci_have_vbus(struct twl4030_bci *bci)
{
	int ret;
	u8 hwsts;

	ret = twl_i2c_read_u8(TWL_MODULE_PM_MASTER, &hwsts,
			      TWL4030_PM_MASTER_STS_HW_CONDITIONS);
	if (ret < 0)
		return 0;

	dev_dbg(bci->dev, "check_vbus: HW_CONDITIONS %02x\n", hwsts);

	return (hwsts & TWL4030_STS_VBUS);
}
/*
 * TI provided formulas:
 * CGAIN == 0: ICHG = (BCIICHG * 1.7) / (2^10 - 1) - 0.85
 * CGAIN == 1: ICHG = (BCIICHG * 3.4) / (2^10 - 1) - 1.7
 * Here we use integer approximation of:
 * CGAIN == 0: val * 1.6618 - 0.85 * 1000
 * CGAIN == 1: (val * 1.6618 - 0.85 * 1000) * 2
 */
/*
 * convert twl register value for currents into uA
 */
static int regval2ua(int regval, bool cgain)
{
	if (cgain)
		return (regval * 16618 - 850 * 10000) / 5;
	else
		return (regval * 16618 - 850 * 10000) / 10;
}

/*
 * convert uA currents into twl register value
 */
static int ua2regval(int ua, bool cgain)
{
	int ret;
	if (cgain & TWL4030_CGAIN)
		ua /= 2;
	ret = (ua * 10 + 850 * 10000) / 16618;
	/* rounding problems */
	if (ret < 512)
		ret = 512;
	return ret;
}


static int twl4030_charger_set_max_current(int cur)
{
	u8 bcictl1;
	int status;
	/* get setting of CGAIN bit */
	status = twl4030_bci_read(TWL4030_BCICTL1, &bcictl1);
	if (status < 0)
		return status;
	cur = ua2regval(cur, bcictl1 & TWL4030_CGAIN);
	/* wie have only 10 bit */
	if (cur > 0x3ff)
		return -EINVAL;
	/* disable write protection for one write access for BCIIREF */
	status = twl_i2c_write_u8(TWL_MODULE_MAIN_CHARGE, 0xE7,
			TWL4030_BCIMFKEY);
	if (status < 0)
		return status;
	status = twl_i2c_write_u8(TWL_MODULE_MAIN_CHARGE,
			(cur & 0x100) ? 3 : 2, TWL4030_BCIIREF2);
	if (status < 0)
		return status;
	/* disable write protection for one write access for BCIIREF */
	status = twl_i2c_write_u8(TWL_MODULE_MAIN_CHARGE, 0xE7,
			TWL4030_BCIMFKEY);
	if (status < 0)
		return status;
	status = twl_i2c_write_u8(TWL_MODULE_MAIN_CHARGE, cur & 0xff,
			TWL4030_BCIIREF1);
	return status;
}

/*
 * Enable/Disable USB Charge functionality.
 */
static int twl4030_charger_enable_usb(struct twl4030_bci *bci, bool enable)
{
	int ret;

	if (enable) {
		/* Check for USB charger connected */
		if (!twl4030_bci_have_vbus(bci))
			return -ENODEV;

		/* Need to keep regulator on */
		if (!bci->usb_enabled) {
			ret = regulator_enable(bci->usb_reg);
			if (ret) {
				dev_err(bci->dev,
					"Failed to enable regulator\n");
				return ret;
			}
			bci->usb_enabled = 1;
		}

		if (allow_usb)
			twl4030_charger_set_max_current(600000);
		else
			twl4030_charger_set_max_current(default_usb_current);
		if (!do_lin) {
			/* forcing the field BCIAUTOUSB (BOOT_BCI[1]) to 1 */
			ret = twl4030_clear_set_boot_bci(0, TWL4030_BCIAUTOUSB);
			if (ret < 0)
				return ret;
		}
		/* forcing USBFASTMCHG(BCIMFSTS4[2]) to 1 */
		ret = twl4030_clear_set(TWL_MODULE_MAIN_CHARGE,
			TWL4030_USBSLOWMCHG,
			TWL4030_USBFASTMCHG, TWL4030_BCIMFSTS4);
		if (do_lin) {
			ret = twl_i2c_write_u8(TWL_MODULE_MAIN_CHARGE, 0x33,
					TWL4030_BCIWDKEY);
			ret = twl_i2c_write_u8(TWL_MODULE_MAIN_CHARGE, 0x2a,
					TWL4030_BCIMDKEY);
			ret = twl_i2c_write_u8(TWL_MODULE_MAIN_CHARGE, 0x26,
					TWL4030_BCIMDKEY);
			ret = twl_i2c_write_u8(TWL_MODULE_MAIN_CHARGE, 0xf3,
					TWL4030_BCIWDKEY);
			ret = twl_i2c_write_u8(TWL_MODULE_MAIN_CHARGE, 0x9c,
					TWL4030_BCIMFKEY);
			ret = twl_i2c_write_u8(TWL_MODULE_MAIN_CHARGE, 0xf0,
					TWL4030_BCIMFEN3);
		}
	} else {
		if (!do_lin) {
			ret = twl4030_clear_set_boot_bci(TWL4030_BCIAUTOUSB, 0);
		} else {
			ret = twl_i2c_write_u8(TWL_MODULE_MAIN_CHARGE, 0x2a,
					TWL4030_BCIMDKEY);
		}
		if (bci->usb_enabled) {
			regulator_disable(bci->usb_reg);
			bci->usb_enabled = 0;
		}
	}

	return ret;
}

/* ugly, pass bci_glob in another way */
void twl4030_charger_enable_usb_ext(bool enable)
{
	if (bci_glob)
		twl4030_charger_enable_usb(bci_glob, enable);
}

/*
 * Enable/Disable AC Charge funtionality.
 */
static int twl4030_charger_enable_ac(bool enable)
{
	int ret;

	if (enable)
		ret = twl4030_clear_set_boot_bci(0, TWL4030_BCIAUTOAC);
	else
		ret = twl4030_clear_set_boot_bci(TWL4030_BCIAUTOAC, 0);

	return ret;
}

/*
 * Enable/Disable charging of Backup Battery.
 */
static int twl4030_charger_enable_backup(int uvolt, int uamp)
{
	int ret;
	u8 flags;

	if (uvolt < 2500000 ||
	    uamp < 25) {
		/* disable charging of backup battery */
		ret = twl4030_clear_set(TWL_MODULE_PM_RECEIVER,
					TWL4030_BBCHEN, 0, TWL4030_BB_CFG);
		return ret;
	}

	flags = TWL4030_BBCHEN;
	if (uvolt >= 3200000)
		flags |= TWL4030_BBSEL_3V2;
	else if (uvolt >= 3100000)
		flags |= TWL4030_BBSEL_3V1;
	else if (uvolt >= 3000000)
		flags |= TWL4030_BBSEL_3V0;
	else
		flags |= TWL4030_BBSEL_2V5;

	if (uamp >= 1000)
		flags |= TWL4030_BBISEL_1000uA;
	else if (uamp >= 500)
		flags |= TWL4030_BBISEL_500uA;
	else if (uamp >= 150)
		flags |= TWL4030_BBISEL_150uA;
	else
		flags |= TWL4030_BBISEL_25uA;

	ret = twl4030_clear_set(TWL_MODULE_PM_RECEIVER,
				TWL4030_BBSEL_MASK | TWL4030_BBISEL_MASK,
				flags,
				TWL4030_BB_CFG);

	return ret;
}

/*
 * TWL4030 CHG_PRES (AC charger presence) events
 */
static irqreturn_t twl4030_charger_interrupt(int irq, void *arg)
{
	struct twl4030_bci *bci = arg;

	dev_dbg(bci->dev, "CHG_PRES irq\n");
	power_supply_changed(&bci->ac);
	power_supply_changed(&bci->usb);

	return IRQ_HANDLED;
}

/*
 * TWL4030 BCI monitoring events
 */
static irqreturn_t twl4030_bci_interrupt(int irq, void *arg)
{
	struct twl4030_bci *bci = arg;
	u8 irqs1, irqs2;
	int ret;

	ret = twl_i2c_read_u8(TWL4030_MODULE_INTERRUPTS, &irqs1,
			      TWL4030_INTERRUPTS_BCIISR1A);
	if (ret < 0)
		return IRQ_HANDLED;

	ret = twl_i2c_read_u8(TWL4030_MODULE_INTERRUPTS, &irqs2,
			      TWL4030_INTERRUPTS_BCIISR2A);
	if (ret < 0)
		return IRQ_HANDLED;

	dev_dbg(bci->dev, "BCI irq %02x %02x\n", irqs2, irqs1);

	if (irqs1 & (TWL4030_ICHGLOW | TWL4030_ICHGEOC)) {
		/* charger state change, inform the core */
		power_supply_changed(&bci->ac);
		power_supply_changed(&bci->usb);
	}

	/* various monitoring events, for now we just log them here */
	if (irqs1 & (TWL4030_TBATOR2 | TWL4030_TBATOR1))
		dev_warn(bci->dev, "battery temperature out of range\n");

	if (irqs1 & TWL4030_BATSTS)
		dev_crit(bci->dev, "battery disconnected\n");

	if (irqs2 & TWL4030_VBATOV)
		dev_crit(bci->dev, "VBAT overvoltage\n");

	if (irqs2 & TWL4030_VBUSOV)
		dev_crit(bci->dev, "VBUS overvoltage\n");

	if (irqs2 & TWL4030_ACCHGOV)
		dev_crit(bci->dev, "Ac charger overvoltage\n");

	return IRQ_HANDLED;
}

static void twl4030_bci_usb_work(struct work_struct *data)
{
	struct twl4030_bci *bci = container_of(data, struct twl4030_bci, work);

	switch (bci->event) {
	case USB_EVENT_VBUS:
	case USB_EVENT_CHARGER:
		twl4030_charger_enable_usb(bci, true);
		break;
	case USB_EVENT_NONE:
		twl4030_charger_enable_usb(bci, false);
		break;
	}
}

static int twl4030_bci_usb_ncb(struct notifier_block *nb, unsigned long val,
			       void *priv)
{
	struct twl4030_bci *bci = container_of(nb, struct twl4030_bci, usb_nb);

	dev_dbg(bci->dev, "OTG notify %lu\n", val);

	bci->event = val;
	schedule_work(&bci->work);

	return NOTIFY_OK;
}

static ssize_t
twl4030_bci_lin_show(struct device *dev, struct device_attribute *attr,
       char *buf)
{
       int ret = -EINVAL;
       u8 val;
       twl_i2c_read_u8(TWL_MODULE_MAIN_CHARGE, &val, TWL4030_BCIMDEN);
       ret = sprintf(buf, "%d\n", (int)val);
       return ret;
}

static ssize_t
twl4030_bci_lin_store(struct device *dev, struct device_attribute *attr,
               const char *buf, size_t n)
{
       int             status = 0;
       if (sysfs_streq(buf, "on")) {
               status  = twl_i2c_write_u8(TWL_MODULE_PM_MASTER, 0x30,
                       TWL4030_PM_MASTER_BOOT_BCI);

               status = twl_i2c_write_u8(TWL_MODULE_MAIN_CHARGE, 0x33,
                       TWL4030_BCIWDKEY);
               status = twl_i2c_write_u8(TWL_MODULE_MAIN_CHARGE, 0x2a,
                       TWL4030_BCIMDKEY);
               status = twl_i2c_write_u8(TWL_MODULE_MAIN_CHARGE, 0x26,
                       TWL4030_BCIMDKEY);
               status = twl_i2c_write_u8(TWL_MODULE_MAIN_CHARGE, 0xf3,
                       TWL4030_BCIWDKEY);
               do_lin = 1;

       } else  if (sysfs_streq(buf, "off")) {
               status = twl_i2c_write_u8(TWL_MODULE_MAIN_CHARGE, 0x2a,
                       TWL4030_BCIMDKEY);
       } else if (sysfs_streq(buf, "auto")) {
               if (do_lin) {
                       struct twl4030_bci *bci = dev_get_drvdata(dev);
                       status = twl_i2c_write_u8(TWL_MODULE_MAIN_CHARGE,
                               0x2a, TWL4030_BCIMDKEY);
                       do_lin = 0;
                       twl4030_charger_enable_usb(bci, true);
               }
       } else {
               return -EINVAL;
       }


       return (status == 0) ? n : status;
}

static DEVICE_ATTR(lin, 0644, twl4030_bci_lin_show, twl4030_bci_lin_store);


/*
 * sysfs max_current store
 */
static ssize_t
twl4030_bci_max_current_store(struct device *dev, struct device_attribute *attr,
	const char *buf, size_t n)
{
	int cur = 0;
	int status = 0;
	status = kstrtoint(buf, 10, &cur);
	if (status)
		return status;
	if (cur < 0)
		return -EINVAL;
	status = twl4030_charger_set_max_current(cur);
	return (status == 0) ? n : status;
}

/*
 * sysfs max_current show
 */
static ssize_t twl4030_bci_max_current_show(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	int status = 0;
	int cur;
	u8 bcictl1;
	cur = twl4030bci_read_adc_val(TWL4030_BCIIREF1);
	if (cur < 0)
		return cur;
	status = twl4030_bci_read(TWL4030_BCICTL1, &bcictl1);
	if (status < 0)
		return status;
	cur = regval2ua(cur, bcictl1 & TWL4030_CGAIN);
	return scnprintf(buf, PAGE_SIZE, "%u\n", cur);
}

static DEVICE_ATTR(max_current, 0644, twl4030_bci_max_current_show,
			twl4030_bci_max_current_store);

static int twl4030_charger_get_current(void)
{
	int curr;
	int ret;
	u8 bcictl1;

	curr = twl4030bci_read_adc_val(TWL4030_BCIICHG);
	if (curr < 0)
		return curr;

	ret = twl4030_bci_read(TWL4030_BCICTL1, &bcictl1);
	if (ret)
		return ret;
	return regval2ua(curr, bcictl1 & TWL4030_CGAIN);
}

/*
 * Returns the main charge FSM state
 * Or < 0 on failure.
 */
static int twl4030bci_state(struct twl4030_bci *bci)
{
	int ret;
	u8 state;

	ret = twl4030_bci_read(TWL4030_BCIMSTATEC, &state);
	if (ret) {
		pr_err("twl4030_bci: error reading BCIMSTATEC\n");
		return ret;
	}

	dev_dbg(bci->dev, "state: %02x\n", state);

	return state;
}

static int twl4030_bci_state_to_status(int state)
{
	state &= TWL4030_MSTATEC_MASK;
	if (TWL4030_MSTATEC_QUICK1 <= state && state <= TWL4030_MSTATEC_QUICK7)
		return POWER_SUPPLY_STATUS_CHARGING;
	else if (TWL4030_MSTATEC_COMPLETE1 <= state &&
					state <= TWL4030_MSTATEC_COMPLETE4)
		return POWER_SUPPLY_STATUS_FULL;
	else
		return POWER_SUPPLY_STATUS_NOT_CHARGING;
}

static int twl4030_bci_get_property(struct power_supply *psy,
				    enum power_supply_property psp,
				    union power_supply_propval *val)
{
	struct twl4030_bci *bci = dev_get_drvdata(psy->dev->parent);
	int is_charging;
	int state;
	int ret;
	u8 i2cval;

	state = twl4030bci_state(bci);
	if (state < 0)
		return state;

	if (psy->type == POWER_SUPPLY_TYPE_USB)
		is_charging = state & TWL4030_MSTATEC_USB;
	else
		is_charging = state & TWL4030_MSTATEC_AC;

	switch (psp) {
	case POWER_SUPPLY_PROP_STATUS:
		if (is_charging)
			val->intval = twl4030_bci_state_to_status(state);
		else
			val->intval = POWER_SUPPLY_STATUS_NOT_CHARGING;
		break;
	case POWER_SUPPLY_PROP_VOLTAGE_NOW:
		/* charging must be active for meaningful result */
		if (!is_charging)
			return -ENODATA;
		if (psy->type == POWER_SUPPLY_TYPE_USB) {
			ret = twl4030bci_read_adc_val(TWL4030_BCIVBUS);
			if (ret < 0)
				return ret;
			/* BCIVBUS uses ADCIN8, 7/1023 V/step */
			val->intval = ret * 6843;
		} else {
			ret = twl4030bci_read_adc_val(TWL4030_BCIVAC);
			if (ret < 0)
				return ret;
			/* BCIVAC uses ADCIN11, 10/1023 V/step */
			val->intval = ret * 9775;
		}
		break;
	case POWER_SUPPLY_PROP_CURRENT_NOW:
		twl4030_bci_read(TWL4030_BCIMFEN3, &i2cval);
		if ((!is_charging) && (i2cval == 0))
			return -ENODATA;
		/* current measurement is shared between AC and USB */
		ret = twl4030_charger_get_current();
		if (ret < 0)
			return ret;
		val->intval = ret;
		break;
	case POWER_SUPPLY_PROP_ONLINE:
		val->intval = is_charging &&
			twl4030_bci_state_to_status(state) !=
				POWER_SUPPLY_STATUS_NOT_CHARGING;
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

static enum power_supply_property twl4030_charger_props[] = {
	POWER_SUPPLY_PROP_STATUS,
	POWER_SUPPLY_PROP_ONLINE,
	POWER_SUPPLY_PROP_VOLTAGE_NOW,
	POWER_SUPPLY_PROP_CURRENT_NOW,
};

static int __init twl4030_bci_probe(struct platform_device *pdev)
{
	struct twl4030_bci *bci;
	struct twl4030_bci_platform_data *pdata = pdev->dev.platform_data;
	int ret;
	u32 reg;

	bci = kzalloc(sizeof(*bci), GFP_KERNEL);
	if (bci == NULL)
		return -ENOMEM;

	bci->dev = &pdev->dev;
	bci->irq_chg = platform_get_irq(pdev, 0);
	bci->irq_bci = platform_get_irq(pdev, 1);

	platform_set_drvdata(pdev, bci);

	bci_glob = bci;

	bci->ac.name = "twl4030_ac";
	bci->ac.type = POWER_SUPPLY_TYPE_MAINS;
	bci->ac.properties = twl4030_charger_props;
	bci->ac.num_properties = ARRAY_SIZE(twl4030_charger_props);
	bci->ac.get_property = twl4030_bci_get_property;

	ret = power_supply_register(&pdev->dev, &bci->ac);
	if (ret) {
		dev_err(&pdev->dev, "failed to register ac: %d\n", ret);
		goto fail_register_ac;
	}

	bci->usb.name = "twl4030_usb";
	bci->usb.type = POWER_SUPPLY_TYPE_USB;
	bci->usb.properties = twl4030_charger_props;
	bci->usb.num_properties = ARRAY_SIZE(twl4030_charger_props);
	bci->usb.get_property = twl4030_bci_get_property;

	bci->usb_reg = regulator_get(bci->dev, "bci3v1");

	ret = power_supply_register(&pdev->dev, &bci->usb);
	if (ret) {
		dev_err(&pdev->dev, "failed to register usb: %d\n", ret);
		goto fail_register_usb;
	}

	ret = request_threaded_irq(bci->irq_chg, NULL,
			twl4030_charger_interrupt, IRQF_ONESHOT, pdev->name,
			bci);
	if (ret < 0) {
		dev_err(&pdev->dev, "could not request irq %d, status %d\n",
			bci->irq_chg, ret);
		goto fail_chg_irq;
	}

	ret = request_threaded_irq(bci->irq_bci, NULL,
			twl4030_bci_interrupt, IRQF_ONESHOT, pdev->name, bci);
	if (ret < 0) {
		dev_err(&pdev->dev, "could not request irq %d, status %d\n",
			bci->irq_bci, ret);
		goto fail_bci_irq;
	}

	INIT_WORK(&bci->work, twl4030_bci_usb_work);

	bci->transceiver = usb_get_phy(USB_PHY_TYPE_USB2);
	if (!IS_ERR_OR_NULL(bci->transceiver)) {
		bci->usb_nb.notifier_call = twl4030_bci_usb_ncb;
		usb_register_notifier(bci->transceiver, &bci->usb_nb);
	}

	/* Enable interrupts now. */
	reg = ~(u32)(TWL4030_ICHGLOW | TWL4030_ICHGEOC | TWL4030_TBATOR2 |
		TWL4030_TBATOR1 | TWL4030_BATSTS);
	ret = twl_i2c_write_u8(TWL4030_MODULE_INTERRUPTS, reg,
			       TWL4030_INTERRUPTS_BCIIMR1A);
	if (ret < 0) {
		dev_err(&pdev->dev, "failed to unmask interrupts: %d\n", ret);
		goto fail_unmask_interrupts;
	}

	reg = ~(u32)(TWL4030_VBATOV | TWL4030_VBUSOV | TWL4030_ACCHGOV);
	ret = twl_i2c_write_u8(TWL4030_MODULE_INTERRUPTS, reg,
			       TWL4030_INTERRUPTS_BCIIMR2A);
	if (ret < 0)
		dev_warn(&pdev->dev, "failed to unmask interrupts: %d\n", ret);

	if (device_create_file(&pdev->dev, &dev_attr_max_current))
		dev_warn(&pdev->dev, "could not create sysfs file\n");
	if (device_create_file(&pdev->dev, &dev_attr_lin))
		dev_warn(&pdev->dev, "could not create sysfs file\n");


	twl4030_charger_enable_ac(true);
	twl4030_charger_enable_usb(bci, true);
	twl4030_charger_enable_backup(pdata->bb_uvolt,
				      pdata->bb_uamp);

	return 0;

fail_unmask_interrupts:
	if (!IS_ERR_OR_NULL(bci->transceiver)) {
		usb_unregister_notifier(bci->transceiver, &bci->usb_nb);
		usb_put_phy(bci->transceiver);
	}
	free_irq(bci->irq_bci, bci);
fail_bci_irq:
	free_irq(bci->irq_chg, bci);
fail_chg_irq:
	power_supply_unregister(&bci->usb);
fail_register_usb:
	power_supply_unregister(&bci->ac);
fail_register_ac:
	kfree(bci);

	return ret;
}

static int __exit twl4030_bci_remove(struct platform_device *pdev)
{
	struct twl4030_bci *bci = platform_get_drvdata(pdev);
	device_remove_file(&pdev->dev, &dev_attr_max_current);
	device_remove_file(&pdev->dev, &dev_attr_lin);
	twl4030_charger_enable_ac(false);
	twl4030_charger_enable_usb(bci, false);
	twl4030_charger_enable_backup(0, 0);

	/* mask interrupts */
	twl_i2c_write_u8(TWL4030_MODULE_INTERRUPTS, 0xff,
			 TWL4030_INTERRUPTS_BCIIMR1A);
	twl_i2c_write_u8(TWL4030_MODULE_INTERRUPTS, 0xff,
			 TWL4030_INTERRUPTS_BCIIMR2A);

	if (!IS_ERR_OR_NULL(bci->transceiver)) {
		usb_unregister_notifier(bci->transceiver, &bci->usb_nb);
		usb_put_phy(bci->transceiver);
	}
	free_irq(bci->irq_bci, bci);
	free_irq(bci->irq_chg, bci);
	power_supply_unregister(&bci->usb);
	power_supply_unregister(&bci->ac);
	kfree(bci);

	return 0;
}

static struct platform_driver twl4030_bci_driver = {
	.driver	= {
		.name	= "twl4030_bci",
		.owner	= THIS_MODULE,
	},
	.remove	= __exit_p(twl4030_bci_remove),
};

module_platform_driver_probe(twl4030_bci_driver, twl4030_bci_probe);

MODULE_AUTHOR("Gražvydas Ignotas");
MODULE_DESCRIPTION("TWL4030 Battery Charger Interface driver");
MODULE_LICENSE("GPL");
MODULE_ALIAS("platform:twl4030_bci");
