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
#define INTERVAL_DEL_LEAVE 50
#define INTERVAL_EXPIRE_MINOR 500

struct major {
    struct list_head minors_head;
    struct timer_list add_timer;
    struct timer_list remove_timer;
    int minors_count;
    spinlock_t major_lock;
    struct tasklet_struct cleanup_tasklet;
    int cleanup;
    atomic_t id_gen;
} *maj;

struct minor {
    struct list_head list;
    struct list_head leaves_head;
    struct timer_list timer;
    spinlock_t minor_lock;
    int leaves_count;
    int is_alive;
    int id;
};

struct leave {
    struct list_head list;
    int minor_id;
    char payload[32];
};

static void add_leave(struct minor *min, int id)
{

}

static void del_leave(struct minor *min, int id)
{

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

    spin_lock(&maj->major_lock);
    list_for_each_entry_safe(ptr, tmp, &maj->minors_head, list) {
retry:
        spin_lock(&ptr->minor_lock);
        if (ptr->is_alive == 0) {
            if (-1 == try_to_del_timer_sync(&ptr->timer)) {
                pr_crit("%s %d: timer handler is running, will allow it to finish\n", __func__, __LINE__);
                spin_unlock(&ptr->minor_lock);
                goto retry;
            }
            
            /*leave_delete()*/
            spin_unlock(&ptr->minor_lock);
            kfree(ptr);
        }
    }
    spin_unlock(&maj->major_lock);
}

static void minor_expire_timer_function(unsigned long arg)
{
    struct minor *min = (struct minor *)arg;
    
    pr_crit("%s %d: stepped here\n", __func__, __LINE__);
    
    spin_lock(&min->minor_lock);
    if (!timer_pending(&min->timer) && !maj->cleanup) {
        min->is_alive = 0;
        tasklet_schedule(&maj->cleanup_tasklet);
    }
    spin_unlock(&min->minor_lock);
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

    return min;
}

static void major_add_handler(unsigned long arg)
{
    struct major *maj = (struct major *)arg;
    struct minor *min = NULL;
    static int count = 20;    
    int id;
    
    pr_crit("%s %d: stepped here, count %d\n", __func__, __LINE__, count);

    if (maj->cleanup)
        return;

    if (!count)
        return;

    count--;
    id = atomic_read(&maj->id_gen);

    if (atomic_dec_and_test(&maj->id_gen)) {
        udelay(250);
        atomic_set(&maj->id_gen, 10);
    }

    spin_lock(&maj->major_lock);
    min = find_minor_by_id(maj, id);
    if (min) {
        spin_lock(&min->minor_lock);
        mod_timer(&min->timer, jiffies + INTERVAL_EXPIRE_MINOR);
        min->is_alive = 1;
        spin_unlock(&min->minor_lock);
    } else {
        min = init_min(maj, id);
    }
    
    spin_lock(&min->minor_lock);
    /* add new leave(min, id) */
    spin_unlock(&min->minor_lock);

    /* rearm itself */
    mod_timer(&maj->add_timer, jiffies + INTERVAL_NEW_LEAVE);
    spin_unlock(&maj->major_lock);
}

static void major_remove_handler(unsigned long arg)
{
    struct major *maj = (struct major *)arg;
    struct minor *min;
    int rand255, rand10;
    
    if (maj->cleanup)
        return;

    get_random_bytes(&rand255, 1);
    rand10 = rand255 % 10;

    spin_lock(&maj->major_lock);
    min = find_minor_by_id(maj, rand10);
    if (min)

    spin_unlock(&maj->major_lock);
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
    maj->add_timer.expires = jiffies + 10;
    maj->add_timer.function = major_add_handler;
    maj->add_timer.data = (long unsigned int)maj;

    init_timer(&maj->remove_timer);
    maj->remove_timer.expires = jiffies + 10 * HZ;
    maj->remove_timer.function = major_remove_handler;
    maj->remove_timer.data = (long unsigned int)maj;
    tasklet_init(&maj->cleanup_tasklet, tasklet_cleanup_handler, (long unsigned int)maj);
    atomic_set(&maj->id_gen, 10);

    pr_info("%s %d setup done\n", __func__, __LINE__);

	return 0;
}

void cleanup_module(void)
{
    maj->cleanup = 1;
    tasklet_schedule(&maj->cleanup_tasklet);

	printk(KERN_INFO "Goodbye world 1.\n");
}