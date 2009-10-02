#ifdef __SHARC__
//#pragma nosharc
#endif

#include <arch/mmu.h>
#include <arch/x86.h>
#include <arch/arch.h>
#include <arch/console.h>
#include <arch/apic.h>
#include <ros/common.h>
#include <smp.h>
#include <assert.h>
#include <pmap.h>
#include <trap.h>
#include <monitor.h>
#include <process.h>
#include <stdio.h>

#include <syscall.h>

taskstate_t RO ts;

/* Interrupt descriptor table.  (Must be built at run time because
 * shifted function addresses can't be represented in relocation records.)
 */
// Aligned on an 8 byte boundary (SDM V3A 5-13)
gatedesc_t __attribute__ ((aligned (8))) (RO idt)[256] = { { 0 } };
pseudodesc_t RO idt_pd = {
	sizeof(idt) - 1, (uint32_t) idt
};

/* global handler table, used by core0 (for now).  allows the registration
 * of functions to be called when servicing an interrupt.  other cores
 * can set up their own later.
 */
#ifdef __IVY__
#pragma cilnoremove("iht_lock")
#endif
spinlock_t iht_lock;
handler_t TP(TV(t)) LCKD(&iht_lock) (RO interrupt_handlers)[NUM_INTERRUPT_HANDLERS];

static const char *NTS trapname(int trapno)
{
    // zra: excnames is SREADONLY because Ivy doesn't trust const
	static const char *NT const (RO excnames)[] = {
		"Divide error",
		"Debug",
		"Non-Maskable Interrupt",
		"Breakpoint",
		"Overflow",
		"BOUND Range Exceeded",
		"Invalid Opcode",
		"Device Not Available",
		"Double Fault",
		"Coprocessor Segment Overrun",
		"Invalid TSS",
		"Segment Not Present",
		"Stack Fault",
		"General Protection",
		"Page Fault",
		"(unknown trap)",
		"x87 FPU Floating-Point Error",
		"Alignment Check",
		"Machine-Check",
		"SIMD Floating-Point Exception"
	};

	if (trapno < sizeof(excnames)/sizeof(excnames[0]))
		return excnames[trapno];
	if (trapno == T_SYSCALL)
		return "System call";
	return "(unknown trap)";
}


void
idt_init(void)
{
	extern segdesc_t (RO gdt)[];

	// This table is made in trapentry.S by each macro in that file.
	// It is layed out such that the ith entry is the ith's traphandler's
	// (uint32_t) trap addr, then (uint32_t) trap number
	struct trapinfo { uint32_t trapaddr; uint32_t trapnumber; };
	extern struct trapinfo (BND(__this,trap_tbl_end) RO trap_tbl)[];
	extern struct trapinfo (SNT RO trap_tbl_end)[];
	int i, trap_tbl_size = trap_tbl_end - trap_tbl;
	extern void ISR_default(void);

	// set all to default, to catch everything
	for(i = 0; i < 256; i++)
		ROSETGATE(idt[i], 0, GD_KT, &ISR_default, 0);

	// set all entries that have real trap handlers
	// we need to stop short of the last one, since the last is the default
	// handler with a fake interrupt number (500) that is out of bounds of
	// the idt[]
	// if we set these to trap gates, be sure to handle the IRQs separately
	// and we might need to break our pretty tables
	for(i = 0; i < trap_tbl_size - 1; i++)
		ROSETGATE(idt[trap_tbl[i].trapnumber], 0, GD_KT, trap_tbl[i].trapaddr, 0);

	// turn on syscall handling and other user-accessible ints
	// DPL 3 means this can be triggered by the int instruction
	// STS_TG32 sets the IDT type to a Trap Gate (interrupts enabled)
	idt[T_SYSCALL].gd_dpl = SINIT(3);
	idt[T_SYSCALL].gd_type = SINIT(STS_TG32);
	idt[T_BRKPT].gd_dpl = SINIT(3);

	// Setup a TSS so that we get the right stack
	// when we trap to the kernel.
	ts.ts_esp0 = SINIT(KSTACKTOP);
	ts.ts_ss0 = SINIT(GD_KD);

	// Initialize the TSS field of the gdt.
	SEG16ROINIT(gdt[GD_TSS >> 3],STS_T32A, (uint32_t)(&ts),sizeof(taskstate_t),0);
	//gdt[GD_TSS >> 3] = (segdesc_t)SEG16(STS_T32A, (uint32_t) (&ts),
	//				   sizeof(taskstate_t), 0);
	gdt[GD_TSS >> 3].sd_s = SINIT(0);

	// Load the TSS
	ltr(GD_TSS);

	// Load the IDT
	asm volatile("lidt idt_pd");

	// This will go away when we start using the IOAPIC properly
	pic_remap();
	// set LINT0 to receive ExtINTs (KVM's default).  At reset they are 0x1000.
	write_mmreg32(LAPIC_LVT_LINT0, 0x700);
	// mask it to shut it up for now
	mask_lapic_lvt(LAPIC_LVT_LINT0);
	// and turn it on
	lapic_enable();
}

void
print_regs(push_regs_t *regs)
{
	cprintf("  edi  0x%08x\n", regs->reg_edi);
	cprintf("  esi  0x%08x\n", regs->reg_esi);
	cprintf("  ebp  0x%08x\n", regs->reg_ebp);
	cprintf("  oesp 0x%08x\n", regs->reg_oesp);
	cprintf("  ebx  0x%08x\n", regs->reg_ebx);
	cprintf("  edx  0x%08x\n", regs->reg_edx);
	cprintf("  ecx  0x%08x\n", regs->reg_ecx);
	cprintf("  eax  0x%08x\n", regs->reg_eax);
}

void
print_trapframe(trapframe_t *tf)
{
	static spinlock_t ptf_lock;

	spin_lock_irqsave(&ptf_lock);
	cprintf("TRAP frame at %p on core %d\n", tf, core_id());
	print_regs(&tf->tf_regs);
	cprintf("  es   0x----%04x\n", tf->tf_es);
	cprintf("  ds   0x----%04x\n", tf->tf_ds);
	cprintf("  trap 0x%08x %s\n", tf->tf_trapno, trapname(tf->tf_trapno));
	cprintf("  err  0x%08x\n", tf->tf_err);
	cprintf("  eip  0x%08x\n", tf->tf_eip);
	cprintf("  cs   0x----%04x\n", tf->tf_cs);
	cprintf("  flag 0x%08x\n", tf->tf_eflags);
	cprintf("  esp  0x%08x\n", tf->tf_esp);
	cprintf("  ss   0x----%04x\n", tf->tf_ss);
	spin_unlock_irqsave(&ptf_lock);
}

static void
trap_dispatch(trapframe_t *tf)
{
	// Handle processor exceptions.
	switch(tf->tf_trapno) {
		case T_BRKPT:
			while (1)
				monitor(tf);
			// never get to this
			assert(0);
		case T_PGFLT:
			page_fault_handler(tf);
			break;
		case T_SYSCALL:
			// check for userspace, for now
			assert(tf->tf_cs != GD_KT);
			// Note we pass the tf ptr along, in case syscall needs to block
			tf->tf_regs.reg_eax =
				syscall(current, tf, tf->tf_regs.reg_eax, tf->tf_regs.reg_edx,
				        tf->tf_regs.reg_ecx, tf->tf_regs.reg_ebx,
				        tf->tf_regs.reg_edi, tf->tf_regs.reg_esi);
			proc_startcore(current, tf); // Note the comment in syscall.c
			break;
		default:
			// Unexpected trap: The user process or the kernel has a bug.
			print_trapframe(tf);
			if (tf->tf_cs == GD_KT)
				panic("Damn Damn!  Unhandled trap in the kernel!");
			else {
				warn("Unexpected trap from userspace");
				proc_destroy(current);
				return;
			}
	}
	return;
}

void
env_push_ancillary_state(env_t* e)
{
	// Here's where you'll save FP/MMX/XMM regs
}

void
env_pop_ancillary_state(env_t* e)
{
	// Here's where you'll restore FP/MMX/XMM regs
}

void
trap(trapframe_t *tf)
{
	//printk("Incoming TRAP frame on core %d at %p\n", core_id(), tf);

	// TODO: do this once we know we are are not returning to the current
	// context.  doing it now is safe. (HSS)
	// we also need to sort this wrt multiple contexts
	env_push_ancillary_state(current);

	if ((tf->tf_cs & ~3) != GD_UT && (tf->tf_cs & ~3) != GD_KT) {
		print_trapframe(tf);
		panic("Trapframe with invalid CS!");
	}

	/* If we're vcore0, save the trapframe in the proc's env_tf.  make sure
	 * silly state is sorted (HSS). This applies to any RUNNING_* state. */
	if (current->vcoremap[0] == core_id()) {
		current->env_tf = *tf;
		tf = &current->env_tf;
	}
	// Dispatch based on what type of trap occurred
	trap_dispatch(tf);

	// should this be if == 3?  Sort out later when we handle traps.
	// so far we never get here.  managed to get here once when a MCP took two
	// page faults and both called proc_destroy().  one of which returned since
	// it was already DYING (though it should have waited for it's death IPI).
	assert(0);
	// Return to the current environment, which should be runnable.
	proc_startcore(current, tf); // Note the comment in syscall.c
}

void
irq_handler(trapframe_t *tf)
{
	//if (core_id())
	//	cprintf("Incoming IRQ, ISR: %d on core %d\n", tf->tf_trapno, core_id());
	// merge this with alltraps?  other than the EOI... or do the same in all traps

	// TODO: do this once we know we are are not returning to the current
	// context.  doing it now is safe. (HSS)
	env_push_ancillary_state(current);

	extern handler_wrapper_t (RO handler_wrappers)[NUM_HANDLER_WRAPPERS];

	// determine the interrupt handler table to use.  for now, pick the global
	handler_t TP(TV(t)) LCKD(&iht_lock) * handler_tbl = interrupt_handlers;

	if (handler_tbl[tf->tf_trapno].isr != 0)
		handler_tbl[tf->tf_trapno].isr(tf, handler_tbl[tf->tf_trapno].data);
	// if we're a general purpose IPI function call, down the cpu_list
	if ((I_SMP_CALL0 <= tf->tf_trapno) && (tf->tf_trapno <= I_SMP_CALL_LAST))
		down_checklist(handler_wrappers[tf->tf_trapno & 0x0f].cpu_list);

	// Send EOI.  might want to do this in assembly, and possibly earlier
	// This is set up to work with an old PIC for now
	// Convention is that all IRQs between 32 and 47 are for the PIC.
	// All others are LAPIC (timer, IPIs, perf, non-ExtINT LINTS, etc)
	// For now, only 235-255 are available
	assert(tf->tf_trapno >= 32); // slows us down, but we should never have this
	
	lapic_send_eoi();
	
	/*
	//Old PIC relatd code. Should be gone for good, but leaving it just incase.
	if (tf->tf_trapno < 48)
		pic_send_eoi(tf->tf_trapno - PIC1_OFFSET);
	else
		lapic_send_eoi();
	*/

}

void
register_interrupt_handler(handler_t TP(TV(t)) table[],
                           uint8_t int_num, poly_isr_t handler, TV(t) data)
{
	table[int_num].isr = handler;
	table[int_num].data = data;
}

void
page_fault_handler(trapframe_t *tf)
{
	uint32_t fault_va;

	// Read processor's CR2 register to find the faulting address
	fault_va = rcr2();

	// Handle kernel-mode page faults.

	// TODO - one day, we'll want to handle this.
	if ((tf->tf_cs & 3) == 0) {
		print_trapframe(tf);
		panic("Page Fault in the Kernel at 0x%08x!", fault_va);
	}

	// We've already handled kernel-mode exceptions, so if we get here,
	// the page fault happened in user mode.

	// Call the environment's page fault upcall, if one exists.  Set up a
	// page fault stack frame on the user exception stack (below
	// UXSTACKTOP), then branch to current->env_pgfault_upcall.
	//
	// The page fault upcall might cause another page fault, in which case
	// we branch to the page fault upcall recursively, pushing another
	// page fault stack frame on top of the user exception stack.
	//
	// The trap handler needs one word of scratch space at the top of the
	// trap-time stack in order to return.  In the non-recursive case, we
	// don't have to worry about this because the top of the regular user
	// stack is free.  In the recursive case, this means we have to leave
	// an extra word between the current top of the exception stack and
	// the new stack frame because the exception stack _is_ the trap-time
	// stack.
	//
	// If there's no page fault upcall, the environment didn't allocate a
	// page for its exception stack, or the exception stack overflows,
	// then destroy the environment that caused the fault.
	//
	// Hints:
	//   user_mem_assert() and env_run() are useful here.
	//   To change what the user environment runs, modify 'current->env_tf'
	//   (the 'tf' variable points at 'current->env_tf').

	// LAB 4: Your code here.

	// Destroy the environment that caused the fault.
	cprintf("[%08x] user fault va %08x ip %08x from core %d\n",
		current->env_id, fault_va, tf->tf_eip, core_id());
	print_trapframe(tf);
	proc_destroy(current);
}

void sysenter_init(void)
{
	write_msr(MSR_IA32_SYSENTER_CS, GD_KT);
	write_msr(MSR_IA32_SYSENTER_ESP, ts.ts_esp0);
	write_msr(MSR_IA32_SYSENTER_EIP, (uint32_t) &sysenter_handler);
}

/* This is called from sysenter's asm, with the tf on the kernel stack. */
void sysenter_callwrapper(struct Trapframe *tf)
{
	/* If we're vcore0, save the trapframe in the proc's env_tf.  make sure
	 * silly state is sorted (HSS). This applies to any RUNNING_* state. */
	if (current->vcoremap[0] == core_id()) {
		current->env_tf = *tf;
		tf = &current->env_tf;
	}
	// Note we pass the tf ptr along, in case syscall needs to block
	tf->tf_regs.reg_eax = (intreg_t) syscall(current, tf,
	                                         tf->tf_regs.reg_eax,
	                                         tf->tf_regs.reg_edx,
	                                         tf->tf_regs.reg_ecx,
	                                         tf->tf_regs.reg_ebx,
	                                         tf->tf_regs.reg_edi,
	                                         0);
	/*
	 * careful here - we need to make sure that this current is the right
	 * process, which could be weird if the syscall blocked.  it would need to
	 * restore the proper value in current before returning to here.
	 * likewise, tf could be pointing to random gibberish.
	 */
	proc_startcore(current, tf);
}

uint32_t send_active_message(uint32_t dst, amr_t pc,
                             TV(a0t) arg0, TV(a1t) arg1, TV(a2t) arg2)
{
	error_t retval = -EBUSY;
	assert(pc);
	spin_lock_irqsave(&per_cpu_info[dst].amsg_lock);
	size_t current_amsg = per_cpu_info[dst].amsg_current;
	// If there's a PC there, then that means it's an outstanding message
	FOR_CIRC_BUFFER(current_amsg, NUM_ACTIVE_MESSAGES, i) {
		if (per_cpu_info[dst].active_msgs[i].pc)
			continue;
		per_cpu_info[dst].active_msgs[i].pc = pc;
		per_cpu_info[dst].active_msgs[i].arg0 = arg0;
		per_cpu_info[dst].active_msgs[i].arg1 = arg1;
		per_cpu_info[dst].active_msgs[i].arg2 = arg2;
		per_cpu_info[dst].active_msgs[i].srcid = core_id();
		retval = 0;
		break;
	}
	spin_unlock_irqsave(&per_cpu_info[dst].amsg_lock);
	// since we touched memory the other core will touch (the lock), we don't
	// need an wmb_f()
	if (!retval)
		send_ipi(dst, 0, I_ACTIVE_MSG);
	return retval;
}

/* Active message handler.  We don't want to block other AMs from coming in, so
 * we'll copy out the message and let go of the lock.  This won't return until
 * all pending AMs are executed.  If the PC is 0, then this was an extra IPI and
 * we already handled the message (or someone is sending IPIs without loading
 * the active message...)
 * Note that all of this happens from interrupt context, and interrupts are
 * currently disabled for this gate. */
void __active_message(trapframe_t *tf)
{
	per_cpu_info_t RO*myinfo = &per_cpu_info[core_id()];
	active_message_t amsg;

	lapic_send_eoi();
	while (1) { // will break out when we find an empty amsg
		/* Get the message */
		spin_lock_irqsave(&myinfo->amsg_lock);
		if (myinfo->active_msgs[myinfo->amsg_current].pc) {
			amsg = myinfo->active_msgs[myinfo->amsg_current];
			myinfo->active_msgs[myinfo->amsg_current].pc = 0;
			myinfo->amsg_current = (myinfo->amsg_current + 1) %
			                       NUM_ACTIVE_MESSAGES;
		} else { // was no PC in the current active message, meaning we do nothing
			spin_unlock_irqsave(&myinfo->amsg_lock);
			return;
		}
		/* In case the function doesn't return (which is common: __startcore,
		 * __death, etc), there is a chance we could lose an amsg.  We can only
		 * have up to two interrupts outstanding, and if we never return, we
		 * never deal with any other amsgs.  This extra IPI hurts performance
		 * but is only necessary if there is another outstanding message in the
		 * buffer, but makes sure we never miss out on an amsg. */
		if (myinfo->active_msgs[myinfo->amsg_current].pc)
			send_ipi(core_id(), 0, I_ACTIVE_MSG);
		spin_unlock_irqsave(&myinfo->amsg_lock);
		/* Execute the active message */
		amsg.pc(tf, amsg.srcid, amsg.arg0, amsg.arg1, amsg.arg2);
	}
}
