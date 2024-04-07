#include "types.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "mmu.h"
#include "x86.h"
#include "proc.h"
#include "spinlock.h"
#include "mmap.h"
//#include "sleeplock.h"
//#include "file.h"
#include "fs.h"
#define MAXMAPS 32  

struct file
{
  enum
  {
    FD_NONE,
    FD_PIPE,
    FD_INODE
  } type;
  int ref; // reference count
  char readable;
  char writable;
  struct pipe *pipe;
  struct inode *ip;
  uint off;
};

struct {
  struct spinlock lock;
  struct proc proc[NPROC];
} ptable;

static struct proc *initproc;

int nextpid = 1;
extern void forkret(void);
extern void trapret(void);

static void wakeup1(void *chan);

int mappages(pde_t *pgdir, void *va, uint size, uint pa, int perm);
pte_t *walkpgdir(pde_t *pgdir, const void *va, int alloc);

void
pinit(void)
{
  initlock(&ptable.lock, "ptable");
}

// Must be called with interrupts disabled
int
cpuid() {
  return mycpu()-cpus;
}

// Must be called with interrupts disabled to avoid the caller being
// rescheduled between reading lapicid and running through the loop.
struct cpu*
mycpu(void)
{
  int apicid, i;
  
  if(readeflags()&FL_IF)
    panic("mycpu called with interrupts enabled\n");
  
  apicid = lapicid();
  // APIC IDs are not guaranteed to be contiguous. Maybe we should have
  // a reverse map, or reserve a register to store &cpus[i].
  for (i = 0; i < ncpu; ++i) {
    if (cpus[i].apicid == apicid)
      return &cpus[i];
  }
  panic("unknown apicid\n");
}

// Disable interrupts so that we are not rescheduled
// while reading proc from the cpu structure
struct proc*
myproc(void) {
  struct cpu *c;
  struct proc *p;
  pushcli();
  c = mycpu();
  p = c->proc;
  popcli();
  return p;
}

//PAGEBREAK: 32
// Look in the process table for an UNUSED proc.
// If found, change state to EMBRYO and initialize
// state required to run in the kernel.
// Otherwise return 0.
static struct proc*
allocproc(void)
{
  struct proc *p;
  char *sp;

  acquire(&ptable.lock);

  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++)
    if(p->state == UNUSED)
      goto found;

  release(&ptable.lock);
  return 0;

found:
  p->state = EMBRYO;
  p->pid = nextpid++;
  

  release(&ptable.lock);
// You may need additional error handling if all slots are occupied
  // Allocate kernel stack.
  if((p->kstack = kalloc()) == 0){
    p->state = UNUSED;
    return 0;
  }
  sp = p->kstack + KSTACKSIZE;

  // Leave room for trap frame.
  sp -= sizeof *p->tf;
  p->tf = (struct trapframe*)sp;

  // Set up new context to start executing at forkret,
  // which returns to trapret.
  sp -= 4;
  *(uint*)sp = (uint)trapret;

  sp -= sizeof *p->context;
  p->context = (struct context*)sp;
  memset(p->context, 0, sizeof *p->context);
  p->context->eip = (uint)forkret;

  return p;
}

//PAGEBREAK: 32
// Set up first user process.
void
userinit(void)
{
  struct proc *p;
  extern char _binary_initcode_start[], _binary_initcode_size[];

  p = allocproc();
  
  initproc = p;
  if((p->pgdir = setupkvm()) == 0)
    panic("userinit: out of memory?");
  inituvm(p->pgdir, _binary_initcode_start, (int)_binary_initcode_size);
  p->sz = PGSIZE;
  memset(p->tf, 0, sizeof(*p->tf));
  p->tf->cs = (SEG_UCODE << 3) | DPL_USER;
  p->tf->ds = (SEG_UDATA << 3) | DPL_USER;
  p->tf->es = p->tf->ds;
  p->tf->ss = p->tf->ds;
  p->tf->eflags = FL_IF;
  p->tf->esp = PGSIZE;
  p->tf->eip = 0;  // beginning of initcode.S

  safestrcpy(p->name, "initcode", sizeof(p->name));
  p->cwd = namei("/");

  // this assignment to p->state lets other cores
  // run this process. the acquire forces the above
  // writes to be visible, and the lock is also needed
  // because the assignment might not be atomic.
  acquire(&ptable.lock);

  p->state = RUNNABLE;

  release(&ptable.lock);
}

// Grow current process's memory by n bytes.
// Return 0 on success, -1 on failure.
int
growproc(int n)
{
  uint sz;
  struct proc *curproc = myproc();

  sz = curproc->sz;
  if(n > 0){
    if((sz = allocuvm(curproc->pgdir, sz, sz + n)) == 0)
      return -1;
  } else if(n < 0){
    if((sz = deallocuvm(curproc->pgdir, sz, sz + n)) == 0)
      return -1;
  }
  curproc->sz = sz;
  switchuvm(curproc);
  return 0;
}

//Helper function
void cleanMaps(struct proc *p)
{
  int i;
  for (i = 0; i < 32; i++)
  {
    if (p->mmap_s[i].valid)
    {
      if (p->mmap_s[i].pf)
      {
        fileclose(p->mmap_s[i].pf);
        p->mmap_s[i].pf = 0;
      }
      p->mmap_s[i].valid = 0;
    }
  }
}

// Helper function
int copyMaps(struct proc *child, struct mmaps *sourcemaps, int index)
{
  struct mmaps *newmaps = &child->mmap_s[index];
  // Check if VMA is valid
  if (!sourcemaps->valid){
    return -1;
  }

  *newmaps = *sourcemaps;
  
  if (sourcemaps->pf){
    newmaps->pf = filedup(sourcemaps->pf);
  }
  newmaps->valid = 1;
  return 0;
}

// Create a new process copying p as the parent.
// Sets up stack to return as if from system call.
// Caller must set state of returned proc to RUNNABLE.
int
fork(void)
{
  int i, pid;
  struct proc *np;
  struct proc *curproc = myproc();

  // Allocate process.
  if((np = allocproc()) == 0){
    return -1;
  }

  // Copy process state from proc.
  if((np->pgdir = copyuvm(curproc->pgdir, curproc->sz)) == 0){
    kfree(np->kstack);
    np->kstack = 0;
    np->state = UNUSED;
    return -1;
  }
  np->sz = curproc->sz;
  np->parent = curproc;
  *np->tf = *curproc->tf;
	
  // Clear %eax so that fork returns 0 in the child.
  np->tf->eax = 0;

  for(i = 0; i < NOFILE; i++)
    if(curproc->ofile[i])
		np->ofile[i] = filedup(curproc->ofile[i]);
  np->cwd = idup(curproc->cwd);

  //parent assecc to child
  for (i = 0; i < MAXMAPS; i++)
  {
    if (curproc->mmap_s[i].valid)
    {
      if (copyMaps(np, &curproc->mmap_s[i], i) != 0)
      {
        // handle error
        kfree(np->kstack);
        np->kstack = 0;
        np->state = UNUSED;
        cleanMaps(np);
        return -1;
      }
    }
  }
		
  //forkHelper(void);
  safestrcpy(np->name, curproc->name, sizeof(curproc->name));

  pid = np->pid;

  acquire(&ptable.lock);

  np->state = RUNNABLE;

  release(&ptable.lock);

  
	return pid;
}

// Implement munmap remove.
//void munmap_all(void){ 
//	struct proc *curproc = myproc();
//
//	if (!curproc)
  //      return;  // Ensure curproc is not NULL

	//// Iterate through the mapping data structure
	//int i;
	//for(i = 0; i < MAX_MMAPS; i++) {
	//	if(curproc->mmap_entries[i].valid) {
      //      // Get the address and length of the mapping
        //    uint addr = (uint)curproc->mmap_entries[i].vaddr;
          //  int length = curproc->mmap_entries[i].length;

		//	// Unmap the memory region
          //  int result = munmap((void *)addr, length);
		//	if (result != 0) {
          //      cprintf("result error: result != 0) ");
			//	return;
            //}
			//curproc->mmap_entries[i].valid = 0;
		//}	
//	}
//	return;	
//}

// Exit the current process.  Does not return.
// An exited process remains in the zombie state
// until its parent calls wait() to find out it exited.
void
exit(void)
{
  struct proc *curproc = myproc();
  struct proc *p;
  int fd;
  
  if(curproc == initproc)
    panic("init exiting");

  // Close all open files.
  for(fd = 0; fd < NOFILE; fd++){
    if(curproc->ofile[fd]){
      fileclose(curproc->ofile[fd]);
      curproc->ofile[fd] = 0;
    }
  }
  
  //remove all mapping
  //munmap_all();
	
  begin_op();
  iput(curproc->cwd);
  end_op();
  curproc->cwd = 0;

  acquire(&ptable.lock);

  // Parent might be sleeping in wait().
  wakeup1(curproc->parent);

  // Pass abandoned children to init.
  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->parent == curproc){
      p->parent = initproc;
      if(p->state == ZOMBIE)
        wakeup1(initproc);
    }
  }
	
  cleanMaps(curproc);
  // Jump into the scheduler, never to return.
  curproc->state = ZOMBIE;
  sched();
  panic("zombie exit");
}

// Wait for a child process to exit and return its pid.
// Return -1 if this process has no children.
int
wait(void)
{
  struct proc *p;
  int havekids, pid;
  struct proc *curproc = myproc();
  
  acquire(&ptable.lock);
  for(;;){
    // Scan through table looking for exited children.
    havekids = 0;
    for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
      if(p->parent != curproc)
        continue;
      havekids = 1;
      if(p->state == ZOMBIE){
        // Found one.
        pid = p->pid;
        kfree(p->kstack);
        p->kstack = 0;
        freevm(p->pgdir);
        p->pid = 0;
        p->parent = 0;
        p->name[0] = 0;
        p->killed = 0;
        p->state = UNUSED;
        release(&ptable.lock);
        return pid;
      }
    }

    // No point waiting if we don't have any children.
    if(!havekids || curproc->killed){
      release(&ptable.lock);
      return -1;
    }

    // Wait for children to exit.  (See wakeup1 call in proc_exit.)
    sleep(curproc, &ptable.lock);  //DOC: wait-sleep
  }
}

//PAGEBREAK: 42
// Per-CPU process scheduler.
// Each CPU calls scheduler() after setting itself up.
// Scheduler never returns.  It loops, doing:
//  - choose a process to run
//  - swtch to start running that process
//  - eventually that process transfers control
//      via swtch back to the scheduler.
void
scheduler(void)
{
  struct proc *p;
  struct cpu *c = mycpu();
  c->proc = 0;
  
  for(;;){
    // Enable interrupts on this processor.
    sti();

    // Loop over process table looking for process to run.
    acquire(&ptable.lock);
    for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
      if(p->state != RUNNABLE)
        continue;

      // Switch to chosen process.  It is the process's job
      // to release ptable.lock and then reacquire it
      // before jumping back to us.
      c->proc = p;
      switchuvm(p);
      p->state = RUNNING;

      swtch(&(c->scheduler), p->context);
      switchkvm();

      // Process is done running for now.
      // It should have changed its p->state before coming back.
      c->proc = 0;
    }
    release(&ptable.lock);

  }
}

// Enter scheduler.  Must hold only ptable.lock
// and have changed proc->state. Saves and restores
// intena because intena is a property of this
// kernel thread, not this CPU. It should
// be proc->intena and proc->ncli, but that would
// break in the few places where a lock is held but
// there's no process.
void
sched(void)
{
  int intena;
  struct proc *p = myproc();

  if(!holding(&ptable.lock))
    panic("sched ptable.lock");
  if(mycpu()->ncli != 1)
    panic("sched locks");
  if(p->state == RUNNING)
    panic("sched running");
  if(readeflags()&FL_IF)
    panic("sched interruptible");
  intena = mycpu()->intena;
  swtch(&p->context, mycpu()->scheduler);
  mycpu()->intena = intena;
}

// Give up the CPU for one scheduling round.
void
yield(void)
{
  acquire(&ptable.lock);  //DOC: yieldlock
  myproc()->state = RUNNABLE;
  sched();
  release(&ptable.lock);
}

// A fork child's very first scheduling by scheduler()
// will swtch here.  "Return" to user space.
void
forkret(void)
{
  static int first = 1;
  // Still holding ptable.lock from scheduler.
  release(&ptable.lock);

  if (first) {
    // Some initialization functions must be run in the context
    // of a regular process (e.g., they call sleep), and thus cannot
    // be run from main().
    first = 0;
    iinit(ROOTDEV);
    initlog(ROOTDEV);
  }

  // Return to "caller", actually trapret (see allocproc).
}

// Atomically release lock and sleep on chan.
// Reacquires lock when awakened.
void
sleep(void *chan, struct spinlock *lk)
{
  struct proc *p = myproc();
  
  if(p == 0)
    panic("sleep");

  if(lk == 0)
    panic("sleep without lk");

  // Must acquire ptable.lock in order to
  // change p->state and then call sched.
  // Once we hold ptable.lock, we can be
  // guaranteed that we won't miss any wakeup
  // (wakeup runs with ptable.lock locked),
  // so it's okay to release lk.
  if(lk != &ptable.lock){  //DOC: sleeplock0
    acquire(&ptable.lock);  //DOC: sleeplock1
    release(lk);
  }
  // Go to sleep.
  p->chan = chan;
  p->state = SLEEPING;

  sched();

  // Tidy up.
  p->chan = 0;

  // Reacquire original lock.
  if(lk != &ptable.lock){  //DOC: sleeplock2
    release(&ptable.lock);
    acquire(lk);
  }
}

//PAGEBREAK!
// Wake up all processes sleeping on chan.
// The ptable lock must be held.
static void
wakeup1(void *chan)
{
  struct proc *p;

  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++)
    if(p->state == SLEEPING && p->chan == chan)
      p->state = RUNNABLE;
}

// Wake up all processes sleeping on chan.
void
wakeup(void *chan)
{
  acquire(&ptable.lock);
  wakeup1(chan);
  release(&ptable.lock);
}

// Kill the process with the given pid.
// Process won't exit until it returns
// to user space (see trap in trap.c).
int
kill(int pid)
{
  struct proc *p;

  acquire(&ptable.lock);
  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->pid == pid){
      p->killed = 1;
      // Wake process from sleep if necessary.
      if(p->state == SLEEPING)
        p->state = RUNNABLE;
      release(&ptable.lock);
      return 0;
    }
  }
  release(&ptable.lock);
  return -1;
}

//PAGEBREAK: 36
// Print a process listing to console.  For debugging.
// Runs when user types ^P on console.
// No lock to avoid wedging a stuck machine further.
void
procdump(void)
{
  static char *states[] = {
  [UNUSED]    "unused",
  [EMBRYO]    "embryo",
  [SLEEPING]  "sleep ",
  [RUNNABLE]  "runble",
  [RUNNING]   "run   ",
  [ZOMBIE]    "zombie"
  };
  int i;
  struct proc *p;
  char *state;
  uint pc[10];

  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->state == UNUSED)
      continue;
    if(p->state >= 0 && p->state < NELEM(states) && states[p->state])
      state = states[p->state];
    else
      state = "???";
    cprintf("%d %s %s", p->pid, state, p->name);
    if(p->state == SLEEPING){
      getcallerpcs((uint*)p->context->ebp+2, pc);
      for(i=0; i<10 && pc[i] != 0; i++)
        cprintf(" %p", pc[i]);
    }
    cprintf("\n");
  }
}

// Helper function to check if a virtual address range is available
//int is_range_available(struct proc *p, void *addr, int length) {
    // Check if the specified address range is within the allowed range (0x60000000 to 0x80000000)
  //  if ((uint)addr < 0x60000000 || (uint)addr + length > 0x80000000) {
    //    return 0; // Address out of allowed range
   // }

    //// Iterate through the process's memory mappings to check for conflicts
    //for (int i = 0; i < MAX_MMAPS; i++) {
      //  struct mmap_entry *entry = &p->mmap_entries[i];
        //if (entry->valid) {
          //  // Check if the specified range overlaps with an existing mapping
            //if ((uint)addr <  (uint)entry->vaddr + (int)entry->length &&
              //  (uint)addr + (int)length > (uint)entry->vaddr) {
                //return 0; // Range overlaps with an existing mapping
            //}
       // }
    //}

    //return 1; // Address range is available
//}

// Helper function to release pages and clear page table entries
//void release_pages(void *addr, int length) {
  //  for (int i = 0; i < length; i += PGSIZE) {
    //    void *release_addr = addr + i;
      //  pte_t *release_pte = walkpgdir(myproc()->pgdir, release_addr, 0);
        //if (release_pte && (*release_pte & PTE_P)) {
          //  char *mem_to_release = (char *)P2V(PTE_ADDR(*release_pte));
            //kfree(mem_to_release);
           // *release_pte = 0; // Clear the page table entry
       // }
	//	addr += PGSIZE;
    //}
//}
int checkMemAvailable(pde_t *pgdir, void *va, uint size)
{
  char *start = (char *)va;
  char *end = start + size;
  pte_t *pte;

  for (char *a = start; a < end; a += PGSIZE)
  {
    pte = walkpgdir(pgdir, a, 0);
    if (pte && (*pte & PTE_P))
    {
      // Page is present, region is not free.
      return 0;
    }
  }
  // No page is present in the region, hence it's free.
  return 1;
}

void *find_free_region(uint *pgdir, uint start, uint end, int length){
  for (void *addr = (void *)start; addr < (void *)end; addr += PGSIZE){
	if ( checkMemAvailable(pgdir, addr, length)){
      return addr;
    }
  }
  return (void *)-1;
}

int is_valid_free_region(uint *pgdir, void *addr, int length){
  if ((uint)addr >= mmaps_START && (uint)addr + length <= mmaps_END){
    return checkMemAvailable(pgdir, addr, length);
  }
  return 0;
}

int find_free_vma_slot(struct proc *curproc){
	
	for(int i = 0; i < vmmaps; i++){
    if (curproc->mmap_s[i].addr == 0)
    {
      return i;
    }
  }
  return -1;
}

int allocate_and_map_pages(struct proc *curproc, void *addr, int alloc_length, int flags, int fd)
{
  char *mem;
  uint a;

  for (a = (uint)addr; a < (uint)addr + alloc_length; a += PGSIZE)
  {
    // Allocate a physical page
    mem = kalloc();
    if (!mem)
    {
      // Allocation failed, free previously allocated pages
      deallocuvm(curproc->pgdir, (uint)addr, a);
      return -1; // Return -1 on failure
    }
    // Clear allocated memory to zero
    memset(mem, 0, PGSIZE);
    // Map the virtual address to the physical page
    if (mappages(curproc->pgdir, (void *)a, PGSIZE, V2P(mem), PTE_W | PTE_U) < 0)
    {
      // Mapping failed, free the allocated page and previously allocated pages
      kfree(mem);
      deallocuvm(curproc->pgdir, (uint)addr, a + PGSIZE);
      return -1; // Return -1 on failure
    }
  }

  return 0; // Return 0 on success
}


void update_mmaps(struct proc *curproc, int mmapIndex, void *addr, int length, int prot, int flags, int fd, int offset)
{
  struct mmaps *mmap_s = &curproc->mmap_s[mmapIndex];
  mmap_s->addr = addr;
  mmap_s->end = (void *)((uint)addr + length);
  mmap_s->prot = prot;
  mmap_s->flags = flags;
  mmap_s->fd = fd;
  mmap_s->offset = offset;
  mmap_s->valid = 1;

  if (!(flags & MAP_ANONYMOUS))
  {
    curproc->mmap_s[mmapIndex].pf = curproc->ofile[fd];
  }
}


void *mmap(void *addr, int length, int prot, int flags, int fd, int offset) {
	struct proc *curproc = myproc();

	if ((flags & (MAP_ANONYMOUS | MAP_FIXED | MAP_SHARED)) !=
        (MAP_ANONYMOUS | MAP_FIXED | MAP_SHARED)) {
        // if invalid flags
        return (void *)-1;
    }
	if(!(flags & MAP_PRIVATE) && !(flags & MAP_SHARED)) {return (void *) -1;}
	if((uint)addr % PGSIZE != 0) {return (void *) -1;}    
	if (length <= 0){return (void *)-1;}

	int alloc_length = PGROUNDUP(length);
	
	if (!(flags & MAP_FIXED)){
		void *start_addr = (void *)mmaps_START;
		void *end_addr = (void *)mmaps_END;
		addr = start_addr;

		// Handle allocation failure by releasing previously allocated pages
		
		while (addr < end_addr){
			if (checkMemAvailable(curproc->pgdir, addr, alloc_length)){
				break;
			}
      
			addr += PGSIZE;
		}
	//for(i = 0; i < MAXi_MAPS; i++){
	//	if(!(curproc->mmaps[i].mapped)){
	//		mmap_entry = &curproc->mmaps[i];
	//		curproc->nummaps++;
	//		break;
	//	}else{
	//		return (void *) -1;
	//}

		if (addr >= end_addr){
			return (void *)-1;
		}
	}
	else
	{
		if ((uint)addr < mmaps_START || (uint)addr + alloc_length > mmaps_END || !checkMemAvailable(curproc->pgdir, addr, alloc_length)){
			return (void *)-1;
		}
	}
	
	int mmapIndex = -1;
  for (int j = 0; j < 32; j++)
  {
    if (curproc->mmap_s[j].addr == 0)
    {
      mmapIndex = j;
      break;
    }
  }
  if (mmapIndex == -1)
  {
    cprintf("mmap: no free slot found in VMA\n");
    return (void *)-1;
  }	
	
   char *mem;
	 for (int i = 0; i < alloc_length; i += PGSIZE){
    // Allocate a physical page of memory
    mem = kalloc();
    if (mem == 0)
    {
      deallocuvm(curproc->pgdir, (int)addr + i, (int)addr);
      return (void *)-1;
    }
    // Initialize the allocated memory to zero
    memset(mem, 0, PGSIZE);
    // Map the virutal memory address (a + i) to the physical address of the allocated memory
    int status = mappages(curproc->pgdir, (char *)(addr + i), PGSIZE, V2P(mem), PTE_W | PTE_U);
    if (status)
    {
      deallocuvm(curproc->pgdir, (int)addr + i + PGSIZE, (int)addr);
      kfree(mem);
      return (void *)-1;
    }
    // copy a page of the file from the disk
    if (!(flags & MAP_ANONYMOUS))
    {
      struct file *f = curproc->ofile[fd];
      ilock(f->ip);
      readi(f->ip, mem, i, PGSIZE); // copy a page of the file from the disk
      iunlock(f->ip);
    }
  }
  // Update VMA
  curproc->mmap_s[mmapIndex].addr = addr;
  curproc->mmap_s[mmapIndex].end = (void *)((uint)addr + length);
  curproc->mmap_s[mmapIndex].prot = prot;
  curproc->mmap_s[mmapIndex].flags = flags;
  curproc->mmap_s[mmapIndex].fd = fd;
  curproc->mmap_s[mmapIndex].offset = offset;
  curproc->mmap_s[mmapIndex].valid = 1;
  if (!(flags & MAP_ANONYMOUS))
  {
    curproc->mmap_s[mmapIndex].pf = curproc->ofile[fd];
  }

	
	
//	if(flags & MAP_FIXED){
    // Naive MAP_FIXED. Works as if it is MAP_FIXED_NOREPLACE in std C
    // implementation of mmap.
    
	//}else {

	//}
	// Check if flags match the mapping type (anonymous or file-backed)
   // if ((flags & MAP_ANONYMOUS)) {

        //for (int i = 0; i < length; i += PGSIZE) {
           // pte_t *pte = walkpgdir(myproc()->pgdir, addr, 1);
           // if (!pte) {
                // Handle allocation failure by releasing previously allocated pages
              //  release_pages(addr, i);
            //    return (void *)-1;
          //  }
	
		//	if (*pte & PTE_P) {
                // Page already mapped; handle this case as needed
            //} else {
                // Allocate a physical page and map it at the chosen virtual address
                //char *mem = kalloc();
                ///if (!mem) {
                    // Handle allocation failure by releasing previously allocated pages
              //      release_pages(addr, i);
            //        return (void *)-1;
          //      }
        //        memset(mem, 0, PGSIZE);
  //              *pte = V2P(mem) | PTE_W | PTE_U | PTE_P; // Set page table entry
    //        }
      //      mapped_addr += PGSIZE;
			//mapped_addr->fd = fd;
			//mapped_addr->offset = offset;

			//mapped_addr->sz = length;
  //mapped_addr->prot = prot;
  //mapped_addr->flags = flags;
  //mapped_addr->mapped = 1;
//		}
//		 return addr;
//
  //  } 

	//if ((flags & MAP_FIXED)) {
	//	if(!((uint)addr % PGSIZE == 0)){
      //      return  (void *)-1;
       // }

        //if ((uint)addr < 0x60000000 || (uint)addr + length > 0x80000000)  {
          //  return  (void *)-1;
       // }
	
//	}
	return addr;
}


int munmap(void* addr, int length) {
    int write_length = length;
  struct proc *curproc = myproc();
  struct mmaps *vp = 0;

  for (int i = 0; i < MAXMAPS; i++)
    if (curproc->mmap_s[i].valid == 1 && curproc->mmap_s[i].addr <= addr && addr < curproc->mmap_s[i].end)
    {
      vp = &curproc->mmap_s[i];
      break;
    }
  if (vp == 0)
    panic("munmap no such vma");
  // Page Table Entry (PTE) Lookup:
  pte_t *pte;
  if ((pte = walkpgdir(curproc->pgdir, (void *)addr, 0)) != 0)
  {
    if (length > PGSIZE)
    {
      length -= PGSIZE;
      write_length = PGSIZE;
    }
    else
    {
      write_length = length;
    }
    // Handle Memory Unmapping and Write Back:
    if (!(vp->flags & MAP_ANONYMOUS) && !(vp->flags & MAP_PRIVATE))
    {
      filewriteoff(vp->pf, addr, write_length, addr - vp->addr);
    }
    // Deallocate Virtual Memory:
    deallocuvm(curproc->pgdir, (int)addr + write_length, (int)addr);
  }
  // Handle Additional VMAs for MAP_GROWSUP:
  if (vp->flags & MAP_GROWSUP && !(vp->flags & MAP_ANONYMOUS) && !(vp->flags & MAP_PRIVATE))
  {
    int off = PGSIZE;
    // Loop Through Continuation of Memory:
    while (vp->next != 0 && length > 0)
    {
      if (length > PGSIZE)
      {
        length -= PGSIZE;
        write_length = PGSIZE;
      }
      else
      {
        write_length = length;
      }
      vp = &curproc->mmap_s[vp->next];
      addr += PGSIZE;
      if (!(vp->flags & MAP_ANONYMOUS))
      {
        filewriteoff(vp->pf, addr, write_length, off);
      }

      deallocuvm(curproc->pgdir, (int)addr + write_length, (int)addr);
      off += PGSIZE;
    }
  }
  return 0;

}

