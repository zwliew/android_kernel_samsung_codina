/*
 * arch/arm/kernel/u8500_hotplug.c
 *
 * Copyright (C) 2014 zwliew <zhaoweiliew@gmail.com>
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * a hotplug driver for u8500 cpus
 *
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/device.h>
#include <linux/miscdevice.h>
#include <linux/cpu.h>
#include <linux/workqueue.h>
#include <linux/sched.h>
#include <linux/platform_device.h>
#include <linux/timer.h>
#include <linux/cpufreq.h>
#include <linux/delay.h>
#include <linux/input.h>
#include <linux/jiffies.h>
#include <linux/earlysuspend.h>

#define DEFAULT_LOAD_THRESHOLD 70
#define DEFAULT_COUNTER_THRESHOLD 5
#define DEFAULT_WORK_DELAY 1
#define DEFAULT_HIGH_LOAD_THRESHOLD 95
#define DEFAULT_UP_TIMER_THRESHOLD 10

extern bool boosted;

struct cpu_stats {
	unsigned int online_cpus;
	unsigned int counter;
	unsigned int up_timer;
} stats = {
	.counter = 0,
	.up_timer = 0,
};

struct hotplug_tunables
{
	unsigned int load_threshold; /* cpu load at which cpu1 goes online */
	unsigned int counter_threshold; /* time to wait before cpu1 goes offlines */
	unsigned int work_delay; /* frequency at which the driver polls for cpu load and hotplugs */
	unsigned int high_load_threshold; /* cpu load at which cpu1 always go online no matter what */
	unsigned int up_timer_threshold; /* time to wait before onlining cpu1 */
} tunables;

static struct workqueue_struct *wq;
static struct delayed_work hotplug_work;
static struct work_struct suspend, resume;

static void online_one(unsigned int load)
{
	struct hotplug_tunables *t = &tunables;

	if (load >= t->high_load_threshold)
	{
		pr_debug("u8500_hotplug: extreme high load\n");

		if (stats.online_cpus < num_possible_cpus())
		{
			cpu_up(stats.online_cpus);
			stats.online_cpus = num_online_cpus();

			pr_debug("u8500_hotplug: extreme high load online\n");
		}

		stats.up_timer = 0;
	}
	else if (stats.up_timer >= t->up_timer_threshold)
	{
		pr_debug("u8500_hotplug: high load\n");

		if (stats.online_cpus < num_possible_cpus())
		{
			cpu_up(stats.online_cpus);
			stats.online_cpus = num_online_cpus();

			pr_debug("u8500_hotplug: high load online\n");
		}

		stats.up_timer = 0;
	}

	stats.counter = 0;
}

static void offline_one(void)
{
	struct hotplug_tunables *t = &tunables;

	if (stats.counter >= t->counter_threshold)
	{
		pr_debug("u8500_hotplug: low load\n");

		if (stats.online_cpus == num_possible_cpus())
		{
			cpu_down(stats.online_cpus - 1);
			stats.online_cpus = num_online_cpus();

			pr_debug("u8500_hotplug: low load offline\n");
		}

		stats.counter = 0;
		stats.up_timer = 0;
	}

	stats.counter++;
}

static void __ref hotplug_work_fn(struct work_struct *work)
{
	unsigned int cur_load;
	struct hotplug_tunables *t = &tunables;

	cur_load = cpufreq_quick_get_util(0);

	stats.up_timer++;

	if (cur_load >= t->load_threshold)
	{
		online_one(cur_load);
	}
	else
	{
		offline_one();
	}

	queue_delayed_work_on(0, wq, &hotplug_work,
		msecs_to_jiffies(t->work_delay * HZ));
}

static void u8500_hotplug_suspend(struct work_struct *work)
{
	int cpu;

	for_each_online_cpu(cpu)
	{
		if (!cpu)
			continue;

		cpu_down(cpu);
	}

	stats.online_cpus = num_online_cpus();
	stats.counter = 0;
	stats.up_timer = 0;

	pr_info("u8500_hotplug: suspend\n");
}

static void __ref u8500_hotplug_resume(struct work_struct *work)
{
	int cpu;

	for_each_possible_cpu(cpu)
	{
		if (!cpu)
			continue;

		cpu_up(cpu);
	}

	stats.online_cpus = num_online_cpus();
	stats.counter = 0;
	stats.up_timer = 0;

	pr_info("u8500_hotplug: resume\n");
}

static void u8500_hotplug_early_suspend(struct early_suspend *handler)
{
	schedule_work(&suspend);
}

static void u8500_hotplug_late_resume(struct early_suspend *handler)
{
	schedule_work(&resume);
}

static struct early_suspend early_suspend =
{
	.level = EARLY_SUSPEND_LEVEL_DISABLE_FB,
	.suspend = u8500_hotplug_early_suspend,
	.resume = u8500_hotplug_late_resume,
};

/* tunables userspace start */
static ssize_t load_threshold_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct hotplug_tunables *t = &tunables;

	return sprintf(buf, "%u\n", t->load_threshold);
}

static ssize_t load_threshold_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t size)
{
	struct hotplug_tunables *t = &tunables;

	unsigned int new_val;

	sscanf(buf, "%u", &new_val);

	if (new_val != t->load_threshold && new_val >= 0 && new_val <= 100)
	{
		t->load_threshold = new_val;
	}

	return size;
}

static ssize_t counter_threshold_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct hotplug_tunables *t = &tunables;

	return sprintf(buf, "%u\n", t->counter_threshold);
}

static ssize_t counter_threshold_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t size)
{
	struct hotplug_tunables *t = &tunables;

	unsigned int new_val;

	sscanf(buf, "%u", &new_val);

	if (new_val != t->counter_threshold && new_val >= 0 && new_val <= 100)
	{
		t->counter_threshold = new_val;
	}

	return size;
}

static ssize_t work_delay_show(struct device *dev, struct device_attribute *attr,
		char *buf)
{
	struct hotplug_tunables *t = &tunables;

	return sprintf(buf, "%u\n", t->work_delay);
}

static ssize_t work_delay_store(struct device *dev, struct device_attribute *attr,
		const char *buf, size_t size)
{
	struct hotplug_tunables *t = &tunables;

	unsigned int new_val;

	sscanf(buf, "%u", &new_val);

	if (new_val != t->work_delay && new_val >= 0 && new_val <= 100)
	{
		t->work_delay = new_val;
	}

	return size;
}

static ssize_t high_load_threshold_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct hotplug_tunables *t = &tunables;

	return sprintf(buf, "%u\n", t->high_load_threshold);
}

static ssize_t high_load_threshold_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t size)
{
	struct hotplug_tunables *t = &tunables;

	unsigned int new_val;

	sscanf(buf, "%u", &new_val);

	if (new_val != t->high_load_threshold && new_val >= 0 && new_val <= 100)
	{
		t->high_load_threshold = new_val;
	}

	return size;
}

static ssize_t up_timer_threshold_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct hotplug_tunables *t = &tunables;

	return sprintf(buf, "%u\n", t->up_timer_threshold);
}

static ssize_t up_timer_threshold_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t size)
{
	struct hotplug_tunables *t = &tunables;

	unsigned int new_val;

	sscanf(buf, "%u", &new_val);

	if (new_val != t->up_timer_threshold && new_val >= 0 && new_val <= 100)
	{
		t->up_timer_threshold = new_val;
	}

	return size;
}

static DEVICE_ATTR(load_threshold, 0664, load_threshold_show, load_threshold_store);
static DEVICE_ATTR(counter_threshold, 0664, counter_threshold_show, counter_threshold_store);
static DEVICE_ATTR(work_delay, 0664, work_delay_show, work_delay_store);
static DEVICE_ATTR(high_load_threshold, 0664, high_load_threshold_show, high_load_threshold_store);
static DEVICE_ATTR(up_timer_threshold, 0664, up_timer_threshold_show, up_timer_threshold_store);

static struct attribute *u8500_hotplug_control_attributes[] =
{
	&dev_attr_load_threshold.attr,
	&dev_attr_counter_threshold.attr,
	&dev_attr_work_delay.attr,
	&dev_attr_high_load_threshold.attr,
	&dev_attr_up_timer_threshold.attr,
	NULL
};

/* tunables userspace end */

static struct attribute_group u8500_hotplug_control_group =
{
	.attrs  = u8500_hotplug_control_attributes,
};

static struct miscdevice u8500_hotplug_control_device =
{
	.minor = MISC_DYNAMIC_MINOR,
	.name = "u8500_hotplug_control",
};

static int __devinit u8500_hotplug_probe(struct platform_device *pdev)
{
	int ret = 0;
	struct hotplug_tunables *t = &tunables;

	wq = alloc_workqueue("u8500_hotplug_workqueue", WQ_FREEZABLE, 1);

	if (!wq)
	{
		ret = -ENOMEM;
		goto err;
	}

	t->load_threshold = DEFAULT_LOAD_THRESHOLD;
	t->counter_threshold = DEFAULT_COUNTER_THRESHOLD;
	t->work_delay = DEFAULT_WORK_DELAY;
	t->high_load_threshold = DEFAULT_HIGH_LOAD_THRESHOLD;
	t->up_timer_threshold = DEFAULT_UP_TIMER_THRESHOLD;

	stats.online_cpus = num_online_cpus();

	ret = misc_register(&u8500_hotplug_control_device);

	if (ret)
	{
		ret = -EINVAL;
		goto err;
	}

	ret = sysfs_create_group(&u8500_hotplug_control_device.this_device->kobj,
			&u8500_hotplug_control_group);

	if (ret)
	{
		ret = -EINVAL;
		goto err;
	}

	register_early_suspend(&early_suspend);

	INIT_WORK(&resume, u8500_hotplug_resume);
	INIT_WORK(&suspend, u8500_hotplug_suspend);
	INIT_DELAYED_WORK(&hotplug_work, hotplug_work_fn);

	queue_delayed_work_on(0, wq, &hotplug_work, HZ * 20);

err:
	return ret;
}

static struct platform_device u8500_hotplug_device = {
	.name = "u8500_hotplug",
	.id = -1,
};

static int u8500_hotplug_remove(struct platform_device *pdev)
{
	destroy_workqueue(wq);

	return 0;
}

static struct platform_driver u8500_hotplug_driver = {
	.probe = u8500_hotplug_probe,
	.remove = u8500_hotplug_remove,
	.driver = {
		.name = "u8500_hotplug",
		.owner = THIS_MODULE,
	},
};

static int __init u8500_hotplug_init(void)
{
	int ret;

	ret = platform_driver_register(&u8500_hotplug_driver);

	if (ret)
	{
		return ret;
	}

	ret = platform_device_register(&u8500_hotplug_device);

	if (ret)
	{
		return ret;
	}

	pr_info("u8500_hotplug: init\n");

	return ret;
}

static void __exit u8500_hotplug_exit(void)
{
	platform_device_unregister(&u8500_hotplug_device);
	platform_driver_unregister(&u8500_hotplug_driver);
}

late_initcall(u8500_hotplug_init);
module_exit(u8500_hotplug_exit);