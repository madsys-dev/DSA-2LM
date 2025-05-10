// Author: LRL52
#include <linux/kthread.h>
#include <linux/completion.h>
#include <linux/preempt.h>
#include <linux/module.h>
#include <linux/dmaengine.h>
#include <linux/dma-mapping.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/delay.h>
#include <linux/idxd.h>
#include <linux/local_lock.h>
#include "idxd.h"
#include "linux/sched.h"

#define THREAD_NUM 1
#define TEST_ITER 1000
#define ORDER 9
#define NR_PAGES (1 << ORDER)
#define BUFFER_SIZE (NR_PAGES * PAGE_SIZE)
#define MAX_CHAN 8
#define MAX_BATCH_SIZE 128 // should lower or equal to 128, because two pages can only allocate 128 dsa_hw_desc(s)
#define MAX_IDXD_DESC 25 // one page can only allocate 25 idxd_desc(s)
#define MAX_HPAGE_NR (MAX_IDXD_DESC - 1)
#define MAX_BPAGE_NR (MAX_CHAN * MAX_BATCH_SIZE)
#define POLL_RETRY_MAX 100000000
#define CPUFREQ 3200000000L
#define copy_pages_wrapper dsa_multi_copy_pages

// #define TEST_COPY_PAGE_LISTS
#define USE_PER_CPU_VARIABLES
// #define TIMER_ENABLE

#define TEST_BATCH_SIZE 32
#define copy_page_lists_wrapper dsa_copy_page_lists
static int test_huge_page_idx[] = {5};
#define HUGE_PAGE_NUM sizeof(test_huge_page_idx) / 4

#ifdef TIMER_ENABLE
#define TIMER_ON(name) uint64_t start_##name = rdtsc()
#define TIMER_OFF(name) uint64_t end_##name = rdtsc()
#define TIMER_SHOW(name) pr_notice_ratelimited("%s: %llu ns\n", #name, (end_##name - start_##name) * 1000000000 / CPUFREQ)
#else
#define TIMER_ON(name)
#define TIMER_OFF(name)
#define TIMER_SHOW(name)
#endif

// #define MAXN 1000000
// static unsigned int fib[MAXN];
static int dsa_async_mode = 0;
static int dsa_sync_mode = 0;
static unsigned long dsa_timeout_ms = 5000;
static unsigned long dsa_timeout_jiffies;
static int limit_chans = MAX_CHAN;
static struct dma_chan *channels[MAX_CHAN];
static struct dma_device *copy_dev[MAX_CHAN];
static struct completion thread_completion[THREAD_NUM];
DEFINE_PER_CPU(local_lock_t, dsa_copy_local_lock) = INIT_LOCAL_LOCK(dsa_copy_local_lock);
DEFINE_PER_CPU(struct mutex, dsa_copy_local_mutex);
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
DEFINE_PER_CPU(struct page*, global_completion_page[MAX_CHAN]);
DEFINE_PER_CPU(struct completion*, global_completion[MAX_CHAN]);
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
static struct page *global_completion_page[NR_CPUS][MAX_CHAN];
static struct completion *global_completion[NR_CPUS][MAX_CHAN];
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

static void dma_complete_func(void *completion) { complete(completion); }
static inline struct idxd_wq *to_idxd_wq(struct dma_chan *c) {
	struct idxd_dma_chan *idxd_chan;

    idxd_chan = container_of(c, struct idxd_dma_chan, chan);
	return idxd_chan->wq;
}

int check_idx_is_huge_page(int idx) {
    int i;

    for (i = 0; i < HUGE_PAGE_NUM; ++i) {
        if (idx == test_huge_page_idx[i]) {
            return 1;
        }
    }
    return 0;
}


static int dsa_test_init(void) {
    dma_cap_mask_t mask;
    struct idxd_desc *idxd_desc;
    struct dsa_hw_desc *dsa_desc, *dsa_bdesc;
    struct dsa_completion_record *dsa_comp, *dsa_bcomp;
    struct completion *done;
    struct idxd_wq *wq;
    struct page *idxd_desc_page, *dsa_desc_page, *dsa_comp_page, *dsa_bdesc_page, *dsa_bcomp_page, *completion_page;
    int cpu, i, j;

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
        for (i = 0; i < MAX_CHAN; ++i) {
            ((struct page**)per_cpu_ptr_wrapper(global_idxd_desc_page, cpu))[i] = alloc_page(GFP_KERNEL);
            idxd_desc_page = ((struct page**)per_cpu_ptr_wrapper(global_idxd_desc_page, cpu))[i];
            ((struct idxd_desc**)per_cpu_ptr_wrapper(global_idxd_desc, cpu))[i] = page_to_virt(idxd_desc_page);
        
            ((struct page**)per_cpu_ptr_wrapper(global_dsa_desc_page, cpu))[i] = alloc_page(GFP_KERNEL);
            dsa_desc_page = ((struct page**)per_cpu_ptr_wrapper(global_dsa_desc_page, cpu))[i];
            ((struct dsa_hw_desc**)per_cpu_ptr_wrapper(global_dsa_desc, cpu))[i] = page_to_virt(dsa_desc_page);

            ((struct page**)per_cpu_ptr_wrapper(global_dsa_comp_page, cpu))[i] = alloc_page(GFP_KERNEL);
            dsa_comp_page = ((struct page**)per_cpu_ptr_wrapper(global_dsa_comp_page, cpu))[i];
            ((struct dsa_completion_record**)per_cpu_ptr_wrapper(global_dsa_comp, cpu))[i] = page_to_virt(dsa_comp_page);

            ((struct page**)per_cpu_ptr_wrapper(global_completion_page, cpu))[i] = alloc_page(GFP_KERNEL);
            completion_page = ((struct page**)per_cpu_ptr_wrapper(global_completion_page, cpu))[i];
            ((struct completion**)per_cpu_ptr_wrapper(global_completion, cpu))[i] = page_to_virt(completion_page);
            
            if (!idxd_desc_page || !dsa_desc_page || !dsa_comp_page || !completion_page) {
                pr_err("Failed to allocate page for idxd_desc, dsa_desc, dsa_comp or completion.\n");
                goto NODEV;
            }

            idxd_desc = page_to_virt(idxd_desc_page);
            memset(idxd_desc, 0, PAGE_SIZE);
            dsa_desc = page_to_virt(dsa_desc_page);
            memset(dsa_desc, 0, PAGE_SIZE);
            dsa_comp = page_to_virt(dsa_comp_page);
            memset(dsa_comp, 0, PAGE_SIZE);
            done = page_to_virt(completion_page);
            memset(done, 0, PAGE_SIZE);
            
            wq = to_idxd_wq(channels[i]);
            for (j = 0; j < MAX_IDXD_DESC; ++j) {
                idxd_desc[j].hw = &dsa_desc[j];
                idxd_desc[j].desc_dma = 0; // desc_dma is not used
                idxd_desc[j].completion = &dsa_comp[j];
                idxd_desc[j].compl_dma = virt_to_phys(idxd_desc[j].completion);
                idxd_desc[j].cpu = cpu;
                idxd_desc[j].id = -1;
                idxd_desc[j].wq = wq;

                dsa_desc[j].priv = 1;
                dsa_desc[j].flags = IDXD_OP_FLAG_CRAV | IDXD_OP_FLAG_RCR;
                dsa_desc[j].completion_addr = idxd_desc[j].compl_dma;
            }
        }
    }

    // Init batch descriptor
    for_each_possible_cpu(cpu) {
        for (i = 0; i < MAX_CHAN; ++i) {
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

    for_each_possible_cpu(cpu) {
        mutex_init(per_cpu_ptr(&dsa_copy_local_mutex, cpu));
    }

    dsa_timeout_jiffies = msecs_to_jiffies(dsa_timeout_ms);

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
    *poll_retry = 0;
    switch (dsa_sync_mode) {
        case 0: 
            while (idxd_desc->completion->status == DSA_COMP_NONE && ++(*poll_retry) < POLL_RETRY_MAX);
            break;
        case 1: 
            while (idxd_desc->completion->status == DSA_COMP_NONE && ++(*poll_retry) < POLL_RETRY_MAX) cond_resched();
            break;
        case 2: 
            while (idxd_desc->completion->status == DSA_COMP_NONE && ++(*poll_retry) < POLL_RETRY_MAX) __builtin_ia32_pause();
            break;
        case 3:
            while (idxd_desc->completion->status == DSA_COMP_NONE && ++(*poll_retry) < POLL_RETRY_MAX) {
                umonitor(idxd_desc->completion);
                if (idxd_desc->completion->status == DSA_COMP_NONE) {
                    umwait(rdtsc() + UMWAIT_DELAY, UMWAIT_STATE);
                }
            }
            break;
    }
    
    // if (idxd_desc->completion->status != DSA_COMP_SUCCESS) {
    //     pr_err("idxd_desc failed with status %u, poll_retry %u.\n", idxd_desc->completion->status, *poll_retry);
    // }
}

static int idxd_fast_submit_desc(struct idxd_wq *wq, struct idxd_desc *desc) {
	void __iomem *portal;
    struct idxd_irq_entry *ie = NULL;
    u32 desc_flags = desc->hw->flags;

	portal = idxd_wq_portal_addr(wq);
	
    wmb();

    if (desc_flags & IDXD_OP_FLAG_RCI) {
		ie = &wq->ie;
		desc->hw->int_handle = ie->int_handle;
		llist_add(&desc->llnode, &ie->pending_llist);
	}

	iosubmit_cmds512(portal, desc->hw, 1);

	return 0;
}


// for bandwidth test 
int dsa_multi_copy_pages(int cpu, int idx, int is_end, struct page *to, struct page *from, int nr_pages) {
    struct mutex *cur_mutex;
    struct idxd_desc **idxd_desc;
    struct completion **done;
    uint32_t page_offset, poll_retry = 0;
    int i;

    done = (struct completion**)per_cpu_ptr_wrapper(global_completion, cpu);
    idxd_desc = (struct idxd_desc**)per_cpu_ptr_wrapper(global_idxd_desc, cpu);
    page_offset = nr_pages * PAGE_SIZE / limit_chans;
    for (i = 0; i < limit_chans; ++i) {
        idxd_desc[i][idx].hw->opcode = DSA_OPCODE_MEMMOVE;
        idxd_desc[i][idx].hw->src_addr = page_to_phys(from) + i * page_offset;
        idxd_desc[i][idx].hw->dst_addr = page_to_phys(to) + i * page_offset;
        idxd_desc[i][idx].hw->xfer_size = page_offset;
        idxd_desc[i][idx].completion->status = DSA_COMP_NONE;
        idxd_desc[i][idx].hw->flags = IDXD_OP_FLAG_CRAV | IDXD_OP_FLAG_RCR;

        idxd_fast_submit_desc(idxd_desc[i][idx].wq, &idxd_desc[i][idx]);
    }
    
    if (is_end) {
        for (i = 0; i < limit_chans; ++i) {
            wait_for_dsa_completion(&idxd_desc[i][idx], &poll_retry);
            if (idxd_desc[i][idx].completion->status != DSA_COMP_SUCCESS) {
                pr_err_ratelimited("idxd_desc[%d][%d] failed with status %u, poll_retry %u.\n", i, idx, idxd_desc[i][idx].completion->status, poll_retry);
                return -EIO;
            }
        }
    }

    return 0;
}

// nr_pages should greater than 1 and less than or equal to MAX_BATCH_SIZE
// use channel idx to process the batch, multiple 4KB pages to test batch 
// for bandwidth test
int dsa_batch_copy_pages(int cpu, int is_end, struct page *to, struct page *from, int nr_pages) {
    struct idxd_desc **idxd_desc;
    struct dsa_hw_desc **dsa_bdesc;
    struct dsa_completion_record **dsa_bcomp;
    uint32_t poll_retry = 0;
    int i, chan = 0;

    idxd_desc = (struct idxd_desc**)per_cpu_ptr_wrapper(global_idxd_desc, cpu);
    dsa_bdesc = (struct dsa_hw_desc**)per_cpu_ptr_wrapper(global_dsa_bdesc, cpu);
    dsa_bcomp = (struct dsa_completion_record**)per_cpu_ptr_wrapper(global_dsa_bcomp, cpu);


    idxd_desc[chan][0].hw->opcode = DSA_OPCODE_BATCH;
    idxd_desc[chan][0].hw->desc_list_addr = virt_to_phys(dsa_bdesc[chan]);
    idxd_desc[chan][0].hw->desc_count = nr_pages;
    idxd_desc[chan][0].completion->status = DSA_COMP_NONE;
    idxd_desc[chan][0].hw->flags = IDXD_OP_FLAG_CRAV | IDXD_OP_FLAG_RCR;

    for (i = 0; i < nr_pages; ++i) {
        dsa_bdesc[chan][i].src_addr = page_to_phys(from + i);
        dsa_bdesc[chan][i].dst_addr = page_to_phys(to + i);
        dsa_bdesc[chan][i].xfer_size = PAGE_SIZE;
        dsa_bcomp[chan][i].status = DSA_COMP_NONE;
    }

    idxd_fast_submit_desc(idxd_desc[chan][0].wq, &idxd_desc[chan][0]);

    if (is_end) {
        wait_for_dsa_completion(&idxd_desc[chan][0], &poll_retry);
        if (idxd_desc[chan][0].completion->status != DSA_COMP_SUCCESS) {
            pr_err_ratelimited("idxd_desc[%d][%d] failed with status %u, poll_retry %u.\n", chan, 0, idxd_desc[chan][0].completion->status, poll_retry);
            return -EIO;
        }
    }

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
    void *src, *dst;
    int i, j, rc, ok, success = 0, is_huge_page;
    int tid = *(int *)arg;
    uint64_t before, after, cpu_id;
    int cpu = 0, idx = -1, is_end, nr_cpus;

    nr_cpus = num_online_cpus();
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
        memset(page_to_virt(page1[tid][i]), 0xAB, BUFFER_SIZE);
        memset(page_to_virt(page2[tid][i]), 0xEF, BUFFER_SIZE);
    }
    after = rdtsc();
    pr_notice("Thread %d/%d: Prepare pages cost %llu ns.\n", tid, THREAD_NUM, (after - before) * 1000000000 / CPUFREQ);

    start = ktime_get();
    for (i = 0; i < TEST_ITER; ++i) {
        ++idx;
        if (idx >= MAX_IDXD_DESC) idx = 0, ++cpu;
        if (cpu >= nr_cpus) cpu = 0;
        is_end = (i + 1 == TEST_ITER) || ((i + 1) % 128 == 0);
        rc = dsa_multi_copy_pages(cpu, idx, is_end, page2[tid][i], page1[tid][i], NR_PAGES);
    }
    // cpu = -1;
    // for (i = 0; i < TEST_ITER; ++i) {
    //     ++cpu;
    //     if (cpu >= nr_cpus) cpu = 0;
    //     is_end = (i + 1 == TEST_ITER) || ((i + 1) % 128 == 0);
    //     rc = dsa_batch_copy_pages(cpu, is_end, page2[tid][i], page1[tid][i], NR_PAGES);
    // }
    end = ktime_get();
    total_time = ktime_sub(end, start);
    
    for (i = 0; i < TEST_ITER; ++i) {
        src = page_to_virt(page1[tid][i]);
        dst = page_to_virt(page2[tid][i]);
        if (memcmp(src, dst, BUFFER_SIZE) == 0) ++success;
    }

    pr_notice("Thread %d/%d: %d/%d tests passed, average copy time is %lld ns.\n", 
         tid, THREAD_NUM, success, TEST_ITER, ktime_to_ns(total_time) / TEST_ITER);

    pr_notice("Bandwidth: %lld MB/s\n", (long long)BUFFER_SIZE * TEST_ITER * 1000000000 / ktime_to_ns(total_time) / 1024 / 1024);
    pr_notice("total transfer size: %lld MB, total time: %lld ns\n", (long long)BUFFER_SIZE * TEST_ITER / 1024 / 1024, ktime_to_ns(total_time));   
NOMEM:

    for (i = 0; i < TEST_ITER; ++i) {
        if (page1[tid][i]) __free_pages(page1[tid][i], ORDER);
        if (page2[tid][i]) __free_pages(page2[tid][i], ORDER);
    }

    complete(&thread_completion[tid]);
    while (!kthread_should_stop()) {}
    return 0;
}

static int __init dsa_test_main(void) {
    int i, rc, cpu;
    int *tid;
    struct task_struct **thread;

    printk(KERN_INFO "DMA memcpy module init.\n");

    // for_each_possible_cpu(cpu) {
    //     local_lock_init(per_cpu_ptr(dsa_copy_local_lock, cpu));
    // }

    rc = dsa_test_init();
    if (rc) {
        printk(KERN_ERR "Failed to initialize DMA test.\n");
        return rc;
    }

#ifdef TEST_COPY_PAGE_LISTS
    pr_notice("Thread_NUM: %d MAX_CHAN: %d TEST_ITER: %d TEST_BATCH_SIZE: %d\n", THREAD_NUM, MAX_CHAN, TEST_ITER, TEST_BATCH_SIZE);
#else
    pr_notice("Thread_NUM: %d MAX_CHAN: %d BUFFER_SIZE: %luKB TEST_ITER: %d\n", THREAD_NUM, MAX_CHAN, BUFFER_SIZE / 1024, TEST_ITER);
#endif

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
    for (i = 0; i < THREAD_NUM; ++i) {
        tid[i] = i;
        init_completion(&thread_completion[i]);
        thread[i] = kthread_run(dsa_test, &tid[i], "dsa_test");
    }

    for (i = 0; i < THREAD_NUM; ++i) {
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
    int cpu, i;

    pr_notice("DMA memcpy module exit.\n");

    for (i = 0; i < MAX_CHAN; ++i) {
        if (channels[i]) {
            dma_release_channel(channels[i]);
        }
    }

    // Free global_idxd_desc_page, global_dsa_desc_page and global_dsa_comp_page
    for_each_possible_cpu(cpu) {
       for (i = 0; i < MAX_CHAN; ++i) {
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
            page = ((struct page**)per_cpu_ptr_wrapper(global_completion_page, cpu))[i];
            if (page) {
                __free_page(page);
            }
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

module_init(dsa_test_main);
module_exit(dsa_test_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("LRL52");
MODULE_DESCRIPTION("Example module using DMA(DSA) to perform memory copy.");
