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
#include <linux/cpu.h>
#include <linux/workqueue.h>
#include <linux/sched.h>
#include <linux/timer.h>
#include <linux/cpufreq.h>
#include <linux/earlysuspend.h>

#define CPU_UP_LOAD 50
#define CPU_HIGH_LOAD 90

#define MAX_POSSIBLE_CPUS 2

#define DOWN_TIMER_COUNT 10

#define HOTPLUG_WORK_DELAY msecs_to_jiffies(HZ)
#define INIT_HOTPLUG_WORK_DELAY (HZ * 20)

struct hotplug_vars
{
	unsigned int online_cpus; /* number of online cpus */
	unsigned long hotplug_time; /* delay between each hotplug work */
	unsigned int down_timer; /* delay between each cpu offline */
} vars;

static struct delayed_work hotplug_work;
static struct workqueue_struct *wq;

static inline void online_cpu1(unsigned long now)
{
	cpu_up(1);
	pr_info("high load - hot-plug.\n");

	vars.hotplug_time = now;
	vars.online_cpus = num_online_cpus();
	vars.down_timer = 0;
}

static inline void offline_cpu1(unsigned long now)
{
	cpu_down(1);
	pr_info("low load - hot-unplug.\n");

	vars.hotplug_time = now;
	vars.online_cpus = num_online_cpus();
	vars.down_timer = 0;
}

static void u8500_hotplug_function(struct work_struct *work)
{
	unsigned int curr_load;
	unsigned long now;

	curr_load = cpufreq_quick_get_util(0);

	if (curr_load >= CPU_HIGH_LOAD)
	{
		cpu_up(1);
		pr_info("extremely high load - hot-plug.\n");
	}

	now = ktime_to_us(ktime_get());
	vars.online_cpus = num_online_cpus();

	if ((now - vars.hotplug_time) < 2000000 / vars.online_cpus)
		goto queue;

	vars.down_timer++;

	pr_debug("%s: current load: %u, online cpus: %u, delay: %u, down timer: %u\n",
		__func__, curr_load, vars.online_cpus, (now - vars.hotplug_time), vars.down_timer);

	if (curr_load >= CPU_UP_LOAD &&
		vars.online_cpus < MAX_POSSIBLE_CPUS)
	{
		online_cpu1(now);
	}

	else if (vars.down_timer >= DOWN_TIMER_COUNT &&
		vars.online_cpus == MAX_POSSIBLE_CPUS)
	{
		offline_cpu1(now);
	}

queue:
	queue_delayed_work_on(0, wq, &hotplug_work, HOTPLUG_WORK_DELAY);
}

static void u8500_hotplug_late_resume(struct early_suspend *handler)
{
	if (vars.online_cpus < MAX_POSSIBLE_CPUS)
	{
		cpu_up(1);
		pr_info("late resume - hot-plug.\n");

		vars.online_cpus = num_online_cpus();
		vars.hotplug_time = ktime_to_us(ktime_get());
		vars.down_timer = 0;
	}

	queue_delayed_work_on(0, wq, &hotplug_work, HZ);
}

static void u8500_hotplug_early_suspend(struct early_suspend *handler)
{
	flush_workqueue(wq);
	cancel_delayed_work_sync(&hotplug_work);

	if (vars.online_cpus == MAX_POSSIBLE_CPUS)
	{
		cpu_down(1);
		pr_info("early suspend - hot-unplug.\n");

		vars.online_cpus = num_online_cpus();
		vars.hotplug_time = ktime_to_us(ktime_get());
	}
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
	vars.hotplug_time = 0;
	vars.down_timer = 0;

	wq = alloc_workqueue("u8500_hotplug_workqueue", WQ_FREEZABLE, 1);

	if (!wq)
		return -ENOMEM;

	INIT_DELAYED_WORK(&hotplug_work, u8500_hotplug_function);

	queue_delayed_work_on(0, wq, &hotplug_work, INIT_HOTPLUG_WORK_DELAY);

	register_early_suspend(&u8500_hotplug_suspend);

	return 0;
}
late_initcall(u8500_hotplug_init);
