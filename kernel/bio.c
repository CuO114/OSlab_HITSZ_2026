// Buffer cache.
//
// The buffer cache is a linked list of buf structures holding
// cached copies of disk block contents.  Caching disk blocks
// in memory reduces the number of disk reads and also provides
// a synchronization point for disk blocks used by multiple processes.
//
// Interface:
// * To get a buffer for a particular disk block, call bread.
// * After changing buffer data, call bwrite to write it to disk.
// * When done with the buffer, call brelse.
// * Do not use the buffer after calling brelse.
// * Only one process at a time can use a buffer,
//     so do not keep them longer than necessary.


#include "types.h"
#include "param.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "riscv.h"
#include "defs.h"
#include "fs.h"
#include "buf.h"
#define NBUCKET 13

static struct
{
  struct spinlock lock;
  struct buf *head;
} buckets[NBUCKET];

static struct {
  struct spinlock reclaim_lock;
  struct buf buf[NBUF];
  uchar assigned[NBUF];
  uint reclaim_cursor;
} bcache;

static uint
bhash(uint dev, uint blockno)
{
  return (blockno + dev) % NBUCKET;
}

static struct buf*
lookup_locked(uint bucket, uint dev, uint blockno)
{
  for(struct buf *b = buckets[bucket].head; b; b = b->next)
    if(bcache.assigned[b - bcache.buf] &&
       b->dev == dev && b->blockno == blockno)
      return b;
  return 0;
}

static void
unlink_locked(uint bucket, struct buf *b)
{
  if(b->prev)
    b->prev->next = b->next;
  else
    buckets[bucket].head = b->next;
  if(b->next)
    b->next->prev = b->prev;
}

static void
link_locked(uint bucket, struct buf *b)
{
  b->prev = 0;
  b->next = buckets[bucket].head;
  if(b->next)
    b->next->prev = b;
  buckets[bucket].head = b;
}

static void
lock_pair(uint a, uint b)
{
  // 两个桶按编号加锁；同桶只加一次。
  if(a == b){
    acquire(&buckets[a].lock);
  } else if(a < b){
    acquire(&buckets[a].lock);
    acquire(&buckets[b].lock);
  } else {
    acquire(&buckets[b].lock);
    acquire(&buckets[a].lock);
  }
}

static void
unlock_pair(uint a, uint b)
{
  if(a == b){
    release(&buckets[a].lock);
  } else if(a < b){
    release(&buckets[b].lock);
    release(&buckets[a].lock);
  } else {
    release(&buckets[a].lock);
    release(&buckets[b].lock);
  }
}

void
binit(void)
{

  initlock(&bcache.reclaim_lock, "bcache.reclaim");
  bcache.reclaim_cursor = 0;

  for(int i = 0; i < NBUCKET; i++){
    initlock(&buckets[i].lock, "bcache.bucket");
    buckets[i].head = 0;
  }

  for(int i = 0; i < NBUF; i++){
    struct buf *b = &bcache.buf[i];
    initsleeplock(&b->lock, "buffer");
    b->valid = 0;
    b->disk = 0;
    b->dev = 0;
    b->blockno = 0;
    b->refcnt = 0;
    bcache.assigned[i] = 0;
    // 未分配的缓冲区暂放在桶 0，身份分配后才参与查找。
    link_locked(0, b);
  }
}

// Look through buffer cache for block on device dev.
// If not found, allocate a buffer.
// In either case, return locked buffer.
static struct buf*
bget(uint dev, uint blockno)
{
  uint target = bhash(dev, blockno);
  struct buf *b;

  acquire(&buckets[target].lock);
  b = lookup_locked(target, dev, blockno);
  if(b){
    b->refcnt++;
    release(&buckets[target].lock);
    acquiresleep(&b->lock);
    return b;
  }
  release(&buckets[target].lock);

  acquire(&bcache.reclaim_lock);
  // 等待回收锁期间别人可能已创建目标缓存，必须重新查找。
  acquire(&buckets[target].lock);
  b = lookup_locked(target, dev, blockno);
  if(b){
    b->refcnt++;
    release(&buckets[target].lock);
    release(&bcache.reclaim_lock);
    acquiresleep(&b->lock);
    return b;
  }
  release(&buckets[target].lock);

  // 全局轮转使用全部 NBUF 个实体，避免一直覆盖目标桶的同一个空闲页。
  for(uint step = 0; step < NBUF; step++){
    uint index = (bcache.reclaim_cursor + step) % NBUF;
    b = &bcache.buf[index];
    // 只有回收路径会改身份；持有 reclaim_lock 时可确定候选所在桶。
    uint source = bcache.assigned[index] ? bhash(b->dev, b->blockno) : 0;
    lock_pair(source, target);
    // 命中路径仍可能获得引用，迁移前必须在桶锁内检查。
    if(b->refcnt != 0){
      unlock_pair(source, target);
      continue;
    }
    if(source != target){
      unlink_locked(source, b);
      link_locked(target, b);
    }
    b->dev = dev;
    b->blockno = blockno;
    b->valid = 0;
    b->refcnt = 1;
    bcache.assigned[index] = 1;
    bcache.reclaim_cursor = (index + 1) % NBUF;
    unlock_pair(source, target);
    release(&bcache.reclaim_lock);
    // 睡眠锁可能阻塞，不能持有桶锁或回收锁等待。
    acquiresleep(&b->lock);
    return b;
  }
  release(&bcache.reclaim_lock);
  panic("bget: no buffers");
}

// Return a locked buf with the contents of the indicated block.
struct buf*
bread(uint dev, uint blockno)
{
  struct buf *b;

  b = bget(dev, blockno);
  if(!b->valid) {
    virtio_disk_rw(b, 0);
    b->valid = 1;
  }
  return b;
}

// Write b's contents to disk.  Must be locked.
void
bwrite(struct buf *b)
{
  if(!holdingsleep(&b->lock))
    panic("bwrite");
  virtio_disk_rw(b, 1);
}

// Release a locked buffer.
// Move to the head of the most-recently-used list.
void
brelse(struct buf *b)
{
  if(!holdingsleep(&b->lock))
    panic("brelse");

  releasesleep(&b->lock);

  // 本函数仍持有一个引用，归零前 dev/blockno 不会被回收方修改。
  uint bucket = bhash(b->dev, b->blockno);
  acquire(&buckets[bucket].lock);
  if(b->refcnt == 0)
    panic("brelse refcnt");
  b->refcnt--;
  release(&buckets[bucket].lock);
}

void
bpin(struct buf *b) {
  uint bucket = bhash(b->dev, b->blockno);
  acquire(&buckets[bucket].lock);
  if(b->refcnt == 0)
    panic("bpin refcnt");
  b->refcnt++;
  release(&buckets[bucket].lock);
}

void
bunpin(struct buf *b) {
  uint bucket = bhash(b->dev, b->blockno);
  acquire(&buckets[bucket].lock);
  if(b->refcnt == 0)
    panic("bunpin refcnt");
  b->refcnt--;
  release(&buckets[bucket].lock);
}


