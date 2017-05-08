/*
 * Copyright (C) 2017, Sultanxda <sultanxda@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#define pr_fmt(fmt) "iosched-swch: " fmt

#include <linux/blkdev.h>
#include <linux/blk_types.h>
#include <linux/elevator.h>
#include <linux/fb.h>

#define NOOP_IOSCHED "noop"
#define RESTORE_DELAY_MS (5000)

struct iosched_conf {
	struct delayed_work restore_prev;
	struct delayed_work sleep_sched;
	struct request_queue *queue;
	char prev_e[ELV_NAME_MAX];
	bool using_noop;
};

static struct iosched_conf *config_g;

static void change_elevator(struct iosched_conf *c, bool use_noop)
{
	struct request_queue *q = c->queue;
	char name[ELV_NAME_MAX];

	if (c->using_noop == use_noop)
		return;

	c->using_noop = use_noop;

	spin_lock_irq(q->queue_lock);
	strcpy(name, q->elevator->type->elevator_name);
	spin_unlock_irq(q->queue_lock);

	if (use_noop) {
		if (strcmp(name, NOOP_IOSCHED)) {
			strcpy(c->prev_e, name);
			elevator_change(q, NOOP_IOSCHED);
		}
	} else {
		if (!strcmp(name, NOOP_IOSCHED))
			elevator_change(q, c->prev_e);
	}
}

static int fb_notifier_callback(struct notifier_block *nb,
		unsigned long action, void *data)
{
	struct iosched_conf *c = config_g;
	struct fb_event *evdata = data;
	int *blank = evdata->data;

	/* Parse framebuffer events as soon as they occur */
	if (action != FB_EARLY_EVENT_BLANK)
		return NOTIFY_OK;

	switch (*blank) {
	case FB_BLANK_UNBLANK:
		/*
		 * Switch back from noop to the original iosched after a delay
		 * when the screen is turned on.
		 */

		if (delayed_work_pending(&sleep_sched))
			cancel_delayed_work_sync(&sleep_sched);
		schedule_delayed_work(&c->restore_prev,
				msecs_to_jiffies(RESTORE_DELAY_MS));
		break;
	default:
		/*
		 * Switch to noop when the screen turns off. Purposely block
		 * the fb notifier chain call in case weird things can happen
		 * when switching elevators while the screen is off.
		 */
		if (delayed_work_pending(&restore_prev))
			cancel_delayed_work_sync(&restore_prev);
		schedule_delayed_work(&sleep_sched,
				msecs_to_jiffies(RESTORE_DELAY_MS));
	}

	return NOTIFY_OK;
}

static struct notifier_block fb_notifier_callback_nb = {
	.notifier_call = fb_notifier_callback,
};

static void restore_prev_fn(struct work_struct *work)
{
	struct iosched_conf *c = container_of(work, typeof(*c),
						restore_prev.work);

	change_elevator(c, false);
}

static void set_sleep_sched_fn(struct work_struct *work)
{
	change_all_elevators(&req_queues.list, true);
}

int init_iosched_switcher(struct request_queue *q)
{
	struct iosched_conf *c;

	if (!q) {
		pr_err("Request queue is NULL!\n");
		return -EINVAL;
	}

	if (config_g) {
		pr_err("Already registered a request queue!\n");
		return -EINVAL;
	}

	c = kzalloc(sizeof(*c), GFP_KERNEL);
	if (!c)
		return -ENOMEM;

	c->queue = q;

	config_g = c;

	INIT_DELAYED_WORK(&c->restore_prev, restore_prev_fn);
	INIT_DELAYED_WORK(&sleep_sched, set_sleep_sched_fn);
	fb_register_client(&fb_notifier_callback_nb);

	return 0;
}
