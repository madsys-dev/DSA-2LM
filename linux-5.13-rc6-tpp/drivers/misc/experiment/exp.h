#ifndef _MISC_EXP_H
#define _MISC_EXP_H

#include "linux/percpu-defs.h"
#include <linux/ktime.h>
#include <linux/spinlock.h>
#include "linux/atomic.h"

extern volatile ktime_t total_time, last_time;
extern volatile unsigned long copy_cnt, dsa_copy_cnt;
extern atomic_long_t dsa_copy_fail, huge_page_cnt, base_page_cnt;
extern volatile int timer_state, dsa_state;
extern spinlock_t timer_lock;
extern volatile unsigned long copy_dir_cnt[];

int dsa_multi_copy_pages(struct page *to, struct page *from, int nr_pages);
int dsa_batch_copy_pages(struct page *to, struct page *from, int nr_pages);
int dsa_copy_page(struct page *to, struct page *from);

enum TIMER_STATE{
    TIMER_OFF,
    TIMER_ON,
};

enum DSA_STATE {
    DSA_OFF,
    DSA_ON,
};

#endif