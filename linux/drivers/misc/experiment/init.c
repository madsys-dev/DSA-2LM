#include "linux/export.h"
#include "linux/kern_levels.h"
#include "linux/ktime.h"
#include "linux/spinlock.h"
#include "linux/spinlock_types.h"
#include "linux/timekeeping.h"
#include <linux/module.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/uaccess.h>
#include <linux/delay.h>
#include <linux/mm_types.h>

#include "exp.h"

#define PROC_FILENAME "timer"

volatile ktime_t total_time, last_time;
volatile unsigned long copy_cnt;
int timer_state;
DEFINE_SPINLOCK(timer_lock);

EXPORT_SYMBOL(total_time);
EXPORT_SYMBOL(last_time);
EXPORT_SYMBOL(copy_cnt);
EXPORT_SYMBOL(timer_state);
EXPORT_SYMBOL(timer_lock);

// void my_copy_page(struct page *to, struct page *from) {
    
// }
// EXPORT_SYMBOL(my_copy_page);

static int timer_proc_show(struct seq_file *m, void *v) {
    seq_printf(m, "timer_state = %d total_time = %llu last_time = %llu copy_cnt = %lu\n", 
        timer_state, ktime_to_ms(total_time), ktime_to_ns(last_time), copy_cnt);
    return 0;
}

static int timer_proc_open(struct inode *inode, struct file *file) {
    return single_open(file, timer_proc_show, NULL);
}

static ssize_t timer_proc_write(struct file *file, const char __user *buffer, size_t len, loff_t *f_pos) {
    unsigned long rc;
    int val = -1;
    static char kbuf[1024];

    rc = copy_from_user(kbuf, buffer, len);
    printk(KERN_INFO "rc: %lu len(buf): %lu buf: %s\n", rc, strlen(kbuf), kbuf);
    rc = sscanf(kbuf, "%d", &val);
    if (rc == 1 && val >= 0 && val <= 1) {
        if (timer_state == TIMER_OFF && val == TIMER_ON) {
            total_time = last_time = ktime_set(0, 0);
            copy_cnt = 0;
        }
        timer_state = val;
    } else {
        printk(KERN_INFO "Error: rc is %lu, fail to parse %s\n", rc, kbuf);
    }
    return len;
}

static const struct proc_ops timer_proc_ops = {
    .proc_open = timer_proc_open,
    .proc_read = seq_read,
    .proc_write = timer_proc_write,
    .proc_lseek = seq_lseek,
    .proc_release = single_release,
};


static int __init exp_init(void) {
    struct proc_dir_entry *proc_file_entry;

    printk(KERN_INFO "exp module is loading\n");

    total_time = last_time = ktime_set(0, 0);
    timer_state = TIMER_OFF;
    copy_cnt = 0;
    spin_lock_init(&timer_lock);

    proc_file_entry = proc_create(PROC_FILENAME, 0, NULL, &timer_proc_ops);
    if (proc_file_entry == NULL) {
        printk(KERN_INFO "Can't create proc entry: %s\n", PROC_FILENAME);
        return 0;
    }

    return 0;
}

static void __exit exp_exit(void) {
    remove_proc_entry(PROC_FILENAME, NULL);
    printk(KERN_INFO "exp module unloaded\n");
}

module_init(exp_init);
module_exit(exp_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("LRL52");
