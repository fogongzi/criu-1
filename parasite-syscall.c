#include <unistd.h>
#include <inttypes.h>

#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/mman.h>

#include "protobuf.h"
#include "protobuf/sa.pb-c.h"
#include "protobuf/itimer.pb-c.h"
#include "protobuf/creds.pb-c.h"
#include "protobuf/core.pb-c.h"
#include "protobuf/pagemap.pb-c.h"

#include "syscall.h"
#include "ptrace.h"
#include "asm/processor-flags.h"
#include "parasite-syscall.h"
#include "parasite-blob.h"
#include "parasite.h"
#include "crtools.h"
#include "namespaces.h"
#include "pstree.h"
#include "net.h"
#include "page-pipe.h"
#include "page-xfer.h"

#include <string.h>
#include <stdlib.h>

#include "asm/parasite-syscall.h"
#include "asm/dump.h"

#define parasite_size		(round_up(sizeof(parasite_blob), sizeof(long)))

static int can_run_syscall(unsigned long ip, unsigned long start, unsigned long end)
{
	return ip >= start && ip < (end - code_syscall_size);
}

static int syscall_fits_vma_area(struct vma_area *vma_area)
{
	return can_run_syscall((unsigned long)vma_area->vma.start,
			       (unsigned long)vma_area->vma.start,
			       (unsigned long)vma_area->vma.end);
}

static struct vma_area *get_vma_by_ip(struct list_head *vma_area_list, unsigned long ip)
{
	struct vma_area *vma_area;

	list_for_each_entry(vma_area, vma_area_list, list) {
		if (vma_area->vma.start >= TASK_SIZE)
			continue;
		if (!(vma_area->vma.prot & PROT_EXEC))
			continue;
		if (syscall_fits_vma_area(vma_area))
			return vma_area;
	}

	return NULL;
}

/* we run at @regs->ip */
int __parasite_execute(struct parasite_ctl *ctl, pid_t pid, user_regs_struct_t *regs)
{
	siginfo_t siginfo;
	int status;
	int ret = -1;

again:
	if (ptrace(PTRACE_SETREGS, pid, NULL, regs)) {
		pr_perror("Can't set registers (pid: %d)", pid);
		goto err;
	}

	/*
	 * Most ideas are taken from Tejun Heo's parasite thread
	 * https://code.google.com/p/ptrace-parasite/
	 */

	if (ptrace(PTRACE_CONT, pid, NULL, NULL)) {
		pr_perror("Can't continue (pid: %d)", pid);
		goto err;
	}

	if (wait4(pid, &status, __WALL, NULL) != pid) {
		pr_perror("Waited pid mismatch (pid: %d)", pid);
		goto err;
	}

	if (!WIFSTOPPED(status)) {
		pr_err("Task is still running (pid: %d)\n", pid);
		goto err;
	}

	if (ptrace(PTRACE_GETSIGINFO, pid, NULL, &siginfo)) {
		pr_perror("Can't get siginfo (pid: %d)", pid);
		goto err;
	}

	if (ptrace(PTRACE_GETREGS, pid, NULL, regs)) {
		pr_perror("Can't obtain registers (pid: %d)", pid);
			goto err;
	}

	if (WSTOPSIG(status) != SIGTRAP || siginfo.si_code != ARCH_SI_TRAP) {
retry_signal:
		pr_debug("** delivering signal %d si_code=%d\n",
			 siginfo.si_signo, siginfo.si_code);

		if (ctl->signals_blocked) {
			pr_err("Unexpected %d task interruption, aborting\n", pid);
			goto err;
		}

		/* FIXME: jerr(siginfo.si_code > 0, err_restore); */

		/*
		 * This requires some explanation. If a signal from original
		 * program delivered while we're trying to execute our
		 * injected blob -- we need to setup original registers back
		 * so the kernel would make sigframe for us and update the
		 * former registers.
		 *
		 * Then we should swap registers back to our modified copy
		 * and retry.
		 */

		if (ptrace(PTRACE_SETREGS, pid, NULL, &ctl->regs_orig)) {
			pr_perror("Can't set registers (pid: %d)", pid);
			goto err;
		}

		if (ptrace(PTRACE_INTERRUPT, pid, NULL, NULL)) {
			pr_perror("Can't interrupt (pid: %d)", pid);
			goto err;
		}

		if (ptrace(PTRACE_CONT, pid, NULL, (void *)(unsigned long)siginfo.si_signo)) {
			pr_perror("Can't continue (pid: %d)", pid);
			goto err;
		}

		if (wait4(pid, &status, __WALL, NULL) != pid) {
			pr_perror("Waited pid mismatch (pid: %d)", pid);
			goto err;
		}

		if (!WIFSTOPPED(status)) {
			pr_err("Task is still running (pid: %d)\n", pid);
			goto err;
		}

		if (ptrace(PTRACE_GETSIGINFO, pid, NULL, &siginfo)) {
			pr_perror("Can't get siginfo (pid: %d)", pid);
			goto err;
		}

		if (SI_EVENT(siginfo.si_code) != PTRACE_EVENT_STOP)
			goto retry_signal;

		/*
		 * Signal is delivered, so we should update
		 * original registers.
		 */
		{
			user_regs_struct_t r;
			if (ptrace(PTRACE_GETREGS, pid, NULL, &r)) {
				pr_perror("Can't obtain registers (pid: %d)", pid);
				goto err;
			}
			ctl->regs_orig = r;
		}

		goto again;
	}

	/*
	 * We've reached this point iif int3 is triggered inside our
	 * parasite code. So we're done.
	 */
	ret = 0;
err:
	return ret;
}

static void *parasite_args_s(struct parasite_ctl *ctl, int args_size)
{
	BUG_ON(args_size > ctl->args_size);
	return ctl->addr_args;
}

#define parasite_args(ctl, type) ({				\
		BUILD_BUG_ON(sizeof(type) > PARASITE_ARG_SIZE_MIN);\
		ctl->addr_args;					\
	})

static int parasite_execute_by_pid(unsigned int cmd, struct parasite_ctl *ctl, pid_t pid)
{
	int ret;
	user_regs_struct_t regs_orig, regs;

	if (ctl->pid == pid)
		regs = ctl->regs_orig;
	else {
		if (ptrace(PTRACE_GETREGS, pid, NULL, &regs_orig)) {
			pr_perror("Can't obtain registers (pid: %d)", pid);
			return -1;
		}
		regs = regs_orig;
	}

	*ctl->addr_cmd = cmd;

	parasite_setup_regs(ctl->parasite_ip, &regs);

	ret = __parasite_execute(ctl, pid, &regs);
	if (ret == 0)
		ret = (int)REG_RES(regs);

	if (ret)
		pr_err("Parasite exited with %d\n", ret);

	if (ctl->pid != pid)
		if (ptrace(PTRACE_SETREGS, pid, NULL, &regs_orig)) {
			pr_perror("Can't restore registers (pid: %d)", ctl->pid);
			return -1;
		}

	return ret;
}

static int parasite_execute(unsigned int cmd, struct parasite_ctl *ctl)
{
	return parasite_execute_by_pid(cmd, ctl, ctl->pid);
}

static int munmap_seized(struct parasite_ctl *ctl, void *addr, size_t length)
{
	unsigned long x;

	return syscall_seized(ctl, __NR_munmap, &x,
			(unsigned long)addr, length, 0, 0, 0, 0);
}

static int gen_parasite_saddr(struct sockaddr_un *saddr, int key)
{
	int sun_len;

	saddr->sun_family = AF_UNIX;
	snprintf(saddr->sun_path, UNIX_PATH_MAX,
			"X/crtools-pr-%d", key);

	sun_len = SUN_LEN(saddr);
	*saddr->sun_path = '\0';

	return sun_len;
}

static int parasite_send_fd(struct parasite_ctl *ctl, int fd)
{
	if (send_fd(ctl->tsock, NULL, 0, fd) < 0) {
		pr_perror("Can't send file descriptor");
		return -1;
	}
	return 0;
}

static int parasite_set_logfd(struct parasite_ctl *ctl, pid_t pid)
{
	int ret;
	struct parasite_log_args *a;

	ret = parasite_send_fd(ctl, log_get_fd());
	if (ret)
		return ret;

	a = parasite_args(ctl, struct parasite_log_args);
	a->log_level = log_get_loglevel();

	ret = parasite_execute(PARASITE_CMD_CFG_LOG, ctl);
	if (ret < 0)
		return ret;

	return 0;
}

static int parasite_init(struct parasite_ctl *ctl, pid_t pid, int nr_threads)
{
	struct parasite_init_args *args;
	static int sock = -1;

	args = parasite_args(ctl, struct parasite_init_args);

	pr_info("Putting tsock into pid %d\n", pid);
	args->h_addr_len = gen_parasite_saddr(&args->h_addr, getpid());
	args->p_addr_len = gen_parasite_saddr(&args->p_addr, pid);
	args->nr_threads = nr_threads;

	if (sock == -1) {
		int rst = -1;

		if (current_ns_mask & CLONE_NEWNET) {
			pr_info("Switching to %d's net for tsock creation\n", pid);

			if (switch_ns(pid, &net_ns_desc, &rst))
				return -1;
		}

		sock = socket(PF_UNIX, SOCK_DGRAM, 0);
		if (sock < 0) {
			pr_perror("Can't create socket");
			return -1;
		}

		if (bind(sock, (struct sockaddr *)&args->h_addr, args->h_addr_len) < 0) {
			pr_perror("Can't bind socket");
			goto err;
		}

		if (rst > 0 && restore_ns(rst, &net_ns_desc) < 0)
			goto err;
	} else {
		struct sockaddr addr = { .sa_family = AF_UNSPEC, };

		/*
		 * When the peer of a dgram socket dies the original socket
		 * remains in connected state, thus denying any connections
		 * from "other" sources. Unconnect the socket by hands thus
		 * allowing for parasite to connect back.
		 */

		if (connect(sock, &addr, sizeof(addr)) < 0) {
			pr_perror("Can't unconnect");
			goto err;
		}
	}

	if (parasite_execute(PARASITE_CMD_INIT, ctl) < 0) {
		pr_err("Can't init parasite\n");
		goto err;
	}

	if (connect(sock, (struct sockaddr *)&args->p_addr, args->p_addr_len) < 0) {
		pr_perror("Can't connect a transport socket");
		goto err;
	}

	ctl->tsock = sock;
	return 0;
err:
	close(sock);
	return -1;
}

int parasite_dump_thread_seized(struct parasite_ctl *ctl, struct pid *tid,
		CoreEntry *core)
{
	struct parasite_dump_thread *args;
	int ret;

	args = parasite_args(ctl, struct parasite_dump_thread);

	ret = parasite_execute_by_pid(PARASITE_CMD_DUMP_THREAD, ctl, tid->real);

	memcpy(&core->thread_core->blk_sigset, &args->blocked, sizeof(args->blocked));
	CORE_THREAD_ARCH_INFO(core)->clear_tid_addr = encode_pointer(args->tid_addr);
	tid->virt = args->tid;
	core_put_tls(core, args->tls);

	return ret;
}

int parasite_dump_sigacts_seized(struct parasite_ctl *ctl, struct cr_fdset *cr_fdset)
{
	struct parasite_dump_sa_args *args;
	int ret, sig, fd;
	SaEntry se = SA_ENTRY__INIT;

	args = parasite_args(ctl, struct parasite_dump_sa_args);

	ret = parasite_execute(PARASITE_CMD_DUMP_SIGACTS, ctl);
	if (ret < 0)
		return ret;

	fd = fdset_fd(cr_fdset, CR_FD_SIGACT);

	for (sig = 1; sig <= SIGMAX; sig++) {
		int i = sig - 1;

		if (sig == SIGSTOP || sig == SIGKILL)
			continue;

		ASSIGN_TYPED(se.sigaction, encode_pointer(args->sas[i].rt_sa_handler));
		ASSIGN_TYPED(se.flags, args->sas[i].rt_sa_flags);
		ASSIGN_TYPED(se.restorer, encode_pointer(args->sas[i].rt_sa_restorer));
		ASSIGN_TYPED(se.mask, args->sas[i].rt_sa_mask.sig[0]);

		if (pb_write_one(fd, &se, PB_SIGACT) < 0)
			return -1;
	}

	return 0;
}

static int dump_one_timer(struct itimerval *v, int fd)
{
	ItimerEntry ie = ITIMER_ENTRY__INIT;

	ie.isec = v->it_interval.tv_sec;
	ie.iusec = v->it_interval.tv_usec;
	ie.vsec = v->it_value.tv_sec;
	ie.vusec = v->it_value.tv_sec;

	return pb_write_one(fd, &ie, PB_ITIMERS);
}

int parasite_dump_itimers_seized(struct parasite_ctl *ctl, struct cr_fdset *cr_fdset)
{
	struct parasite_dump_itimers_args *args;
	int ret, fd;

	args = parasite_args(ctl, struct parasite_dump_itimers_args);

	ret = parasite_execute(PARASITE_CMD_DUMP_ITIMERS, ctl);
	if (ret < 0)
		return ret;

	fd = fdset_fd(cr_fdset, CR_FD_ITIMERS);

	ret = dump_one_timer(&args->real, fd);
	if (!ret)
		ret = dump_one_timer(&args->virt, fd);
	if (!ret)
		ret = dump_one_timer(&args->prof, fd);

	return ret;
}

int parasite_dump_misc_seized(struct parasite_ctl *ctl, struct parasite_dump_misc *misc)
{
	struct parasite_dump_misc *ma;

	ma = parasite_args(ctl, struct parasite_dump_misc);
	if (parasite_execute(PARASITE_CMD_DUMP_MISC, ctl) < 0)
		return -1;

	*misc = *ma;
	return 0;
}

struct parasite_tty_args *parasite_dump_tty(struct parasite_ctl *ctl, int fd)
{
	struct parasite_tty_args *p;

	p = parasite_args(ctl, struct parasite_tty_args);
	p->fd = fd;

	if (parasite_execute(PARASITE_CMD_DUMP_TTY, ctl) < 0)
		return NULL;

	return p;
}

int parasite_dump_creds(struct parasite_ctl *ctl, CredsEntry *ce)
{
	struct parasite_dump_creds *pc;

	pc = parasite_args(ctl, struct parasite_dump_creds);
	if (parasite_execute(PARASITE_CMD_DUMP_CREDS, ctl) < 0)
		return -1;

	ce->secbits = pc->secbits;
	ce->n_groups = pc->ngroups;

	/*
	 * Achtung! We leak the parasite args pointer to the caller.
	 * It's not safe in general, but in our case is OK, since the
	 * latter doesn't go to parasite before using the data in it.
	 */

	BUILD_BUG_ON(sizeof(ce->groups[0]) != sizeof(pc->groups[0]));
	ce->groups = pc->groups;
	return 0;
}

static unsigned int vmas_pagemap_size(struct vm_area_list *vmas)
{
	/*
	 * In the worst case I need one iovec for half of the
	 * pages (e.g. every odd/even)
	 */

	return sizeof(struct parasite_dump_pages_args) +
		vmas->priv_size * sizeof(struct iovec) / 2;
}

#define PME_PRESENT	(1ULL << 63)
#define PME_SWAP	(1ULL << 62)
#define PME_FILE	(1ULL << 61)

static inline bool should_dump_page(VmaEntry *vmae, u64 pme)
{
	if (vma_entry_is(vmae, VMA_AREA_VDSO))
		return true;
	/*
	 * Optimisation for private mapping pages, that haven't
	 * yet being COW-ed
	 */
	if (vma_entry_is(vmae, VMA_FILE_PRIVATE) && (pme & PME_FILE))
		return false;
	if (pme & (PME_PRESENT | PME_SWAP))
		return true;

	return false;
}

static int generate_iovs(struct vma_area *vma, int pagemap, struct page_pipe *pp, u64 *map)
{
	unsigned long pfn, nr_to_scan;
	u64 aux;

	aux = vma->vma.start / PAGE_SIZE * sizeof(*map);
	if (lseek(pagemap, aux, SEEK_SET) != aux) {
		pr_perror("Can't rewind pagemap file");
		return -1;
	}

	nr_to_scan = vma_area_len(vma) / PAGE_SIZE;
	aux = nr_to_scan * sizeof(*map);
	if (read(pagemap, map, aux) != aux) {
		pr_perror("Can't read pagemap file");
		return -1;
	}

	for (pfn = 0; pfn < nr_to_scan; pfn++) {
		if (!should_dump_page(&vma->vma, map[pfn]))
			continue;

		if (page_pipe_add_page(pp, vma->vma.start + pfn * PAGE_SIZE))
			return -1;
	}

	return 0;
}

int parasite_dump_pages_seized(struct parasite_ctl *ctl, int vpid,
		struct vm_area_list *vma_area_list, struct cr_fdset *cr_fdset)
{
	struct parasite_dump_pages_args *args;
	u64 *map;
	int pagemap;
	struct page_pipe *pp;
	struct page_pipe_buf *ppb;
	struct vma_area *vma_area;
	int ret = -1;
	struct page_xfer xfer;

	pr_info("\n");
	pr_info("Dumping pages (type: %d pid: %d)\n", CR_FD_PAGES, ctl->pid);
	pr_info("----------------------------------------\n");

	pr_debug("   Private vmas %lu/%lu pages\n",
			vma_area_list->longest, vma_area_list->priv_size);

	args = parasite_args_s(ctl, vmas_pagemap_size(vma_area_list));

	map = xmalloc(vma_area_list->longest * sizeof(*map));
	if (!map)
		goto out;

	ret = pagemap = open_proc(ctl->pid, "pagemap");
	if (ret < 0)
		goto out_free;

	ret = -1;
	pp = create_page_pipe(vma_area_list->priv_size / 2, args->iovs);
	if (!pp)
		goto out_close;

	list_for_each_entry(vma_area, &vma_area_list->h, list) {
		if (!privately_dump_vma(vma_area))
			continue;

		ret = generate_iovs(vma_area, pagemap, pp, map);
		if (ret < 0)
			goto out_pp;
	}

	args->off = 0;
	list_for_each_entry(ppb, &pp->bufs, l) {
		ret = parasite_send_fd(ctl, ppb->p[1]);
		if (ret)
			goto out_pp;

		args->nr = ppb->nr_segs;
		args->nr_pages = ppb->pages_in;
		pr_debug("PPB: %d pages %d segs %u pipe %d off\n",
				args->nr_pages, args->nr, ppb->pipe_size, args->off);

		ret = parasite_execute(PARASITE_CMD_DUMPPAGES, ctl);
		if (ret < 0)
			goto out_pp;

		args->off += args->nr;
	}

	ret = open_page_xfer(&xfer, CR_FD_PAGEMAP, vpid);
	if (ret < 0)
		goto out_pp;

	ret = -1;
	list_for_each_entry(ppb, &pp->bufs, l) {
		int i;

		pr_debug("Dump pages %d/%d\n", ppb->pages_in, ppb->nr_segs);

		for (i = 0; i < ppb->nr_segs; i++) {
			struct iovec *iov = &ppb->iov[i];

			pr_debug("\t%p [%u]\n", iov->iov_base,
					(unsigned int)(iov->iov_len / PAGE_SIZE));

			if (xfer.write_pagemap(&xfer, iov, ppb->p[0]))
				goto out_xfer;
		}
	}

	ret = 0;
out_xfer:
	xfer.close(&xfer);
out_pp:
	destroy_page_pipe(pp);
out_close:
	close(pagemap);
out_free:
	xfree(map);
out:
	pr_info("----------------------------------------\n");
	return ret;
}

int parasite_drain_fds_seized(struct parasite_ctl *ctl,
		struct parasite_drain_fd *dfds, int *lfds, struct fd_opts *opts)
{
	int ret = -1, size;
	struct parasite_drain_fd *args;

	size = drain_fds_size(dfds);
	args = parasite_args_s(ctl, size);
	memcpy(args, dfds, size);

	ret = parasite_execute(PARASITE_CMD_DRAIN_FDS, ctl);
	if (ret) {
		pr_err("Parasite failed to drain descriptors\n");
		goto err;
	}

	ret = recv_fds(ctl->tsock, lfds, dfds->nr_fds, opts);
	if (ret) {
		pr_err("Can't retrieve FDs from socket\n");
		goto err;
	}

err:
	return ret;
}

int parasite_get_proc_fd_seized(struct parasite_ctl *ctl)
{
	int ret = -1, fd;

	ret = parasite_execute(PARASITE_CMD_GET_PROC_FD, ctl);
	if (ret) {
		pr_err("Parasite failed to get proc fd\n");
		return ret;
	}

	fd = recv_fd(ctl->tsock);
	if (fd < 0) {
		pr_err("Can't retrieve FD from socket\n");
		return fd;
	}

	return fd;
}

int parasite_init_threads_seized(struct parasite_ctl *ctl, struct pstree_item *item)
{
	int ret = 0, i;

	for (i = 0; i < item->nr_threads; i++) {
		if (item->pid.real == item->threads[i].real)
			continue;

		ret = parasite_execute_by_pid(PARASITE_CMD_INIT_THREAD, ctl,
					      item->threads[i].real);
		if (ret) {
			pr_err("Can't init thread in parasite %d\n",
			       item->threads[i].real);
			break;
		}
	}

	return ret;
}

int parasite_fini_threads_seized(struct parasite_ctl *ctl, struct pstree_item *item)
{
	int ret = 0, i;

	for (i = 0; i < item->nr_threads; i++) {
		if (item->pid.real == item->threads[i].real)
			continue;

		ret = parasite_execute_by_pid(PARASITE_CMD_FINI_THREAD, ctl,
					      item->threads[i].real);
		/*
		 * Note the thread's fini() can be called even when not
		 * all threads were init()'ed, say we're rolling back from
		 * error happened while we were init()'ing some thread, thus
		 * -ENOENT will be returned but we should continie for the
		 * rest of threads set.
		 *
		 * Strictly speaking we always init() threads in sequence thus
		 * we could simply break the loop once first -ENOENT returned
		 * but I prefer to be on a safe side even if some future changes
		 * would change the code logic.
		 */
		if (ret && ret != -ENOENT) {
			pr_err("Can't fini thread in parasite %d\n",
			       item->threads[i].real);
			break;
		}
	}

	return ret;
}

int parasite_cure_seized(struct parasite_ctl *ctl, struct pstree_item *item)
{
	int ret = 0;

	ctl->tsock = -1;

	if (ctl->parasite_ip) {
		ctl->signals_blocked = 0;
		parasite_fini_threads_seized(ctl, item);
		parasite_execute(PARASITE_CMD_FINI, ctl);
	}

	if (ctl->remote_map) {
		if (munmap_seized(ctl, (void *)ctl->remote_map, ctl->map_length)) {
			pr_err("munmap_seized failed (pid: %d)\n", ctl->pid);
			ret = -1;
		}
	}

	if (ctl->local_map) {
		if (munmap(ctl->local_map, ctl->map_length)) {
			pr_err("munmap failed (pid: %d)\n", ctl->pid);
			ret = -1;
		}
	}

	if (ptrace_poke_area(ctl->pid, (void *)ctl->code_orig,
			     (void *)ctl->syscall_ip, sizeof(ctl->code_orig))) {
		pr_err("Can't restore syscall blob (pid: %d)\n", ctl->pid);
		ret = -1;
	}

	if (ptrace(PTRACE_SETREGS, ctl->pid, NULL, &ctl->regs_orig)) {
		pr_err("Can't restore registers (pid: %d)\n", ctl->pid);
		ret = -1;
	}

	free(ctl);
	return ret;
}

struct parasite_ctl *parasite_prep_ctl(pid_t pid, struct vm_area_list *vma_area_list)
{
	struct parasite_ctl *ctl = NULL;
	struct vma_area *vma_area;

	if (task_in_compat_mode(pid)) {
		pr_err("Can't checkpoint task running in compat mode\n");
		goto err;
	}

	/*
	 * Control block early setup.
	 */
	ctl = xzalloc(sizeof(*ctl));
	if (!ctl) {
		pr_err("Parasite control block allocation failed (pid: %d)\n", pid);
		goto err;
	}

	ctl->tsock = -1;

	if (ptrace(PTRACE_GETREGS, pid, NULL, &ctl->regs_orig)) {
		pr_err("Can't obtain registers (pid: %d)\n", pid);
		goto err;
	}

	vma_area = get_vma_by_ip(&vma_area_list->h, REG_IP(ctl->regs_orig));
	if (!vma_area) {
		pr_err("No suitable VMA found to run parasite "
		       "bootstrap code (pid: %d)\n", pid);
		goto err;
	}

	ctl->pid	= pid;
	ctl->syscall_ip	= vma_area->vma.start;

	/*
	 * Inject syscall instruction and remember original code,
	 * we will need it to restore original program content.
	 */
	memcpy(ctl->code_orig, code_syscall, sizeof(ctl->code_orig));
	if (ptrace_swap_area(ctl->pid, (void *)ctl->syscall_ip,
			     (void *)ctl->code_orig, sizeof(ctl->code_orig))) {
		pr_err("Can't inject syscall blob (pid: %d)\n", pid);
		goto err;
	}

	return ctl;

err:
	xfree(ctl);
	return NULL;
}

int parasite_map_exchange(struct parasite_ctl *ctl, unsigned long size)
{
	int fd;

	ctl->remote_map = mmap_seized(ctl, NULL, size,
				      PROT_READ | PROT_WRITE | PROT_EXEC,
				      MAP_ANONYMOUS | MAP_SHARED, -1, 0);
	if (!ctl->remote_map) {
		pr_err("Can't allocate memory for parasite blob (pid: %d)\n", ctl->pid);
		return -1;
	}

	ctl->map_length = round_up(size, PAGE_SIZE);

	fd = open_proc_rw(ctl->pid, "map_files/%p-%p",
		 ctl->remote_map, ctl->remote_map + ctl->map_length);
	if (fd < 0)
		return -1;

	ctl->local_map = mmap(NULL, size, PROT_READ | PROT_WRITE,
			      MAP_SHARED | MAP_FILE, fd, 0);
	close(fd);

	if (ctl->local_map == MAP_FAILED) {
		ctl->local_map = NULL;
		pr_perror("Can't map remote parasite map");
		return -1;
	}

	return 0;
}

static unsigned long parasite_args_size(struct vm_area_list *vmas, struct parasite_drain_fd *dfds)
{
	unsigned long size = PARASITE_ARG_SIZE_MIN;

	size = max(size, (unsigned long)drain_fds_size(dfds));
	size = max(size, (unsigned long)vmas_pagemap_size(vmas));

	return size;
}

struct parasite_ctl *parasite_infect_seized(pid_t pid, struct pstree_item *item,
		struct vm_area_list *vma_area_list, struct parasite_drain_fd *dfds)
{
	int ret;
	struct parasite_ctl *ctl;

	ctl = parasite_prep_ctl(pid, vma_area_list);
	if (!ctl)
		return NULL;

	/*
	 * Inject a parasite engine. Ie allocate memory inside alien
	 * space and copy engine code there. Then re-map the engine
	 * locally, so we will get an easy way to access engine memory
	 * without using ptrace at all.
	 */

	ctl->args_size = parasite_args_size(vma_area_list, dfds);
	ret = parasite_map_exchange(ctl, parasite_size + ctl->args_size);
	if (ret)
		goto err_restore;

	pr_info("Putting parasite blob into %p->%p\n", ctl->local_map, ctl->remote_map);
	memcpy(ctl->local_map, parasite_blob, sizeof(parasite_blob));

	/* Setup the rest of a control block */
	ctl->parasite_ip	= (unsigned long)parasite_sym(ctl->remote_map, __export_parasite_head_start);
	ctl->addr_cmd		= parasite_sym(ctl->local_map, __export_parasite_cmd);
	ctl->addr_args		= parasite_sym(ctl->local_map, __export_parasite_args);

	ret = parasite_init(ctl, pid, item->nr_threads);
	if (ret) {
		pr_err("%d: Can't create a transport socket\n", pid);
		goto err_restore;
	}

	ctl->signals_blocked = 1;

	ret = parasite_set_logfd(ctl, pid);
	if (ret) {
		pr_err("%d: Can't set a logging descriptor\n", pid);
		goto err_restore;
	}

	ret = parasite_init_threads_seized(ctl, item);
	if (ret)
		goto err_restore;

	return ctl;

err_restore:
	parasite_cure_seized(ctl, item);
	return NULL;
}

