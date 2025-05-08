// Author: LRL52
#include <linux/kthread.h>
#include "linux/preempt.h"
#include "linux/sched.h"
#include <linux/module.h>
#include <linux/dmaengine.h>
#include <linux/dma-mapping.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/delay.h>
#include <linux/idxd.h>
#include "idxd.h"

#define THREAD_NUM 1
#define TEST_ITER 1000
#define ORDER 9
#define NR_PAGES (1 << ORDER)
#define BUFFER_SIZE (NR_PAGES * PAGE_SIZE)
#define MAX_CHAN 1
#define MAX_BATCH_SIZE 128 // should lower or equal to 128, because two pages can only allocate 128 dsa_hw_desc(s)
#define POLL_RETRY_MAX 100000000
#define CPUFREQ 3200000000L
#define copy_pages_wrapper cpu_copy_pages

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
static struct completion thread_completion[THREAD_NUM];
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

static bool dmatest_match_channel(const char *chan_name, struct dma_chan *chan) {
	if (chan_name[0] == '\0') return true;
	return strcmp(dma_chan_name(chan), chan_name) == 0;
}
static bool filter(struct dma_chan *chan, void *chan_name) { return dmatest_match_channel(chan_name, chan); }
static void dma_complete_func(void *completion) { complete(completion); }
static inline struct idxd_wq *to_idxd_wq(struct dma_chan *c) {
	struct idxd_dma_chan *idxd_chan;

    idxd_chan = container_of(c, struct idxd_dma_chan, chan);
	return idxd_chan->wq;
}


static int dsa_test_init(void) {
    dma_cap_mask_t mask;
    struct idxd_desc *idxd_desc;
    struct dsa_hw_desc *dsa_desc, *dsa_bdesc;
    struct dsa_completion_record *dsa_comp, *dsa_bcomp;
    struct idxd_wq *wq;
    struct page *dsa_comp_page, *dsa_bdesc_page, *dsa_bcomp_page;
    int cpu, i;

    dma_cap_zero(mask);
	dma_cap_set(DMA_MEMCPY, mask);

    // Get the DMA channel
    for (i = 0; i < MAX_CHAN; ++i) {
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
        for (int i = 0; i < MAX_CHAN; ++i) {
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
        for (int i = 0; i < MAX_CHAN; ++i) {
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
            for (int j = 0; j < MAX_BATCH_SIZE; ++j) {
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

// require nr_pages mod MAX_CHAN equals 0
// it's non-reentrant functions because of per-cpu variables, or use preempt_disable/enable
static int dsa_multi_copy_pages(struct page *to, struct page *from, int nr_pages) {
    struct idxd_desc *idxd_desc;
    uint32_t page_offset, poll_retry = 0;
    int i;

    // preempt_disable();
    // TIMER_ON(prepare_and_submit_desc);
    TIMER_ON(prepare_desc);
    idxd_desc = (struct idxd_desc*)this_cpu_ptr_wrapper(global_idxd_desc);
    // page_offset = nr_pages / MAX_CHAN;
    page_offset = nr_pages * PAGE_SIZE / MAX_CHAN;
    for (i = 0; i < MAX_CHAN; ++i) {
        idxd_desc[i].hw->opcode = DSA_OPCODE_MEMMOVE;
        idxd_desc[i].hw->src_addr = page_to_phys(from) + i * page_offset;
        idxd_desc[i].hw->dst_addr = page_to_phys(to) + i * page_offset;
        idxd_desc[i].hw->xfer_size = page_offset;
        idxd_desc[i].completion->status = DSA_COMP_NONE;
        // pr_notice("wq name: %s id %d size %u max_wq %d\n", idxd_desc[i].wq->name, idxd_desc[i].wq->id, idxd_desc[i].wq->size, idxd_desc[i].wq->idxd->max_wqs);
        idxd_submit_desc(idxd_desc[i].wq, &idxd_desc[i]);
    }
    TIMER_OFF(prepare_desc);

    TIMER_ON(submit_desc);
    // for (int i = 0; i < MAX_CHAN; ++i) {
    //     idxd_submit_desc(idxd_desc[i].wq, &idxd_desc[i]);
    // }
    TIMER_OFF(submit_desc);

    TIMER_ON(wait_for_completion);
    for (i = 0; i < MAX_CHAN; ++i) {
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

// nr_pages should greater than 1 and less than or equal to MAX_BATCH_SIZE
// use channel idx to process the batch, multiple 4KB pages to test batch processing
static int dsa_batch_copy_pages(struct page *to, struct page *from, int nr_pages) {
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
    idxd_submit_desc(idxd_desc[idx].wq, &idxd_desc[idx]);
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

// use one dsa to copy multiple pages with a maximum queue depth of 128
// just for test, only allow ONE THREAD!
static int dsa_copy_pages(struct page *to, struct page *from, int nr_pages) {
    struct idxd_desc *idxd_desc;
    uint32_t poll_retry = 0;
    int i, idx = 0;

    TIMER_ON(prepare_and_submit_desc);
    for (i = 0; i < nr_pages; ++i) {
        idxd_desc = (struct idxd_desc*)per_cpu_ptr_wrapper(global_idxd_desc, i);
        idxd_desc[idx].hw->opcode = DSA_OPCODE_MEMMOVE;
        idxd_desc[idx].hw->src_addr = page_to_phys(from + i);
        idxd_desc[idx].hw->dst_addr = page_to_phys(to + i);
        idxd_desc[idx].hw->xfer_size = PAGE_SIZE;
        idxd_desc[idx].completion->status = DSA_COMP_NONE;

        idxd_submit_desc(idxd_desc[idx].wq, &idxd_desc[idx]);
    }
    TIMER_OFF(prepare_and_submit_desc);

    TIMER_ON(wait_for_completion);
    for (i = 0; i < nr_pages; ++i) {
        idxd_desc = (struct idxd_desc*)per_cpu_ptr_wrapper(global_idxd_desc, i);
        wait_for_dsa_completion(&idxd_desc[idx], &poll_retry);
        if (idxd_desc[idx].completion->status != DSA_COMP_SUCCESS) {
            pr_err_ratelimited("idxd_desc[%d] failed with status %u, poll_retry %u.\n", idx, idxd_desc[idx].completion->status, poll_retry);
            return -EIO;
        }
    }
    TIMER_OFF(wait_for_completion);

#ifdef TIMER_ENABLE
    TIMER_SHOW(prepare_and_submit_desc);
    TIMER_SHOW(wait_for_completion);
#endif

    return 0;
}

static int dsa_copy_page(struct page *to, struct page *from) {
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
    idxd_submit_desc(idxd_desc[idx].wq, &idxd_desc[idx]);
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

static int cpu_copy_pages(struct page *to, struct page *from, int nr_pages) {
    int i;
    void *dst, *src;
    
    src = page_to_virt(from);
    dst = page_to_virt(to);

    for (i = 0; i < nr_pages; ++i) {
        copy_page(dst + i * PAGE_SIZE, src + i * PAGE_SIZE);
    }

    return 0;
}


static int dsa_test(void *arg) {
    static struct page *page1[THREAD_NUM][TEST_ITER], *page2[THREAD_NUM][TEST_ITER];
    ktime_t total_time, start, end;
    void *src_fail = NULL, *dst_fail = NULL;
    void *src, *dst;
    int i, rc, success = 0;
    int tid = *(int *)arg;
    uint64_t before, after, cpu_id;

    cpu_id = smp_processor_id();
    pr_notice("CPU: %llu Freq: %ldMHz Thread %d/%d started.\n", cpu_id, CPUFREQ / 1000000, tid, THREAD_NUM);

    before = rdtsc();
    for (i = 0; i < TEST_ITER; ++i) {
        page1[tid][i] = alloc_pages_node(0, GFP_KERNEL, ORDER);
        page2[tid][i] = alloc_pages_node(1, GFP_KERNEL, ORDER);
        if (!page1[tid][i] || !page2[tid][i]) {
            pr_notice("Thread %d/%d: Failed to allocate page at iteration %d.\n", tid, THREAD_NUM, i);
            goto NOMEM;
        }
        memset(page_to_virt(page1[tid][i]), 0x3f, BUFFER_SIZE);
        memset(page_to_virt(page2[tid][i]), 0x00, BUFFER_SIZE);
    }
    after = rdtsc();
    pr_notice("Thread %d/%d: Prepare pages cost %llu ns.\n", tid, THREAD_NUM, (after - before) * 1000000000 / CPUFREQ);

    total_time = ktime_set(0, 0);
    for (i = 0; i < TEST_ITER; ++i) {

        src = page_to_virt(page1[tid][i]);
        dst = page_to_virt(page2[tid][i]);

        preempt_disable();
        start = ktime_get();
        // before = rdtsc();
        wmb();
        rc = copy_pages_wrapper(page2[tid][i], page1[tid][i], NR_PAGES);
        wmb();
        // after = rdtsc();
        end = ktime_get();
        preempt_enable();
        // total_time = ktime_add(total_time, ktime_set(0, (after - before) * 1000000000 / CPUFREQ));
        total_time = ktime_add(total_time, ktime_sub(end, start));
        // pr_notice_ratelimited("cycles %llu\n", after - before);

        if (memcmp(src, dst, BUFFER_SIZE) == 0) ++success;
        else {
            src_fail = src, dst_fail = dst;
            // for (int i = 0; i < BUFFER_SIZE; i += 8) {
            //     printk(KERN_INFO "src[%d] = %x, dst[%d] = %x\n", i, ((unsigned char *)src)[i], i, ((unsigned char *)dst)[i]);
            // }
        }

    }

    printk(KERN_INFO "Thread %d/%d: %d/%d tests passed, average copy time is %lld ns.\n", 
         tid, THREAD_NUM, success, TEST_ITER, ktime_to_ns(total_time) / TEST_ITER);
    
NOMEM:
    for (i = 0; i < TEST_ITER; ++i) {
        if (page1[tid][i]) __free_pages(page1[tid][i], ORDER);
        if (page2[tid][i]) __free_pages(page2[tid][i], ORDER);
    }

    complete(&thread_completion[tid]);
    while (!kthread_should_stop()) { schedule(); }
    return 0;
}

static int __init dsa_test_main(void) {
    int rc;
    int *tid;
    struct task_struct **thread;

    printk(KERN_INFO "DMA memcpy module init.\n");

    rc = dsa_test_init();
    if (rc) {
        printk(KERN_ERR "Failed to initialize DMA test.\n");
        return rc;
    }

    pr_notice("Thread_NUM: %d MAX_CHAN: %d BUFFER_SIZE: %luKB TEST_ITER: %d\n", THREAD_NUM, MAX_CHAN, BUFFER_SIZE / 1024, TEST_ITER);

#if defined (dsa_multi_copy_pages) 
    pr_notice("Using dsa_multi_copy_pages.\n");
#elif defined(dsa_batch_copy_pages)
    pr_notice("Using dsa_batch_copy_pages.\n");
#elif defined (dsa_copy_pages)
    pr_notice("Using dsa_copy_pages.\n");
#elif defined (dsa_copy_page)
    pr_notice("Using dsa_copy_page.\n");
#elif defined (cpu_copy_pages)
    pr_notice("Using cpu_copy_pages.\n");
#endif

    tid = kmalloc(THREAD_NUM * sizeof(int), GFP_KERNEL);
    thread = kmalloc(THREAD_NUM * sizeof(struct task_struct*), GFP_KERNEL);
    for (int i = 0; i < THREAD_NUM; ++i) {
        tid[i] = i;
        init_completion(&thread_completion[i]);
        thread[i] = kthread_run(dsa_test, &tid[i], "dsa_test");
    }

    for (int i = 0; i < THREAD_NUM; ++i) {
        if (thread[i]) {
            wait_for_completion_timeout(&thread_completion[i], msecs_to_jiffies(1000));
            kthread_stop(thread[i]);
        }
    }

    kfree(thread);
    kfree(tid);

    return 0;
}

static void __exit dsa_test_exit(void) {
    struct page *page;
    int cpu;

    printk(KERN_INFO "DMA memcpy module exit.\n");

    for (int i = 0; i < MAX_CHAN; ++i) {
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
        for (int i = 0; i < MAX_CHAN; ++i) {
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

module_init(dsa_test_main);
module_exit(dsa_test_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("LRL52");
MODULE_DESCRIPTION("Example module using DMA(DSA) to perform memory copy.");
