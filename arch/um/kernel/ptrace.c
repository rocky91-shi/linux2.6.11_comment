/* 
 * Copyright (C) 2000 Jeff Dike (jdike@karaya.com)
 * Licensed under the GPL
 */

#include "linux/sched.h"
#include "linux/mm.h"
#include "linux/errno.h"
#include "linux/smp_lock.h"
#include "linux/security.h"
#include "linux/ptrace.h"
#ifdef CONFIG_PROC_MM
#include "linux/proc_mm.h"
#endif
#include "asm/ptrace.h"
#include "asm/uaccess.h"
#include "kern_util.h"
#include "ptrace_user.h"
#include "skas_ptrace.h"

/*
 * Called by kernel/ptrace.c when detaching..
 */
void ptrace_disable(struct task_struct *child)
{ 
	child->ptrace &= ~PT_DTRACE;
	child->thread.singlestep_syscall = 0;
}

long sys_ptrace(long request, long pid, long addr, long data)
{
	struct task_struct *child;
	int i, ret;

	lock_kernel();
	ret = -EPERM;
	if (request == PTRACE_TRACEME) {
		/* are we already being traced? */
		if (current->ptrace & PT_PTRACED)
			goto out;

		ret = security_ptrace(current->parent, current);
		if (ret)
 			goto out;

		/* set the ptrace bit in the process flags. */
		current->ptrace |= PT_PTRACED;
		ret = 0;
		goto out;
	}
	ret = -ESRCH;
	read_lock(&tasklist_lock);
	child = find_task_by_pid(pid);
	if (child)
		get_task_struct(child);
	read_unlock(&tasklist_lock);
	if (!child)
		goto out;

	ret = -EPERM;
	if (pid == 1)		/* you may not mess with init */
		goto out_tsk;

	if (request == PTRACE_ATTACH) {
		ret = ptrace_attach(child);
		goto out_tsk;
	}

	ret = ptrace_check_attach(child, request == PTRACE_KILL);
	if (ret < 0)
		goto out_tsk;

	switch (request) {
		/* when I and D space are separate, these will need to be fixed. */
	case PTRACE_PEEKTEXT: /* read word at location addr. */ 
	case PTRACE_PEEKDATA: {
		unsigned long tmp;
		int copied;

		ret = -EIO;
		copied = access_process_vm(child, addr, &tmp, sizeof(tmp), 0);
		if (copied != sizeof(tmp))
			break;
		ret = put_user(tmp, (unsigned long __user *) data);
		break;
	}

	/* read the word at location addr in the USER area. */
	case PTRACE_PEEKUSR: {
		unsigned long tmp;

		ret = -EIO;
		if ((addr & 3) || addr < 0) 
			break;

		tmp = 0;  /* Default return condition */
		if(addr < MAX_REG_OFFSET){
			tmp = getreg(child, addr);
		}
		else if((addr >= offsetof(struct user, u_debugreg[0])) &&
			(addr <= offsetof(struct user, u_debugreg[7]))){
			addr -= offsetof(struct user, u_debugreg[0]);
			addr = addr >> 2;
			tmp = child->thread.arch.debugregs[addr];
		}
		ret = put_user(tmp, (unsigned long __user *) data);
		break;
	}

	/* when I and D space are separate, this will have to be fixed. */
	case PTRACE_POKETEXT: /* write the word at location addr. */
	case PTRACE_POKEDATA:
		ret = -EIO;
		if (access_process_vm(child, addr, &data, sizeof(data), 
				      1) != sizeof(data))
			break;
		ret = 0;
		break;

	case PTRACE_POKEUSR: /* write the word at location addr in the USER area */
		ret = -EIO;
		if ((addr & 3) || addr < 0)
			break;

		if (addr < MAX_REG_OFFSET) {
			ret = putreg(child, addr, data);
			break;
		}
#if 0 /* XXX x86_64 */
		else if((addr >= offsetof(struct user, u_debugreg[0])) &&
			(addr <= offsetof(struct user, u_debugreg[7]))){
			  addr -= offsetof(struct user, u_debugreg[0]);
			  addr = addr >> 2;
			  if((addr == 4) || (addr == 5)) break;
			  child->thread.arch.debugregs[addr] = data;
			  ret = 0;
		}
#endif

		break;

	case PTRACE_SYSCALL: /* continue and stop at next (return from) syscall */
	case PTRACE_CONT: { /* restart after signal. */
		ret = -EIO;
		if ((unsigned long) data > _NSIG)
			break;

		child->ptrace &= ~PT_DTRACE;
		child->thread.singlestep_syscall = 0;
		if (request == PTRACE_SYSCALL) {
			set_tsk_thread_flag(child, TIF_SYSCALL_TRACE);
		}
		else {
			clear_tsk_thread_flag(child, TIF_SYSCALL_TRACE);
		}
		child->exit_code = data;
		wake_up_process(child);
		ret = 0;
		break;
	}

/*
 * make the child exit.  Best I can do is send it a sigkill. 
 * perhaps it should be put in the status that it wants to 
 * exit.
 */
	case PTRACE_KILL: {
		ret = 0;
		if (child->exit_state == EXIT_ZOMBIE)	/* already dead */
			break;

		child->ptrace &= ~PT_DTRACE;
		child->thread.singlestep_syscall = 0;
		child->exit_code = SIGKILL;
		wake_up_process(child);
		break;
	}

	case PTRACE_SINGLESTEP: {  /* set the trap flag. */
		ret = -EIO;
		if ((unsigned long) data > _NSIG)
			break;
		clear_tsk_thread_flag(child, TIF_SYSCALL_TRACE);
		child->ptrace |= PT_DTRACE;
		child->thread.singlestep_syscall = 0;
		child->exit_code = data;
		/* give it a chance to run. */
		wake_up_process(child);
		ret = 0;
		break;
	}

	case PTRACE_DETACH:
		/* detach a process that was attached. */
		ret = ptrace_detach(child, data);
 		break;

#ifdef PTRACE_GETREGS
	case PTRACE_GETREGS: { /* Get all gp regs from the child. */
	  	if (!access_ok(VERIFY_WRITE, (unsigned long *)data, 
			       MAX_REG_OFFSET)) {
			ret = -EIO;
			break;
		}
		for ( i = 0; i < MAX_REG_OFFSET; i += sizeof(long) ) {
			__put_user(getreg(child, i),
				   (unsigned long __user *) data);
			data += sizeof(long);
		}
		ret = 0;
		break;
	}
#endif
#ifdef PTRACE_SETREGS
	case PTRACE_SETREGS: { /* Set all gp regs in the child. */
		unsigned long tmp = 0;
	  	if (!access_ok(VERIFY_READ, (unsigned *)data, 
			       MAX_REG_OFFSET)) {
			ret = -EIO;
			break;
		}
		for ( i = 0; i < MAX_REG_OFFSET; i += sizeof(long) ) {
			__get_user(tmp, (unsigned long __user *) data);
			putreg(child, i, tmp);
			data += sizeof(long);
		}
		ret = 0;
		break;
	}
#endif
#ifdef PTRACE_GETFPREGS
	case PTRACE_GETFPREGS: /* Get the child FPU state. */
		ret = get_fpregs(data, child);
		break;
#endif
#ifdef PTRACE_SETFPREGS
	case PTRACE_SETFPREGS: /* Set the child FPU state. */
	        ret = set_fpregs(data, child);
		break;
#endif
#ifdef PTRACE_GETFPXREGS
	case PTRACE_GETFPXREGS: /* Get the child FPU state. */
		ret = get_fpxregs(data, child);
		break;
#endif
#ifdef PTRACE_SETFPXREGS
	case PTRACE_SETFPXREGS: /* Set the child FPU state. */
		ret = set_fpxregs(data, child);
		break;
#endif
	case PTRACE_FAULTINFO: {
		struct ptrace_faultinfo fault;

		fault = ((struct ptrace_faultinfo) 
			{ .is_write	= child->thread.err,
			  .addr		= child->thread.cr2 });
		ret = copy_to_user((unsigned long __user *) data, &fault,
				   sizeof(fault));
		if(ret)
			break;
		break;
	}
	case PTRACE_SIGPENDING:
		ret = copy_to_user((unsigned long __user *) data,
				   &child->pending.signal,
				   sizeof(child->pending.signal));
		break;

	case PTRACE_LDT: {
		struct ptrace_ldt ldt;

		if(copy_from_user(&ldt, (unsigned long __user *) data,
				  sizeof(ldt))){
			ret = -EIO;
			break;
		}

		/* This one is confusing, so just punt and return -EIO for 
		 * now
		 */
		ret = -EIO;
		break;
	}
#ifdef CONFIG_PROC_MM
	case PTRACE_SWITCH_MM: {
		struct mm_struct *old = child->mm;
		struct mm_struct *new = proc_mm_get_mm(data);

		if(IS_ERR(new)){
			ret = PTR_ERR(new);
			break;
		}

		atomic_inc(&new->mm_users);
		child->mm = new;
		child->active_mm = new;
		mmput(old);
		ret = 0;
		break;
	}
#endif
	default:
		ret = ptrace_request(child, request, addr, data);
		break;
	}
 out_tsk:
	put_task_struct(child);
 out:
	unlock_kernel();
	return ret;
}

void send_sigtrap(struct task_struct *tsk, union uml_pt_regs *regs,
		  int error_code)
{
	struct siginfo info;

	memset(&info, 0, sizeof(info));
	info.si_signo = SIGTRAP;
	info.si_code = TRAP_BRKPT;

	/* User-mode eip? */
	info.si_addr = UPT_IS_USER(regs) ? (void __user *) UPT_IP(regs) : NULL;

	/* Send us the fakey SIGTRAP */
	force_sig_info(SIGTRAP, &info, tsk);
}

/* XXX Check PT_DTRACE vs TIF_SINGLESTEP for singlestepping check and
 * PT_PTRACED vs TIF_SYSCALL_TRACE for syscall tracing check
 */
void syscall_trace(union uml_pt_regs *regs, int entryexit)
{
	int is_singlestep = (current->ptrace & PT_DTRACE) && entryexit;
	int tracesysgood;

	if (unlikely(current->audit_context)) {
		if (!entryexit)
			audit_syscall_entry(current, regs->orig_eax,
					    regs->ebx, regs->ecx,
					    regs->edx, regs->esi);
		else
			audit_syscall_exit(current, regs->eax);
	}

	/* Fake a debug trap */
	if (is_singlestep)
		send_sigtrap(current, regs, 0);

	if (!test_thread_flag(TIF_SYSCALL_TRACE))
		return;

	if (!(current->ptrace & PT_PTRACED))
		return;

	/* the 0x80 provides a way for the tracing parent to distinguish
	   between a syscall stop and SIGTRAP delivery */
	tracesysgood = (current->ptrace & PT_TRACESYSGOOD);
	ptrace_notify(SIGTRAP | (tracesysgood ? 0x80 : 0));

	if (entryexit) /* force do_signal() --> is_syscall() */
		set_thread_flag(TIF_SIGPENDING);

	/* this isn't the same as continuing with a signal, but it will do
	 * for normal use.  strace only continues with a signal if the
	 * stopping signal is not SIGTRAP.  -brl
	 */
	if (current->exit_code) {
		send_sig(current->exit_code, current, 1);
		current->exit_code = 0;
	}
}

/*
 * Overrides for Emacs so that we follow Linus's tabbing style.
 * Emacs will notice this stuff at the end of the file and automatically
 * adjust the settings for this buffer only.  This must remain at the end
 * of the file.
 * ---------------------------------------------------------------------------
 * Local variables:
 * c-file-style: "linux"
 * End:
 */
