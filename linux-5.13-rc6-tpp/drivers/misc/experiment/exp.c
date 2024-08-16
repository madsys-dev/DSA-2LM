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
#include <linux/sysctl.h>
#include <linux/idxd.h>

#include "../../dma/idxd/idxd.h"
#include "exp.h"

#define PROC_FILENAME "timer"
#define MAX_CHAN 8
#define MAX_BATCH_SIZE 128 // should lower or equal to 128, because two pages can only allocate 128 dsa_hw_desc(s)
#define MAX_IDXD_DESC 25 // one page can only allocate 25 idxd_desc(s)
#define POLL_RETRY_MAX 100000000

ktime_t total_time, last_time;
atomic_long_t dsa_copy_fail, hpage_cnt, bpage_cnt, dsa_hpage_cnt, dsa_bpage_cnt;
unsigned long last_cnt;
int timer_state, dsa_state, use_dsa_copy_pages, dsa_copy_threshold = 12;
// int max_hpage_cnt, max_bpage_cnt;
int limit_chans = MAX_CHAN;
DEFINE_SPINLOCK(timer_lock);

EXPORT_SYMBOL(total_time);
EXPORT_SYMBOL(last_time);
EXPORT_SYMBOL(dsa_copy_fail);
EXPORT_SYMBOL(hpage_cnt);
EXPORT_SYMBOL(bpage_cnt);
EXPORT_SYMBOL(dsa_hpage_cnt);
EXPORT_SYMBOL(dsa_bpage_cnt);
EXPORT_SYMBOL(last_cnt);
EXPORT_SYMBOL(timer_state);
EXPORT_SYMBOL(dsa_state);
EXPORT_SYMBOL(use_dsa_copy_pages);
EXPORT_SYMBOL(dsa_copy_threshold);
// EXPORT_SYMBOL(max_bpage_cnt);
// EXPORT_SYMBOL(max_hpage_cnt);
EXPORT_SYMBOL(limit_chans);
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

static struct dma_chan *channels[MAX_CHAN];
static struct dma_device *copy_dev[MAX_CHAN];
DEFINE_PER_CPU(local_lock_t, dsa_copy_local_lock) = INIT_LOCAL_LOCK(dsa_copy_local_lock);
#ifdef USE_PER_CPU_VARIABLES
DEFINE_PER_CPU(struct page*, global_idxd_desc_page[MAX_CHAN]);
DEFINE_PER_CPU(struct idxd_desc*, global_idxd_desc[MAX_CHAN]);
DEFINE_PER_CPU(struct page*, global_dsa_desc_page[MAX_CHAN]);
DEFINE_PER_CPU(struct dsa_hw_desc*, global_dsa_desc[MAX_CHAN]);
DEFINE_PER_CPU(struct page*, global_dsa_comp_page[MAX_CHAN]);
DEFINE_PER_CPU(struct dsa_completion_record*, global_dsa_comp[MAX_CHAN]);
DEFINE_PER_CPU(struct page*, global_dsa_bdesc_page[MAX_CHAN]);
DEFINE_PER_CPU(struct dsa_hw_desc*, global_dsa_bdesc[MAX_CHAN]); // Descriptor List Address (VA)
DEFINE_PER_CPU(struct page*, global_dsa_bcomp_page[MAX_CHAN]);
DEFINE_PER_CPU(struct dsa_completion_record*, global_dsa_bcomp[MAX_CHAN]); // Completion List Address (VA)
#else
static struct page *global_idxd_desc_page[NR_CPUS][MAX_CHAN];
static struct idxd_desc *global_idxd_desc[NR_CPUS][MAX_CHAN];
static struct page *global_dsa_desc_page[NR_CPUS][MAX_CHAN];
static struct dsa_hw_desc *global_dsa_desc[NR_CPUS][MAX_CHAN];
static struct page *global_dsa_comp_page[NR_CPUS][MAX_CHAN];
static struct dsa_completion_record *global_dsa_comp[NR_CPUS];
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
    struct page *idxd_desc_page, *dsa_desc_page, *dsa_comp_page, *dsa_bdesc_page, *dsa_bcomp_page;
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
        for (i = 0; i < limit_chans; ++i) {
            ((struct page**)per_cpu_ptr_wrapper(global_idxd_desc_page, cpu))[i] = alloc_page(GFP_KERNEL);
            idxd_desc_page = ((struct page**)per_cpu_ptr_wrapper(global_idxd_desc_page, cpu))[i];
            ((struct idxd_desc**)per_cpu_ptr_wrapper(global_idxd_desc, cpu))[i] = page_to_virt(idxd_desc_page);
        
            ((struct page**)per_cpu_ptr_wrapper(global_dsa_desc_page, cpu))[i] = alloc_page(GFP_KERNEL);
            dsa_desc_page = ((struct page**)per_cpu_ptr_wrapper(global_dsa_desc_page, cpu))[i];
            ((struct dsa_hw_desc**)per_cpu_ptr_wrapper(global_dsa_desc, cpu))[i] = page_to_virt(dsa_desc_page);

            ((struct page**)per_cpu_ptr_wrapper(global_dsa_comp_page, cpu))[i] = alloc_page(GFP_KERNEL);
            dsa_comp_page = ((struct page**)per_cpu_ptr_wrapper(global_dsa_comp_page, cpu))[i];
            ((struct dsa_completion_record**)per_cpu_ptr_wrapper(global_dsa_comp, cpu))[i] = page_to_virt(dsa_comp_page);

            if (!idxd_desc_page || !dsa_desc_page || !dsa_comp_page) {
                pr_err("Failed to allocate page for idxd_desc, dsa_desc or dsa_comp.\n");
                goto NODEV;
            }

            idxd_desc = page_to_virt(idxd_desc_page);
            memset(idxd_desc, 0, PAGE_SIZE);
            dsa_desc = page_to_virt(dsa_desc_page);
            memset(dsa_desc, 0, PAGE_SIZE);
            dsa_comp = page_to_virt(dsa_comp_page);
            memset(dsa_comp, 0, PAGE_SIZE);
            
            wq = to_idxd_wq(channels[i]);
            for (j = 0; j < MAX_IDXD_DESC; ++j) {
                idxd_desc[j].hw = &dsa_desc[j];
                idxd_desc[j].desc_dma = 0; // desc_dma is not used
                idxd_desc[j].completion = &dsa_comp[j];
                idxd_desc[j].compl_dma = virt_to_phys(idxd_desc[j].completion);
                idxd_desc[j].cpu = cpu;
                idxd_desc[j].wq = wq;

                dsa_desc[j].priv = 1;
                dsa_desc[j].flags = IDXD_OP_FLAG_CRAV | IDXD_OP_FLAG_RCR;
                dsa_desc[j].completion_addr = idxd_desc[j].compl_dma;
            }
        }
    }

    // Init batch descriptor
    for_each_possible_cpu(cpu) {
        for (i = 0; i < limit_chans; ++i) {
            ((struct page**)per_cpu_ptr_wrapper(global_dsa_bdesc_page, cpu))[i] = alloc_pages(GFP_KERNEL, 1);
            dsa_bdesc_page = ((struct page**)per_cpu_ptr_wrapper(global_dsa_bdesc_page, cpu))[i];
            ((struct dsa_hw_desc**)per_cpu_ptr_wrapper(global_dsa_bdesc, cpu))[i] = page_to_virt(dsa_bdesc_page);
            ((struct page**)per_cpu_ptr_wrapper(global_dsa_bcomp_page, cpu))[i] = alloc_pages(GFP_KERNEL, 1);
            dsa_bcomp_page = ((struct page**)per_cpu_ptr_wrapper(global_dsa_bcomp_page, cpu))[i];
            ((struct dsa_completion_record**)per_cpu_ptr_wrapper(global_dsa_bcomp, cpu))[i] = page_to_virt(dsa_bcomp_page);

            if (!dsa_bdesc_page || !dsa_bcomp_page) {
                pr_err("Failed to allocate page for dsa_bdesc or dsa_bcomp.\n");
                goto NODEV;
            }

            dsa_bdesc = page_to_virt(dsa_bdesc_page);
            memset(dsa_bdesc, 0, PAGE_SIZE * 2);
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

    for (i = 0; i < limit_chans; ++i) {
        if (channels[i]) {
            dma_release_channel(channels[i]);
        }
    }

    // Free global_idxd_desc_page, global_dsa_desc_page and global_dsa_comp_page
    for_each_possible_cpu(cpu) {
       for (i = 0; i < limit_chans; ++i) {
            page = ((struct page**)per_cpu_ptr_wrapper(global_idxd_desc_page, cpu))[i];
            if (page) {
                __free_page(page);
            }
            page = ((struct page**)per_cpu_ptr_wrapper(global_dsa_desc_page, cpu))[i];
            if (page) {
                __free_page(page);
            }
            page = ((struct page**)per_cpu_ptr_wrapper(global_dsa_comp_page, cpu))[i];
            if (page) {
                __free_page(page);
            }
        }
    }

    // Free global_dsa_bdesc_page and global_dsa_bcomp_page
    for_each_possible_cpu(cpu) {
        for (i = 0; i < limit_chans; ++i) {
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
    *poll_retry = 0;
    // while (idxd_desc->completion->status == DSA_COMP_NONE && ++(*poll_retry) < POLL_RETRY_MAX);
    // while (idxd_desc->completion->status == DSA_COMP_NONE && ++(*poll_retry) < POLL_RETRY_MAX) __builtin_ia32_pause();
    while (idxd_desc->completion->status == DSA_COMP_NONE && ++(*poll_retry) < POLL_RETRY_MAX) {
        umonitor(idxd_desc->completion);
        if (idxd_desc->completion->status == DSA_COMP_NONE) {
            umwait(rdtsc() + UMWAIT_DELAY, UMWAIT_STATE);
        }
    }
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
    struct idxd_desc **idxd_desc;
    uint32_t page_offset, poll_retry = 0;
    int i;

    local_lock(&dsa_copy_local_lock);

    TIMER_ON(prepare_desc);
    idxd_desc = (struct idxd_desc**)this_cpu_ptr_wrapper(global_idxd_desc);
    // page_offset = nr_pages / limit_chans;
    page_offset = nr_pages * PAGE_SIZE / limit_chans;
    for (i = 0; i < limit_chans; ++i) {
        idxd_desc[i][0].hw->opcode = DSA_OPCODE_MEMMOVE;
        idxd_desc[i][0].hw->src_addr = page_to_phys(from) + i * page_offset;
        idxd_desc[i][0].hw->dst_addr = page_to_phys(to) + i * page_offset;
        idxd_desc[i][0].hw->xfer_size = page_offset;
        idxd_desc[i][0].completion->status = DSA_COMP_NONE;
        // pr_notice("wq name: %s id %d size %u max_wq %d\n", idxd_desc[i].wq->name, idxd_desc[i].wq->id, idxd_desc[i].wq->size, idxd_desc[i].wq->idxd->max_wqs);
        idxd_fast_submit_desc(idxd_desc[i][0].wq, &idxd_desc[i][0]);
    }
    TIMER_OFF(prepare_desc);

    TIMER_ON(submit_desc);
    // for (int i = 0; i < limit_chans; ++i) {
    //     idxd_fast_submit_desc(idxd_desc[i].wq, &idxd_desc[i]);
    // }
    TIMER_OFF(submit_desc);

    TIMER_ON(wait_for_completion);
    for (i = 0; i < limit_chans; ++i) {
        wait_for_dsa_completion(&idxd_desc[i][0], &poll_retry);
        // pr_notice("idxd_desc[%d] status %u, poll_retry %u.\n", i, idxd_desc[i].completion->status, poll_retry);
        if (idxd_desc[i][0].completion->status != DSA_COMP_SUCCESS) {
            pr_err_ratelimited("idxd_desc[%d][%d] failed with status %u, poll_retry %u.\n", i, 0, idxd_desc[i][0].completion->status, poll_retry);
            use_dsa_copy_pages = 0;
            return -EIO;
        }
    }
    TIMER_OFF(wait_for_completion);

    local_unlock(&dsa_copy_local_lock);

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
__always_unused int dsa_batch_copy_pages(struct page *to, struct page *from, int nr_pages) {
    struct idxd_desc **idxd_desc;
    struct dsa_hw_desc *dsa_bdesc;
    struct dsa_completion_record *dsa_bcomp;
    uint32_t poll_retry = 0;
    int i, idx = 0;

    TIMER_ON(prepare_desc);
    idxd_desc = (struct idxd_desc**)this_cpu_ptr_wrapper(global_idxd_desc);
    // dsa_bdesc = *(struct dsa_hw_desc**)this_cpu_ptr(global_dsa_bdesc[idx]);
    dsa_bdesc = ((struct dsa_hw_desc**)this_cpu_ptr_wrapper(global_dsa_bdesc))[idx];
    // dsa_bcomp = *(struct dsa_completion_record**)this_cpu_ptr(global_dsa_bcomp[idx]);
    dsa_bcomp = ((struct dsa_completion_record**)this_cpu_ptr_wrapper(global_dsa_bcomp))[idx];

    idxd_desc[idx][0].hw->opcode = DSA_OPCODE_BATCH;
    idxd_desc[idx][0].hw->desc_list_addr = virt_to_phys(dsa_bdesc);
    idxd_desc[idx][0].hw->dst_addr = 0; // not used but must be set to 0
    idxd_desc[idx][0].hw->desc_count = nr_pages;
    idxd_desc[idx][0].completion->status = DSA_COMP_NONE;

    for (i = 0; i < nr_pages; ++i) {
        dsa_bdesc[i].src_addr = page_to_phys(from + i);
        dsa_bdesc[i].dst_addr = page_to_phys(to + i);
        dsa_bdesc[i].xfer_size = PAGE_SIZE;
        dsa_bcomp[i].status = DSA_COMP_NONE;
    }
    TIMER_OFF(prepare_desc);

    TIMER_ON(submit_desc);
    idxd_fast_submit_desc(idxd_desc[idx][0].wq, &idxd_desc[idx][0]);
    TIMER_OFF(submit_desc);

    TIMER_ON(wait_for_completion);
    wait_for_dsa_completion(&idxd_desc[idx][0], &poll_retry);
    if (idxd_desc[idx][0].completion->status != DSA_COMP_SUCCESS) {
        pr_err_ratelimited("idxd_desc[%d][%d] failed with status %u, poll_retry %u.\n", idx, 0, idxd_desc[idx][0].completion->status, poll_retry);
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

__always_unused int dsa_copy_page(struct page *to, struct page *from) {
    struct idxd_desc **idxd_desc;
    uint32_t poll_retry = 0;
    int idx = 0;

    TIMER_ON(prepare_desc);
    idxd_desc = (struct idxd_desc**)this_cpu_ptr_wrapper(global_idxd_desc);
    idxd_desc[idx][0].hw->opcode = DSA_OPCODE_MEMMOVE;
    idxd_desc[idx][0].hw->src_addr = page_to_phys(from);
    idxd_desc[idx][0].hw->dst_addr = page_to_phys(to);
    idxd_desc[idx][0].hw->xfer_size = PAGE_SIZE;
    idxd_desc[idx][0].completion->status = DSA_COMP_NONE;
    TIMER_OFF(prepare_desc);

    TIMER_ON(submit_desc);
    idxd_fast_submit_desc(idxd_desc[idx][0].wq, &idxd_desc[idx][0]);
    TIMER_OFF(submit_desc);

    TIMER_ON(wait_for_completion);
    wait_for_dsa_completion(&idxd_desc[idx][0], &poll_retry);
    if (idxd_desc[idx][0].completion->status != DSA_COMP_SUCCESS) {
        pr_err_ratelimited("idxd_desc[%d][%d] failed with status %u, poll_retry %u.\n", idx, 0, idxd_desc[idx][0].completion->status, poll_retry);
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

// use dsa to copy a list of pages containing 4KB base pages and 2MB thp pages
// use multiple channels to copy 2MB pages and batch processing to copy 4KB pages
int dsa_copy_page_lists(struct page **to, struct page **from, int nr_items) {
    struct idxd_desc **idxd_desc;
    struct dsa_hw_desc **dsa_bdesc;
    struct dsa_completion_record **dsa_bcomp;
    uint32_t page_offset, poll_retry = 0;
    int i, j, t, nr_base_pages = 0, cur_chan = 0, sub_pages, batch_size, batch_size_remain;
    int tail[MAX_CHAN] = {0};

    local_lock(&dsa_copy_local_lock);

    idxd_desc = (struct idxd_desc**)this_cpu_ptr_wrapper(global_idxd_desc);
    dsa_bdesc = (struct dsa_hw_desc**)this_cpu_ptr_wrapper(global_dsa_bdesc);
    dsa_bcomp = (struct dsa_completion_record**)this_cpu_ptr_wrapper(global_dsa_bcomp);

    // use multiple channels to copy 2MB pages
    for (i = 0; i < nr_items; ++i) {
        if (thp_nr_pages(from[i]) != thp_nr_pages(to[i])) {
            pr_err("The number of pages in from[%d] and to[%d] are not equal, thp_nr_pages(from[%d]) = %d, thp_nr_pages(to[%d]) = %d.\n", i, i, i, thp_nr_pages(from[i]), i, thp_nr_pages(to[i]));
            return -EINVAL;
        }
        sub_pages = thp_nr_pages(from[i]);
        if (sub_pages > 1) {
            page_offset = sub_pages * PAGE_SIZE / limit_chans;
            for (j = 0; j < limit_chans; ++j) {
                t = tail[j]++;
                idxd_desc[j][t].hw->opcode = DSA_OPCODE_MEMMOVE;
                idxd_desc[j][t].hw->src_addr = page_to_phys(from[i]) + j * page_offset;
                idxd_desc[j][t].hw->dst_addr = page_to_phys(to[i]) + j * page_offset;
                idxd_desc[j][t].hw->xfer_size = page_offset;
                idxd_desc[j][t].completion->status = DSA_COMP_NONE;
                // pr_notice("wq name: %s id %d size %u max_wq %d\n", idxd_desc[j].wq->name, idxd_desc[j].wq->id, idxd_desc[j].wq->size, idxd_desc[j].wq->idxd->max_wqs);
                idxd_fast_submit_desc(idxd_desc[j][t].wq, &idxd_desc[j][t]);
            }
        } else {
            ++nr_base_pages;
        }
    }

    batch_size = nr_base_pages / limit_chans;
    batch_size_remain = nr_base_pages % limit_chans;
    nr_base_pages = 0;
    for (i = 0; i < nr_items; ++i) {
        if (thp_nr_pages(from[i]) > 1) continue;

        // for corner case, we use simple dsa-copy way (i.e., dsa_copy_page), but we can still ultilize multiple channels
        if (batch_size < 2) {
            t = tail[cur_chan]++;

            idxd_desc[cur_chan][t].hw->opcode = DSA_OPCODE_MEMMOVE;
            idxd_desc[cur_chan][t].hw->src_addr = page_to_phys(from[i]);
            idxd_desc[cur_chan][t].hw->dst_addr = page_to_phys(to[i]);
            idxd_desc[cur_chan][t].hw->xfer_size = PAGE_SIZE;
            idxd_desc[cur_chan][t].completion->status = DSA_COMP_NONE;

            idxd_fast_submit_desc(idxd_desc[cur_chan][t].wq, &idxd_desc[cur_chan][t]);
            cur_chan = (cur_chan + 1 < limit_chans ? cur_chan + 1 : 0);
        // use batch processing to copy 4KB pages 
        } else {
            t = nr_base_pages++;

            dsa_bdesc[cur_chan][t].src_addr = page_to_phys(from[i]);
            dsa_bdesc[cur_chan][t].dst_addr = page_to_phys(to[i]);
            dsa_bdesc[cur_chan][t].xfer_size = PAGE_SIZE;
            dsa_bcomp[cur_chan][t].status = DSA_COMP_NONE;
            
            if (nr_base_pages == (batch_size + (cur_chan < batch_size_remain))) {
                t = tail[cur_chan]++;

                idxd_desc[cur_chan][t].hw->opcode = DSA_OPCODE_BATCH;
                idxd_desc[cur_chan][t].hw->desc_list_addr = virt_to_phys(dsa_bdesc[cur_chan]);
                idxd_desc[cur_chan][t].hw->dst_addr = 0; // not used but must be set to 0
                idxd_desc[cur_chan][t].hw->desc_count = nr_base_pages;
                idxd_desc[cur_chan][t].completion->status = DSA_COMP_NONE;

                idxd_fast_submit_desc(idxd_desc[cur_chan][t].wq, &idxd_desc[cur_chan][t]);

                nr_base_pages = 0;
                ++cur_chan;
            }
        }
    }
    
    // wait for completion
    for (i = 0; i < limit_chans; ++i) {
        for (j = 0; j < tail[i]; ++j) {
            wait_for_dsa_completion(&idxd_desc[i][j], &poll_retry);
            if (idxd_desc[i][j].completion->status != DSA_COMP_SUCCESS) {
                pr_err_ratelimited("idxd_desc[%d][%d] failed with status %u, poll_retry %u.\n", i, j, idxd_desc[i][j].completion->status, poll_retry);
                use_dsa_copy_pages = 0;
                return -EIO;
            }
        }
    }


    local_unlock(&dsa_copy_local_lock);

    return 0;
}
EXPORT_SYMBOL(dsa_copy_page_lists);

static int timer_proc_show(struct seq_file *m, void *v) {
    seq_printf(m, 
        "timer_state = %d total_time = %llums last_time = %lluns last_cnt = %lu dsa_hpage_cnt = %lu dsa_bpage_cnt = %lu dsa_copy_fail = %lu hpage_cnt = %lu bpage_cnt = %lu\n",
        timer_state, ktime_to_ms(total_time), ktime_to_ns(last_time), last_cnt, atomic_long_read(&dsa_hpage_cnt), atomic_long_read(&dsa_bpage_cnt), atomic_long_read(&dsa_copy_fail), atomic_long_read(&hpage_cnt), atomic_long_read(&bpage_cnt)
        // max_hpage_cnt, max_bpage_cnt
    );
    return 0;
}

static int timer_proc_open(struct inode *inode, struct file *file) {
    return single_open(file, timer_proc_show, NULL);
}

static ssize_t timer_proc_write(struct file *file, const char __user *buffer, size_t len, loff_t *f_pos) {
    static char kbuf[1024];
    unsigned long rc;
    int val;

    rc = copy_from_user(kbuf, buffer, len);
    pr_notice("rc: %lu len(buf): %lu buf: %s\n", rc, strlen(kbuf), kbuf);
    rc = sscanf(kbuf, "%d", &val);
    if (rc == 1) {
        if (timer_state == TIMER_OFF && val == TIMER_ON) {
            total_time = last_time = ktime_set(0, 0);
            last_cnt = 0;
            atomic_long_set(&dsa_copy_fail, 0);
            atomic_long_set(&hpage_cnt, 0);
            atomic_long_set(&bpage_cnt, 0);
            atomic_long_set(&dsa_hpage_cnt, 0);
            atomic_long_set(&dsa_bpage_cnt, 0);
        }
        if (val == TIMER_ON || val == TIMER_OFF)
            timer_state = val;
    } else {
        pr_notice("Error: rc is %lu, fail to parse %s\n", rc, kbuf);
    }
    return len;
}

int sysctl_dsa_state_handler(struct ctl_table *table, int write,
	void *buffer, size_t *lenp, loff_t *ppos) {
    int ret, oldval, rc;
    
    oldval = dsa_state;
    ret = proc_dointvec_minmax(table, write, buffer, lenp, ppos);

    if (dsa_state != oldval) {
        if (dsa_state) {
            rc = dsa_init();
            if (rc) {
                pr_err("Failed to initialize DSA.\n");
            }
        } else {
            dsa_release();
        }
    }

    return ret;
}
EXPORT_SYMBOL(sysctl_dsa_state_handler);

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
    atomic_long_set(&dsa_copy_fail, 0);
    atomic_long_set(&hpage_cnt, 0);
    atomic_long_set(&bpage_cnt, 0);
    atomic_long_set(&dsa_hpage_cnt, 0);
    atomic_long_set(&dsa_bpage_cnt, 0);
    last_cnt = dsa_state = use_dsa_copy_pages = 0;
    // max_bpage_cnt = max_hpage_cnt = 0;
    timer_state = TIMER_OFF;

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
