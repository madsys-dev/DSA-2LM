#ifndef _MISC_EXP_H
#define _MISC_EXP_H

#include "linux/percpu-defs.h"
#include <linux/ktime.h>
#include <linux/spinlock.h>
#include "linux/atomic.h"

extern ktime_t total_time, last_time;
extern atomic_long_t dsa_copy_fail, hpage_cnt, bpage_cnt, dsa_hpage_cnt, dsa_bpage_cnt;
extern int timer_state, dsa_state, use_dsa_copy_pages, dsa_copy_threshold, dsa_async_mode, dsa_sync_mode;
extern unsigned long last_cnt;
// extern int max_hpage_cnt, max_bpage_cnt;
extern int limit_chan;
extern spinlock_t timer_lock;

extern int dsa_multi_copy_pages(struct page *to, struct page *from, int nr_pages);
extern int dsa_batch_copy_pages(struct page *to, struct page *from, int nr_pages);
extern int dsa_copy_page(struct page *to, struct page *from);
extern int dsa_copy_page_lists(struct page **to, struct page **from, int nr_items);

extern int sysctl_dsa_state_handler(struct ctl_table *table, int write,
	void *buffer, size_t *lenp, loff_t *ppos);

enum TIMER_STATE{
    TIMER_OFF,
    TIMER_ON,
};

#endif