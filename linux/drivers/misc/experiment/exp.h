#ifndef _MISC_EXP_H
#define _MISC_EXP_H

#include <linux/ktime.h>
#include <linux/spinlock.h>

extern volatile ktime_t total_time, last_time;
extern volatile unsigned long copy_cnt;
extern int timer_state;
extern spinlock_t timer_lock;

// void my_copy_page(struct page *to, struct page *from);

enum TIMER_STATE{
    TIMER_OFF,
    TIMER_ON,
};

#endif