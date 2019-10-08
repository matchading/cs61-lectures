#include "kernel.hh"
#include "k-apic.hh"
#include "k-vmiter.hh"
#include <atomic>

// kernel.cc
//
//    This is the kernel.


// INITIAL PHYSICAL MEMORY LAYOUT
//
//  +-------------- Base Memory --------------+
//  v                                         v
// +-----+--------------------+----------------+--------------------+---------/
// |     | Kernel      Kernel |       :    I/O | App 1        App 1 | App 2
// |     | Code + Data  Stack |  ...  : Memory | Code + Data  Stack | Code ...
// +-----+--------------------+----------------+--------------------+---------/
// 0  0x40000              0x80000 0xA0000 0x100000             0x140000
//                                             ^
//                                             | \___ PROC_SIZE ___/
//                                      PROC_START_ADDR

#define PROC_SIZE 0x40000       // initial state only

proc ptable[NPROC];             // array of process descriptors
                                // Note that `ptable[0]` is never used.
proc* current;                  // pointer to currently executing proc

#define HZ 100                  // timer interrupt frequency (interrupts/sec)
static std::atomic<unsigned long> ticks; // # timer interrupts so far


// Memory state
//    Information about physical page with address `pa` is stored in
//    `pages[pa / PAGESIZE]`. In the handout code, each `pages` entry
//    holds an *owner*, which is 0 for free pages and non-zero for
//    allocated pages. You can change this as you see fit.

pageinfo pages[NPAGES];


[[noreturn]] void schedule();
[[noreturn]] void run(proc* p);
void exception(regstate* regs);
uintptr_t syscall(regstate* regs);


// kernel(command)
//    Initialize the hardware and processes and start running. The `command`
//    string is an optional string passed from the boot loader.

static void process_setup(pid_t pid, const char* program_name);

void kernel(const char* command) {
    // initialize hardware
    init_hardware();

    // clear screen
    console_clear();

    // initalize memory mapping:
    // all of physical memory is accessible except `nullptr`
    for (vmiter it(kernel_pagetable);
         it.va() < MEMSIZE_PHYSICAL;
         it += PAGESIZE) {
        if (it.va() != 0) {
            it.map(it.va(), PTE_P | PTE_W | PTE_U);
        } else {
            it.map(it.va(), 0);
        }
    }

    // set up process descriptors
    for (pid_t i = 0; i < NPROC; i++) {
        ptable[i].pid = i;
        ptable[i].state = P_FREE;
    }

    if (command && program_loader(command).present()== 0) {
        process_setup(1, command);
    } else {
        process_setup(1, "eve");
        process_setup(2, "alice");
    }

    // Switch to the first process using run()
    run(&ptable[1]);
}


// kalloc_physical_page(pa, owner)
//    Allocate the physical page at address `pa` for process `owner`.

void* kalloc_physical_page(uintptr_t pa, pid_t owner) {
    assert((pa & PAGEOFFMASK) == 0);

    if (allocatable_physical_address(pa)
        && pages[pa / PAGESIZE].owner == 0) {
        pages[pa / PAGESIZE].owner = owner;
        memset((void*) pa, 0, PAGESIZE);
        return (void*) pa;
    } else {
        return nullptr;
    }
}


// process_setup(pid, program_name)
//    Load application program `program_name` as process number `pid`.
//    This loads the application's code and data into memory, sets its
//    %rip and %rsp, gives it a stack page, and marks it as runnable.

void process_setup(pid_t pid, const char* program_name) {
    init_process(&ptable[pid], 0);

    // set up initial page table
    ptable[pid].pagetable = kernel_pagetable;

    // load the program
    program_loader loader(program_name);

    // allocate and map all memory
    for (loader.reset(); loader.present(); ++loader) {
        for (uintptr_t a = round_down(loader.va(), PAGESIZE);
             a < loader.va() + loader.size();
             a += PAGESIZE) {
            if (!kalloc_physical_page(a, pid)) {
                panic("Cannot allocate memory for process!");
            }
            vmiter(ptable[pid].pagetable, a).map(a, PTE_PWU);
        }
    }

    // copy instructions and data into place
    for (loader.reset(); loader.present(); ++loader) {
        memset((void*) loader.va(), 0, loader.size());
        memcpy((void*) loader.va(), loader.data(), loader.data_size());
    }

    // mark entry point
    ptable[pid].regs.reg_rip = loader.entry();

    // allocate stack
    uintptr_t stack_addr = PROC_START_ADDR + PROC_SIZE * pid - PAGESIZE;
    if (!kalloc_physical_page(stack_addr, pid)) {
        panic("Cannot allocate stack memory for process!");
    }
    vmiter(ptable[pid].pagetable, stack_addr).map(stack_addr, PTE_PWU);
    ptable[pid].regs.reg_rsp = stack_addr + PAGESIZE;

    // allow process to control interrupts
    ptable[pid].regs.reg_rflags |= EFLAGS_IOPL_3;

    // mark process as runnable
    ptable[pid].state = P_RUNNABLE;
}



// exception(regs)
//    Exception handler (for interrupts, traps, and faults).
//
//    The register values from exception time are stored in `regs`.
//    The processor responds to an exception by saving application state on
//    the kernel's stack, then jumping to kernel assembly code (in
//    k-exception.S). That code saves more registers on the kernel's stack,
//    then calls exception().
//
//    Note that hardware interrupts are disabled when the kernel is running.

void exception(regstate* regs) {
    // Copy the saved registers into the `current` process descriptor.
    current->regs = *regs;

    // It can be useful to log events using `log_printf`.
    // Events logged this way are stored in the host's `log.txt` file.
    /* log_printf("proc %d: exception %d at rip %p\n",
                current->pid, regs->reg_intno, regs->reg_rip); */

    // show the current cursor location
    console_show_cursor(cursorpos);

    // If Control-C was typed, exit the virtual machine.
    check_keyboard();


    // Actually handle the exception.
    switch (regs->reg_intno) {

    case INT_PF: {
        // Analyze faulting address and access type.
        uintptr_t addr = rdcr2();
        const char* entity = regs->reg_errcode & PFERR_USER
                ? "Process" : "Kernel";
        const char* operation = regs->reg_errcode & PFERR_WRITE
                ? "write" : "read";
        const char* problem = regs->reg_errcode & PFERR_PRESENT
                ? "protection" : "missing";

        panic("%s %d page fault on %p (%s %s, rip=%p)!\n",
              entity, current->pid, addr, operation, problem, regs->reg_rip);
    }

    default:
        panic("Unexpected exception %d!\n", regs->reg_intno);

    }


    // Return to the current process (or run something else).
    if (current->state == P_RUNNABLE) {
        run(current);
    } else {
        schedule();
    }
}


// syscall(regs)
//    System call handler.
//
//    The register values from system call time are stored in `regs`.
//    The return value, if any, is returned to the user process in `%rax`.
//
//    Note that hardware interrupts are disabled when the kernel is running.

uintptr_t syscall(regstate* regs) {
    // Copy the saved registers into the `current` process descriptor.
    current->regs = *regs;

    // It can be useful to log events using `log_printf`.
    // Events logged this way are stored in the host's `log.txt` file.
    /* log_printf("proc %d: syscall %d at rip %p\n",
                 current->pid, regs->reg_rax, regs->reg_rip); */

    // show the current cursor location
    console_show_cursor(cursorpos);

    // If Control-C was typed, exit the virtual machine.
    check_keyboard();


    // Actually handle the exception.
    switch (regs->reg_rax) {

    case SYSCALL_PANIC:
        panic(nullptr);         // does not return

    case SYSCALL_GETPID:
        return current->pid;

    case SYSCALL_YIELD:
        current->regs.reg_rax = 0;
        schedule();             // does not return

    case SYSCALL_GETSYSNAME: {
        const char* osname = "DemoOS 61.61";
        char* buf = (char*) current->regs.reg_rdi;
        strcpy(buf, osname);
        return 0;
    }

    default:
        panic("Unexpected system call %ld!\n", regs->reg_rax);

    }

    panic("Should not get here!\n");
}


// schedule
//    Pick the next process to run and then run it.
//    If there are no runnable processes, spins forever.

void schedule() {
    pid_t pid = current->pid;
    for (unsigned spins = 1; true; ++spins) {
        pid = (pid + 1) % NPROC;
        if (ptable[pid].state == P_RUNNABLE) {
            run(&ptable[pid]);
        }

        // If Control-C was typed, exit the virtual machine.
        check_keyboard();
    }
}


// run(p)
//    Run process `p`. This involves setting `current = p` and calling
//    `exception_return` to restore its page table and registers.

void run(proc* p) {
    assert(p->state == P_RUNNABLE);
    current = p;

    // Check the process's current pagetable.
    check_pagetable(p->pagetable);

    // This function is defined in k-exception.S. It restores the process's
    // registers then jumps back to user mode.
    exception_return(p->pagetable, &p->regs);

    // should never get here
    while (true) {
    }
}
