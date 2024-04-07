#include "types.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "mmu.h"
#include "proc.h"
#include "x86.h"
#include "traps.h"
#include "spinlock.h"
#include "mmap.h"
#include "sleeplock.h"
#include "fs.h"
#include "file.h"
// Interrupt descriptor table (shared by all CPUs).
struct gatedesc idt[256];
extern uint vectors[];  // in vectors.S: array of 256 entry pointers
struct spinlock tickslock;
uint ticks;


int mappages(pde_t *pgdir, void *va, uint size, uint pa, int perm);
pte_t *walkpgdir(pde_t *pgdir, const void *va, int alloc);

void
tvinit(void)
{
  int i;

  for(i = 0; i < 256; i++)
    SETGATE(idt[i], 0, SEG_KCODE<<3, vectors[i], 0);
  SETGATE(idt[T_SYSCALL], 1, SEG_KCODE<<3, vectors[T_SYSCALL], DPL_USER);

  initlock(&tickslock, "time");
}

void
idtinit(void)
{
  lidt(idt, sizeof(idt));
}

void guard(struct proc *p)
{
  p->killed = 1;
}

//PAGEBREAK: 41
void
trap(struct trapframe *tf)
{
  if(tf->trapno == T_SYSCALL){
    if(myproc()->killed)
      exit();
    myproc()->tf = tf;
    syscall();
    if(myproc()->killed)
      exit();
    return;
  }

  switch(tf->trapno){
  case T_IRQ0 + IRQ_TIMER:
    if(cpuid() == 0){
      acquire(&tickslock);
      ticks++;
      wakeup(&ticks);
      release(&tickslock);
    }
    lapiceoi();
    break;
  case T_IRQ0 + IRQ_IDE:
    ideintr();
    lapiceoi();
    break;
  case T_IRQ0 + IRQ_IDE+1:
    // Bochs generates spurious IDE1 interrupts.
    break;
  case T_IRQ0 + IRQ_KBD:
    kbdintr();
    lapiceoi();
    break;
  case T_IRQ0 + IRQ_COM1:
    uartintr();
    lapiceoi();
    break;
  case T_IRQ0 + 7:
  case T_IRQ0 + IRQ_SPURIOUS:
    cprintf("cpu%d: spurious interrupt at %x:%x\n",
            cpuid(), tf->cs, tf->eip);
    lapiceoi();
    break;
  case T_PGFLT:

    struct proc *p = myproc();    
    uint addr = rcr2();
    if (addr < mmaps_START || addr + PGSIZE > mmaps_END){
      guard(p);
      break;
    }
    
    if (!checkMemAvailable(p->pgdir, (void *)addr + PGSIZE, PGSIZE)){
      guard(p);
      break;
    }
    
    if (checkMemAvailable(p->pgdir, (void *)addr - PGSIZE, PGSIZE)){
      myproc()->killed = 1;
      break;
    }

    uint a = PGROUNDDOWN(addr);
    struct mmaps *mmaps_g = 0;
    struct mmaps *mmaps_n = 0;
    int nextIndex = 0;
    
    for (int i = 0; i < MAXMAPS; i++)
    {
      if (i < (MAXMAPS-2) && p->mmap_s[i + 2].valid == 1)
      {
        mmaps_g = 0;
        break;
      }
      if (i != MAXMAPS-1)
      {
        mmaps_n = &p->mmap_s[i + 1];
        nextIndex = i + 1;
      }
      struct mmaps *cur = &p->mmap_s[i];
      if ((uint)cur->end <= addr && addr < (uint)cur->end + PGSIZE)
      {
        mmaps_g = cur;
        break;
      }
    }
    if (!mmaps_g || !(mmaps_g->flags & MAP_GROWSUP))
    {
      guard(p);

      break;
    }
    // Allocation and Mapping of New Memorm
    char *mem = kalloc();
    if (mem == 0)

    { cprintf("Segmentation Fault\n");
      deallocuvm(p->pgdir, a, a);
      myproc()->killed = 1;
      break;
    }
    memset(mem, 0, PGSIZE);
    mappages(p->pgdir, (void *)a, PGSIZE, V2P(mem), PTE_W | PTE_U);
    
    mmaps_g->next = nextIndex;
    mmaps_n->addr = mmaps_g->addr + PGSIZE;
    mmaps_n->end = mmaps_g->end + PGSIZE;
    mmaps_n->prot = mmaps_g->prot;
    mmaps_n->flags = mmaps_g->flags;
    mmaps_n->fd = mmaps_g->fd;
    mmaps_n->offset = mmaps_g->offset + PGSIZE;
    mmaps_n->valid = 1;
    mmaps_n->pf = mmaps_g->pf;

    
    if (!(mmaps_n->flags & MAP_ANONYMOUS)){
      struct file *f = p->ofile[mmaps_n->fd];
      ilock(f->ip);
      readi(f->ip, mem, PGSIZE, PGSIZE); // copy a page of the file from the disk
      iunlock(f->ip);
    }
    break;

	
  //PAGEBREAK: 13
  default:
    if(myproc() == 0 || (tf->cs&3) == 0){
      // In kernel, it must be our mistake.
      cprintf("unexpected trap %d from cpu %d eip %x (cr2=0x%x)\n",
              tf->trapno, cpuid(), tf->eip, rcr2());
      panic("trap");
    }
    // In user space, assume process misbehaved.
    cprintf("pid %d %s: trap %d err %d on cpu %d "
            "eip 0x%x addr 0x%x--kill proc\n",
            myproc()->pid, myproc()->name, tf->trapno,
            tf->err, cpuid(), tf->eip, rcr2());
    myproc()->killed = 1;
  }

  // Force process exit if it has been killed and is in user space.
  // (If it is still executing in the kernel, let it keep running
  // until it gets to the regular system call return.)
  if(myproc() && myproc()->killed && (tf->cs&3) == DPL_USER)
    exit();

  // Force process to give up CPU on clock tick.
  // If interrupts were on while locks held, would need to check nlock.
  if(myproc() && myproc()->state == RUNNING &&
     tf->trapno == T_IRQ0+IRQ_TIMER)
    yield();

  // Check if the process has been killed since we yielded
  if(myproc() && myproc()->killed && (tf->cs&3) == DPL_USER)
    exit();
}
