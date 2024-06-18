#include "linux/cpumask.h"
#include "linux/export.h"
#include "linux/kern_levels.h"
#include "linux/kernel.h"
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
#include <linux/fortify-string.h>
#include <linux/dmaengine.h>
#include <linux/dma-mapping.h>
#include <linux/completion.h>
#include "linux/printk.h"
#include "linux/sched.h"
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/sbitmap.h>

#include "../../dma/idxd/idxd.h"
#include "exp.h"

#define PROC_FILENAME "timer"
#define POLL_RETRY_MAX 100000000

volatile ktime_t total_time, last_time;
volatile unsigned long copy_cnt, dsa_copy_cnt;
atomic_long_t dsa_copy_fail;
volatile unsigned long copy_dir_cnt[4];
volatile int timer_state, dsa_state;
DEFINE_PER_CPU(struct page*, page_bak); 
DEFINE_PER_CPU(void*, page_bak_addr);
DEFINE_SPINLOCK(timer_lock);
// DEFINE_SPINLOCK(dsa_lock);

EXPORT_SYMBOL(total_time);
EXPORT_SYMBOL(last_time);
EXPORT_SYMBOL(copy_cnt);
EXPORT_SYMBOL(dsa_copy_cnt);
EXPORT_SYMBOL(dsa_copy_fail);
EXPORT_SYMBOL(timer_state);
EXPORT_SYMBOL(timer_lock);
// EXPORT_SYMBOL(dsa_lock);
EXPORT_SYMBOL(copy_dir_cnt);
EXPORT_SYMBOL(dsa_state);
EXPORT_SYMBOL(page_bak_addr);

#define MAX_CHAN 8
static struct dma_chan *channels[MAX_CHAN];
static struct dma_device *copy_dev[MAX_CHAN] = {0};
static dma_addr_t dma_src, dma_dst;
static spinlock_t map_and_prep_lock[MAX_CHAN];

static int timer_proc_show(struct seq_file *m, void *v) {
    seq_printf(m, 
        "timer_state = %d dsa_state = %d total_time = %llums last_time = %lluns copy_cnt = %lu dsa_copy_cnt = %lu dsa_copy_fail = %lu "
        "copy_dir_cnt[0->0] = %lu copy_dir_cnt[0->1] = %lu copy_dir_cnt[1->0] = %lu copy_dir_cnt[1->1] = %lu\n", 
        timer_state, dsa_state, ktime_to_ms(total_time), ktime_to_ns(last_time), copy_cnt, dsa_copy_cnt, atomic_long_read(&dsa_copy_fail),
        copy_dir_cnt[0], copy_dir_cnt[1], copy_dir_cnt[2], copy_dir_cnt[3]
    );
    return 0;
}

static int timer_proc_open(struct inode *inode, struct file *file) {
    return single_open(file, timer_proc_show, NULL);
}

static int dsa_test_init(void) {
    dma_cap_mask_t mask;
    dma_cap_zero(mask);
	dma_cap_set(DMA_MEMCPY, mask);

    // Get the DMA channel
    for (int i = 0; i < MAX_CHAN; ++i) {
        channels[i] = dma_request_channel(mask, NULL, NULL);
        if (IS_ERR(channels[i]) || !channels[i]) {
            printk(KERN_ERR "Failed to get DMA channel %d.\n", i);
            return PTR_ERR(channels[i]);
        }
        pr_notice("Successfully get DMA channel %d = %px\n", i, channels[i]);
        copy_dev[i] = channels[i]->device;
        if (!copy_dev[i]) {
            pr_err("%s: no device: %d\n", __func__, i);
            continue;
        }
    }

    // Initialize map_and_prep_lock
    for (int i = 0; i < MAX_CHAN; ++i) {
        spin_lock_init(&map_and_prep_lock[i]);
    }
    
    return 0;
}

static ssize_t timer_proc_write(struct file *file, const char __user *buffer, size_t len, loff_t *f_pos) {
    unsigned long rc;
    int val1 = -1, val2 = -1, val3 = -1;
    static char kbuf[1024];

    rc = copy_from_user(kbuf, buffer, len);
    printk(KERN_INFO "rc: %lu len(buf): %lu buf: %s\n", rc, strlen(kbuf), kbuf);
    rc = sscanf(kbuf, "%d %d %d", &val1, &val2, &val3);
    if (rc == 3) {
        if (timer_state == TIMER_OFF && val1 == TIMER_ON) {
            total_time = last_time = ktime_set(0, 0);
            copy_cnt = dsa_copy_cnt = 0;
            atomic_long_set(&dsa_copy_fail, 0);
            memset((void*)copy_dir_cnt, 0, sizeof(copy_dir_cnt));
        }
        if (val2 == DSA_ON) {
            rc = dsa_test_init();
            if (rc) {
                printk(KERN_ERR "Failed to initialize DMA test.\n");
            }
        }
        if (val2 == DSA_OFF) {
            for (int i = 0; i < MAX_CHAN; ++i) {
                if (channels[i]) {
                    dma_release_channel(channels[i]);
                }
            }
        }
        if (val1 == TIMER_OFF || val1 == TIMER_ON) {
            WRITE_ONCE(timer_state, val1);
        }
        if (val3 == 0 || val3 == 1) {
            WRITE_ONCE(dsa_state, val3);
        }
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

// #define TIMER_ENABLE

#ifdef TIMER_ENABLE
#define TIMER_ON(name) ktime_t start_##name = ktime_get()
#define TIMER_OFF(name) ktime_t end_##name = ktime_get()
#define TIMER_SHOW(name) printk_ratelimited(KERN_INFO "%s: %lld ns\n", #name, ktime_to_ns(ktime_sub(end_##name, start_##name)))
#else
#define TIMER_ON(name)
#define TIMER_OFF(name)
#define TIMER_SHOW(name)
#endif

void idxd_free_desc(struct idxd_wq *wq, struct idxd_desc *desc)
{
	int cpu = desc->cpu;

	desc->cpu = -1;
	sbitmap_queue_clear(&wq->sbq, desc->id, cpu);
}


void dsa_copy(void *to, void *from, size_t size) {
    static atomic_t idx = ATOMIC_INIT(0);
    struct dma_chan *chan;
    struct dma_device *dma_dev;
    struct dma_async_tx_descriptor *desc;
    // struct completion cmp;
    struct idxd_desc *idxd_desc;
    dma_cookie_t cookie;
    enum dma_ctrl_flags flags = DMA_CTRL_ACK;
    // unsigned long timeout = msecs_to_jiffies(1); // 1ms timeout
    unsigned long lock_flags;
    int poll_retry = 0, _idx;

    // Get the DMA channel
    _idx = ((unsigned)atomic_inc_return(&idx)) % MAX_CHAN;
    chan = channels[_idx];
    dma_dev = chan->device;

    // Initialize the completion structure
    // TIMER_ON(init_completion);
    // init_completion(&cmp);
    // TIMER_OFF(init_completion);

    // Map the buffers for DMA
    spin_lock_irqsave(&map_and_prep_lock[0], lock_flags);
    TIMER_ON(dma_map_single);
    dma_src = dma_map_single(dma_dev->dev, from, size, DMA_TO_DEVICE);
    dma_dst = dma_map_single(dma_dev->dev, to, size, DMA_FROM_DEVICE);
    TIMER_OFF(dma_map_single);

    // Prepare the DMA transfer
    TIMER_ON(dmaengine_prep_dma_memcpy);
    desc = dmaengine_prep_dma_memcpy(chan, dma_dst, dma_src, size, flags);
    TIMER_OFF(dmaengine_prep_dma_memcpy);
    spin_unlock_irqrestore(&map_and_prep_lock[0], lock_flags);

    idxd_desc = container_of(desc, struct idxd_desc, txd);

    // desc->callback = dma_complete_func;
    // desc->callback_param = &cmp;
    desc->callback = NULL;
    desc->callback_param = NULL;

    // Submit the transaction
    TIMER_ON(dmaengine_submit);
    cookie = dmaengine_submit(desc);
    TIMER_OFF(dmaengine_submit);

    // Issue the DMA transfer
    // TIMER_ON(dma_async_issue_pending);
    // dma_async_issue_pending(chan);
    // TIMER_OFF(dma_async_issue_pending);

    // Wait for completion
    TIMER_ON(wait_for_completion);
    // wait_for_completion(&cmp);
    // wait_for_completion_timeout(&cmp, timeout);
    // while (!completion_done(&cmp)) {
    // while (READ_ONCE(cmp.done) == false) {
    //     if (poll_retry++ > POLL_RETRY_MAX) {
    //     //    __printk_ratelimit(KERN_INFO "DMA transfer timeout\n");
    //         break;
    //     }
    //     // cpu_relax(); // Optional: Provide a hint to the CPU to optimize waiting
    // }
    // while (dma_async_is_tx_complete(chan, cookie, NULL, NULL) != DMA_COMPLETE) {
    //     if (poll_retry++ > POLL_RETRY_MAX) {
    //         printk_ratelimited(KERN_INFO "DMA transfer timeout\n");
    //         break;
    //     }
    //     cpu_relax();
    // }
    while (idxd_desc->completion->status == DSA_COMP_NONE && poll_retry++ < POLL_RETRY_MAX);
    if (idxd_desc->completion->status != DSA_COMP_SUCCESS || poll_retry >= POLL_RETRY_MAX) {
        pr_err("DSA transfer failed: status = %d, poll_retry = %d\n", idxd_desc->completion->status, poll_retry);
    }
    TIMER_OFF(wait_for_completion);
    

    // Check if the transfer was successful
    // if (dma_async_is_complete(cookie, chan->completed_cookie, chan->cookie) != DMA_COMPLETE) {
    //     return -EIO;
    // }

    // Unmap the buffers
    TIMER_ON(dma_unmap_single);
    dma_unmap_single(dma_dev->dev, dma_src, size, DMA_TO_DEVICE);
    dma_unmap_single(dma_dev->dev, dma_dst, size, DMA_FROM_DEVICE);
    TIMER_OFF(dma_unmap_single);

    idxd_free_desc(idxd_desc->wq, idxd_desc);

#ifdef TIMER_ENABLE
    TIMER_SHOW(init_completion);
    TIMER_SHOW(dma_map_single);
    TIMER_SHOW(dmaengine_prep_dma_memcpy);
    TIMER_SHOW(dmaengine_submit);
    TIMER_SHOW(dma_async_issue_pending);
    TIMER_SHOW(wait_for_completion);
    TIMER_SHOW(dma_unmap_single);
#endif

}
EXPORT_SYMBOL(dsa_copy);

#define NUM_AVAIL_DMA_CHAN MAX_CHAN
static int limit_dma_chans = NUM_AVAIL_DMA_CHAN;

int dsa_copy_page(struct page *to, struct page *from, int nr_pages) {
	struct dma_async_tx_descriptor *tx[NUM_AVAIL_DMA_CHAN] = {0};
    struct idxd_desc *idxd_desc[NUM_AVAIL_DMA_CHAN] = {0};
	dma_cookie_t cookie[NUM_AVAIL_DMA_CHAN];
	enum dma_ctrl_flags flags[NUM_AVAIL_DMA_CHAN] = {0};
	struct dmaengine_unmap_data *unmap[NUM_AVAIL_DMA_CHAN] = {0};
    unsigned long lock_flags[NUM_AVAIL_DMA_CHAN], poll_retry = 0;
	int ret_val = 0;
	int total_available_chans = NUM_AVAIL_DMA_CHAN;
	int i;
	size_t page_offset;

    TIMER_ON(calc_total_available_chans);
	for (i = 0; i < NUM_AVAIL_DMA_CHAN; ++i) {
		if (!channels[i]) {
			total_available_chans = i;
		}
	}
	if (total_available_chans != NUM_AVAIL_DMA_CHAN) {
		pr_err("%d channels are missing", NUM_AVAIL_DMA_CHAN - total_available_chans);
	}

	total_available_chans = min_t(int, total_available_chans, limit_dma_chans);

	/* round down to closest 2^x value  */
	total_available_chans = 1 << ilog2(total_available_chans);
    TIMER_OFF(calc_total_available_chans);

	if ((nr_pages != 1) && (nr_pages % total_available_chans != 0))
		return -5;

    TIMER_ON(dma_map_page);
	for (i = 0; i < total_available_chans; ++i) {
		unmap[i] = dmaengine_get_unmap_data(copy_dev[i]->dev, 2, GFP_NOWAIT);
		if (!unmap[i]) {
			pr_err("%s: no unmap data at chan %d\n", __func__, i);
			ret_val = -3;
			goto unmap_dma;
		}
	}

	for (i = 0; i < total_available_chans; ++i) {
		if (nr_pages == 1) {
			page_offset = PAGE_SIZE / total_available_chans;

			unmap[i]->to_cnt = 1;
			unmap[i]->addr[0] = dma_map_page(copy_dev[i]->dev, from, page_offset*i,
							  page_offset,
							  DMA_TO_DEVICE);
			unmap[i]->from_cnt = 1;
			unmap[i]->addr[1] = dma_map_page(copy_dev[i]->dev, to, page_offset*i,
							  page_offset,
							  DMA_FROM_DEVICE);
			unmap[i]->len = page_offset;
		} else {
			page_offset = nr_pages / total_available_chans;

			unmap[i]->to_cnt = 1;
			unmap[i]->addr[0] = dma_map_page(copy_dev[i]->dev,
								from + page_offset*i,
								0,
								PAGE_SIZE*page_offset,
								DMA_TO_DEVICE);
			unmap[i]->from_cnt = 1;
			unmap[i]->addr[1] = dma_map_page(copy_dev[i]->dev,
								to + page_offset*i,
								0,
								PAGE_SIZE*page_offset,
								DMA_FROM_DEVICE);
			unmap[i]->len = PAGE_SIZE*page_offset;
		}
	}
    TIMER_OFF(dma_map_page);

    TIMER_ON(prepare_dma_memcpy);
	for (i = 0; i < total_available_chans; ++i) {
        // spin_lock_irqsave(&map_and_prep_lock[i], lock_flags[i]);
		tx[i] = copy_dev[i]->device_prep_dma_memcpy(channels[i],
							unmap[i]->addr[1],
							unmap[i]->addr[0],
							unmap[i]->len,
							flags[i]);
		if (!tx[i]) {
			pr_err("%s: no tx descriptor at chan %d\n", __func__, i);
			ret_val = -4;
			goto unmap_dma;
		}
        // spin_unlock_irqrestore(&map_and_prep_lock[i], lock_flags[i]);

        idxd_desc[i] = container_of(tx[i], struct idxd_desc, txd);
	}
    TIMER_OFF(prepare_dma_memcpy);

    TIMER_ON(dma_submit);
	for (i = 0; i < total_available_chans; ++i) {
		cookie[i] = tx[i]->tx_submit(tx[i]);

		if (dma_submit_error(cookie[i])) {
			pr_err("%s: submission error at chan %d\n", __func__, i);
			ret_val = -5;
			goto unmap_dma;
		}

		// dma_async_issue_pending(channels[i]);
	}
    TIMER_OFF(dma_submit);

    TIMER_ON(wait_for_completion);
	for (i = 0; i < total_available_chans; ++i) {
        poll_retry = 0;
        while (idxd_desc[i]->completion->status == DSA_COMP_NONE && ++poll_retry < POLL_RETRY_MAX);
        if (poll_retry >= POLL_RETRY_MAX || idxd_desc[i]->completion->status != DSA_COMP_SUCCESS) {
            pr_err("idxd_desc[%d]->completion->status = %d, poll_retry = %lu\n", i, idxd_desc[i]->completion->status, poll_retry);
        }
		// if (dma_sync_wait(channels[i], cookie[i]) != DMA_COMPLETE) {
		// 	ret_val = -6;
		// 	pr_err("%s: dma does not complete at chan %d\n", __func__, i);
		// }
	}
    TIMER_OFF(wait_for_completion);

    
unmap_dma:
    i = 0;
    TIMER_ON(dma_unmap_page);
	for (i = 0; i < total_available_chans; ++i) {
		if (unmap[i])
			dmaengine_unmap_put(unmap[i]);
	}
    TIMER_OFF(dma_unmap_page);

    TIMER_ON(free_desc);
    for (int i = 0; i < total_available_chans; ++i) {
        if (idxd_desc[i]) {
            idxd_free_desc(idxd_desc[i]->wq, idxd_desc[i]);
        }
    }
    TIMER_OFF(free_desc);

#ifdef TIMER_ENABLE
    TIMER_SHOW(calc_total_available_chans);
    TIMER_SHOW(dma_map_page);
    TIMER_SHOW(prepare_dma_memcpy);
    TIMER_SHOW(dma_submit);
    TIMER_SHOW(wait_for_completion);
    TIMER_SHOW(dma_unmap_page);
    TIMER_SHOW(free_desc);
#endif

	return ret_val;
}
EXPORT_SYMBOL(dsa_copy_page);

static int __init exp_init(void) {
    struct proc_dir_entry *proc_file_entry;
    int cpu;

    printk(KERN_INFO "exp module is loading\n");

    total_time = last_time = ktime_set(0, 0);
    timer_state = TIMER_OFF;
    copy_cnt = 0;
    memset((void*)copy_dir_cnt, 0, sizeof(copy_dir_cnt));
    spin_lock_init(&timer_lock);
    // spin_lock_init(&dsa_lock);
    
    for_each_possible_cpu(cpu) {
        per_cpu(page_bak, cpu) = alloc_page(GFP_KERNEL);
        per_cpu(page_bak_addr, cpu) = page_address(per_cpu(page_bak, cpu));
        printk(KERN_INFO "per_cpu(page_bak_addr, %d) = %px\n", cpu, per_cpu(page_bak_addr, cpu));
    }

    proc_file_entry = proc_create(PROC_FILENAME, 0, NULL, &timer_proc_ops);
    if (proc_file_entry == NULL) {
        printk(KERN_INFO "Can't create proc entry: %s\n", PROC_FILENAME);
        return 0;
    }

    return 0;
}

static void __exit exp_exit(void) {
    remove_proc_entry(PROC_FILENAME, NULL);
    if (page_bak) {
        __free_page(page_bak);
    }
    printk(KERN_INFO "exp module unloaded\n");
}

module_init(exp_init);
module_exit(exp_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("LRL52");
