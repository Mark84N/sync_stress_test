#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/list.h>
#include <linux/interrupt.h>
#include <linux/timer.h>
#include <linux/slab.h>
#include <linux/delay.h>
#include <linux/random.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("DMDE");

#define INTERVAL_NEW_LEAVE 250
#define INTERVAL_DEL_LEAVE 1500
#define INTERVAL_EXPIRE_MINOR 20 * HZ

struct major {
	struct list_head minors_head;
	struct timer_list add_timer;
	struct timer_list remove_timer;
	int minors_count;
	spinlock_t major_lock;
	struct tasklet_struct cleanup_tasklet;
	atomic_t cleanup;
} *maj;

struct minor {
	struct list_head list;
	struct list_head leaves_head;
	struct timer_list timer;
	struct major *maj;
	spinlock_t minor_lock;
	int leaves_count;
	int is_alive;
	int id;
};

struct leave {
	struct list_head list;
	int leave_id;
	char payload[32];
};

static int get_rand_id(int del)
{
	int rand255;

	BUG_ON(!del);
	get_random_bytes(&rand255, 1);
	return rand255 % del;
}

static void add_leave(struct minor *min, int id)
{
	struct leave *l;

	l = kzalloc(sizeof(*l), GFP_ATOMIC);
	if (!l) {
		pr_crit("%s %d: -ENOMEM\n", __func__, __LINE__);
		return;
	}

	l->leave_id = id;
	INIT_LIST_HEAD(&l->list);
	list_add(&l->list, &min->leaves_head);
	min->leaves_count++;
}

static void del_leave(struct minor *min, int id)
{
	struct leave *l, *tmp;

	list_for_each_entry_safe(l, tmp, &min->leaves_head, list) {
		if (l->leave_id == id) {
			list_del(&l->list);
			kfree(l);
			min->leaves_count--;
		}
		if (!min->leaves_count) {
			min->is_alive = 0;
			tasklet_schedule(&min->maj->cleanup_tasklet);
		}
	}
}

static void del_all_leaves(struct minor *min)
{
	struct leave *l, *tmp;

	list_for_each_entry_safe(l, tmp, &min->leaves_head, list) {
			list_del(&l->list);
			kfree(l);
			min->leaves_count--;
	}
}

static struct minor *find_minor_by_id(struct major* maj, int id)
{
	struct minor *min = NULL;

	list_for_each_entry(min, &maj->minors_head, list)
		if (min->id == id)
			return min;
	return NULL;
}

static void tasklet_cleanup_handler(unsigned long arg)
{
	struct major *maj = (struct major *)arg;
	struct minor *ptr, *tmp;

	spin_lock_bh(&maj->major_lock);
	list_for_each_entry_safe(ptr, tmp, &maj->minors_head, list) {
retry:
		spin_lock_bh(&ptr->minor_lock);
		if (ptr->is_alive == 0) {
			if (-1 == try_to_del_timer_sync(&ptr->timer)) {
				pr_crit("%s %d: timer handler for min %d is running, will allow it to finish\n", 
							__func__, __LINE__, ptr->id);
				spin_unlock_bh(&ptr->minor_lock);
				goto retry;
			}

			list_del(&ptr->list);
			del_all_leaves(ptr);
			spin_unlock_bh(&ptr->minor_lock);
			kfree(ptr);
			continue;
		}
		spin_unlock_bh(&ptr->minor_lock);
	}
	spin_unlock_bh(&maj->major_lock);
	atomic_set(&maj->cleanup, 0);
}

static void minor_expire_timer_function(unsigned long arg)
{
	struct minor *min = (struct minor *)arg;
	
	spin_lock_bh(&min->minor_lock);
	if (!timer_pending(&min->timer) && !atomic_read(&min->maj->cleanup)) {
		min->is_alive = 0;
		tasklet_schedule(&min->maj->cleanup_tasklet);
	}
	spin_unlock_bh(&min->minor_lock);
}

static struct minor *init_min(struct major *maj, int id)
{
	struct minor *min = NULL;

	min = (struct minor *)kzalloc(sizeof(*min), GFP_ATOMIC);
	if (!min) {
		pr_crit("%s() %d -ENOMEM\n",__func__,  __LINE__);
		return NULL;
	}

	INIT_LIST_HEAD(&min->list);
	INIT_LIST_HEAD(&min->leaves_head);
	spin_lock_init(&min->minor_lock);
	list_add(&min->list, &maj->minors_head);
	min->is_alive = 1;
	min->id = id;
	min->maj = maj;
	init_timer(&min->timer);
	min->timer.function = minor_expire_timer_function;
	min->timer.expires = jiffies + INTERVAL_EXPIRE_MINOR;
	min->timer.data = (unsigned long)min;
	add_timer(&min->timer);

	return min;
}

static void major_add_leave_handler(unsigned long arg)
{
	struct major *maj = (struct major *)arg;
	struct minor *min = NULL;
	int leave_id, minor_id;
	
	leave_id = get_rand_id(10);
	minor_id = leave_id / 2;

	if (atomic_read(&maj->cleanup))
		return;

	spin_lock_bh(&maj->major_lock);
	min = find_minor_by_id(maj, minor_id);
	if (min) {
		spin_lock_bh(&min->minor_lock);
		mod_timer(&min->timer, jiffies + INTERVAL_EXPIRE_MINOR);
		min->is_alive = 1;
		spin_unlock_bh(&min->minor_lock);
	} else {
		min = init_min(maj, minor_id);
	}

	if (min) {
		spin_lock_bh(&min->minor_lock);
		add_leave(min, leave_id);
		spin_unlock_bh(&min->minor_lock);
	}
	mod_timer(&maj->add_timer, jiffies + INTERVAL_NEW_LEAVE);
	spin_unlock_bh(&maj->major_lock);
}

static void major_remove_leave_handler(unsigned long arg)
{
	struct major *maj = (struct major *)arg;
	struct minor *min;
	int leave_id, minor_id;

	if (atomic_read(&maj->cleanup)) {
		pr_crit("%s %d: return because of cleanup\n", __func__, __LINE__);
		return;
	}

	leave_id = get_rand_id(10);
	minor_id = leave_id / 2;

	spin_lock_bh(&maj->major_lock);

	min = find_minor_by_id(maj, minor_id);
	if (min) {
		spin_lock_bh(&min->minor_lock);
		del_leave(min, leave_id);
		spin_unlock_bh(&min->minor_lock);
	}
	mod_timer(&maj->remove_timer, jiffies + INTERVAL_DEL_LEAVE);
	spin_unlock_bh(&maj->major_lock);
}

int init_module(void)
{
	maj = kzalloc(sizeof(struct major), GFP_KERNEL);
	if (!maj) {
		pr_crit("%s() %d -ENOMEM\n",__func__,  __LINE__);
		return -ENOMEM;
	}

	spin_lock_init(&maj->major_lock);
	INIT_LIST_HEAD(&maj->minors_head);
	init_timer(&maj->add_timer);
	maj->add_timer.expires = jiffies + INTERVAL_NEW_LEAVE;
	maj->add_timer.function = major_add_leave_handler;
	maj->add_timer.data = (long unsigned int)maj;
	add_timer(&maj->add_timer);

	init_timer(&maj->remove_timer);
	maj->remove_timer.expires = jiffies + INTERVAL_DEL_LEAVE;
	maj->remove_timer.function = major_remove_leave_handler;
	maj->remove_timer.data = (long unsigned int)maj;
	add_timer(&maj->remove_timer);
	atomic_set(&maj->cleanup, 0);

	tasklet_init(&maj->cleanup_tasklet, tasklet_cleanup_handler, (long unsigned int)maj);

	return 0;
}

static void minor_mark_all_to_remove(struct major *maj)
{
	struct minor *min;

	spin_lock_bh(&maj->major_lock);
	list_for_each_entry(min, &maj->minors_head, list) {
		spin_lock_bh(&min->minor_lock);
		min->is_alive = 0;
		spin_unlock_bh(&min->minor_lock);
	}
	spin_unlock_bh(&maj->major_lock);
	tasklet_schedule(&maj->cleanup_tasklet);
}

void cleanup_module(void)
{
	atomic_set(&maj->cleanup, 1);
	del_timer_sync(&maj->add_timer);
	del_timer_sync(&maj->remove_timer);
	minor_mark_all_to_remove(maj);

	while(1 == atomic_read(&maj->cleanup))
		pr_crit("%s() looping to wait for removal completion\n", __func__);

	kfree(maj);
}