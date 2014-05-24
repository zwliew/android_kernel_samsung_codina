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

#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/cpu.h>
#include <linux/workqueue.h>
#include <linux/sched.h>
#include <linux/timer.h>
#include <linux/earlysuspend.h>
#include <linux/cpufreq.h>

#define UP_LOAD_SINGLE_CPU 50
#define DOWN_LOAD_DUAL_CPU 80
#define MAX_POSSIBLE_CPUS 2
#define MIN_POSSIBLE_CPUS 1
#define HOTPLUG_WORK_DELAY HZ

struct hotplug_vars
{
	unsigned int online_cpus;
	unsigned int stored_time;
};

static struct hotplug_vars vars;
static struct delayed_work hotplug_work;
static struct workqueue_struct *wq;

static void u8500_hotplug_function(struct work_struct *work)
{
	if (!is_interactive)
		goto queue;

	unsigned int now = ktime_to_ms(ktime_get());

	if ((now - vars.stored_time) <= 2000 / vars.online_cpus)
		goto queue;

	unsigned int cpu;
	unsigned int load = 0;

	for_each_online_cpu(cpu)
		load += cpufreq_quick_get_util(cpu);

	if (load >= UP_LOAD_SINGLE_CPU &&
		vars.online_cpus == MIN_POSSIBLE_CPUS)
	{
		cpu_up(1);
		pr_info("high load - hot-plug.\n");

		vars.stored_time = now;
	}

	else if (load <= DOWN_LOAD_DUAL_CPU &&
		vars.online_cpus == MAX_POSSIBLE_CPUS)
	{
		cpu_down(1);
		pr_info("low load - hot-unplug.\n");

		vars.stored_time = now;
	}

queue:
	vars.online_cpus = num_online_cpus();
	queue_delayed_work_on(0, wq, &hotplug_work, HOTPLUG_WORK_DELAY);
}

static void u8500_hotplug_late_resume(struct early_suspend *handler)
{
	if (vars.online_cpus == 1)
	{
		cpu_up(1);
		pr_info("late resume - hot-plug.\n");
	}

	vars.stored_time = ktime_to_ms(ktime_get());
	vars.online_cpus = num_online_cpus();

	queue_delayed_work_on(0, wq, &hotplug_work, HZ);
}

static void u8500_hotplug_early_suspend(struct early_suspend *handler)
{
	flush_workqueue(wq);
	cancel_delayed_work_sync(&hotplug_work);

	if (num_online_cpus() == 2)
	{
		cpu_down(1);
		pr_info("early suspend - hot-unplug.\n");
	}

	vars.stored_time = ktime_to_ms(ktime_get());
	vars.online_cpus = num_online_cpus();
}

static struct early_suspend u8500_hotplug_suspend =
{
	.level = EARLY_SUSPEND_LEVEL_DISABLE_FB,
	.suspend = u8500_hotplug_early_suspend,
	.resume = u8500_hotplug_late_resume,
};

static int u8500_hotplug_init(void)
{
	pr_info("u8500 hotplug init.\n");

	vars.online_cpus = num_online_cpus();
	vars.stored_time = 0;

	wq = alloc_workqueue("u8500_hotplug_workqueue", WQ_FREEZABLE, 1);

	if (!wq)
		return -ENOMEM;

	INIT_DELAYED_WORK(&hotplug_work, u8500_hotplug_function);

	queue_delayed_work_on(0, wq, &hotplug_work, 2000);

	register_early_suspend(&u8500_hotplug_suspend);

	return 0;
}
late_initcall(u8500_hotplug_init);
