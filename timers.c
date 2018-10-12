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

#define INTERVAL_NEW_LEAVE 10
#define INTERVAL_DEL_LEAVE 30
#define INTERVAL_EXPIRE_MINOR 500

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

	pr_crit("%s %d adding LIST %d to minor %d\n", 
				__func__, __LINE__, id, min->id);
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
			pr_crit("%s %d removing LIST %d from minor %d\n", 
						__func__, __LINE__, id, min->id);
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
			break;
	return min;
}

static void tasklet_cleanup_handler(unsigned long arg)
{
	struct major *maj = (struct major *)arg;
	struct minor *ptr, *tmp;

	pr_crit("%s %d: stepped here\n", __func__, __LINE__);

	spin_lock_bh(&maj->major_lock);
	list_for_each_entry_safe(ptr, tmp, &maj->minors_head, list) {
retry:
		spin_lock(&ptr->minor_lock);
		if (ptr->is_alive == 0) {
			if (-1 == try_to_del_timer_sync(&ptr->timer)) {
				pr_crit("%s %d: timer handler is running, will allow it to finish\n", __func__, __LINE__);
				spin_unlock(&ptr->minor_lock);
				goto retry;
			} else 
				pr_crit("%s %d timer deactivated\n", __func__, __LINE__);
			del_all_leaves(ptr);
			spin_unlock(&ptr->minor_lock);
			kfree(ptr);
		}
	}
	spin_unlock_bh(&maj->major_lock);

	atomic_set(&maj->cleanup, 0);
}

static void minor_expire_timer_function(unsigned long arg)
{
	struct minor *min = (struct minor *)arg;
	
	pr_crit("%s %d: stepped here\n", __func__, __LINE__);
	
	spin_lock_bh(&min->minor_lock);
	if (!timer_pending(&min->timer) && !atomic_read(&maj->cleanup)) {
		min->is_alive = 0;
		tasklet_schedule(&min->maj->cleanup_tasklet);
	}
	spin_unlock_bh(&min->minor_lock);
}

static struct minor *init_min(struct major *maj, int id)
{
	struct minor *min;

	min = (struct minor *)kzalloc(sizeof(*min), GFP_ATOMIC);
	if (!min) {
		pr_crit("%s() %d -ENOMEM\n",__func__,  __LINE__);
		return min;
	}

	INIT_LIST_HEAD(&min->list);
	INIT_LIST_HEAD(&min->leaves_head);
	spin_lock_init(&min->minor_lock);
	list_add(&min->list, &maj->minors_head);
	min->is_alive = 1;
	init_timer(&min->timer);
	min->timer.function = minor_expire_timer_function;
	min->timer.expires = jiffies + INTERVAL_EXPIRE_MINOR;
	add_timer(&min->timer);
	min->id = id;
	min->maj = maj;

	return min;
}

static void major_add_leave_handler(unsigned long arg)
{
	struct major *maj = (struct major *)arg;
	struct minor *min = NULL;
	static int count = 20;    
	int leave_id, minor_id;
	
	leave_id = get_rand_id(10);
	minor_id = leave_id / 2;

	pr_crit("%s %d: stepped here, count %d\n", __func__, __LINE__, count);

	if (atomic_read(&maj->cleanup))
		return;

	if (!count)
		return;

	count--;

	spin_lock_bh(&maj->major_lock);
	min = find_minor_by_id(maj, minor_id);
	if (min) {
		spin_lock(&min->minor_lock);
		mod_timer(&min->timer, jiffies + INTERVAL_EXPIRE_MINOR);
		min->is_alive = 1;
		spin_unlock(&min->minor_lock);
	} else {
		min = init_min(maj, minor_id);
	}
	
	spin_lock(&min->minor_lock);
	add_leave(min, leave_id);
	spin_unlock(&min->minor_lock);

	/* rearm itself */
	mod_timer(&maj->add_timer, jiffies + INTERVAL_NEW_LEAVE);
	spin_unlock_bh(&maj->major_lock);
}

/* generate random leave id and remove it */
static void major_remove_leave_handler(unsigned long arg)
{
	struct major *maj = (struct major *)arg;
	struct minor *min;
	int leave_id, minor_id;

	if (atomic_read(&maj->cleanup))
		return;

	leave_id = get_rand_id(10);
	minor_id = leave_id / 2;

	

	spin_lock_bh(&maj->major_lock);
	min = find_minor_by_id(maj, minor_id);
	if (min) {
		spin_lock(&min->minor_lock);
		del_leave(min, leave_id);
		spin_unlock(&min->minor_lock);
	}
	spin_unlock_bh(&maj->major_lock);
}

int init_module(void)
{
	maj = kmalloc(sizeof(struct major), GFP_KERNEL);
	if (!maj) {
		pr_crit("%s() %d -ENOMEM\n",__func__,  __LINE__);
		return -ENOMEM;
	}
	memset(maj, 0, sizeof(*maj));

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

	tasklet_init(&maj->cleanup_tasklet, tasklet_cleanup_handler, (long unsigned int)maj);
	pr_info("%s %d setup done\n", __func__, __LINE__);

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
	minor_mark_all_to_remove(maj);
	while(atomic_read(&maj->cleanup))
		;

	printk(KERN_INFO "Goodbye world 1.\n");
}