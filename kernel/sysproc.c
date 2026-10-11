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

uint64 sys_seccomp_ctl(void)
{
  int op;
  uint64 value;
  struct proc *p = myproc();

  // value 是 64 位数值，不是用户指针。
  if (argint(0, &op) < 0 || argaddr(1, &value) < 0)
    return -1;

  if (op == 0) {
    acquire(&p->lock);
    p->seccomp_mask = value;
    p->seccomp_enabled = 1; // 零掩码仍启用过滤。
    release(&p->lock);
    return 0;
  }

  if (op == 1) {
    // 避免截断后变为负数。
    if (value > 0x7fffffffULL)
      return -1;
    acquire(&p->lock);
    p->max_children = (int)value;
    release(&p->lock);
    return 0;
  }

  return -1;
}

uint64 sys_seccomp_getlog(void)
{
  struct proc *p = myproc();
  uint64 bufaddr, lenaddr;
  uint64 snapshot[32];
  int capacity, n;

  if (argaddr(0, &bufaddr) < 0 || argaddr(1, &lenaddr) < 0)
    return -1;
  // 通过 copyin 读取用户容量。
  if (copyin(p->pagetable, (char *)&capacity,
             lenaddr, sizeof(capacity)) < 0)
    return -1;
  if (capacity < 0)
    return -1;

  acquire(&p->lock);
  n = p->blocked_count;
  if (n > capacity)
    n = capacity;
  int start = (p->blocked_next + 32 - p->blocked_count) % 32;
  // 从最旧记录起复制，保持时间顺序。
  for (int i = 0; i < n; i++)
    snapshot[i] = p->blocked_log[(start + i) % 32];
  // 先释放锁，再 copyout 快照。
  release(&p->lock);

  if (n > 0 && copyout(p->pagetable, bufaddr,
                       (char *)snapshot, n * sizeof(uint64)) < 0)
    return -1;
  if (copyout(p->pagetable, lenaddr, (char *)&n, sizeof(n)) < 0)
    return -1;
  return 0;
}
