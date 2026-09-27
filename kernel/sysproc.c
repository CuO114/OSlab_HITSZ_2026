#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "date.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "proc.h"

uint64 sys_exit(void) {
  int n;
  if (argint(0, &n) < 0) return -1;
  exit(n);
  return 0;  // not reached
}

uint64 sys_getpid(void) { return myproc()->pid; }

uint64 sys_fork(void) { return fork(); }

uint64 sys_wait(void) {
  uint64 p;
  int flags;
  if (argaddr(0, &p) < 0) return -1;
  if (argint(1, &flags) < 0) return -1;
  return wait(p, flags);
}

uint64 sys_sbrk(void) {
  int addr;
  int n;

  if (argint(0, &n) < 0) return -1;
  addr = myproc()->sz;
  if (growproc(n) < 0) return -1;
  return addr;
}

uint64 sys_sleep(void) {
  int n;
  uint ticks0;

  if (argint(0, &n) < 0) return -1;
  acquire(&tickslock);
  ticks0 = ticks;
  while (ticks - ticks0 < n) {
    if (myproc()->killed) {
      release(&tickslock);
      return -1;
    }
    sleep(&ticks, &tickslock);
  }
  release(&tickslock);
  return 0;
}

uint64 sys_kill(void) {
  int pid;

  if (argint(0, &pid) < 0) return -1;
  return kill(pid);
}

// return how many clock tick interrupts have occurred
// since start.
uint64 sys_uptime(void) {
  uint xticks;

  acquire(&tickslock);
  xticks = ticks;
  release(&tickslock);
  return xticks;
}

uint64 sys_rename(void) {
  char name[16];
  int len = argstr(0, name, MAXPATH);
  if (len < 0) {
    return -1;
  }
  struct proc *p = myproc();
  memmove(p->name, name, len);
  p->name[len] = '\0';
  return 0;
}

uint64 sys_yield(void) {
  struct proc *p = myproc();

  uint64 context_begin = (uint64)&p->context;
  uint64 context_end = (uint64)(&p->context + 1);
  printf("Save the context of the process to the memory region from address %p to %p\n", context_begin, context_end);
  printf("Current running process pid is %d and user pc is %p\n", p->pid, p->trapframe->epc);

  int current_index = (int)(p - proc);
  int found = 0;
  int next_pid = 1;
  uint64 next_pc = 0;

  for (int offset = 1; offset < NPROC; offset++) {
    int index = (current_index + offset) % NPROC;
    struct proc *q = &proc[index];

    acquire(&q->lock);
    if (q->state == RUNNABLE) {
      next_pid = q->pid;
      next_pc = q->trapframe->epc;
      release(&q->lock);
      found = 1;
      break;
    }
    release(&q->lock);
  }

  if (found) {
    printf("Next runnable process pid is %d and user pc is %p\n", next_pid, next_pc);
  } else {
    printf("Next runnable process pid was not found\n");
  }

  yield();
  return 0;
}
