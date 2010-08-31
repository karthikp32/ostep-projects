#include "types.h"
#include "defs.h"
#include "param.h"
#include "mmu.h"
#include "proc.h"
#include "x86.h"

static void bootothers(void);
static void mpmain(void);
void jkstack(void)  __attribute__((noreturn));
void mainc(void);

// Bootstrap processor starts running C code here.
int
main(void)
{
  mpinit();        // collect info about this machine
  lapicinit(mpbcpu());
  ksegment();      // set up segments
  picinit();       // interrupt controller
  ioapicinit();    // another interrupt controller
  consoleinit();   // I/O devices & their interrupts
  uartinit();      // serial port
  kinit();         // initialize memory allocator
  jkstack();       // call mainc() on a properly-allocated stack 
}

void
jkstack(void)
{
  char *kstack = kalloc();
  if (!kstack)
    panic("jkstack\n");
  char *top = kstack + PGSIZE;
  asm volatile("movl %0,%%esp" : : "r" (top));
  asm volatile("call mainc");
  panic("jkstack");
}

void
mainc(void)
{
  cprintf("\ncpu%d: starting xv6\n\n", cpu->id);
  kvmalloc();      // initialize the kernel page table
  pinit();         // process table
  tvinit();        // trap vectors
  binit();         // buffer cache
  fileinit();      // file table
  iinit();         // inode cache
  ideinit();       // disk
  if(!ismp)
    timerinit();   // uniprocessor timer
  userinit();      // first user process
  bootothers();    // start other processors

  // Finish setting up this processor in mpmain.
  mpmain();
}

// Common CPU setup code.
// Bootstrap CPU comes here from mainc().
// Other CPUs jump here from bootother.S.
static void
mpmain(void)
{
  if(cpunum() != mpbcpu()) {
    ksegment();
    lapicinit(cpunum());
  }
  vmenable();        // turn on paging
  cprintf("cpu%d: starting\n", cpu->id);
  idtinit();       // load idt register
  xchg(&cpu->booted, 1);
  scheduler();     // start running processes
}

static void
bootothers(void)
{
  extern uchar _binary_bootother_start[], _binary_bootother_size[];
  uchar *code;
  struct cpu *c;
  char *stack;

  // Write bootstrap code to unused memory at 0x7000.  The linker has
  // placed the start of bootother.S there.
  code = (uchar *) 0x7000;
  memmove(code, _binary_bootother_start, (uint)_binary_bootother_size);

  for(c = cpus; c < cpus+ncpu; c++){
    if(c == cpus+cpunum())  // We've started already.
      continue;

    // Fill in %esp, %eip and start code on cpu.
    stack = kalloc();
    *(void**)(code-4) = stack + KSTACKSIZE;
    *(void**)(code-8) = mpmain;
    lapicstartap(c->id, (uint)code);

    // Wait for cpu to finish mpmain()
    while(c->booted == 0)
      ;
  }
}

