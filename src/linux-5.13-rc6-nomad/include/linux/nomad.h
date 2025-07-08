#ifndef _LINUX_NOMAD_H
#define _LINUX_NOMAD_H
#include <linux/rmap.h>
#include <linux/types.h>
#include <linux/nomad2.h>

enum nomad_stat_async_promotion {
    NOMAD_MODULE_NOT_ENABLED,
    NOMAD_SYNC_SUCCESS,
	NOMAD_SYNC_FAIL,
    NOMAD_ASYNC_QUEUED_SUCCESS,
    NOMAD_ASYNC_QUEUED_FAIL,
};


struct nomad_remap_status {
	bool use_new_page;
	pte_t old_pteval;
	enum ttu_flags flags;
	struct page *new_page;
};

// TODO(lingfeng): when rmmod, how to safely reset the function pointer is not considered
// yet 
struct async_promote_ctrl {
	int initialized;
	/**
     * @brief return the number of pages to be queued, if should not queue, return 0
     * 
     */
	int (*queue_page_fault)(struct page *page, pte_t *ptep, int target_nid);

	int (*link_shadow_page)(struct page *newpage, struct page *oldpage);
	struct page *(*demote_shadow_page_find)(
		struct page *page, struct demote_shadow_page_context *contxt);
	bool (*demote_shadow_page_breakup)(
		struct page *page, struct demote_shadow_page_context *contxt);
	struct page* (*release_shadow_page)(struct page *page, void *private, bool in_fault);
	unsigned long (*reclaim_page)(int nid, int nr_to_reclaim);
};

void async_mod_glob_init_set(void);
bool async_mod_glob_inited(void);

bool nomad_try_to_remap(struct page *page, struct nomad_remap_status *arg);

extern struct async_promote_ctrl async_mod_glob_ctrl;
#endif