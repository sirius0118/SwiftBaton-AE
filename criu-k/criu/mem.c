#include "sb-kernel-transfer.h"
#include "sb-stage.h"
#include "sb-fork.h"
#include <unistd.h>
#include <stdio.h>
#include <sys/mman.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/syscall.h>
#include <sys/prctl.h>

#include "types.h"
#include "cr_options.h"
#include "servicefd.h"
#include "mem.h"
#include "parasite-syscall.h"
#include "parasite.h"
#include "page-pipe.h"
#include "page-xfer.h"
#include "log.h"
#include "kerndat.h"
#include "stats.h"
#include "vma.h"
#include "shmem.h"
#include "uffd.h"
#include "pstree.h"
#include "restorer.h"
#include "rst-malloc.h"
#include "bitmap.h"
#include "sk-packet.h"
#include "files-reg.h"
#include "pagemap-cache.h"
#include "fault-injection.h"
#include "prctl.h"
#include "compel/infect-util.h"
#include "pidfd-store.h"

#include "protobuf.h"
#include "images/pagemap.pb-c.h"

#ifdef RDMA_CODESIGN
#include "common/shregion.h"
#include "RDMA.h"

extern int item_num;
extern uint64_t pidset[MAX_PROCESS];
extern int socketset[MAX_PROCESS];

// struct resources PF_res;
// struct resources TS_res;
static inline void *sharemem_create(struct parasite_ctl *ctl, unsigned long size)
{
	int ret;
	void *mem;

	mem = (void *)mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, 0, 0);
	
	return mem;
}

/* The dumpee lacks host CAP_CHECKPOINT_RESTORE for another process's
 * /proc/PID/map_files. Open from CRIU and pass the backing file on its RPC
 * socket; no privilege expansion is needed inside the container. */
static int send_shared_region(struct parasite_ctl *ctl, pid_t owner, uint64_t address, uint64_t size)
{
    char path[128];
    int fd, ret;
    snprintf(path, sizeof(path), "/proc/%d/map_files/%llx-%llx", owner,
             (unsigned long long)address, (unsigned long long)(address + size));
    fd = open(path, O_RDWR | O_CLOEXEC);
    if (fd < 0) { pr_perror("Open dumpee shared region %s", path); return -1; }
    ret = compel_util_send_fd(ctl, fd);
    close(fd);
    return ret;
}

static inline void *sharemem_receive(struct parasite_ctl *ctl, unsigned long size, long pid)
{
	/* master -> parasite not implemented yet */
	// Let me to implement it
	int fd = -1;
    long ret;
	void *mem;
	// struct shmem_plugin_msg spi;
    char path[100];
	struct shmem_plugin_msg *args;
    
    // for ( int i = 0; i < item_num; i++ ){
    //     if ( pidset[i] == pid ){
    //         ssock = socketset[i];
    //         break;
    //     }
    // }
	// ssock = compel_rpc_sock(ctl);
	args = compel_parasite_args(ctl, struct shmem_plugin_msg);
	// 等待对端写入，然后从socket里面接收数据
	// ret = recv(ssock, (void *)&spi, sizeof(spi), MSG_WAITALL);
	
    // if (ret == -1)
    //     pr_err("Can't recv from sock:%d \n", ssock);
    
	// fd = mem_open_proc(pid, O_RDWR, "map_files/", (long)spi.start,
	//  (long)spi.start + spi.len);
	// pr_err("这里输出接收到的start:%lx, len:%lx\n", args->start, args->len);
    sprintf(path, "/proc/%ld/map_files/%lx-%lx", pid, (long)args->start, (long)(args->start + args->len));
	fd = open(path, O_RDWR);
	if (fd < 0) {
		pr_err("Can't open %s\n", path);
		return NULL;
	}
	mem = (void *)mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FILE, fd, 0);
	if ( mem == MAP_FAILED){
		mem = NULL;
		pr_err("Can't map remote parasite map!\n");
		return NULL;
	}

	return mem;
}
#endif

static int task_reset_dirty_track(int pid)
{
	int ret;

	if (!opts.track_mem)
		return 0;

	BUG_ON(!kdat.has_dirty_track);

	ret = do_task_reset_dirty_track(pid);
	BUG_ON(ret == 1);
	return ret;
}

int do_task_reset_dirty_track(int pid)
{
	int fd, ret;
	char cmd[] = "4";

	pr_info("Reset %d's dirty tracking\n", pid);

	fd = __open_proc(pid, EACCES, O_RDWR, "clear_refs");
	if (fd < 0)
		return errno == EACCES ? 1 : -1;

	ret = write(fd, cmd, sizeof(cmd));
	if (ret < 0) {
		if (errno == EINVAL) /* No clear-soft-dirty in kernel */
			ret = 1;
		else {
			pr_perror("Can't reset %d's dirty memory tracker", pid);
			ret = -1;
		}
	} else {
		pr_info(" ... done\n");
		ret = 0;
	}

	close(fd);
	return ret;
}

unsigned long dump_pages_args_size(struct vm_area_list *vmas)
{
#ifndef RDMA_CODESIGN
	/* In the worst case I need one iovec for each page */
	return sizeof(struct parasite_dump_pages_args) + vmas->nr * sizeof(struct parasite_vma_entry) +
	       (vmas->nr_priv_pages + 1) * sizeof(struct iovec);
#else
	return sizeof(struct parasite_dump_pages_args) + vmas->nr * sizeof(struct parasite_vma_entry) +
	       (vmas->nr_priv_pages + 1) * sizeof(struct iovec);
#endif
}

static inline bool __page_is_zero(u64 pme)
{
	return (pme & PME_PFRAME_MASK) == kdat.zero_page_pfn;
}

static inline bool __page_in_parent(bool dirty)
{
	/*
	 * If we do memory tracking, but w/o parent images,
	 * then we have to dump all memory
	 */

	return opts.track_mem && opts.img_parent && !dirty;
}

static bool should_dump_entire_vma(VmaEntry *vmae)
{
	/*
	 * vDSO area must be always dumped because on restore
	 * we might need to generate a proxy.
	 */
	if (vma_entry_is(vmae, VMA_AREA_VDSO))
		return true;
	if (vma_entry_is(vmae, VMA_AREA_AIORING))
		return true;

	return false;
}

/*
 * should_dump_page returns vaddr if an addressed page has to be dumped.
 * Otherwise, it returns an address that has to be inspected next.
 */
u64 should_dump_page(pmc_t *pmc, VmaEntry *vmae, u64 vaddr, bool *softdirty)
{
	if (vaddr >= pmc->end && pmc_fill(pmc, vaddr, vmae->end))
		return -1;

	if (pmc->regs) {
		while (1) {
			if (pmc->regs_idx == pmc->regs_len)
				return pmc->end;
			if (vaddr < pmc->regs[pmc->regs_idx].end)
				break;
			pmc->regs_idx++;
		}
		if (vaddr < pmc->regs[pmc->regs_idx].start)
			return pmc->regs[pmc->regs_idx].start;
		if (softdirty)
			*softdirty = pmc->regs[pmc->regs_idx].categories & PAGE_IS_SOFT_DIRTY;
		return vaddr;
	} else {
		u64 pme = pmc->map[PAGE_PFN(vaddr - pmc->start)];

		/*
		 * Optimisation for private mapping pages, that haven't
		 * yet being COW-ed
		 */
		if (vma_entry_is(vmae, VMA_FILE_PRIVATE) && (pme & PME_FILE))
			return vaddr + PAGE_SIZE;
		if ((pme & (PME_PRESENT | PME_SWAP)) && !__page_is_zero(pme)) {
			if (softdirty)
				*softdirty = pme & PME_SOFT_DIRTY;
			return vaddr;
		}

		return vaddr + PAGE_SIZE;
	}
}

bool page_is_zero(u64 pme)
{
	return __page_is_zero(pme);
}

bool page_in_parent(bool dirty)
{
	return __page_in_parent(dirty);
}

static bool is_stack(struct pstree_item *item, unsigned long vaddr)
{
	int i;

	for (i = 0; i < item->nr_threads; i++) {
		uint64_t sp = dmpi(item)->thread_sp[i];

		if (!((sp ^ vaddr) & ~PAGE_MASK))
			return true;
	}

	return false;
}

/*
 * This routine finds out what memory regions to grab from the
 * dumpee. The iovs generated are then fed into vmsplice to
 * put the memory into the page-pipe's pipe.
 *
 * "Holes" in page-pipe are regions, that should be dumped, but
 * the memory contents is present in the parent image set.
 */

static int generate_iovs(struct pstree_item *item, struct vma_area *vma, struct page_pipe *pp, pmc_t *pmc, u64 *pvaddr,
			 bool has_parent)
{
	unsigned long nr_scanned;
	unsigned long pages[3] = {};
	unsigned long vaddr;
	bool dump_all_pages;
	int ret = 0;

	dump_all_pages = should_dump_entire_vma(vma->e);

	nr_scanned = 0;
	for (vaddr = *pvaddr; vaddr < vma->e->end; vaddr += PAGE_SIZE, nr_scanned++) {
		unsigned int ppb_flags = 0;
		bool softdirty = false;
		u64 next;
		int st;

		/* If dump_all_pages is true, should_dump_page is called to get pme. */
		next = should_dump_page(pmc, vma->e, vaddr, &softdirty);
		if (!dump_all_pages && next != vaddr) {
			vaddr = next - PAGE_SIZE;
			continue;
		}
		// 判断这个页面是否可以进行延迟加载
		if (vma_entry_can_be_lazy(vma->e) && !is_stack(item, vaddr))
			ppb_flags |= PPB_LAZY;

		/*
		 * If we're doing incremental dump (parent images
		 * specified) and page is not soft-dirty -- we dump
		 * hole and expect the parent images to contain this
		 * page. The latter would be checked in page-xfer.
		 */

		if (has_parent && page_in_parent(softdirty)) {
			ret = page_pipe_add_hole(pp, vaddr, PP_HOLE_PARENT);
			st = 0;
		} else {
			ret = page_pipe_add_page(pp, vaddr, ppb_flags);
			if (ppb_flags & PPB_LAZY && opts.lazy_pages)
				st = 1;
			else
				st = 2;
		}

		if (ret) {
			/* Do not do pfn++, just bail out */
			pr_debug("Pagemap full\n");
			break;
		}

		pages[st]++;
	}

	*pvaddr = vaddr;
	cnt_add(CNT_PAGES_SCANNED, nr_scanned);
	cnt_add(CNT_PAGES_SKIPPED_PARENT, pages[0]);
	cnt_add(CNT_PAGES_LAZY, pages[1]);
	cnt_add(CNT_PAGES_WRITTEN, pages[2]);

	pr_info("Pagemap generated: %lu pages (%lu lazy) %lu holes\n", pages[2] + pages[1], pages[1], pages[0]);
	return ret;
}

static struct parasite_dump_pages_args *
prep_dump_pages_args(struct parasite_ctl *ctl, struct vm_area_list *vma_area_list, bool skip_non_trackable)
{
	struct parasite_dump_pages_args *args;
	struct parasite_vma_entry *p_vma;
	struct vma_area *vma;

	args = compel_parasite_args_s(ctl, dump_pages_args_size(vma_area_list));

	p_vma = pargs_vmas(args);
	args->nr_vmas = 0;

	list_for_each_entry(vma, &vma_area_list->h, list) {
		if (!vma_area_is_private(vma, kdat.task_size))
			continue;
		/*
		 * Kernel write to aio ring is not soft-dirty tracked,
		 * so we ignore them at pre-dump.
		 */
		if (vma_entry_is(vma->e, VMA_AREA_AIORING) && skip_non_trackable)
			continue;
		/*
		 * We totally ignore MAP_HUGETLB on pre-dump.
		 * See also generate_vma_iovs() comment.
		 */
		if ((vma->e->flags & MAP_HUGETLB) && skip_non_trackable)
			continue;
		if (vma->e->prot & PROT_READ)
			continue;

		p_vma->start = vma->e->start;
		p_vma->len = vma_area_len(vma);
		p_vma->prot = vma->e->prot;

		args->nr_vmas++;
		p_vma++;
	}

	return args;
}

static int drain_pages(struct page_pipe *pp, struct parasite_ctl *ctl, struct parasite_dump_pages_args *args)
{
	struct page_pipe_buf *ppb;
	int ret = 0;

	debug_show_page_pipe(pp);

	/* Step 2 -- grab pages into page-pipe */
	list_for_each_entry(ppb, &pp->bufs, l) {
		args->nr_segs = ppb->nr_segs;
		args->nr_pages = ppb->pages_in;
		pr_debug("PPB: %d pages %d segs %u pipe %d off\n", args->nr_pages, args->nr_segs, ppb->pipe_size,
			 args->off);

		ret = compel_rpc_call(PARASITE_CMD_DUMPPAGES, ctl);
		if (ret < 0)
			return -1;
		ret = compel_util_send_fd(ctl, ppb->p[1]);
		if (ret)
			return -1;

		ret = compel_rpc_sync(PARASITE_CMD_DUMPPAGES, ctl);
		if (ret < 0)
			return -1;

		args->off += args->nr_segs;
	}

	return 0;
}

static int xfer_pages(struct page_pipe *pp, struct page_xfer *xfer)
{
	int ret;

	/*
	 * Step 3 -- write pages into image (or delay writing for
	 *           pre-dump action (see pre_dump_one_task)
	 */
	timing_start(TIME_MEMWRITE);
	ret = page_xfer_dump_pages(xfer, pp);
	timing_stop(TIME_MEMWRITE);

	return ret;
}

static int detect_pid_reuse(struct pstree_item *item, struct proc_pid_stat *pps, InventoryEntry *parent_ie)
{
	unsigned long long dump_ticks;
	struct proc_pid_stat pps_buf;
	unsigned long long tps; /* ticks per second */
	int ret;

	/* Check pid reuse using pidfds */
	if (pidfd_store_ready())
		return pidfd_store_check_pid_reuse(item->pid->real);

	if (!parent_ie) {
		pr_err("Pid-reuse detection failed: no parent inventory, "
		       "check warnings in get_parent_inventory\n");
		return -1;
	}

	tps = sysconf(_SC_CLK_TCK);
	if (tps == -1) {
		pr_perror("Failed to get clock ticks via sysconf");
		return -1;
	}

	if (!pps) {
		pps = &pps_buf;
		ret = parse_pid_stat(item->pid->real, pps);
		if (ret < 0)
			return -1;
	}

	dump_ticks = parent_ie->dump_uptime / (USEC_PER_SEC / tps);

	if (pps->start_time >= dump_ticks) {
		/* Print "*" if unsure */
		pr_warn("Pid reuse%s detected for pid %d\n", pps->start_time == dump_ticks ? "*" : "", item->pid->real);
		return 1;
	}
	return 0;
}

static int generate_vma_iovs(struct pstree_item *item, struct vma_area *vma, struct page_pipe *pp,
			     struct page_xfer *xfer, struct parasite_dump_pages_args *args, struct parasite_ctl *ctl,
			     pmc_t *pmc, bool has_parent, bool pre_dump, int parent_predump_mode)
{
	u64 vaddr;
	int ret;

	if (!vma_area_is_private(vma, kdat.task_size) && !vma_area_is(vma, VMA_ANON_SHARED))
		return 0;
	/*
	 * In turn VVAR area is special and referenced from
	 * vDSO area by IP addressing (at least on x86) thus
	 * never ever dump its content but always use one provided
	 * by the kernel on restore, ie runtime VVAR area must
	 * be remapped into proper place..
	 */
	if (vma_entry_is(vma->e, VMA_AREA_VVAR))
		return 0;

	/*
	 * To facilitate any combination of pre-dump modes to run after
	 * one another, we need to take extra care as discussed below.
	 *
	 * The SPLICE mode pre-dump, processes all type of memory regions,
	 * whereas READ mode pre-dump skips processing those memory regions
	 * which lacks PROT_READ flag.
	 *
	 * Now on mixing pre-dump modes:
	 * 	If SPLICE mode follows SPLICE mode	: no issue
	 *		-> everything dumped both the times
	 *
	 * 	If READ mode follows READ mode		: no issue
	 *		-> non-PROT_READ skipped both the time
	 *
	 * 	If READ mode follows SPLICE mode   	: no issue
	 *		-> everything dumped at first,
	 *		   the non-PROT_READ skipped later
	 *
	 * 	If SPLICE mode follows READ mode   	: Need special care
	 *
	 * If READ pre-dump happens first, then it has skipped processing
	 * non-PROT_READ regions. Following SPLICE pre-dump expects pagemap
	 * entries for all mappings in parent pagemap, but last READ mode
	 * pre-dump cycle has skipped processing & pagemap generation for
	 * non-PROT_READ regions. So SPLICE mode throws error of missing
	 * pagemap entry for encountered non-PROT_READ mapping.
	 *
	 * To resolve this, the pre-dump-mode is stored in current pre-dump's
	 * inventoy file. This pre-dump mode is read back from this file
	 * (present in parent pre-dump dir) as parent-pre-dump-mode during
	 * next pre-dump.
	 *
	 * If parent-pre-dump-mode and next-pre-dump-mode are in READ-mode ->
	 * SPLICE-mode order, then SPLICE mode doesn't expect mappings for
	 * non-PROT_READ regions in parent-image and marks "has_parent=false".
	 */

	if (!(vma->e->prot & PROT_READ)) {
		if (opts.pre_dump_mode == PRE_DUMP_READ && pre_dump)
			return 0;
		if ((parent_predump_mode == PRE_DUMP_READ && opts.pre_dump_mode == PRE_DUMP_SPLICE) || !pre_dump)
			has_parent = false;
	}

	/*
	 * We want to completely ignore these VMA types on the pre-dump:
	 * 1. VMA_AREA_AIORING because it is not soft-dirty trackable (kernel writes)
	 * 2. MAP_HUGETLB mappings because they are not premapped and we can't use
	 * parent images from pre-dump stages. Instead, the content is restored from
	 * the parasite context using full memory image.
	 */
	if (vma_entry_is(vma->e, VMA_AREA_AIORING) || vma->e->flags & MAP_HUGETLB) {
		if (pre_dump)
			return 0;
		has_parent = false;
	}

	if (pmc_get_map(pmc, vma))
		return -1;

	if (vma_area_is(vma, VMA_ANON_SHARED))
		return add_shmem_area(item->pid->real, vma->e, pmc);
	vaddr = vma->e->start;
again:
	ret = generate_iovs(item, vma, pp, pmc, &vaddr, has_parent);
	if (ret == -EAGAIN) {
		BUG_ON(!(pp->flags & PP_CHUNK_MODE));

		ret = drain_pages(pp, ctl, args);
		if (!ret)
			ret = xfer_pages(pp, xfer);
		if (!ret) {
			page_pipe_reinit(pp);
			goto again;
		}
	}

	return ret;
}

static int __parasite_dump_pages_seized(struct pstree_item *item, struct parasite_dump_pages_args *args,
					struct vm_area_list *vma_area_list, struct mem_dump_ctl *mdc,
					struct parasite_ctl *ctl)
{
	pmc_t pmc = PMC_INIT;
	struct page_pipe *pp;
	struct vma_area *vma_area;
	struct page_xfer xfer = { .parent = NULL };
	int ret, exit_code = -1;
	unsigned cpp_flags = 0;
	unsigned long pmc_size;
	int possible_pid_reuse = 0;
	bool has_parent;
	int parent_predump_mode = -1;

	pr_info("\n");
	pr_info("Dumping pages (type: %d pid: %d)\n", CR_FD_PAGES, item->pid->real);
	pr_info("----------------------------------------\n");

	timing_start(TIME_MEMDUMP);

	pr_debug("   Private vmas %lu/%lu pages\n", vma_area_list->nr_priv_pages_longest, vma_area_list->nr_priv_pages);

	/*
	 * Step 0 -- prepare
	 */

	pmc_size = max(vma_area_list->nr_priv_pages_longest, vma_area_list->nr_shared_pages_longest);
	if (pmc_init(&pmc, item->pid->real, &vma_area_list->h, pmc_size * PAGE_SIZE))
		return -1;

	if (!(mdc->pre_dump || mdc->lazy))
		/*
		 * Chunk mode pushes pages portion by portion. This mode
		 * only works when we don't need to keep pp for later
		 * use, i.e. on non-lazy non-predump.
		 */
		cpp_flags |= PP_CHUNK_MODE;
	pp = create_page_pipe(vma_area_list->nr_priv_pages, mdc->lazy ? NULL : pargs_iovs(args), cpp_flags);
	if (!pp)
		goto out;

	if (!mdc->pre_dump) {
		/*
		 * Regular dump -- create xfer object and send pages to it
		 * right here. For pre-dumps the pp will be taken by the
		 * caller and handled later.
		 */
		ret = open_page_xfer(&xfer, CR_FD_PAGEMAP, vpid(item));
		if (ret < 0)
			goto out_pp;

		xfer.transfer_lazy = !mdc->lazy;
	} else {
		ret = check_parent_page_xfer(CR_FD_PAGEMAP, vpid(item));
		if (ret < 0)
			goto out_pp;

		if (ret)
			xfer.parent = NULL + 1;
	}

	if (xfer.parent) {
		possible_pid_reuse = detect_pid_reuse(item, mdc->stat, mdc->parent_ie);
		if (possible_pid_reuse == -1)
			goto out_xfer;
	}

	/*
	 * Step 1 -- generate the pagemap
	 */
	args->off = 0;
	has_parent = !!xfer.parent && !possible_pid_reuse;
	if (mdc->parent_ie)
		parent_predump_mode = mdc->parent_ie->pre_dump_mode;

	list_for_each_entry(vma_area, &vma_area_list->h, list) {
		ret = generate_vma_iovs(item, vma_area, pp, &xfer, args, ctl, &pmc, has_parent, mdc->pre_dump,
					parent_predump_mode);
		if (ret < 0)
			goto out_xfer;
	}

	if (mdc->lazy)
		memcpy(pargs_iovs(args), pp->iovs, sizeof(struct iovec) * pp->nr_iovs);

	/*
	 * Faking drain_pages for pre-dump here. Actual drain_pages for pre-dump
	 * will happen after task unfreezing in cr_pre_dump_finish(). This is
	 * actual optimization which reduces time for which process was frozen
	 * during pre-dump.
	 */
	if (mdc->pre_dump && opts.pre_dump_mode == PRE_DUMP_READ)
		ret = 0;
	else
		ret = drain_pages(pp, ctl, args);
	// 这里写page和pagemap到img,并且这里决定了pagemap中item的大小
	if (!ret && !mdc->pre_dump)
		ret = xfer_pages(pp, &xfer);
	if (ret)
		goto out_xfer;

	timing_stop(TIME_MEMDUMP);

	/*
	 * Step 4 -- clean up
	 */

	ret = task_reset_dirty_track(item->pid->real);
	if (ret)
		goto out_xfer;
	exit_code = 0;
out_xfer:
	if (!mdc->pre_dump)
		xfer.close(&xfer);
out_pp:
	if (ret || !(mdc->pre_dump || mdc->lazy))
		destroy_page_pipe(pp);
	else
		dmpi(item)->mem_pp = pp;
out:
	pmc_fini(&pmc);
	pr_info("----------------------------------------\n");
	return exit_code;
}

#ifdef RDMA_CODESIGN

static inline int need_to_dump(struct vma_area *vma){
	// 应该dump那些不能被lazy，又需要被dump的页面
	if (!vma_entry_can_be_lazy(vma->e) && !vma_entry_is(vma->e, VMA_AREA_VVAR)
		&& (vma_area_is_private(vma, kdat.task_size) || vma_area_is(vma, VMA_ANON_SHARED))
		&& !vma_entry_is(vma->e, VMA_AREA_VSYSCALL) && !vma_entry_is(vma->e, VMA_FILE_PRIVATE)){
		return 1;
	}else
		return 0;
}

__maybe_unused static int is_stack_vma(struct vma_area *vma, struct pstree_item *item){
	int i;

	for (i = 0; i < item->nr_threads; i++) {
		uint64_t sp = dmpi(item)->thread_sp[i];

		if (sp < vma->e->end && sp >= vma->e->start)
			return true;
	}

	return false;
}

int page_add_hole(struct iovec *iovs, int *off, u64 vaddr){
	return 0;
}

int page_add_page(struct iovec *iovs, int *off, u64 vaddr){
	int index;
	index = *off;
	if (iovs[index].iov_len + 4096 > 4194304){
		*off = *off + 1;
		index = *off;
		iovs[index].iov_base = (void *)vaddr;
		iovs[index].iov_len = 4096;
	} else{
		if ((unsigned long)iovs[index].iov_base + iovs[index].iov_len == vaddr){
			iovs[index].iov_len += 4096;
		} else{
			*off = *off + 1;
			index = *off;
			iovs[index].iov_base = (void *)vaddr;
			iovs[index].iov_len = 4096;
		}
	}
	return 0;
}

int RDMA_parasite_dump_pages_seized(struct pstree_item *item, struct vm_area_list *vma_area_list, struct mem_dump_ctl *mdc, struct parasite_ctl *ctl)
{
	int ret, index, num;
	void *mem, *tsmem_args;
	u32 flags;
	struct vma_area *vma;
	struct parasite_dump_pages_args *pargs;
	struct page_xfer xfer = { .parent = NULL };
	struct shmem_plugin_msg *args;
	struct iovec *iovs;

	u64 vaddr, pvaddr;
	unsigned long pages[3] = {};
	bool dump_all_pages;
	unsigned long nr_scanned;
	pmc_t pmc = PMC_INIT;
	unsigned long pmc_size;

	// step0: 初始化一个pmc
	pmc_size = max(vma_area_list->nr_priv_pages_longest, vma_area_list->nr_shared_pages_longest);
	if (pmc_init(&pmc, item->pid->real, &vma_area_list->h, pmc_size * PAGE_SIZE))
		return -1;

	pargs = prep_dump_pages_args(ctl, vma_area_list, mdc->pre_dump);
	// 需要生成pagemap,以vma级别来实现pagemap的生成
	// step1: 初始化一个xfer,在这个xfer里面会初始化write_pagemap函数 
	ret = open_page_xfer(&xfer, CR_FD_PAGEMAP, vpid(item));

	// 为内存添加可读属性，不然可能会导致parasite code在读取时报错
	if (!mdc->pre_dump || opts.pre_dump_mode == PRE_DUMP_SPLICE) {
		pargs->add_prot = PROT_READ;
		ret = compel_rpc_call_sync(PARASITE_CMD_MPROTECT_VMAS, ctl);
		if (ret) {
			pr_err("Can't dump unprotect vmas with parasite\n");
			return ret;
		}
	}

	iovs = (struct iovec *)malloc(sizeof(struct iovec) * vma_area_list->nr_priv_pages);
	memset(iovs, 0, sizeof(struct iovec) * vma_area_list->nr_priv_pages);
	num = 0;
	list_for_each_entry(vma, &vma_area_list->h, list) {
		if (!vma_area_is_private(vma, kdat.task_size) && !vma_area_is(vma, VMA_ANON_SHARED))
			continue;
		if (vma_entry_is(vma->e, VMA_AREA_VVAR))
			continue;

		// if (vma_entry_is(vma->e, VMA_AREA_AIORING) || vma->e->flags & MAP_HUGETLB)
		// 	has_parent = false;
		
		// 只有在vma为share类型或者不能lazy类型的才需要进行pmc_get_map。
		// 因为pmc_get_map操作对于大的vma区域时比较耗时。
		if (vma_area_is(vma, VMA_ANON_SHARED)){
			pmc_get_map(&pmc, vma);
			add_shmem_area(item->pid->real, vma->e, &pmc);
			continue;
		}

		pvaddr = vma->e->start;

		if (vma_entry_can_be_lazy(vma->e) && opts.lazy_pages){
			pages[1] += (vma->e->end - vma->e->start) / PAGE_SIZE;
			pr_warn("vma可以lazy: %lx~%lx\n", vma->e->start, vma->e->end);
		}
		else{
			dump_all_pages = should_dump_entire_vma(vma->e);
			for( vaddr = pvaddr; vaddr < vma->e->end; vaddr += PAGE_SIZE, nr_scanned++){
				bool softdirty = false;
				u64 next;

				pmc_get_map(&pmc, vma);
				// 如果不是需要dump全部page，并且这个页面也没有需要被dump，那么就跳过
				next = should_dump_page(&pmc, vma->e, vaddr, &softdirty);
				if (!dump_all_pages && next != vaddr){
					vaddr = next - PAGE_SIZE;
					continue;
				}
				// TODO:这里现不进行 hole 的判断，这里的hole仅仅是对不能进行lazy的页面处理提升不一定会大
				// 如果后续发现进程的pages文件很大，并且有不少的数据是继承自父进程，那么就可以考虑优化这个
				// if (has_parent && page_in_parent(softdirty)){
				// 	ret = page_add_hole(iovs, &index, vaddr);
				// 	pages[0]++;
				// } else {
				ret = page_add_page(iovs, &num, next);
				if (ret)
					pr_err("page add page iovs");
				if (opts.lazy_pages)
					pages[1]++;
				else
					pages[2]++;
				// }
			}
		}
	}

	// for(int i = 0; i < num + 1; i++){
	// 	pr_err("index:%d, iov_base:%lx, iov_len:%ld\n",i, (uint64_t)iovs[i].iov_base, iovs[i].iov_len);
	// }

	memcpy(pargs_iovs(pargs), iovs, sizeof(struct iovec) * (num + 1));
	index = 1;
	pargs->off = 1;
	list_for_each_entry(vma, &vma_area_list->h, list){
		if (vma_entry_can_be_lazy(vma->e) && opts.lazy_pages){
			struct iovec iov = {
				.iov_base = (void *)vma->e->start,
				.iov_len = vma->e->end - vma->e->start
			};
			flags = PE_LAZY;
			if (xfer.write_pagemap(&xfer, &iov, flags))
				pr_err("can not write pagemap\n");
		}else{
			flags = PE_PRESENT;
			// pr_err("index:%d, vma:start:%lx, end:%lx, iov_base:%lx, iov_len:%ld\n", index, vma->e->start, vma->e->end, (uint64_t)iovs[index].iov_base, (uint64_t)iovs[index].iov_len);
			for(index = 1; index <= num; index++){
				if(vma->e->start <= (uint64_t)iovs[index].iov_base && (uint64_t)iovs[index].iov_base < vma->e->end){
					int p[2];
					// int temp[2];
					// char *buf;
					
					if (xfer.write_pagemap(&xfer, &iovs[index], flags))
						pr_err("can not write pagemap\n");
					
					pargs->nr_segs = 1;
					pargs->nr_pages = iovs[index].iov_len / PAGE_SIZE;
									
					ret = pipe(p);
					ret = fcntl(p[0], F_SETPIPE_SZ, 1024 * PAGE_SIZE);
					if(ret < 0)
						pr_err("create pipe error\n");

					ret = compel_rpc_call(PARASITE_CMD_DUMPPAGES, ctl);
					if(ret < 0)
						return -1;
					ret = compel_util_send_fd(ctl, p[1]);
					if(ret < 0)
						return -1;
					ret = compel_rpc_sync(PARASITE_CMD_DUMPPAGES, ctl);
					if(ret < 0)
						return -1;

					// temp 输出页面数据
					// mutex_lock(&barriers->mutex1);
					// ret = pipe(temp);
					// ret = fcntl(temp[0], F_SETPIPE_SZ, 1024 * PAGE_SIZE);
					// buf = malloc(iovs[index].iov_len);
					// ret = read(p[0], buf, iovs[index].iov_len);
					// ret = write(temp[1], buf, iovs[index].iov_len);
					// for (int i = 0; i < iovs[index].iov_len; i += 64){
					// 	pr_warn("0x%lx 0x%lx 0x%lx 0x%lx 0x%lx 0x%lx 0x%lx 0x%lx\n",
					// 	(uint64_t)*(buf+i), (uint64_t)*(buf+i+8), (uint64_t)*(buf+i+16), (uint64_t)*(buf+i+24), (uint64_t)*(buf+i+32), (uint64_t)*(buf+i+40), (uint64_t)*(buf+i+48), (uint64_t)*(buf+i+56));
					// }
					// mutex_unlock(&barriers->mutex1);

					if (xfer.write_pages(&xfer, p[0], iovs[index].iov_len))
						pr_err("can not write page\n");
									
					pargs->off += 1;
				}
			}
		}
	}
	xfer.close(&xfer);
    if (opts.sb_kernel_transfer) {
        struct sbk_rdma_region *regions = calloc(SBK_MAX_REGIONS, sizeof(*regions));
        unsigned int count = 0;
        if (!regions) return -1;
        list_for_each_entry(vma, &vma_area_list->h, list) {
            if (!vma_entry_can_be_lazy(vma->e) || !opts.lazy_pages) continue;
            for (uint64_t address = vma->e->start; address < vma->e->end;) {
                uint64_t pages = (vma->e->end - address) / PAGE_SIZE;
                if (pages > (1ULL << 20)) pages = 1ULL << 20;
                if (count == SBK_MAX_REGIONS) {free(regions);return -1;}
                regions[count++] = (struct sbk_rdma_region){.address=address,.pages=pages};
                address += pages * PAGE_SIZE;
            }
        }
        ret = sb_kernel_register_final(ctl, item->pid->real, vpid(item), regions, count);
        free(regions);free(iovs);pmc_fini(&pmc);
        return ret;
    }

	// // step2: 生成简化的pagemap
	// // 创建iovs，用于复制到args参数的后面，到时候dumpee里面会读取这些iovs
	// iovs = (struct iovec *)malloc(sizeof(struct iovec) * pargs->nr_vmas);
	// index = 0;
	// list_for_each_entry(vma, &vma_area_list->h, list) {
	// 	if (need_to_dump(vma)){
	// 		// TODO: 这里需要判断vma是否是vvar, vvar的内存不需要判断
	// 		iovs[index].iov_base = (void *)vma->e->start;
	// 		iovs[index].iov_len = (vma->e->end - vma->e->start);
	// 		// pr_warn("iovs [%d] %lx %ld\n", index, (uint64_t)iovs[index].iov_base, iovs[index].iov_len);
	// 		index++;
	// 	}
	// }
	
	// memcpy(pargs_iovs(pargs), iovs, sizeof(struct iovec) * index);

	// index = 0;
	// iovs = pargs_iovs(pargs);
	// list_for_each_entry(vma, &vma_area_list->h, list) {
	// 	if (need_to_dump(vma)){
	// 		pr_warn("iovs [%d] %lx %ld\n", index, (uint64_t)iovs[index].iov_base, iovs[index].iov_len);
	// 		index++;
	// 	}
	// }

	// pargs->off = 0;
	// list_for_each_entry(vma, &vma_area_list->h, list) {
	// 	if(vma_area_is(vma, VMA_AREA_VSYSCALL))
	// 		continue;
	// 	if (vma_entry_can_be_lazy(vma->e))
	// 		flags = PE_LAZY;
	// 	else
	// 		flags = PE_PRESENT;
		
	// 	if (xfer.write_pagemap_vma(&xfer, vma, flags))
	// 		return -1;
		
	// 	// 好像只有需要写到page.img中的数据才需要调用write_pages方法
	// 	if((flags & PE_PRESENT)){
	// 		if (1){
	// 			int ret;
	// 			int p[2];

	// 			pargs->nr_segs = 1;
	// 			pargs->nr_pages = (vma->e->end - vma->e->start) / PAGE_SIZE;
				
	// 			ret = pipe(p);
	// 			ret = fcntl(p[0], F_SETPIPE_SZ, 1024 * PAGE_SIZE);
	// 			if(ret < 0)
	// 				pr_err("create pipe error\n");

	// 			mutex_lock(&barriers->mutex2);
	// 			ret = compel_rpc_call(PARASITE_CMD_DUMPPAGES, ctl);
	// 			if(ret < 0)
	// 				return -1;
	// 			ret = compel_util_send_fd(ctl, p[1]);
	// 			if(ret < 0)
	// 				return -1;
	// 			ret = compel_rpc_sync(PARASITE_CMD_DUMPPAGES, ctl);
	// 			if(ret < 0)
	// 				return -1;
	// 			mutex_unlock(&barriers->mutex2);

	// 			if (xfer.write_pages(&xfer, p[0], vma->e->end - vma->e->start))
	// 				return -1;
				
	// 			pargs->off += 1;
	// 		}
	// 	}
	// }
	// // 需要close xfer数据才能正常flush到img中
	// xfer.close(&xfer);PARASITE_CMD_CREATE_PREFETCH_SHREGION


	if (fault_injected(FI_DUMP_PAGES)) {
		pr_err("fault: Dump VMA pages failure!\n");
		return -1;
	}

	
	// ret = RDMA__parasite_dump_pages_seized(item, pargs, vma_area_list, mdc, ctl);
	// 这里需要的是receive shmem，先注册共享内存并且将这些共享内存存储到SharedRegions中。
	// 并且将这一块空间进行RDMA注册。使这些空间可以被读取

    mem = NULL;
    tsmem_args = compel_parasite_args_p(ctl);
    if (opts.sb_parallel_transfer) {
        uint64_t shared_address = 0;
        unsigned workers[SB_FAST_LANES] = {
            opts.sb_fault_workers ? opts.sb_fault_workers : 2,
            opts.sb_prefetch_workers ? opts.sb_prefetch_workers : 1
        };
        for (unsigned lane = 0; lane < SB_FAST_LANES; lane++) {
            if (workers[lane] > SB_FAST_WORKERS) return -1;
            for (unsigned worker = 0; worker < workers[lane]; worker++) {
                bool create = lane == 0 && worker == 0;
                *(uint64_t *)tsmem_args = create ? 0 : UINT64_MAX;
                *(uint64_t *)(tsmem_args + 32) = worker;
                *(uint64_t *)(tsmem_args + 48) = 1;
                *(uint64_t *)(tsmem_args + 56) = lane;
                ret = compel_rpc_call(PARASITE_CMD_CREATE_PAGEFAULT_SHREGION, ctl);
                if (ret) return ret;
                if (!create && send_shared_region(ctl, item->pid->real, shared_address, SHMEM_REGION_SIZE))
                    return -1;
                ret = compel_rpc_sync(PARASITE_CMD_CREATE_PAGEFAULT_SHREGION, ctl);
                if (ret) return ret;
                if (create) {
                    shared_address = *(uint64_t *)tsmem_args;
                    mem = sharemem_receive(ctl, SHMEM_REGION_SIZE, item->pid->real);
                    if (!mem) return -1;
                }
            }
        }
        pr_info("SB_TRANSFER fast_helpers source_pid=%d fault_workers=%u prefetch_workers=%u slots=%u\n",
                item->pid->real, workers[0], workers[1], SB_FAST_SLOTS);
    } else {
        *(uint64_t *)(tsmem_args + 48) = 0;
        ret = compel_rpc_call(PARASITE_CMD_CREATE_PAGEFAULT_SHREGION, ctl);
        if (ret) return ret;
        ret = compel_rpc_sync(PARASITE_CMD_CREATE_PAGEFAULT_SHREGION, ctl);
        if (ret) return ret;
        mem = sharemem_receive(ctl, SHMEM_REGION_SIZE, item->pid->real);
        if (!mem) return -1;
    }

    if (!mem) return -1;

	// TODO：做下面这段的时候出现了段错误，估计是访问了没有申请的内存。
	for ( int i = 0; i < item_num; i++){
		if ( pidset[i] == item->pid->real){
			SharedRegions->PIDs[i] = item->pid->real;
			SharedRegions->shregions[i] = (struct shregion_t *)mem;
		}
	}

	// // 下面进行RDMA的初始化
	// resources_init(&PF_res);
	// PF_res.config.dev_name = "mlx5_1";
	// PF_res.config.server_name = NULL;
	// PF_res.config.ib_port = 1;

	if (root_item != item)
		mutex_lock(&barriers->proc_parse);
	// resources_create(&PF_res, 1);
	/**
	 * 这里需要进行互斥，因为如果这里不搞互斥的话，就会导致多个dumpee进程同时读取/dev/shm/ts_mem文件。
	 * 这样就会导致竞争事件的产生。或许open系统调用并不能像libc里面的库一样可以线程安全的open，因此这
	 * 这里就报错，通过加锁实现互斥的读取共享存储文件。或者在dumpee进程中加锁，这个比较麻烦。
	 *  */ 
	tsmem_args = compel_parasite_args_p(ctl);
    /* One persistent copying helper per configured worker. Every helper maps
     * the same root-owned object through a host-opened fd, including additional
     * root helpers. Each consumes its arguments before acknowledging the RPC. */
    {
        unsigned workers = opts.sb_parallel_transfer ?
            (opts.sb_copy_workers ? opts.sb_copy_workers : 4) : 1;
        int source_index = -1;
        for (int i = 0; i < item_num; i++)
            if (pidset[i] == item->pid->real) { source_index = i; break; }
        if (opts.sb_parallel_transfer && source_index < 0) return -1;
        if (workers > SB_MAX_COPY_WORKERS) return -1;
        for (unsigned worker = 0; worker < workers; worker++) {
            bool create = item == root_item && worker == 0;
            *(uint64_t *)tsmem_args = create ? 0 : UINT64_MAX;
            *(uint64_t *)(tsmem_args + 8) = tsmem_addr;
            *(uint64_t *)(tsmem_args + 16) = tsmem_size;
            *(uint64_t *)(tsmem_args + 24) = item->pid->ns[0].virt;
            *(uint64_t *)(tsmem_args + 32) = worker;
            *(uint64_t *)(tsmem_args + 40) = workers;
            *(uint64_t *)(tsmem_args + 48) = opts.sb_parallel_transfer;
            *(uint64_t *)(tsmem_args + 56) = source_index;
            ret = compel_rpc_call(PARASITE_CMD_CREATE_TRANSFER_SHREGION, ctl);
            if (ret) return ret;
            if (!create && send_shared_region(ctl, root_item->pid->real, tsmem_addr, tsmem_size))
                return -1;
            ret = compel_rpc_sync(PARASITE_CMD_CREATE_TRANSFER_SHREGION, ctl);
            if (ret) return ret;
            if (create) {
                tsmem_addr = *(uint64_t *)tsmem_args;
                tsmem_size = *(uint64_t *)(tsmem_args + 8);
                TransferRegions = sharemem_receive(ctl, TRANSFER_REGION_SIZE, item->pid->real);
                if (!TransferRegions) return -1;
            }
        }
        pr_info("SB_TRANSFER helpers source_pid=%d copy_workers=%u\n", item->pid->real, workers);
    }


    if (!opts.sb_parallel_transfer) {

	if (item == root_item){
		*(uint64_t *)(tsmem_args) = 0;
		*(uint64_t *)(tsmem_args + 24) = item->pid->ns[0].virt;
	}else{
		*(uint64_t *)(tsmem_args) = UINT64_MAX; /* SCM_RIGHTS from CRIU */
		*(uint64_t *)(tsmem_args + 8) = ftmem_addr;
		*(uint64_t *)(tsmem_args + 16) = ftmem_size;
		*(uint64_t *)(tsmem_args + 24) = item->pid->ns[0].virt;
	}
	pr_debug("创建FT共享区域");
	// 接收共享存储
	ret = compel_rpc_call(PARASITE_CMD_CREATE_PREFETCH_SHREGION, ctl);
	if (ret) {
		pr_err("Can't create transfer shregion with parasite\n");
		return ret;
	}

	if (item != root_item && send_shared_region(ctl, root_item->pid->real, ftmem_addr, ftmem_size))
        return -1;
    ret = compel_rpc_sync(PARASITE_CMD_CREATE_PREFETCH_SHREGION, ctl);
	if (ret) {
		pr_err("Can't receive the transfer shared memory\n");
		return ret;
	}
	if (item == root_item){
		ftmem_addr = *(uint64_t *)(tsmem_args);
		ftmem_size = *(uint64_t *)(tsmem_args + 8);
		pr_warn("得到的FT地址为:%lx, 大小为:%lx\n", ftmem_addr, ftmem_size);
		PrefetchRegions = sharemem_receive(ctl, PREFETCH_REGION_SIZE, item->pid->real);
	}



    }

	mutex_unlock(&barriers->proc_parse);
	
	// 这里需要使用另一种方式注册共享内存。
	// ts_mem = sharemem_receive(ctl, TRANSFER_REGION_SIZE, item->pid->real);
	// 内存没有创建成功导致内存注册失败
	// ts_mem = sharemem_open("ts_mem", TRANSFER_REGION_SIZE);

	// resources_init(&TS_res);
	// TS_res.config.dev_name = "mlx5_1";
	// TS_res.config.server_name = NULL;
	// TS_res.config.ib_port = 1;
	// TS_res.buf = (void *)TransferRegions;

	// // TransferRegions = malloc(TRANSFER_REGION_SIZE);
	// resources_create_ts(&TS_res);
	// pr_warn("TS创建完毕\n");
	
	// QP的连接要在启动page server时才能进行，这里只能进行mermory region的注册
	// connect_qp(&res);

	// if (ret) {
	// 	pr_err("Can't receive the shared memory\n");
	// 	/* Parasite will unprotect VMAs after fail in fini() */
	// 	return ret;
	// }

	/* 这里由于内存还没有读取出来，因此需要保证内存数据的可读属性，因此下面的内存属性恢复要被注销。
		可以考虑当出错时，没有恢复完全的时候，或者在RDMA page server传输完数据之后，恢复内存属性。*/
	
	// if (!mdc->pre_dump || opts.pre_dump_mode == PRE_DUMP_SPLICE) {
	// 	pargs->add_prot = 0;
	// 	if (compel_rpc_call_sync(PARASITE_CMD_MPROTECT_VMAS, ctl)) {
	// 		pr_err("Can't rollback unprotected vmas with parasite\n");
	// 		ret = -1;
	// 	}
	// }

	return ret;
}


#endif

int parasite_dump_pages_seized(struct pstree_item *item, struct vm_area_list *vma_area_list, struct mem_dump_ctl *mdc,
			       struct parasite_ctl *ctl)
{
	int ret;
	struct parasite_dump_pages_args *pargs;

	pargs = prep_dump_pages_args(ctl, vma_area_list, mdc->pre_dump);

	/*
	 * Add PROT_READ protection for all VMAs we're about to
	 * dump if they don't have one. Otherwise we'll not be
	 * able to read the memory contents.
	 *
	 * Afterwards -- reprotect memory back.
	 *
	 * This step is required for "splice" mode pre-dump and dump.
	 * Skip this step for "read" mode pre-dump.
	 * "read" mode pre-dump delegates processing of non-PROT_READ
	 * regions to dump stage. Adding PROT_READ works fine for
	 * static processing (target process frozen during pre-dump)
	 * and fails for dynamic as explained below.
	 *
	 * Consider following sequence of instances to reason, why
	 * not to add PROT_READ in "read" mode pre-dump ?
	 *
	 *	CRIU- "read" pre-dump		    Target Process
	 *
	 *					1. Creates mapping M
	 *					   without PROT_READ
	 * 2. CRIU freezes target
	 *    process
	 * 3. Collect the mappings
	 * 4. Add PROT_READ to M
	 *    (non-PROT_READ region)
	 * 5. CRIU unfreezes target
	 *    process
	 *					6. Add flag PROT_READ
	 *					   to mapping M
	 *					7. Revoke flag PROT_READ
	 *					   from mapping M
	 * 8. process_vm_readv tries
	 *    to copy mapping M
	 *    (believing M have
	 *     PROT_READ flag)
	 * 9. syscall fails to copy
	 *    data from M
	 */

	if (!mdc->pre_dump || opts.pre_dump_mode == PRE_DUMP_SPLICE) {
		pargs->add_prot = PROT_READ;
		ret = compel_rpc_call_sync(PARASITE_CMD_MPROTECT_VMAS, ctl);
		if (ret) {
			pr_err("Can't dump unprotect vmas with parasite\n");
			return ret;
		}
	}

	if (fault_injected(FI_DUMP_PAGES)) {
		pr_err("fault: Dump VMA pages failure!\n");
		return -1;
	}

	ret = __parasite_dump_pages_seized(item, pargs, vma_area_list, mdc, ctl);
	if (ret) {
		pr_err("Can't dump page with parasite\n");
		/* Parasite will unprotect VMAs after fail in fini() */
		return ret;
	}
	// 将内存数据导出之后，就可以恢复内存的属性，原来可能是不可读的
	if (!mdc->pre_dump || opts.pre_dump_mode == PRE_DUMP_SPLICE) {
		pargs->add_prot = 0;
		if (compel_rpc_call_sync(PARASITE_CMD_MPROTECT_VMAS, ctl)) {
			pr_err("Can't rollback unprotected vmas with parasite\n");
			ret = -1;
		}
	}

	return ret;
}

int prepare_mm_pid(struct pstree_item *i)
{
	pid_t pid = vpid(i);
	int ret = -1, vn = 0;
	struct cr_img *img;
	struct rst_info *ri = rsti(i);

	img = open_image(CR_FD_MM, O_RSTR, pid);
	if (!img)
		return -1;

	ret = pb_read_one_eof(img, &ri->mm, PB_MM);
	close_image(img);
	if (ret <= 0)
		return ret;

	if (collect_special_file(ri->mm->exe_file_id) == NULL)
		return -1;

	pr_debug("Found %zd VMAs in image\n", ri->mm->n_vmas);
	img = NULL;
	if (ri->mm->n_vmas == 0) {
		/*
		 * Old image. Read VMAs from vma-.img
		 */
		img = open_image(CR_FD_VMAS, O_RSTR, pid);
		if (!img)
			return -1;
	}

	while (vn < ri->mm->n_vmas || img != NULL) {
		struct vma_area *vma;

		ret = -1;
		vma = alloc_vma_area();
		if (!vma)
			break;

		ri->vmas.nr++;
		if (!img)
			vma->e = ri->mm->vmas[vn++];
		else {
			ret = pb_read_one_eof(img, &vma->e, PB_VMA);
			if (ret <= 0) {
				xfree(vma);
				close_image(img);
				img = NULL;
				break;
			}
		}
		list_add_tail(&vma->list, &ri->vmas.h);

		if (vma_area_is_private(vma, kdat.task_size)) {
			ri->vmas.rst_priv_size += vma_area_len(vma);
			if (vma_has_guard_gap_hidden(vma))
				ri->vmas.rst_priv_size += PAGE_SIZE;
			if (vma_area_is(vma, VMA_AREA_SHSTK))
				ri->vmas.rst_priv_size += PAGE_SIZE;
		}

		pr_info("vma 0x%" PRIx64 " 0x%" PRIx64 "\n", vma->e->start, vma->e->end);

		if (vma_area_is(vma, VMA_ANON_SHARED))
			ret = collect_shmem(pid, vma);
		else if (vma_area_is(vma, VMA_FILE_PRIVATE) || vma_area_is(vma, VMA_FILE_SHARED))
			ret = collect_filemap(vma);
		else if (vma_area_is(vma, VMA_AREA_SOCKET))
			ret = collect_socket_map(vma);
		else
			ret = 0;
		if (ret)
			break;
	}

	if (img)
		close_image(img);
	return ret;
}

static inline bool check_cow_vmas(struct vma_area *vma, struct vma_area *pvma)
{
	/*
	 * VMAs that _may_[1] have COW-ed pages should ...
	 *
	 * [1] I say "may" because whether or not particular pages are
	 * COW-ed is determined later in restore_priv_vma_content() by
	 * memcmp'aring the contents.
	 */

	/* ... coincide by start/stop pair (start is checked by caller) */
	if (vma->e->end != pvma->e->end)
		return false;
	/* ... both be private (and thus have space in premmaped area) */
	if (!vma_area_is_private(vma, kdat.task_size))
		return false;
	if (!vma_area_is_private(pvma, kdat.task_size))
		return false;
	/* ... but not hugetlb mappings */
	if (vma->e->flags & MAP_HUGETLB || pvma->e->flags & MAP_HUGETLB)
		return false;
	/* ... have growsdown and anon flags coincide */
	if ((vma->e->flags ^ pvma->e->flags) & (MAP_GROWSDOWN | MAP_ANONYMOUS))
		return false;
	/* ... belong to the same file if being filemap */
	if (!(vma->e->flags & MAP_ANONYMOUS) && vma->e->shmid != pvma->e->shmid)
		return false;

	pr_debug("Found two COW VMAs @0x%" PRIx64 "-0x%" PRIx64 "\n", vma->e->start, pvma->e->end);
	return true;
}

static inline bool vma_inherited(struct vma_area *vma)
{
	return (vma->pvma != NULL && vma->pvma != VMA_COW_ROOT);
}

static void prepare_cow_vmas_for(struct vm_area_list *vmas, struct vm_area_list *pvmas)
{
	struct vma_area *vma, *pvma;

	vma = list_first_entry(&vmas->h, struct vma_area, list);
	pvma = list_first_entry(&pvmas->h, struct vma_area, list);

	while (1) {
		if ((vma->e->start == pvma->e->start) && check_cow_vmas(vma, pvma)) {
			vma->pvma = pvma;
			if (pvma->pvma == NULL)
				pvma->pvma = VMA_COW_ROOT;
		}

		/* <= here to shift from matching VMAs and ... */
		while (vma->e->start <= pvma->e->start) {
			vma = vma_next(vma);
			if (&vma->list == &vmas->h)
				return;
		}

		/* ... no == here since we must stop on matching pair */
		while (pvma->e->start < vma->e->start) {
			pvma = vma_next(pvma);
			if (&pvma->list == &pvmas->h)
				return;
		}
	}
}

void prepare_cow_vmas(void)
{
	struct pstree_item *pi;

	for_each_pstree_item(pi) {
		struct pstree_item *ppi;
		struct vm_area_list *vmas, *pvmas;

		ppi = pi->parent;
		if (!ppi)
			continue;

		vmas = &rsti(pi)->vmas;
		if (vmas->nr == 0) /* Zombie */
			continue;

		pvmas = &rsti(ppi)->vmas;
		if (pvmas->nr == 0) /* zombies cannot have kids,
				     * but helpers can (and do) */
			continue;

		if (rsti(pi)->mm->exe_file_id != rsti(ppi)->mm->exe_file_id)
			/*
			 * Tasks running different executables have
			 * close to zero chance of having cow-ed areas
			 * and actually kernel never creates such.
			 */
			continue;

		prepare_cow_vmas_for(vmas, pvmas);
	}
}

/* Map a private vma, if it is not mapped by a parent yet */
static int premap_private_vma(struct pstree_item *t, struct vma_area *vma, void **tgt_addr)
{
	int ret;
	void *addr;
	unsigned long nr_pages, size;

	nr_pages = vma_entry_len(vma->e) / PAGE_SIZE;
	vma->page_bitmap = xzalloc(BITS_TO_LONGS(nr_pages) * sizeof(long));
	if (vma->page_bitmap == NULL)
		return -1;

	/*
	 * A grow-down VMA has a guard page, which protect a VMA below it.
	 * So one more page is mapped here to restore content of the first page
	 */
	if (vma_has_guard_gap_hidden(vma))
		vma->e->start -= PAGE_SIZE;

	size = vma_entry_len(vma->e);

	/*
	 * map an extra page for shadow stack VMAs, it will be used as a
	 * temporary shadow stack
	 */
	if (vma_area_is(vma, VMA_AREA_SHSTK))
		size += PAGE_SIZE;

	if (!vma_inherited(vma)) {
		int flag = 0;
		/*
		 * The respective memory area was NOT found in the parent.
		 * Map a new one.
		 */

		/*
		 * Restore AIO ring buffer content to temporary anonymous area.
		 * This will be placed in io_setup'ed AIO in restore_aio_ring().
		 */
		if (vma_entry_is(vma->e, VMA_AREA_AIORING))
			flag |= MAP_ANONYMOUS;
		else if (vma_area_is(vma, VMA_FILE_PRIVATE)) {
			ret = vma->vm_open(vpid(t), vma);
			if (ret < 0) {
				pr_err("Can't fixup VMA's fd\n");
				return -1;
			}
		}

		/*
		 * All mappings here get PROT_WRITE regardless of whether we
		 * put any data into it or not, because this area will get
		 * mremap()-ed (branch below) so we MIGHT need to have WRITE
		 * bits there. Ideally we'd check for the whole COW-chain
		 * having any data in.
		 */
		addr = mmap(*tgt_addr, size, vma->e->prot | PROT_WRITE, vma->e->flags | MAP_FIXED | flag, vma->e->fd,
			    vma->e->pgoff);

		if (addr == MAP_FAILED) {
			pr_perror("Unable to map ANON_VMA");
			return -1;
		}
	} else {
		void *paddr;

		/*
		 * The area in question can be COWed with the parent. Remap the
		 * parent area. Note, that it has already being passed through
		 * the restore_priv_vma_content() call and thus may have some
		 * pages in it.
		 */

		paddr = decode_pointer(vma->pvma->premmaped_addr);
		if (vma_has_guard_gap_hidden(vma))
			paddr -= PAGE_SIZE;

		addr = mremap(paddr, size, size, MREMAP_FIXED | MREMAP_MAYMOVE, *tgt_addr);
		if (addr != *tgt_addr) {
			pr_perror("Unable to remap a private vma");
			return -1;
		}
	}

	vma->e->status |= VMA_PREMMAPED;
	vma->premmaped_addr = (unsigned long)addr;
	pr_debug("\tpremap %#016" PRIx64 "-%#016" PRIx64 " -> %016lx\n", vma->e->start, vma->e->end,
		 (unsigned long)addr);

	if (vma_has_guard_gap_hidden(vma)) { /* Skip guard page */
		vma->e->start += PAGE_SIZE;
		vma->premmaped_addr += PAGE_SIZE;
	}

	if (vma_area_is(vma, VMA_FILE_PRIVATE))
		vma->vm_open = NULL; /* prevent from 2nd open in prepare_vmas */

	*tgt_addr += size;
	return 0;
}

static inline bool vma_force_premap(struct vma_area *vma, struct list_head *head)
{
	/*
	 * Shadow stack VMAs cannot be mmap()ed, they must be created using
	 * map_shadow_stack() system call.
	 * Premap them to reserve virtual address space and populate them
	 * to have there contents available for later copying.
	 */
	if (vma_area_is(vma, VMA_AREA_SHSTK))
		return true;

	/*
	 * On kernels with 4K guard pages, growsdown VMAs
	 * always have one guard page at the
	 * beginning and sometimes this page contains data.
	 * In case the VMA is premmaped, we premmap one page
	 * larger VMA. In case of in place restore we can only
	 * do this if the VMA in question is not "guarded" by
	 * some other VMA.
	 */
	if (vma->e->flags & MAP_GROWSDOWN) {
		if (vma->list.prev != head) {
			struct vma_area *prev;

			prev = list_entry(vma->list.prev, struct vma_area, list);
			if (prev->e->end == vma->e->start) {
				pr_debug("Force premmap for 0x%" PRIx64 ":0x%" PRIx64 "\n", vma->e->start, vma->e->end);
				return true;
			}
		}
	}

	return false;
}

/*
 * Ensure for s390x that vma is below task size on restore system
 */
static int task_size_check(pid_t pid, VmaEntry *entry)
{
#ifdef __s390x__
	if (entry->end <= kdat.task_size)
		return 0;
	pr_err("Can't restore high memory region %lx-%lx because kernel does only support vmas up to %lx\n",
	       entry->start, entry->end, kdat.task_size);
	return -1;
#else
	return 0;
#endif
}

static int premap_priv_vmas(struct pstree_item *t, struct vm_area_list *vmas, void **at, struct page_read *pr)
{
	struct vma_area *vma;
	unsigned long pstart = 0;
	int ret = 0;
	LIST_HEAD(empty);

	filemap_ctx_init(true);

	list_for_each_entry(vma, &vmas->h, list) {
		if (task_size_check(vpid(t), vma->e)) {
			ret = -1;
			break;
		}
		if (pstart > vma->e->start) {
			ret = -1;
			pr_err("VMA-s are not sorted in the image file\n");
			break;
		}
		pstart = vma->e->start;

		if (!vma_area_is_private(vma, kdat.task_size))
			continue;

		if (vma->e->flags & MAP_HUGETLB)
			continue;

		/* VMA offset may change due to plugin so we cannot premap */
		if (vma->e->status & VMA_EXT_PLUGIN)
			continue;

		if (vma->pvma == NULL && pr->pieok && !vma_force_premap(vma, &vmas->h)) {
			/*
			 * VMA in question is not shared with anyone. We'll
			 * restore it with its contents in restorer.
			 * Now let's check whether we need to map it with
			 * PROT_WRITE or not.
			 */
			do {
				if (pr->pe->vaddr + pr->pe->nr_pages * PAGE_SIZE <= vma->e->start)
					continue;
				if (pr->pe->vaddr >= vma->e->end)
					vma->e->status |= VMA_NO_PROT_WRITE;
				break;
			} while (pr->advance(pr));

			continue;
		}

		ret = premap_private_vma(t, vma, at);

		if (ret < 0)
			break;
	}

	filemap_ctx_fini();

	return ret;
}

static int restore_priv_vma_content(struct pstree_item *t, struct page_read *pr)
{
	struct vma_area *vma;
	int ret = 0;
	struct list_head *vmas = &rsti(t)->vmas.h;
	struct list_head *vma_io = &rsti(t)->vma_io;

	unsigned int nr_restored = 0;
	unsigned int nr_shared = 0;
	unsigned int nr_dropped = 0;
	unsigned int nr_compared = 0;
	unsigned int nr_enqueued = 0;
	unsigned int nr_lazy = 0;
	unsigned long va;

	vma = list_first_entry(vmas, struct vma_area, list);
	rsti(t)->pages_img_id = pr->pages_img_id;

	/*
	 * Read page contents.
	 */
	while (1) {
		unsigned long off, i, nr_pages;

		ret = pr->advance(pr);
		if (ret <= 0)
			break;

		va = (unsigned long)decode_pointer(pr->pe->vaddr);
		nr_pages = pr->pe->nr_pages;

		/*
		 * This means that userfaultfd is used to load the pages
		 * on demand.
		 */
		// 这里可以实现按需加载内存,因此restore的速度比较快
		if (opts.lazy_pages && pagemap_lazy(pr->pe)) {
			pr_debug("Lazy restore skips %ld pages at %lx\n", nr_pages, va);
			pr->skip_pages(pr, nr_pages * PAGE_SIZE);
			nr_lazy += nr_pages;
			continue;
		}

		for (i = 0; i < nr_pages; i++) {
			unsigned char buf[PAGE_SIZE];
			void *p;

			/*
			 * The lookup is over *all* possible VMAs
			 * read from image file.
			 */
			while (va >= vma->e->end) {
				if (vma->list.next == vmas)
					goto err_addr;
				vma = vma_next(vma);
			}

			/*
			 * Make sure the page address is inside existing VMA
			 * and the VMA it refers to still private one, since
			 * there is no guarantee that the data from pagemap is
			 * valid.
			 */
			if (va < vma->e->start)
				goto err_addr;
			else if (unlikely(!vma_area_is_private(vma, kdat.task_size))) {
				pr_err("Trying to restore page for non-private VMA\n");
				goto err_addr;
			}

			if (!vma_area_is(vma, VMA_PREMMAPED)) {
				unsigned long len = min_t(unsigned long, (nr_pages - i) * PAGE_SIZE, vma->e->end - va);

				if (vma->e->status & VMA_NO_PROT_WRITE) {
					pr_debug("VMA 0x%" PRIx64 ":0x%" PRIx64 " RO %#lx:%lu IO\n", vma->e->start,
						 vma->e->end, va, nr_pages);
					BUG();
				}

				if (pagemap_enqueue_iovec(pr, (void *)va, len, vma_io))
					return -1;

				pr->skip_pages(pr, len);

				va += len;
				len >>= PAGE_SHIFT;
				nr_restored += len;
				i += len - 1;

				nr_enqueued++;
				continue;
			}

			/*
			 * Otherwise to the COW restore
			 */
			// p是当前进程的处理的内存页起始地址
			off = (va - vma->e->start) / PAGE_SIZE;
			p = decode_pointer((off)*PAGE_SIZE + vma->premmaped_addr);

			set_bit(off, vma->page_bitmap);
			// 判断vma是否为继承的
			if (vma_inherited(vma)) {
				clear_bit(off, vma->pvma->page_bitmap);

				ret = pr->read_pages(pr, va, 1, buf, 0);
				if (ret < 0)
					goto err_read;

				va += PAGE_SIZE;
				nr_compared++;
				// 判断是不是父进程中已经有的数据
				if (memcmp(p, buf, PAGE_SIZE) == 0) {
					nr_shared++; /* the page is cowed */
					continue;
				}

				nr_restored++;
				memcpy(p, buf, PAGE_SIZE);
			} else {
				int nr;

				/*
				 * Try to read as many pages as possible at once.
				 *
				 * Within the t pagemap we still have
				 * nr_pages - i pages (not all, as we might have
				 * switched VMA above), within the t VMA
				 * we have at most (vma->end - t_addr) bytes.
				 */

				nr = min_t(int, nr_pages - i, (vma->e->end - va) / PAGE_SIZE);
				ret = pr->read_pages(pr, va, nr, p, PR_ASYNC);
				if (ret < 0)
					goto err_read;

				va += nr * PAGE_SIZE;
				nr_restored += nr;
				i += nr - 1;
				bitmap_set(vma->page_bitmap, off + 1, nr - 1);
			}
		}
	}

err_read:
	// 正常执行完上面的循环，执行到这里。下面这个函数会卡进死循环
	if (pr->sync(pr))
		return -1;

	pr->close(pr);
	if (ret < 0)
		return ret;

	/* Remove pages, which were not shared with a child */
	list_for_each_entry(vma, vmas, list) {
		unsigned long size, i = 0;
		void *addr = decode_pointer(vma->premmaped_addr);

		if (!vma_inherited(vma))
			continue;

		size = vma_entry_len(vma->e) / PAGE_SIZE;
		while (1) {
			/* Find all pages, which are not shared with this child */
			i = find_next_bit(vma->pvma->page_bitmap, size, i);

			if (i >= size)
				break;

			ret = madvise(addr + PAGE_SIZE * i, PAGE_SIZE, MADV_DONTNEED);
			if (ret < 0) {
				pr_perror("madvise failed");
				return -1;
			}
			i++;
			nr_dropped++;
		}
	}

	cnt_add(CNT_PAGES_COMPARED, nr_compared);
	cnt_add(CNT_PAGES_SKIPPED_COW, nr_shared);
	cnt_add(CNT_PAGES_RESTORED, nr_restored);

	pr_info("nr_restored_pages: %d\n", nr_restored);
	pr_info("nr_shared_pages:   %d\n", nr_shared);
	pr_info("nr_dropped_pages:  %d\n", nr_dropped);
	pr_info("nr_enqueued:       %d\n", nr_enqueued);
	pr_info("nr_lazy:           %d\n", nr_lazy);

	return 0;

err_addr:
	pr_err("Page entry address %lx outside of VMA %lx-%lx\n", va, (long)vma->e->start, (long)vma->e->end);
	return -1;
}

static int maybe_disable_thp(struct pstree_item *t, struct page_read *pr)
{
	/*
	 * There is no need to disable it if the page read doesn't
	 * have parent. In this case VMA will be empty until
	 * userfaultfd_register, so there would be no pages to
	 * collapse. And, once we register the VMA with uffd,
	 * khugepaged will skip it.
	 */
	if (!(opts.lazy_pages && page_read_has_parent(pr)))
		return 0;

	if (!kdat.has_thp_disable)
		pr_warn("Disabling transparent huge pages. "
			"It may affect performance!\n");

	/*
	 * temporarily disable THP to avoid collapse of pages
	 * in the areas that will be monitored by uffd
	 */
	if (prctl(PR_SET_THP_DISABLE, 1, 0, 0, 0)) {
		pr_perror("Cannot disable THP");
		return -1;
	}

	return 0;
}

int prepare_private_fork(struct pstree_item *t)
{
	struct vma_area *vma;
	unsigned long bytes = 0, count = 0, cow_bytes = 0;
	if (!opts.sb_parent_stage || list_empty(&t->children))
		return 0;
	list_for_each_entry(vma, &rsti(t)->vmas.h, list) {
		unsigned long flags = vma->e->flags;
		if (vma->pvma && vma_area_is(vma, VMA_PREMMAPED))
			cow_bytes += vma_entry_len(vma->e);
		/* A non-NULL pvma includes both inherited mappings and COW roots.
		 * Keep all of them: descendants may need the restored parent bytes.
		 * Exclude only ordinary anonymous premap VMAs with no COW links. */
		if (vma->pvma || !vma_area_is(vma, VMA_PREMMAPED) ||
		    (flags & (MAP_PRIVATE | MAP_ANONYMOUS)) != (MAP_PRIVATE | MAP_ANONYMOUS) ||
		    (flags & ~(MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED | MAP_NORESERVE)) ||
		    vma_area_is(vma, VMA_AREA_SHSTK))
			continue;
		if (sb_fork_wipe_private(decode_pointer(vma->premmaped_addr), vma_entry_len(vma->e))) {
			pr_perror("Prepare temporary mapping inheritance");
			sb_fork_restore_inheritance();
			return -1;
		}
		bytes += vma_entry_len(vma->e);
		count++;
	}
	pr_info("SB_FORK private_unneeded pid=%d regions=%lu bytes=%lu cow_preserved_bytes=%lu\n",
		vpid(t), count, bytes, cow_bytes);
	return 0;
}

int prepare_mappings(struct pstree_item *t)
{
	int ret = 0;
	void *addr;
	struct vm_area_list *vmas;
	struct page_read pr;

	void *old_premmapped_addr = NULL;
	unsigned long old_premmapped_len;

	vmas = &rsti(t)->vmas;
	if (vmas->nr == 0) /* Zombie */
		goto out;

	// 申请一大块空间用于将所有vma的数据放到这个里面,但是vma的类型有三种,因此使用rst-malloc.h来管理这个内存
	// 然后使用pie在目标进程恢复时将所有的原来vma mmap,再将这些premap里的数据移动到正确位置
	/* Reserve a place for mapping private vma-s one by one */
	addr = mmap(NULL, vmas->rst_priv_size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, 0, 0);
	if (addr == MAP_FAILED) {
		ret = -1;
		pr_perror("Unable to reserve memory (%lu bytes)", vmas->rst_priv_size);
		goto out;
	}

	old_premmapped_addr = rsti(t)->premmapped_addr;
	old_premmapped_len = rsti(t)->premmapped_len;
	rsti(t)->premmapped_addr = addr;
	rsti(t)->premmapped_len = vmas->rst_priv_size;

	ret = open_page_read(vpid(t), &pr, PR_TASK);
	if (ret <= 0)
		return -1;

	if (maybe_disable_thp(t, &pr))
		return -1;

	pr.advance(&pr); /* shift to the 1st iovec */

	ret = premap_priv_vmas(t, vmas, &addr, &pr);
	if (ret < 0)
		goto out;

	pr.reset(&pr);

	if (opts.sb_parent_stage) {
		struct vma_area *vma;
		list_for_each_entry(vma, &vmas->h, list) {
			if (!vma_area_is(vma, VMA_PREMMAPED) || !vma_entry_can_be_lazy(vma->e)) continue;
			ret = sb_stage_adopt(vpid(t), vma->e->start, vma->e->end,
				decode_pointer(vma->premmaped_addr), vma->e->flags, vma->e->prot,
				vma->page_bitmap, vma_inherited(vma) ? vma->pvma->page_bitmap : NULL);
			if (ret < 0) goto out;
		}
	}
	ret = restore_priv_vma_content(t, &pr);
	if (ret < 0)
		goto out;

	if (old_premmapped_addr) {
		ret = munmap(old_premmapped_addr, old_premmapped_len);
		if (ret < 0)
			pr_perror("Unable to unmap %p(%lx)", old_premmapped_addr, old_premmapped_len);
	}

	/*
	 * Not all VMAs were premmaped. Find out the unused tail of the
	 * premapped area and unmap it.
	 */
	old_premmapped_len = addr - rsti(t)->premmapped_addr;
	if (old_premmapped_len < rsti(t)->premmapped_len) {
		unsigned long tail;

		tail = rsti(t)->premmapped_len - old_premmapped_len;
		ret = munmap(addr, tail);
		if (ret < 0)
			pr_perror("Unable to unmap %p(%lx)", addr, tail);
		rsti(t)->premmapped_len = old_premmapped_len;
		pr_info("Shrunk premap area to %p(%lx)\n", rsti(t)->premmapped_addr, rsti(t)->premmapped_len);
	}

out:
	return ret;
}

bool vma_has_guard_gap_hidden(struct vma_area *vma)
{
	return kdat.stack_guard_gap_hidden && (vma->e->flags & MAP_GROWSDOWN);
}

/*
 * A guard page must be unmapped after restoring content and
 * forking children to restore COW memory.
 */
int unmap_guard_pages(struct pstree_item *t)
{
	struct vma_area *vma;
	struct list_head *vmas = &rsti(t)->vmas.h;

	if (!kdat.stack_guard_gap_hidden)
		return 0;

	list_for_each_entry(vma, vmas, list) {
		if (!vma_area_is(vma, VMA_PREMMAPED))
			continue;

		if (vma->e->flags & MAP_GROWSDOWN) {
			void *addr = decode_pointer(vma->premmaped_addr);

			if (munmap(addr - PAGE_SIZE, PAGE_SIZE)) {
				pr_perror("Can't unmap guard page");
				return -1;
			}
		}
	}

	return 0;
}

int open_vmas(struct pstree_item *t)
{
	int pid = vpid(t);
	struct vma_area *vma;
	struct vm_area_list *vmas = &rsti(t)->vmas;

	filemap_ctx_init(false);

	list_for_each_entry(vma, &vmas->h, list) {
		if (!vma_area_is(vma, VMA_AREA_REGULAR) || !vma->vm_open)
			continue;

		pr_info("Opening %#016" PRIx64 "-%#016" PRIx64 " %#016" PRIx64 " (%x) vma\n", vma->e->start,
			vma->e->end, vma->e->pgoff, vma->e->status);

		if (vma->vm_open(pid, vma)) {
			pr_err("`- Can't open vma\n");
			return -1;
		}

		/*
		 * File mappings have vm_open set to open_filemap which, in
		 * turn, puts the VMA_CLOSE bit itself. For all the rest we
		 * need to put it by hands, so that the restorer closes the fd
		 */
		if (!(vma_area_is(vma, VMA_FILE_PRIVATE) || vma_area_is(vma, VMA_FILE_SHARED)))
			vma->e->status |= VMA_CLOSE;
	}

	filemap_ctx_fini();

	return 0;
}

static int prepare_vma_ios(struct pstree_item *t, struct task_restore_args *ta)
{
	struct cr_img *pages;

	/*
	 * We optimize the case when rsti(t)->vma_io is empty.
	 *
	 * This is useful when using the image streamer, where all VMAs are
	 * premapped (pr->pieok is false). This avoids re-opening the
	 * CR_FD_PAGES file, which may only be readable only once.
	 */
	if (list_empty(&rsti(t)->vma_io)) {
		ta->vma_ios = NULL;
		ta->vma_ios_n = 0;
		ta->vma_ios_fd = -1;
		return 0;
	}

	/*
	 * If auto-dedup is on we need RDWR mode to be able to punch holes in
	 * the input files (in restorer.c)
	 */
	pages = open_image(CR_FD_PAGES, opts.auto_dedup ? O_RDWR : O_RSTR, rsti(t)->pages_img_id);
	if (!pages)
		return -1;

	ta->vma_ios_fd = img_raw_fd(pages);
	return pagemap_render_iovec(&rsti(t)->vma_io, ta);
}

int prepare_vmas(struct pstree_item *t, struct task_restore_args *ta)
{
	struct vma_area *vma;
	struct vm_area_list *vmas = &rsti(t)->vmas;

	ta->vmas = (VmaEntry *)rst_mem_align_cpos(RM_PRIVATE);
	ta->vmas_n = vmas->nr;

	list_for_each_entry(vma, &vmas->h, list) {
		VmaEntry *vme;

		vme = rst_mem_alloc(sizeof(*vme), RM_PRIVATE);
		if (!vme)
			return -1;

		/*
		 * Copy VMAs to private rst memory so that it's able to
		 * walk them and m(un|re)map.
		 */
		*vme = *vma->e;

		if (vma_area_is(vma, VMA_PREMMAPED))
			vma_premmaped_start(vme) = vma->premmaped_addr;
	}

	return prepare_vma_ios(t, ta);
}

#ifdef RDMA_CODESIGN
int clean_mprotect(struct parasite_ctl *parasite_ctl, struct vm_area_list *vmas){
	int ret = 0;
	struct parasite_dump_pages_args *pargs;
	pargs = prep_dump_pages_args(parasite_ctl, vmas, true);
	// 将内存的保护权限释放掉
	if (opts.pre_dump_mode == PRE_DUMP_SPLICE) {
		pargs->add_prot = 0;
		if (compel_rpc_call_sync(PARASITE_CMD_MPROTECT_VMAS, parasite_ctl)) {
			pr_err("Can't rollback unprotected vmas with parasite\n");
			ret = -1;
		}
	}
	return ret;
}
#endif
