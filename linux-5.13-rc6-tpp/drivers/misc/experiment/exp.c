// Author: LRL52
#include <linux/export.h>
#include <linux/kthread.h>
#include "linux/preempt.h"
#include "linux/sched.h"
#include <linux/module.h>
#include <linux/dmaengine.h>
#include <linux/dma-mapping.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/delay.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/idxd.h>

#include "../../dma/idxd/idxd.h"
#include "exp.h"

#define PROC_FILENAME "timer"
#define MAX_CHAN 8
#define MAX_BATCH_SIZE 128 // should lower or equal to 128, because two pages can only allocate 128 dsa_hw_desc(s)
#define POLL_RETRY_MAX 100000000
#define copy_pages_wrapper dsa_multi_copy_pages

volatile ktime_t total_time, last_time;
volatile unsigned long copy_cnt, dsa_copy_cnt;
atomic_long_t dsa_copy_fail, huge_page_cnt, base_page_cnt;
volatile unsigned long copy_dir_cnt[4];
volatile int timer_state, dsa_state;
DEFINE_SPINLOCK(timer_lock);

EXPORT_SYMBOL(total_time);
EXPORT_SYMBOL(last_time);
EXPORT_SYMBOL(copy_cnt);
EXPORT_SYMBOL(dsa_copy_cnt);
EXPORT_SYMBOL(dsa_copy_fail);
EXPORT_SYMBOL(huge_page_cnt);
EXPORT_SYMBOL(base_page_cnt);
EXPORT_SYMBOL(timer_state);
EXPORT_SYMBOL(copy_dir_cnt);
EXPORT_SYMBOL(dsa_state);
EXPORT_SYMBOL(timer_lock);

#define USE_PER_CPU_VARIABLES
// #define TIMER_ENABLE

#ifdef TIMER_ENABLE
#define TIMER_ON(name) uint64_t start_##name = rdtsc()
#define TIMER_OFF(name) uint64_t end_##name = rdtsc()
#define TIMER_SHOW(name) pr_notice_ratelimited("%s: %llu ns\n", #name, (end_##name - start_##name) * 1000000000 / CPUFREQ)
#else
#define TIMER_ON(name)
#define TIMER_OFF(name)
#define TIMER_SHOW(name)
#endif

static int limit_chans = MAX_CHAN;
static struct dma_chan *channels[MAX_CHAN];
static struct dma_device *copy_dev[MAX_CHAN];
#ifdef USE_PER_CPU_VARIABLES
DEFINE_PER_CPU(struct idxd_desc, global_idxd_desc[MAX_CHAN]);
DEFINE_PER_CPU(struct dsa_hw_desc, global_dsa_desc[MAX_CHAN]);
DEFINE_PER_CPU(struct page*, global_dsa_comp_page);
DEFINE_PER_CPU(struct page*, global_dsa_bdesc_page[MAX_CHAN]);
DEFINE_PER_CPU(struct dsa_hw_desc*, global_dsa_bdesc[MAX_CHAN]); // Descriptor List Address (VA)
DEFINE_PER_CPU(struct page*, global_dsa_bcomp_page[MAX_CHAN]);
DEFINE_PER_CPU(struct dsa_completion_record*, global_dsa_bcomp[MAX_CHAN]); // Completion List Address (VA)
#else
static struct idxd_desc global_idxd_desc[NR_CPUS][MAX_CHAN];
static struct dsa_hw_desc global_dsa_desc[NR_CPUS][MAX_CHAN];
static struct page *global_dsa_comp_page[NR_CPUS];
static struct page *global_dsa_bdesc_page[NR_CPUS][MAX_CHAN];
static struct dsa_hw_desc *global_dsa_bdesc[NR_CPUS][MAX_CHAN];
static struct page *global_dsa_bcomp_page[NR_CPUS][MAX_CHAN];
static struct dsa_completion_record *global_dsa_bcomp[NR_CPUS][MAX_CHAN];
#endif

#ifdef USE_PER_CPU_VARIABLES
    #define per_cpu_ptr_wrapper(ptr, cpu) per_cpu_ptr(ptr, cpu)
    #define per_cpu_wrapper(ptr, cpu) per_cpu(ptr, cpu)
    #define this_cpu_ptr_wrapper(ptr) this_cpu_ptr(ptr)
#else
    #define per_cpu_ptr_wrapper(ptr, cpu) (&((ptr)[cpu]))
    #define per_cpu_wrapper(ptr, cpu) ((ptr)[cpu])
    #define this_cpu_ptr_wrapper(ptr) (&((ptr)[smp_processor_id()]))
#endif

static inline struct idxd_wq *to_idxd_wq(struct dma_chan *c) {
	struct idxd_dma_chan *idxd_chan;

    idxd_chan = container_of(c, struct idxd_dma_chan, chan);
	return idxd_chan->wq;
}

static int dsa_init(void) {
    dma_cap_mask_t mask;
    struct idxd_desc *idxd_desc;
    struct dsa_hw_desc *dsa_desc, *dsa_bdesc;
    struct dsa_completion_record *dsa_comp, *dsa_bcomp;
    struct idxd_wq *wq;
    struct page *dsa_comp_page, *dsa_bdesc_page, *dsa_bcomp_page;
    int cpu, i, j;

    dma_cap_zero(mask);
	dma_cap_set(DMA_MEMCPY, mask);

    // Get the DMA channel
    for (i = 0; i < limit_chans; ++i) {
        channels[i] = dma_request_channel(mask, NULL, NULL);
        if (IS_ERR(channels[i]) || !channels[i]) {
            pr_err("Failed to get DMA channel %d.\n", i);
            goto NODEV;
        }
        copy_dev[i] = channels[i]->device;
        if (!copy_dev[i]) {
            pr_err("%s: no device: %d\n", __func__, i);
            goto NODEV;
        }
    }

    // Init dsa descriptor
    for_each_possible_cpu(cpu) {
        idxd_desc = (struct idxd_desc*)per_cpu_ptr_wrapper(global_idxd_desc, cpu);
        dsa_desc = (struct dsa_hw_desc*)per_cpu_ptr_wrapper(global_dsa_desc, cpu);

        per_cpu_wrapper(global_dsa_comp_page, cpu) = alloc_page(GFP_KERNEL); // TODO: Error handling
        dsa_comp_page = (struct page*)per_cpu_wrapper(global_dsa_comp_page, cpu);
        dsa_comp = page_to_virt(dsa_comp_page);
        memset(dsa_comp, 0, PAGE_SIZE);
        for (i = 0; i < limit_chans; ++i) {
            wq = to_idxd_wq(channels[i]);
            idxd_desc[i].hw = &dsa_desc[i];
            idxd_desc[i].desc_dma = 0; // desc_dma is not used
            idxd_desc[i].completion = &dsa_comp[i];
            idxd_desc[i].compl_dma = virt_to_phys(idxd_desc[i].completion);
            idxd_desc[i].cpu = cpu;
            idxd_desc[i].wq = wq;

            dsa_desc[i].priv = 1;
            dsa_desc[i].flags = IDXD_OP_FLAG_CRAV | IDXD_OP_FLAG_RCR;
            dsa_desc[i].completion_addr = idxd_desc[i].compl_dma;
        }
    }

    // Init batch descriptor
    for_each_possible_cpu(cpu) {
        for (i = 0; i < limit_chans; ++i) {
            ((struct page**)per_cpu_ptr_wrapper(global_dsa_bdesc_page, cpu))[i] = alloc_pages(GFP_KERNEL, 1); // TODO: Error handling
            dsa_bdesc_page = ((struct page**)per_cpu_ptr_wrapper(global_dsa_bdesc_page, cpu))[i];
            ((struct dsa_hw_desc**)per_cpu_ptr_wrapper(global_dsa_bdesc, cpu))[i] = page_to_virt(dsa_bdesc_page);
            dsa_bdesc = page_to_virt(dsa_bdesc_page);
            memset(dsa_bdesc, 0, PAGE_SIZE * 2);

            ((struct page**)per_cpu_ptr_wrapper(global_dsa_bcomp_page, cpu))[i] = alloc_pages(GFP_KERNEL, 1); // TODO: Error handling
            dsa_bcomp_page = ((struct page**)per_cpu_ptr_wrapper(global_dsa_bcomp_page, cpu))[i];
            ((struct dsa_completion_record**)per_cpu_ptr_wrapper(global_dsa_bcomp, cpu))[i] = page_to_virt(dsa_bcomp_page);
            dsa_bcomp = page_to_virt(dsa_bcomp_page);
            memset(dsa_bcomp, 0, PAGE_SIZE * 2);
            for (j = 0; j < MAX_BATCH_SIZE; ++j) {
                dsa_bdesc[j].priv = 1;
                dsa_bdesc[j].flags = IDXD_OP_FLAG_CRAV | IDXD_OP_FLAG_RCR;
                dsa_bdesc[j].opcode = DSA_OPCODE_MEMMOVE;
                dsa_bdesc[j].completion_addr = virt_to_phys(&dsa_bcomp[j]);
            }
        }
    }

    return 0;

NODEV:
    for (; i >= 0; --i) {
        if (channels[i]) {
            dma_release_channel(channels[i]);
        }
    }
    return -ENODEV;
}

static void dsa_release(void) {
    struct page *page;
    int cpu, i;

    pr_notice("DMA memcpy module exit.\n");

    for (i = 0; i < MAX_CHAN; ++i) {
        if (channels[i]) {
            dma_release_channel(channels[i]);
        }
    }

    // Free global_dsa_comp_page
    for_each_possible_cpu(cpu) {
        page = (struct page*)per_cpu_wrapper(global_dsa_comp_page, cpu);
        if (page) {
            __free_page(page);
        }
    }

    // Free global_dsa_bdesc_page and global_dsa_bcomp_page
    for_each_possible_cpu(cpu) {
        for (i = 0; i < MAX_CHAN; ++i) {
            page = ((struct page**)per_cpu_ptr_wrapper(global_dsa_bdesc_page, cpu))[i];
            if (page) {
                __free_pages(page, 1);
            }
            page = ((struct page**)per_cpu_ptr_wrapper(global_dsa_bcomp_page, cpu))[i];
            if (page) {
                __free_pages(page, 1);
            }
        }
    }
}

#define UMWAIT_DELAY 100000
#define UMWAIT_STATE 1

static inline void umonitor(volatile void *addr) {
	asm volatile(".byte 0xf3, 0x48, 0x0f, 0xae, 0xf0" : : "a"(addr));
}

static inline int umwait(unsigned long timeout, unsigned int state) {
	uint8_t r;
	uint32_t timeout_low = (uint32_t)timeout;
	uint32_t timeout_high = (uint32_t)(timeout >> 32);

	timeout_low = (uint32_t)timeout;
	timeout_high = (uint32_t)(timeout >> 32);

	asm volatile(".byte 0xf2, 0x48, 0x0f, 0xae, 0xf1\t\n"
		"setc %0\t\n"
		: "=r"(r)
		: "c"(state), "a"(timeout_low), "d"(timeout_high));
	return r;
}

static __always_inline void wait_for_dsa_completion(struct idxd_desc *idxd_desc, int *poll_retry) {
    while (idxd_desc->completion->status == DSA_COMP_NONE && ++(*poll_retry) < POLL_RETRY_MAX);
    // while (idxd_desc->completion->status == DSA_COMP_NONE && ++(*poll_retry) < POLL_RETRY_MAX) __builtin_ia32_pause();
    // while (idxd_desc->completion->status == DSA_COMP_NONE && ++(*poll_retry) < POLL_RETRY_MAX) {
    //     umonitor(idxd_desc->completion);
    //     if (idxd_desc->completion->status == DSA_COMP_NONE) {
    //         umwait(rdtsc() + UMWAIT_DELAY, UMWAIT_STATE);
    //     }
    // }
    // if (idxd_desc->completion->status != DSA_COMP_SUCCESS) {
    //     pr_err("idxd_desc failed with status %u, poll_retry %u.\n", idxd_desc->completion->status, *poll_retry);
    // }
}

static int idxd_fast_submit_desc(struct idxd_wq *wq, struct idxd_desc *desc) {
	void __iomem *portal;

	portal = idxd_wq_portal_addr(wq);
	wmb();
	iosubmit_cmds512(portal, desc->hw, 1);

	return 0;
}

// require nr_pages mod limit_chans equals 0
// it's non-reentrant functions because of per-cpu variables, or use preempt_disable/enable
int dsa_multi_copy_pages(struct page *to, struct page *from, int nr_pages) {
    struct idxd_desc *idxd_desc;
    uint32_t page_offset, poll_retry = 0;
    int i;

    // preempt_disable();
    // TIMER_ON(prepare_and_submit_desc);
    TIMER_ON(prepare_desc);
    idxd_desc = (struct idxd_desc*)this_cpu_ptr_wrapper(global_idxd_desc);
    // page_offset = nr_pages / limit_chans;
    page_offset = nr_pages * PAGE_SIZE / limit_chans;
    for (i = 0; i < limit_chans; ++i) {
        idxd_desc[i].hw->opcode = DSA_OPCODE_MEMMOVE;
        idxd_desc[i].hw->src_addr = page_to_phys(from) + i * page_offset;
        idxd_desc[i].hw->dst_addr = page_to_phys(to) + i * page_offset;
        idxd_desc[i].hw->xfer_size = page_offset;
        idxd_desc[i].completion->status = DSA_COMP_NONE;
        // pr_notice("wq name: %s id %d size %u max_wq %d\n", idxd_desc[i].wq->name, idxd_desc[i].wq->id, idxd_desc[i].wq->size, idxd_desc[i].wq->idxd->max_wqs);
        idxd_fast_submit_desc(idxd_desc[i].wq, &idxd_desc[i]);
    }
    TIMER_OFF(prepare_desc);

    TIMER_ON(submit_desc);
    // for (int i = 0; i < limit_chans; ++i) {
    //     idxd_fast_submit_desc(idxd_desc[i].wq, &idxd_desc[i]);
    // }
    TIMER_OFF(submit_desc);

    TIMER_ON(wait_for_completion);
    for (i = 0; i < limit_chans; ++i) {
        wait_for_dsa_completion(&idxd_desc[i], &poll_retry);
        // pr_notice("idxd_desc[%d] status %u, poll_retry %u.\n", i, idxd_desc[i].completion->status, poll_retry);
        if (idxd_desc[i].completion->status != DSA_COMP_SUCCESS) {
            pr_err_ratelimited("idxd_desc[%d] failed with status %u, poll_retry %u.\n", i, idxd_desc[i].completion->status, poll_retry);
            // preempt_enable();
            return -EIO;
        }
    }
    TIMER_OFF(wait_for_completion);

    // preempt_enable();

#ifdef TIMER_ENABLE
    // TIMER_SHOW(prepare_and_submit_desc);
    TIMER_SHOW(prepare_desc);
    TIMER_SHOW(submit_desc);
    TIMER_SHOW(wait_for_completion);
#endif

    return 0;
}
EXPORT_SYMBOL(dsa_multi_copy_pages);

// nr_pages should greater than 1 and less than or equal to MAX_BATCH_SIZE
// use channel idx to process the batch, multiple 4KB pages to test batch processing
int dsa_batch_copy_pages(struct page *to, struct page *from, int nr_pages) {
    struct idxd_desc *idxd_desc;
    struct dsa_hw_desc *dsa_bdesc;
    struct dsa_completion_record *dsa_bcomp;
    uint32_t poll_retry = 0;
    int i, idx = 0;

    TIMER_ON(prepare_desc);
    idxd_desc = (struct idxd_desc*)this_cpu_ptr_wrapper(global_idxd_desc);
    // dsa_bdesc = *(struct dsa_hw_desc**)this_cpu_ptr(global_dsa_bdesc[idx]);
    dsa_bdesc = ((struct dsa_hw_desc**)this_cpu_ptr_wrapper(global_dsa_bdesc))[idx];
    // dsa_bcomp = *(struct dsa_completion_record**)this_cpu_ptr(global_dsa_bcomp[idx]);
    dsa_bcomp = ((struct dsa_completion_record**)this_cpu_ptr_wrapper(global_dsa_bcomp))[idx];

    idxd_desc[idx].hw->opcode = DSA_OPCODE_BATCH;
    idxd_desc[idx].hw->desc_list_addr = virt_to_phys(dsa_bdesc);
    idxd_desc[idx].hw->desc_count = nr_pages;
    idxd_desc[idx].completion->status = DSA_COMP_NONE;

    for (i = 0; i < nr_pages; ++i) {
        dsa_bdesc[i].src_addr = page_to_phys(from + i);
        dsa_bdesc[i].dst_addr = page_to_phys(to + i);
        dsa_bdesc[i].xfer_size = PAGE_SIZE;
        dsa_bcomp[i].status = DSA_COMP_NONE;
    }
    TIMER_OFF(prepare_desc);

    TIMER_ON(submit_desc);
    idxd_fast_submit_desc(idxd_desc[idx].wq, &idxd_desc[idx]);
    TIMER_OFF(submit_desc);

    TIMER_ON(wait_for_completion);
    wait_for_dsa_completion(&idxd_desc[idx], &poll_retry);
    if (idxd_desc[idx].completion->status != DSA_COMP_SUCCESS) {
        pr_err_ratelimited("idxd_desc[%d] failed with status %u, poll_retry %u.\n", idx, idxd_desc[idx].completion->status, poll_retry);
        // for (i = 0; i < nr_pages; ++i) {
        //     if (dsa_bcomp[i].status != DSA_COMP_SUCCESS) {
        //         pr_err("dsa_bcomp[%d] failed with status %u.\n", i, dsa_bcomp[i].status);
        //     }
        // }
        return -EIO;
    }
    TIMER_OFF(wait_for_completion);

#ifdef TIMER_ENABLE
    TIMER_SHOW(prepare_desc);
    TIMER_SHOW(submit_desc);
    TIMER_SHOW(wait_for_completion);
#endif

    return 0;
}
EXPORT_SYMBOL(dsa_batch_copy_pages);

int dsa_copy_page(struct page *to, struct page *from) {
    struct idxd_desc *idxd_desc;
    uint32_t poll_retry = 0;
    int idx = 0;

    TIMER_ON(prepare_desc);
    idxd_desc = (struct idxd_desc*)this_cpu_ptr_wrapper(global_idxd_desc);
    idxd_desc[idx].hw->opcode = DSA_OPCODE_MEMMOVE;
    idxd_desc[idx].hw->src_addr = page_to_phys(from);
    idxd_desc[idx].hw->dst_addr = page_to_phys(to);
    idxd_desc[idx].hw->xfer_size = PAGE_SIZE;
    idxd_desc[idx].completion->status = DSA_COMP_NONE;
    TIMER_OFF(prepare_desc);

    TIMER_ON(submit_desc);
    idxd_fast_submit_desc(idxd_desc[idx].wq, &idxd_desc[idx]);
    TIMER_OFF(submit_desc);

    TIMER_ON(wait_for_completion);
    wait_for_dsa_completion(&idxd_desc[idx], &poll_retry);
    if (idxd_desc[idx].completion->status != DSA_COMP_SUCCESS) {
        pr_err_ratelimited("idxd_desc[%d] failed with status %u, poll_retry %u.\n", idx, idxd_desc[idx].completion->status, poll_retry);
        return -EIO;
    }
    TIMER_OFF(wait_for_completion);

#ifdef TIMER_ENABLE
    TIMER_SHOW(prepare_desc);
    TIMER_SHOW(submit_desc);
    TIMER_SHOW(wait_for_completion);
#endif

    return 0;
}
EXPORT_SYMBOL(dsa_copy_page);

static int timer_proc_show(struct seq_file *m, void *v) {
    seq_printf(m, 
        "timer_state = %d dsa_state = %d total_time = %llums last_time = %lluns copy_cnt = %lu dsa_copy_cnt = %lu dsa_copy_fail = %lu "
        "copy_dir_cnt[0->0] = %lu copy_dir_cnt[0->1] = %lu copy_dir_cnt[1->0] = %lu copy_dir_cnt[1->1] = %lu "
        "huge_page_cnt = %lu base_page_cnt = %lu\n", 
        timer_state, dsa_state, ktime_to_ms(total_time), ktime_to_ns(last_time), copy_cnt, dsa_copy_cnt, atomic_long_read(&dsa_copy_fail),
        copy_dir_cnt[0], copy_dir_cnt[1], copy_dir_cnt[2], copy_dir_cnt[3],
        atomic_long_read(&huge_page_cnt), atomic_long_read(&base_page_cnt)
    );
    return 0;
}

static int timer_proc_open(struct inode *inode, struct file *file) {
    return single_open(file, timer_proc_show, NULL);
}

static ssize_t timer_proc_write(struct file *file, const char __user *buffer, size_t len, loff_t *f_pos) {
    static char kbuf[1024];
    unsigned long rc;
    int val1 = -1, val2 = -1, val3 = -1, val4 = -1;

    rc = copy_from_user(kbuf, buffer, len);
    pr_notice("rc: %lu len(buf): %lu buf: %s\n", rc, strlen(kbuf), kbuf);
    // (val1, val2, val3, val4) = (timer_state, enable/disable dsa, dsa_state, limit_dma_chans)
    rc = sscanf(kbuf, "%d %d %d %d", &val1, &val2, &val3, &val4); 
    if (rc == 4) {
        if (timer_state == TIMER_OFF && val1 == TIMER_ON) {
            total_time = last_time = ktime_set(0, 0);
            copy_cnt = dsa_copy_cnt = 0;
            atomic_long_set(&dsa_copy_fail, 0);
            atomic_long_set(&huge_page_cnt, 0);
            atomic_long_set(&base_page_cnt, 0);
            memset((void*)copy_dir_cnt, 0, sizeof(copy_dir_cnt));
        }
        if (val2 == DSA_ON) {
            rc = dsa_init();
            if (rc) {
                pr_err("Failed to initialize DSA.\n");
            }
        }
        if (val2 == DSA_OFF) {
            dsa_release();
        }
        if (val1 == TIMER_OFF || val1 == TIMER_ON) {
            WRITE_ONCE(timer_state, val1);
        }
        if (val3 == 0 || val3 == 1) {
            WRITE_ONCE(dsa_state, val3);
        }
        if (val4 > 0) {
            WRITE_ONCE(limit_chans, val4);
        }
    } else {
        pr_notice("Error: rc is %lu, fail to parse %s\n", rc, kbuf);
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

    pr_notice("exp module is loading\n");

    total_time = last_time = ktime_set(0, 0);
    timer_state = TIMER_OFF;
    copy_cnt = 0;
    memset((void*)copy_dir_cnt, 0, sizeof(copy_dir_cnt));

    spin_lock_init(&timer_lock);

    proc_file_entry = proc_create(PROC_FILENAME, 0, NULL, &timer_proc_ops);
    if (proc_file_entry == NULL) {
        pr_err("Can't create proc entry: %s\n", PROC_FILENAME);
    }

    return 0;
}

static void __exit exp_exit(void) {
    remove_proc_entry(PROC_FILENAME, NULL);
    pr_notice("exp module unloaded\n");
}

module_init(exp_init);
module_exit(exp_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("LRL52");
