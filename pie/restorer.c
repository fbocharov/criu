#include <stdio.h>
#include <stdlib.h>

#include <linux/securebits.h>
#include <linux/capability.h>
#include <sys/types.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/time.h>
#include <sys/shm.h>
#include <fcntl.h>
#include <unistd.h>
#include <sched.h>
#include <sys/resource.h>
#include <signal.h>

#include "compiler.h"
#include "asm/types.h"
#include "syscall.h"
#include "config.h"
#include "prctl.h"
#include "log.h"
#include "util.h"
#include "image.h"
#include "sk-inet.h"
#include "vma.h"

#include "crtools.h"
#include "lock.h"
#include "restorer.h"
#include "aio.h"
#include "seccomp.h"

#include "protobuf/creds.pb-c.h"
#include "protobuf/mm.pb-c.h"

#include "asm/restorer.h"

#ifndef PR_SET_PDEATHSIG
#define PR_SET_PDEATHSIG 1
#endif

#define sys_prctl_safe(opcode, val1, val2, val3)			\
	({								\
		long __ret = sys_prctl(opcode, val1, val2, val3, 0);	\
		if (__ret)						\
			 pr_err("prctl failed @%d with %ld\n", __LINE__, __ret);\
		__ret;							\
	})

static struct task_entries *task_entries;
static futex_t thread_inprogress;
static int cap_last_cap;
static pid_t *helpers;
static int n_helpers;
static pid_t *zombies;
static int n_zombies;

extern void cr_restore_rt (void) asm ("__cr_restore_rt")
			__attribute__ ((visibility ("hidden")));

static void sigchld_handler(int signal, siginfo_t *siginfo, void *data)
{
	char *r;
	int i;

	/* We can ignore helpers that die, we expect them to after
	 * CR_STATE_RESTORE is finished. */
	for (i = 0; i < n_helpers; i++)
		if (siginfo->si_pid == helpers[i])
			return;

	for (i = 0; i < n_zombies; i++)
		if (siginfo->si_pid == zombies[i])
			return;

	if (siginfo->si_code & CLD_EXITED)
		r = " exited, status=";
	else if (siginfo->si_code & CLD_KILLED)
		r = " killed by signal ";
	else
		r = "disappeared with ";

	pr_info("Task %d %s %d\n", siginfo->si_pid, r, siginfo->si_status);

	futex_abort_and_wake(&task_entries->nr_in_progress);
	/* sa_restorer may be unmaped, so we can't go back to userspace*/
	sys_kill(sys_getpid(), SIGSTOP);
	sys_exit_group(1);
}

static int lsm_set_label(char *label, int procfd)
{
	int ret = -1, len, lsmfd;
	char path[LOG_SIMPLE_CHUNK];

	if (!label)
		return 0;

	pr_info("restoring lsm profile %s\n", label);

	simple_sprintf(path, "self/task/%ld/attr/current", sys_gettid());

	lsmfd = sys_openat(procfd, path, O_WRONLY, 0);
	if (lsmfd < 0) {
		pr_err("failed openat %d\n", lsmfd);
		return -1;
	}

	for (len = 0; label[len]; len++)
		;

	ret = sys_write(lsmfd, label, len);
	sys_close(lsmfd);
	if (ret < 0) {
		pr_err("can't write lsm profile %d\n", ret);
		return -1;
	}

	return 0;
}

static int restore_creds(CredsEntry *ce, int procfd)
{
	int b, i, ret;
	struct cap_header hdr;
	struct cap_data data[_LINUX_CAPABILITY_U32S_3];

	/*
	 * We're still root here and thus can do it without failures.
	 */

	/*
	 * First -- set the SECURE_NO_SETUID_FIXUP bit not to
	 * lose caps bits when changing xids.
	 */

	ret = sys_prctl(PR_SET_SECUREBITS, 1 << SECURE_NO_SETUID_FIXUP, 0, 0, 0);
	if (ret) {
		pr_err("Unable to set SECURE_NO_SETUID_FIXUP: %d\n", ret);
		return -1;
	}

	/*
	 * Second -- restore xids. Since we still have the CAP_SETUID
	 * capability nothing should fail. But call the setfsXid last
	 * to override the setresXid settings.
	 */

	ret = sys_setresuid(ce->uid, ce->euid, ce->suid);
	if (ret) {
		pr_err("Unable to set real, effective and saved user ID: %d\n", ret);
		return -1;
	}

	sys_setfsuid(ce->fsuid);
	if (sys_setfsuid(-1) != ce->fsuid) {
		pr_err("Unable to set fsuid\n");
		return -1;
	}

	ret = sys_setresgid(ce->gid, ce->egid, ce->sgid);
	if (ret) {
		pr_err("Unable to set real, effective and saved group ID: %d\n", ret);
		return -1;
	}

	sys_setfsgid(ce->fsgid);
	if (sys_setfsgid(-1) != ce->fsgid) {
		pr_err("Unable to set fsgid\n");
		return -1;
	}

	/*
	 * Third -- restore securebits. We don't need them in any
	 * special state any longer.
	 */

	ret = sys_prctl(PR_SET_SECUREBITS, ce->secbits, 0, 0, 0);
	if (ret) {
		pr_err("Unable to set PR_SET_SECUREBITS: %d\n", ret);
		return -1;
	}

	/*
	 * Fourth -- trim bset. This can only be done while
	 * having the CAP_SETPCAP capablity.
	 */

	for (b = 0; b < CR_CAP_SIZE; b++) {
		for (i = 0; i < 32; i++) {
			if (b * 32 + i > cap_last_cap)
				break;
			if (ce->cap_bnd[b] & (1 << i))
				/* already set */
				continue;
			ret = sys_prctl(PR_CAPBSET_DROP, i + b * 32, 0, 0, 0);
			if (ret) {
				pr_err("Unable to drop capability %d: %d\n",
								i + b * 32, ret);
				return -1;
			}
		}
	}

	/*
	 * Fifth -- restore caps. Nothing but cap bits are changed
	 * at this stage, so just do it.
	 */

	hdr.version = _LINUX_CAPABILITY_VERSION_3;
	hdr.pid = 0;

	BUILD_BUG_ON(_LINUX_CAPABILITY_U32S_3 != CR_CAP_SIZE);

	for (i = 0; i < CR_CAP_SIZE; i++) {
		data[i].eff = ce->cap_eff[i];
		data[i].prm = ce->cap_prm[i];
		data[i].inh = ce->cap_inh[i];
	}

	ret = sys_capset(&hdr, data);
	if (ret) {
		pr_err("Unable to restore capabilities: %d\n", ret);
		return -1;
	}

	if (lsm_set_label(ce->lsm_profile, procfd) < 0)
		return -1;

	return 0;
}

/*
 * This should be done after creds restore, as
 * some creds changes might drop the value back
 * to zero.
 */

static inline int restore_pdeath_sig(struct thread_restore_args *ta)
{
	if (ta->pdeath_sig)
		return sys_prctl(PR_SET_PDEATHSIG, ta->pdeath_sig, 0, 0, 0);
	else
		return 0;
}

static int restore_dumpable_flag(MmEntry *mme)
{
	int current_dumpable;
	int ret;

	if (!mme->has_dumpable) {
		pr_warn("Dumpable flag not present in criu dump.\n");
		return 0;
	}

	if (mme->dumpable == 0 || mme->dumpable == 1) {
		ret = sys_prctl(PR_SET_DUMPABLE, mme->dumpable, 0, 0, 0);
		if (ret) {
			pr_err("Unable to set PR_SET_DUMPABLE: %d\n", ret);
			return -1;
		}
		return 0;
	}

	/*
	 * If dumpable flag is present but it is not 0 or 1, then we can not
	 * use prctl to set it back.  Try to see if it is already correct
	 * (which is likely if sysctl fs.suid_dumpable is the same when dump
	 * and restore are run), in which case there is nothing to do.
	 * Otherwise, set dumpable to 0 which should be a secure fallback.
	 */
	current_dumpable = sys_prctl(PR_GET_DUMPABLE, 0, 0, 0, 0);
	if (mme->dumpable != current_dumpable) {
		pr_warn("Dumpable flag [%d] does not match current [%d]. "
			"Will fallback to setting it to 0 to disable it.\n",
			mme->dumpable, current_dumpable);
		ret = sys_prctl(PR_SET_DUMPABLE, 0, 0, 0, 0);
		if (ret) {
			pr_err("Unable to set PR_SET_DUMPABLE: %d\n", ret);
			return -1;
		}
	}
	return 0;
}

static void restore_sched_info(struct rst_sched_param *p)
{
	struct sched_param parm;

	pr_info("Restoring scheduler params %d.%d.%d\n",
			p->policy, p->nice, p->prio);

	sys_setpriority(PRIO_PROCESS, 0, p->nice);
	parm.sched_priority = p->prio;
	sys_sched_setscheduler(0, p->policy, &parm);
}

static void restore_rlims(struct task_restore_args *ta)
{
	int r;

	for (r = 0; r < ta->rlims_n; r++) {
		struct krlimit krlim;

		krlim.rlim_cur = ta->rlims[r].rlim_cur;
		krlim.rlim_max = ta->rlims[r].rlim_max;
		sys_setrlimit(r, &krlim);
	}
}

static int restore_signals(siginfo_t *ptr, int nr, bool group)
{
	int ret, i;

	for (i = 0; i < nr; i++) {
		siginfo_t *info = ptr + i;

		pr_info("Restore signal %d group %d\n", info->si_signo, group);
		if (group)
			ret = sys_rt_sigqueueinfo(sys_getpid(), info->si_signo, info);
		else
			ret = sys_rt_tgsigqueueinfo(sys_getpid(),
						sys_gettid(), info->si_signo, info);
		if (ret) {
			pr_err("Unable to send siginfo %d %x with code %d\n",
					info->si_signo, info->si_code, ret);
			return -1;;
		}
	}

	return 0;
}

static void restore_seccomp(struct task_restore_args *args)
{
	switch (args->seccomp_mode) {
	case SECCOMP_MODE_DISABLED:
		return;
	case SECCOMP_MODE_STRICT:
		if (sys_prctl(PR_SET_SECCOMP, SECCOMP_MODE_STRICT, 0, 0, 0))
			goto die;
		return;
	case SECCOMP_MODE_FILTER: {
		int i;
		void *filter_data;

		filter_data = &args->seccomp_filters[args->seccomp_filters_n];

		for (i = 0; i < args->seccomp_filters_n; i++) {
			struct sock_fprog *fprog = &args->seccomp_filters[i];

			fprog->filter = filter_data;

			/* We always TSYNC here, since we require that the
			 * creds for all threads be the same; this means we
			 * don't have to restore_seccomp() in threads, and that
			 * future TSYNC behavior will be correct.
			 */
			if (sys_seccomp(SECCOMP_SET_MODE_FILTER, SECCOMP_FILTER_FLAG_TSYNC, (char *) fprog) < 0)
				goto die;

			filter_data += fprog->len * sizeof(struct sock_filter);
		}

		return;
	}
	default:
		goto die;
	}

die:
	/*
	 * If preparing any seccomp state failed, we should make sure this
	 * process doesn't continue so that it can't do things outside the
	 * sandbox. Unfortunately, the rest of the restore has to continue
	 * since we're too late in the process to stop it and have unlocked the
	 * network.
	 */
	sys_exit_group(1);
}

static int restore_thread_common(struct rt_sigframe *sigframe,
		struct thread_restore_args *args)
{
	sys_set_tid_address((int *)decode_pointer(args->clear_tid_addr));

	if (args->has_futex && args->futex_rla_len) {
		int ret;

		ret = sys_set_robust_list(decode_pointer(args->futex_rla),
					  args->futex_rla_len);
		if (ret) {
			pr_err("Failed to recover futex robust list: %d\n", ret);
			return -1;
		}
	}

	restore_sched_info(&args->sp);

	if (restore_nonsigframe_gpregs(&args->gpregs))
		return -1;

	restore_tls(&args->tls);

	return 0;
}

static void noinline rst_sigreturn(unsigned long new_sp)
{
	ARCH_RT_SIGRETURN(new_sp);
}

/*
 * Threads restoration via sigreturn. Note it's locked
 * routine and calls for unlock at the end.
 */
long __export_restore_thread(struct thread_restore_args *args)
{
	struct rt_sigframe *rt_sigframe;
	k_rtsigset_t to_block;
	unsigned long new_sp;
	int my_pid = sys_gettid();
	int ret;

	if (my_pid != args->pid) {
		pr_err("Thread pid mismatch %d/%d\n", my_pid, args->pid);
		goto core_restore_end;
	}

	/* All signals must be handled by thread leader */
	ksigfillset(&to_block);
	ret = sys_sigprocmask(SIG_SETMASK, &to_block, NULL, sizeof(k_rtsigset_t));
	if (ret) {
		pr_err("Unable to block signals %d\n", ret);
		goto core_restore_end;
	}

	rt_sigframe = (void *)args->mem_zone.rt_sigframe;

	if (restore_thread_common(rt_sigframe, args))
		goto core_restore_end;

	ret = restore_creds(&args->ta->creds, args->ta->proc_fd);
	if (ret)
		goto core_restore_end;

	ret = restore_dumpable_flag(&args->ta->mm);
	if (ret)
		goto core_restore_end;

	pr_info("%ld: Restored\n", sys_gettid());

	restore_finish_stage(CR_STATE_RESTORE);

	if (restore_signals(args->siginfo, args->siginfo_n, false))
		goto core_restore_end;

	restore_finish_stage(CR_STATE_RESTORE_SIGCHLD);
	restore_pdeath_sig(args);

	if (args->ta->seccomp_mode != SECCOMP_MODE_DISABLED)
		pr_info("Restoring seccomp mode %d for %ld\n", args->ta->seccomp_mode, sys_getpid());

	restore_finish_stage(CR_STATE_RESTORE_CREDS);

	futex_dec_and_wake(&thread_inprogress);

	new_sp = (long)rt_sigframe + SIGFRAME_OFFSET;
	rst_sigreturn(new_sp);

core_restore_end:
	pr_err("Restorer abnormal termination for %ld\n", sys_getpid());
	futex_abort_and_wake(&task_entries->nr_in_progress);
	sys_exit_group(1);
	return -1;
}

static long restore_self_exe_late(struct task_restore_args *args)
{
	int fd = args->fd_exe_link, ret;

	pr_info("Restoring EXE link\n");
	ret = sys_prctl_safe(PR_SET_MM, PR_SET_MM_EXE_FILE, fd, 0);
	if (ret)
		pr_err("Can't restore EXE link (%d)\n", ret);
	sys_close(fd);

	return ret;
}

static unsigned long restore_mapping(const VmaEntry *vma_entry)
{
	int prot	= vma_entry->prot;
	int flags	= vma_entry->flags | MAP_FIXED;
	unsigned long addr;

	if (vma_entry_is(vma_entry, VMA_AREA_SYSVIPC))
		return sys_shmat(vma_entry->fd, decode_pointer(vma_entry->start),
				 (vma_entry->prot & PROT_WRITE) ? 0 : SHM_RDONLY);

	/*
	 * Restore or shared mappings are tricky, since
	 * we open anonymous mapping via map_files/
	 * MAP_ANONYMOUS should be eliminated so fd would
	 * be taken into account by a kernel.
	 */
	if (vma_entry_is(vma_entry, VMA_ANON_SHARED) && (vma_entry->fd != -1UL))
		flags &= ~MAP_ANONYMOUS;

	/* A mapping of file with MAP_SHARED is up to date */
	if (vma_entry->fd == -1 || !(vma_entry->flags & MAP_SHARED))
		prot |= PROT_WRITE;

	pr_debug("\tmmap(%"PRIx64" -> %"PRIx64", %x %x %d)\n",
			vma_entry->start, vma_entry->end,
			prot, flags, (int)vma_entry->fd);
	/*
	 * Should map memory here. Note we map them as
	 * writable since we're going to restore page
	 * contents.
	 */
	addr = sys_mmap(decode_pointer(vma_entry->start),
			vma_entry_len(vma_entry),
			prot, flags,
			vma_entry->fd,
			vma_entry->pgoff);

	if (vma_entry->fd != -1)
		sys_close(vma_entry->fd);

	return addr;
}

static void rst_tcp_repair_off(struct rst_tcp_sock *rts)
{
	int aux, ret;

	aux = rts->reuseaddr;
	pr_debug("pie: Turning repair off for %d (reuse %d)\n", rts->sk, aux);
	tcp_repair_off(rts->sk);

	ret = sys_setsockopt(rts->sk, SOL_SOCKET, SO_REUSEADDR, &aux, sizeof(aux));
	if (ret < 0)
		pr_err("Failed to restore of SO_REUSEADDR on socket (%d)\n", ret);
}

static void rst_tcp_socks_all(struct task_restore_args *ta)
{
	int i;

	for (i = 0; i < ta->tcp_socks_n; i++)
		rst_tcp_repair_off(&ta->tcp_socks[i]);
}

static int vma_remap(unsigned long src, unsigned long dst, unsigned long len)
{
	unsigned long guard = 0, tmp;

	pr_info("Remap %lx->%lx len %lx\n", src, dst, len);

	if (src - dst < len)
		guard = dst;
	else if (dst - src < len)
		guard = dst + len - PAGE_SIZE;

	if (src == dst)
		return 0;

	if (guard != 0) {
		/*
		 * mremap() returns an error if a target and source vma-s are
		 * overlapped. In this case the source vma are remapped in
		 * a temporary place and then remapped to the target address.
		 * Here is one hack to find non-ovelapped temporary place.
		 *
		 * 1. initial placement. We need to move src -> tgt.
		 * |       |+++++src+++++|
		 * |-----tgt-----|       |
		 *
		 * 2. map a guard page at the non-ovelapped border of a target vma.
		 * |       |+++++src+++++|
		 * |G|----tgt----|       |
		 *
		 * 3. remap src to any other place.
		 *    G prevents src from being remaped on tgt again
		 * |       |-------------| -> |+++++src+++++|
		 * |G|---tgt-----|                          |
		 *
		 * 4. remap src to tgt, no overlapping any longer
		 * |+++++src+++++|   <----    |-------------|
		 * |G|---tgt-----|                          |
		 */

		unsigned long addr;

		/* Map guard page (step 2) */
		tmp = sys_mmap((void *) guard, PAGE_SIZE, PROT_NONE,
					MAP_PRIVATE | MAP_ANONYMOUS, 0, 0);
		if (tmp != guard) {
			pr_err("Unable to map a guard page %lx (%lx)\n", guard, tmp);
			return -1;
		}

		/* Move src to non-overlapping place (step 3) */
		addr = sys_mmap(NULL, len, PROT_NONE,
					MAP_PRIVATE | MAP_ANONYMOUS, 0, 0);
		if (addr == (unsigned long) MAP_FAILED) {
			pr_err("Unable to reserve memory (%lx)\n", addr);
			return -1;
		}

		tmp = sys_mremap(src, len, len,
					MREMAP_MAYMOVE | MREMAP_FIXED, addr);
		if (tmp != addr) {
			pr_err("Unable to remap %lx -> %lx (%lx)\n", src, addr, tmp);
			return -1;
		}

		src = addr;
	}

	tmp = sys_mremap(src, len, len, MREMAP_MAYMOVE | MREMAP_FIXED, dst);
	if (tmp != dst) {
		pr_err("Unable to remap %lx -> %lx\n", src, dst);
		return -1;
	}

	return 0;
}

static int timerfd_arm(struct task_restore_args *args)
{
	int i;

	for (i = 0; i < args->timerfd_n; i++) {
		struct restore_timerfd *t = &args->timerfd[i];
		int ret;

		pr_debug("timerfd: arm for fd %d (%d)\n", t->fd, i);

		if (t->settime_flags & TFD_TIMER_ABSTIME) {
			struct timespec ts = { };

			/*
			 * We might need to adjust value because the checkpoint
			 * and restore procedure takes some time itself. Note
			 * we don't adjust nanoseconds, since the result may
			 * overflow the limit NSEC_PER_SEC FIXME
			 */
			if (sys_clock_gettime(t->clockid, &ts)) {
				pr_err("Can't get current time\n");
				return -1;
			}

			t->val.it_value.tv_sec += (time_t)ts.tv_sec;

			pr_debug("Ajust id %#x it_value(%llu, %llu) -> it_value(%llu, %llu)\n",
				 t->id, (unsigned long long)ts.tv_sec,
				 (unsigned long long)ts.tv_nsec,
				 (unsigned long long)t->val.it_value.tv_sec,
				 (unsigned long long)t->val.it_value.tv_nsec);
		}

		ret  = sys_timerfd_settime(t->fd, t->settime_flags, &t->val, NULL);
		if (t->ticks)
			ret |= sys_ioctl(t->fd, TFD_IOC_SET_TICKS, (unsigned long)&t->ticks);
		if (ret) {
			pr_err("Can't restore ticks/time for timerfd - %d\n", i);
			return ret;
		}
	}
	return 0;
}

static int create_posix_timers(struct task_restore_args *args)
{
	int ret, i;
	timer_t next_id;
	struct sigevent sev;

	for (i = 0; i < args->posix_timers_n; i++) {
		sev.sigev_notify = args->posix_timers[i].spt.it_sigev_notify;
		sev.sigev_signo = args->posix_timers[i].spt.si_signo;
		sev.sigev_value.sival_ptr = args->posix_timers[i].spt.sival_ptr;

		while (1) {
			ret = sys_timer_create(args->posix_timers[i].spt.clock_id, &sev, &next_id);
			if (ret < 0) {
				pr_err("Can't create posix timer - %d\n", i);
				return ret;
			}

			if ((long)next_id == args->posix_timers[i].spt.it_id)
				break;

			ret = sys_timer_delete(next_id);
			if (ret < 0) {
				pr_err("Can't remove temporaty posix timer %lx\n", (long) next_id);
				return ret;
			}

			if ((long)next_id > args->posix_timers[i].spt.it_id) {
				pr_err("Can't create timers, kernel don't give them consequently\n");
				return -1;
			}
		}
	}

	return 0;
}

static void restore_posix_timers(struct task_restore_args *args)
{
	int i;
	struct restore_posix_timer *rt;

	for (i = 0; i < args->posix_timers_n; i++) {
		rt = &args->posix_timers[i];
		sys_timer_settime((timer_t)rt->spt.it_id, 0, &rt->val, NULL);
	}
}
static void *bootstrap_start;
static unsigned int bootstrap_len;

/*
 * sys_munmap must not return here. The controll process must
 * trap us on the exit from sys_munmap.
 */
#ifdef CONFIG_VDSO
static unsigned long vdso_rt_size;
#else
#define vdso_rt_size	(0)
#endif

void __export_unmap(void)
{
	sys_munmap(bootstrap_start, bootstrap_len - vdso_rt_size);
}

/*
 * This function unmaps all VMAs, which don't belong to
 * the restored process or the restorer.
 *
 * The restorer memory is two regions -- area with restorer, its stack
 * and arguments and the one with private vmas of the tasks we restore
 * (a.k.a. premmaped area):
 *
 * 0                       task_size
 * +----+====+----+====+---+
 *
 * Thus to unmap old memory we have to do 3 unmaps:
 * [ 0 -- 1st area start ]
 * [ 1st end -- 2nd start ]
 * [ 2nd start -- task_size ]
 */
static int unmap_old_vmas(void *premmapped_addr, unsigned long premmapped_len,
		      void *bootstrap_start, unsigned long bootstrap_len,
		      unsigned long task_size)
{
	unsigned long s1, s2;
	void *p1, *p2;
	int ret;

	if (premmapped_addr < bootstrap_start) {
		p1 = premmapped_addr;
		s1 = premmapped_len;
		p2 = bootstrap_start;
		s2 = bootstrap_len;
	} else {
		p2 = premmapped_addr;
		s2 = premmapped_len;
		p1 = bootstrap_start;
		s1 = bootstrap_len;
	}

	ret = sys_munmap(NULL, p1 - NULL);
	if (ret) {
		pr_err("Unable to unmap (%p-%p): %d\n", NULL, p1, ret);
		return -1;
	}

	ret = sys_munmap(p1 + s1, p2 - (p1 + s1));
	if (ret) {
		pr_err("Unable to unmap (%p-%p): %d\n", p1 + s1, p2, ret);
		return -1;
	}

	ret = sys_munmap(p2 + s2, task_size - (unsigned long)(p2 + s2));
	if (ret) {
		pr_err("Unable to unmap (%p-%p): %d\n",
				p2 + s2, (void *)task_size, ret);
		return -1;
	}

	return 0;
}

static int wait_helpers(struct task_restore_args *task_args)
{
	int i;

	for (i = 0; i < task_args->helpers_n; i++) {
		int status;
		pid_t pid = task_args->helpers[i];

		/* Check that a helper completed. */
		if (sys_wait4(pid, &status, 0, NULL) == -1) {
			/* It has been waited in sigchld_handler */
			continue;
		}
		if (!WIFEXITED(status) || WEXITSTATUS(status)) {
			pr_err("%d exited with non-zero code (%d,%d)\n", pid,
				WEXITSTATUS(status), WTERMSIG(status));
			return -1;
		}
	}

	return 0;
}

static int wait_zombies(struct task_restore_args *task_args)
{
	int i;

	atomic_add(task_args->zombies_n, &task_entries->nr_zombies);

	for (i = 0; i < task_args->zombies_n; i++) {
		if (sys_waitid(P_PID, task_args->zombies[i], NULL, WNOWAIT | WEXITED, NULL) < 0) {
			pr_err("Wait on %d zombie failed\n", task_args->zombies[i]);
			return -1;
		}
		pr_debug("%ld: Collect a zombie with pid %d\n",
			sys_getpid(), task_args->zombies[i]);
		futex_dec_and_wake(&task_entries->nr_in_progress);
	}

	return 0;
}

/*
 * The main routine to restore task via sigreturn.
 * This one is very special, we never return there
 * but use sigreturn facility to restore core registers
 * and jump execution to some predefined ip read from
 * core file.
 */
long __export_restore_task(struct task_restore_args *args)
{
	long ret = -1;
	int i;
	VmaEntry *vma_entry;
	unsigned long va;

	struct rt_sigframe *rt_sigframe;
	struct prctl_mm_map prctl_map;
	unsigned long new_sp;
	k_rtsigset_t to_block;
	pid_t my_pid = sys_getpid();
	rt_sigaction_t act;

	bootstrap_start = args->bootstrap_start;
	bootstrap_len	= args->bootstrap_len;

#ifdef CONFIG_VDSO
	vdso_rt_size	= args->vdso_rt_size;
#endif

	task_entries = args->task_entries;
	helpers = args->helpers;
	n_helpers = args->helpers_n;
	zombies = args->zombies;
	n_zombies = args->zombies_n;
	*args->breakpoint = rst_sigreturn;

	ksigfillset(&act.rt_sa_mask);
	act.rt_sa_handler = sigchld_handler;
	act.rt_sa_flags = SA_SIGINFO | SA_RESTORER | SA_RESTART;
	act.rt_sa_restorer = cr_restore_rt;
	sys_sigaction(SIGCHLD, &act, NULL, sizeof(k_rtsigset_t));

	log_set_fd(args->logfd);
	log_set_loglevel(args->loglevel);

	cap_last_cap = args->cap_last_cap;

	pr_info("Switched to the restorer %d\n", my_pid);

	if (vdso_do_park(&args->vdso_sym_rt, args->vdso_rt_parked_at, vdso_rt_size))
		goto core_restore_end;

	if (unmap_old_vmas((void *)args->premmapped_addr, args->premmapped_len,
				bootstrap_start, bootstrap_len, args->task_size))
		goto core_restore_end;

	/* Shift private vma-s to the left */
	for (i = 0; i < args->vmas_n; i++) {
		vma_entry = args->vmas + i;

		if (!vma_entry_is_private(vma_entry, args->task_size))
			continue;

		if (vma_entry->end >= args->task_size)
			continue;

		if (vma_entry->start > vma_entry->shmid)
			break;

		if (vma_remap(vma_premmaped_start(vma_entry),
				vma_entry->start, vma_entry_len(vma_entry)))
			goto core_restore_end;
	}

	/* Shift private vma-s to the right */
	for (i = args->vmas_n - 1; i >= 0; i--) {
		vma_entry = args->vmas + i;

		if (!vma_entry_is_private(vma_entry, args->task_size))
			continue;

		if (vma_entry->start > args->task_size)
			continue;

		if (vma_entry->start < vma_entry->shmid)
			break;

		if (vma_remap(vma_premmaped_start(vma_entry),
				vma_entry->start, vma_entry_len(vma_entry)))
			goto core_restore_end;
	}

	/*
	 * OK, lets try to map new one.
	 */
	for (i = 0; i < args->vmas_n; i++) {
		vma_entry = args->vmas + i;

		if (!vma_entry_is(vma_entry, VMA_AREA_REGULAR))
			continue;

		if (vma_entry_is_private(vma_entry, args->task_size))
			continue;

		va = restore_mapping(vma_entry);

		if (va != vma_entry->start) {
			pr_err("Can't restore %"PRIx64" mapping with %lx\n", vma_entry->start, va);
			goto core_restore_end;
		}
	}

#ifdef CONFIG_VDSO
	/*
	 * Proxify vDSO.
	 */
	for (i = 0; i < args->vmas_n; i++) {
		if (vma_entry_is(&args->vmas[i], VMA_AREA_VDSO) ||
		    vma_entry_is(&args->vmas[i], VMA_AREA_VVAR)) {
			if (vdso_proxify("dumpee", &args->vdso_sym_rt,
					 args->vdso_rt_parked_at,
					 i, args->vmas, args->vmas_n))
				goto core_restore_end;
			break;
		}
	}
#endif

	/*
	 * Walk though all VMAs again to drop PROT_WRITE
	 * if it was not there.
	 */
	for (i = 0; i < args->vmas_n; i++) {
		vma_entry = args->vmas + i;

		if (!(vma_entry_is(vma_entry, VMA_AREA_REGULAR)))
			continue;

		if (vma_entry->prot & PROT_WRITE)
			continue;

		sys_mprotect(decode_pointer(vma_entry->start),
			     vma_entry_len(vma_entry),
			     vma_entry->prot);
	}

	/*
	 * Finally restore madivse() bits
	 */
	for (i = 0; i < args->vmas_n; i++) {
		unsigned long m;

		vma_entry = args->vmas + i;
		if (!vma_entry->has_madv || !vma_entry->madv)
			continue;

		for (m = 0; m < sizeof(vma_entry->madv) * 8; m++) {
			if (vma_entry->madv & (1ul << m)) {
				ret = sys_madvise(vma_entry->start,
						  vma_entry_len(vma_entry),
						  m);
				if (ret) {
					pr_err("madvise(%"PRIx64", %"PRIu64", %ld) "
					       "failed with %ld\n",
						vma_entry->start,
						vma_entry_len(vma_entry),
						m, ret);
					goto core_restore_end;
				}
			}
		}
	}

	/*
	 * Now when all VMAs are in their places time to set
	 * up AIO rings.
	 */

	for (i = 0; i < args->rings_n; i++) {
		struct rst_aio_ring *raio = &args->rings[i];
		unsigned long ctx = 0;
		int ret;

		ret = sys_io_setup(raio->nr_req, &ctx);
		if (ret < 0) {
			pr_err("Ring setup failed with %d\n", ret);
			goto core_restore_end;
		}

		if (ctx == raio->addr) /* Lucky bastards we are! */
			continue;

		/*
		 * If we failed to get the proper nr_req right and
		 * created smaller or larger ring, then this remap
		 * will (should) fail, since AIO rings has immutable
		 * size.
		 *
		 * This is not great, but anyway better than putting
		 * a ring of wrong size into correct place.
		 */

		ctx = sys_mremap(ctx, raio->len, raio->len,
					MREMAP_FIXED | MREMAP_MAYMOVE,
					raio->addr);
		if (ctx != raio->addr) {
			pr_err("Ring remap failed with %ld\n", ctx);
			goto core_restore_end;
		}

		/*
		 * Now check that kernel not just remapped the
		 * ring into new place, but updated the internal
		 * context state respectively.
		 */

		ret = sys_io_getevents(ctx, 0, 1, NULL, NULL);
		if (ret != 0) {
			if (ret < 0)
				pr_err("Kernel doesn't remap AIO rings\n");
			else
				pr_err("AIO context screwed up\n");

			goto core_restore_end;
		}
	}

	ret = 0;

	/*
	 * Tune up the task fields.
	 */
	ret = sys_prctl_safe(PR_SET_NAME, (long)args->comm, 0, 0);
	if (ret)
		goto core_restore_end;

	/*
	 * New kernel interface with @PR_SET_MM_MAP will become
	 * more widespread once kernel get deployed over the world.
	 * Thus lets be opportunistic and use new inteface as a try.
	 */
	prctl_map = (struct prctl_mm_map) {
		.start_code	= args->mm.mm_start_code,
		.end_code	= args->mm.mm_end_code,
		.start_data	= args->mm.mm_start_data,
		.end_data	= args->mm.mm_end_data,
		.start_stack	= args->mm.mm_start_stack,
		.start_brk	= args->mm.mm_start_brk,
		.brk		= args->mm.mm_brk,
		.arg_start	= args->mm.mm_arg_start,
		.arg_end	= args->mm.mm_arg_end,
		.env_start	= args->mm.mm_env_start,
		.env_end	= args->mm.mm_env_end,
		.auxv		= (void *)args->mm_saved_auxv,
		.auxv_size	= args->mm_saved_auxv_size,
		.exe_fd		= args->fd_exe_link,
	};
	ret = sys_prctl(PR_SET_MM, PR_SET_MM_MAP, (long)&prctl_map, sizeof(prctl_map), 0);
	if (ret == -EINVAL) {
		ret  = sys_prctl_safe(PR_SET_MM, PR_SET_MM_START_CODE,	(long)args->mm.mm_start_code, 0);
		ret |= sys_prctl_safe(PR_SET_MM, PR_SET_MM_END_CODE,	(long)args->mm.mm_end_code, 0);
		ret |= sys_prctl_safe(PR_SET_MM, PR_SET_MM_START_DATA,	(long)args->mm.mm_start_data, 0);
		ret |= sys_prctl_safe(PR_SET_MM, PR_SET_MM_END_DATA,	(long)args->mm.mm_end_data, 0);
		ret |= sys_prctl_safe(PR_SET_MM, PR_SET_MM_START_STACK,	(long)args->mm.mm_start_stack, 0);
		ret |= sys_prctl_safe(PR_SET_MM, PR_SET_MM_START_BRK,	(long)args->mm.mm_start_brk, 0);
		ret |= sys_prctl_safe(PR_SET_MM, PR_SET_MM_BRK,		(long)args->mm.mm_brk, 0);
		ret |= sys_prctl_safe(PR_SET_MM, PR_SET_MM_ARG_START,	(long)args->mm.mm_arg_start, 0);
		ret |= sys_prctl_safe(PR_SET_MM, PR_SET_MM_ARG_END,	(long)args->mm.mm_arg_end, 0);
		ret |= sys_prctl_safe(PR_SET_MM, PR_SET_MM_ENV_START,	(long)args->mm.mm_env_start, 0);
		ret |= sys_prctl_safe(PR_SET_MM, PR_SET_MM_ENV_END,	(long)args->mm.mm_env_end, 0);
		ret |= sys_prctl_safe(PR_SET_MM, PR_SET_MM_AUXV,	(long)args->mm_saved_auxv, args->mm_saved_auxv_size);

		/*
		 * Because of requirements applied from kernel side
		 * we need to restore /proc/pid/exe symlink late,
		 * after old existing VMAs are superseded with
		 * new ones from image file.
		 */
		ret |= restore_self_exe_late(args);
	} else
		sys_close(args->fd_exe_link);

	if (ret)
		goto core_restore_end;

	/*
	 * We need to prepare a valid sigframe here, so
	 * after sigreturn the kernel will pick up the
	 * registers from the frame, set them up and
	 * finally pass execution to the new IP.
	 */
	rt_sigframe = (void *)args->t->mem_zone.rt_sigframe;

	if (restore_thread_common(rt_sigframe, args->t))
		goto core_restore_end;

	/*
	 * Threads restoration. This requires some more comments. This
	 * restorer routine and thread restorer routine has the following
	 * memory map, prepared by a caller code.
	 *
	 * | <-- low addresses                                          high addresses --> |
	 * +-------------------------------------------------------+-----------------------+
	 * | this proc body | own stack | rt_sigframe space | thread restore zone   |
	 * +-------------------------------------------------------+-----------------------+
	 *
	 * where each thread restore zone is the following
	 *
	 * | <-- low addresses                                     high addresses --> |
	 * +--------------------------------------------------------------------------+
	 * | thread restore proc | thread1 stack | thread1 rt_sigframe |
	 * +--------------------------------------------------------------------------+
	 */

	if (args->nr_threads > 1) {
		struct thread_restore_args *thread_args = args->thread_args;
		long clone_flags = CLONE_VM | CLONE_FILES | CLONE_SIGHAND	|
				   CLONE_THREAD | CLONE_SYSVSEM;
		long last_pid_len;
		long parent_tid;
		int i, fd;

		fd = sys_openat(args->proc_fd, LAST_PID_PATH, O_RDWR, 0);
		if (fd < 0) {
			pr_err("can't open last pid fd %d\n", fd);
			goto core_restore_end;
		}

		ret = sys_flock(fd, LOCK_EX);
		if (ret) {
			pr_err("Can't lock last_pid %d\n", fd);
			sys_close(fd);
			goto core_restore_end;
		}

		for (i = 0; i < args->nr_threads; i++) {
			char last_pid_buf[16], *s;

			/* skip self */
			if (thread_args[i].pid == args->t->pid)
				continue;

			new_sp = restorer_stack(thread_args + i);
			last_pid_len = vprint_num(last_pid_buf, sizeof(last_pid_buf), thread_args[i].pid - 1, &s);
			sys_lseek(fd, 0, SEEK_SET);
			ret = sys_write(fd, s, last_pid_len);
			if (ret < 0) {
				pr_err("Can't set last_pid %ld/%s\n", ret, last_pid_buf);
				sys_close(fd);
				goto core_restore_end;
			}

			/*
			 * To achieve functionality like libc's clone()
			 * we need a pure assembly here, because clone()'ed
			 * thread will run with own stack and we must not
			 * have any additional instructions... oh, dear...
			 */

			RUN_CLONE_RESTORE_FN(ret, clone_flags, new_sp, parent_tid, thread_args, args->clone_restore_fn);
		}

		ret = sys_flock(fd, LOCK_UN);
		if (ret) {
			pr_err("Can't unlock last_pid %ld\n", ret);
			sys_close(fd);
			goto core_restore_end;
		}

		sys_close(fd);
	}

	restore_rlims(args);

	ret = create_posix_timers(args);
	if (ret < 0) {
		pr_err("Can't restore posix timers %ld\n", ret);
		goto core_restore_end;
	}

	ret = timerfd_arm(args);
	if (ret < 0) {
		pr_err("Can't restore timerfd %ld\n", ret);
		goto core_restore_end;
	}

	pr_info("%ld: Restored\n", sys_getpid());

	restore_finish_stage(CR_STATE_RESTORE);

	if (wait_zombies(args) < 0)
		goto core_restore_end;

	if (wait_helpers(args) < 0)
		goto core_restore_end;

	ksigfillset(&to_block);
	ret = sys_sigprocmask(SIG_SETMASK, &to_block, NULL, sizeof(k_rtsigset_t));
	if (ret) {
		pr_err("Unable to block signals %ld\n", ret);
		goto core_restore_end;
	}

	sys_sigaction(SIGCHLD, &args->sigchld_act, NULL, sizeof(k_rtsigset_t));

	ret = restore_signals(args->siginfo, args->siginfo_n, true);
	if (ret)
		goto core_restore_end;

	ret = restore_signals(args->t->siginfo, args->t->siginfo_n, false);
	if (ret)
		goto core_restore_end;

	restore_finish_stage(CR_STATE_RESTORE_SIGCHLD);

	rst_tcp_socks_all(args);

	/*
	 * Writing to last-pid is CAP_SYS_ADMIN protected,
	 * turning off TCP repair is CAP_SYS_NED_ADMIN protected,
	 * thus restore* creds _after_ all of the above.
	 */

	ret = restore_creds(&args->creds, args->proc_fd);
	ret = ret || restore_dumpable_flag(&args->mm);
	ret = ret || restore_pdeath_sig(args->t);

	futex_set_and_wake(&thread_inprogress, args->nr_threads);

	/*
	 * We have to close the log before restoring seccomp, because
	 * SECCOMP_MODE_STRICT blocks close().
	 */
	if (args->seccomp_mode != SECCOMP_MODE_DISABLED)
		pr_info("restoring seccomp mode %d for %ld\n", args->seccomp_mode, sys_getpid());

	restore_finish_stage(CR_STATE_RESTORE_CREDS);

	if (ret)
		BUG();

	/* Wait until children stop to use args->task_entries */
	futex_wait_while_gt(&thread_inprogress, 1);

	sys_close(args->proc_fd);
	log_set_fd(-1);

	/*
	 * The code that prepared the itimers makes shure the
	 * code below doesn't fail due to bad timing values.
	 */

#define itimer_armed(args, i)				\
		(args->itimers[i].it_interval.tv_sec ||	\
		 args->itimers[i].it_interval.tv_usec)

	if (itimer_armed(args, 0))
		sys_setitimer(ITIMER_REAL, &args->itimers[0], NULL);
	if (itimer_armed(args, 1))
		sys_setitimer(ITIMER_VIRTUAL, &args->itimers[1], NULL);
	if (itimer_armed(args, 2))
		sys_setitimer(ITIMER_PROF, &args->itimers[2], NULL);

	restore_posix_timers(args);

	restore_seccomp(args);

	sys_munmap(args->rst_mem, args->rst_mem_size);

	/*
	 * Sigframe stack.
	 */
	new_sp = (long)rt_sigframe + SIGFRAME_OFFSET;

	/*
	 * Prepare the stack and call for sigreturn,
	 * pure assembly since we don't need any additional
	 * code insns from gcc.
	 */
	rst_sigreturn(new_sp);

core_restore_end:
	futex_abort_and_wake(&task_entries->nr_in_progress);
	pr_err("Restorer fail %ld\n", sys_getpid());
	sys_exit_group(1);
	return -1;
}
