#include "types.h"
#include "stat.h"
#include "param.h"
#include "x86.h"
#include "mmu.h"
#include "proc.h"
#include "defs.h"
#include "spinlock.h"
#include "buf.h"
#include "fs.h"
#include "fsvar.h"
#include "dev.h"

// these are inodes currently in use
// an entry is free if count == 0
struct inode inode[NINODE];
struct spinlock inode_table_lock;

uint rootdev = 1;

void
iinit(void)
{
  initlock(&inode_table_lock, "inode_table");
}

// Allocate a disk block.
static uint
balloc(uint dev)
{
  int b;
  struct buf *bp;
  struct superblock *sb;
  int bi = 0;
  int size;
  int ninodes;
  uchar m;

  bp = bread(dev, 1);
  sb = (struct superblock*) bp->data;
  size = sb->size;
  ninodes = sb->ninodes;

  for(b = 0; b < size; b++) {
    if(b % BPB == 0) {
      brelse(bp);
      bp = bread(dev, BBLOCK(b, ninodes));
    }
    bi = b % BPB;
    m = 0x1 << (bi % 8);
    if((bp->data[bi/8] & m) == 0) {  // is block free?
      break;
    }
  }
  if(b >= size)
    panic("balloc: out of blocks");

  bp->data[bi/8] |= 0x1 << (bi % 8);
  bwrite(bp, BBLOCK(b, ninodes));  // mark it allocated on disk
  brelse(bp);
  return b;
}

static void
bfree(int dev, uint b)
{
  struct buf *bp;
  struct superblock *sb;
  int bi;
  int ninodes;
  uchar m;

  bp = bread(dev, 1);
  sb = (struct superblock*) bp->data;
  ninodes = sb->ninodes;
  brelse(bp);

  bp = bread(dev, b);
  memset(bp->data, 0, BSIZE);
  bwrite(bp, b);
  brelse(bp);

  bp = bread(dev, BBLOCK(b, ninodes));
  bi = b % BPB;
  m = ~(0x1 << (bi %8));
  bp->data[bi/8] &= m;
  bwrite(bp, BBLOCK(b, ninodes));  // mark it free on disk
  brelse(bp);
}

// Find the inode with number inum on device dev
// and return an in-memory copy.  Loads the inode
// from disk into the in-core table if necessary.
// The returned inode has busy set and has its ref count incremented.
// Caller must iput the return value when done with it.
struct inode*
iget(uint dev, uint inum)
{
  struct inode *ip, *nip;
  struct dinode *dip;
  struct buf *bp;

  acquire(&inode_table_lock);

 loop:
  nip = 0;
  for(ip = &inode[0]; ip < &inode[NINODE]; ip++){
    if(ip->count > 0 && ip->dev == dev && ip->inum == inum){
      if(ip->busy){
        sleep(ip, &inode_table_lock);
        goto loop;
      }
      ip->count++;
      ip->busy = 1;
      release(&inode_table_lock);
      return ip;
    }
    if(nip == 0 && ip->count == 0)
      nip = ip;
  }

  if(nip == 0)
    panic("out of inodes");

  nip->dev = dev;
  nip->inum = inum;
  nip->count = 1;
  nip->busy = 1;

  release(&inode_table_lock);

  bp = bread(dev, IBLOCK(inum));
  dip = &((struct dinode*)(bp->data))[inum % IPB];
  nip->type = dip->type;
  nip->major = dip->major;
  nip->minor = dip->minor;
  nip->nlink = dip->nlink;
  nip->size = dip->size;
  memmove(nip->addrs, dip->addrs, sizeof(nip->addrs));
  brelse(bp);

  return nip;
}

void
iupdate(struct inode *ip)
{
  struct buf *bp;
  struct dinode *dip;

  bp = bread(ip->dev, IBLOCK(ip->inum));
  dip = &((struct dinode*)(bp->data))[ip->inum % IPB];
  dip->type = ip->type;
  dip->major = ip->major;
  dip->minor = ip->minor;
  dip->nlink = ip->nlink;
  dip->size = ip->size;
  memmove(dip->addrs, ip->addrs, sizeof(ip->addrs));
  bwrite(bp, IBLOCK(ip->inum));   // mark it allocated on the disk
  brelse(bp);
}

struct inode*
ialloc(uint dev, short type)
{
  struct inode *ip;
  struct dinode *dip = 0;
  struct superblock *sb;
  int ninodes;
  int inum;
  struct buf *bp;

  bp = bread(dev, 1);
  sb = (struct superblock*) bp->data;
  ninodes = sb->ninodes;
  brelse(bp);

  for(inum = 1; inum < ninodes; inum++) {  // loop over inode blocks
    bp = bread(dev, IBLOCK(inum));
    dip = &((struct dinode*)(bp->data))[inum % IPB];
    if(dip->type == 0) {  // a free inode
      break;
    }
    brelse(bp);
  }

  if(inum >= ninodes)
    panic("ialloc: no inodes left");

  memset(dip, 0, sizeof(*dip));
  dip->type = type;
  bwrite(bp, IBLOCK(inum));   // mark it allocated on the disk
  brelse(bp);
  ip = iget(dev, inum);
  return ip;
}

static void
ifree(struct inode *ip)
{
  ip->type = 0;
  iupdate(ip);
}

void
ilock(struct inode *ip)
{
  if(ip->count < 1)
    panic("ilock");

  acquire(&inode_table_lock);

  while(ip->busy)
    sleep(ip, &inode_table_lock);
  ip->busy = 1;

  release(&inode_table_lock);
}

// caller is holding onto a reference to this inode, but no
// longer needs to examine or change it, so clear ip->busy.
void
iunlock(struct inode *ip)
{
  if(ip->busy != 1 || ip->count < 1)
    panic("iunlock");

  acquire(&inode_table_lock);

  ip->busy = 0;
  wakeup(ip);

  release(&inode_table_lock);
}

uint
bmap(struct inode *ip, uint bn)
{
  unsigned x;
  uint *a;
  struct buf *inbp;

  if(bn >= MAXFILE)
    panic("bmap 1");
  if(bn < NDIRECT) {
    x = ip->addrs[bn];
    if(x == 0)
      panic("bmap 2");
  } else {
    if(ip->addrs[INDIRECT] == 0)
      panic("bmap 3");
    inbp = bread(ip->dev, ip->addrs[INDIRECT]);
    a = (uint*) inbp->data;
    x = a[bn - NDIRECT];
    brelse(inbp);
    if(x == 0)
      panic("bmap 4");
  }
  return x;
}

void
itrunc(struct inode *ip)
{
  int i, j;
  struct buf *inbp;

  for(i = 0; i < NADDRS; i++) {
    if(ip->addrs[i] != 0) {
      if(i == INDIRECT) {
        inbp = bread(ip->dev, ip->addrs[INDIRECT]);
        uint *a = (uint*) inbp->data;
        for(j = 0; j < NINDIRECT; j++) {
          if(a[j] != 0) {
            bfree(ip->dev, a[j]);
            a[j] = 0;
          }
        }
        brelse(inbp);
      }
      bfree(ip->dev, ip->addrs[i]);
      ip->addrs[i] = 0;
    }
  }
  ip->size = 0;
  iupdate(ip);
}

// caller is releasing a reference to this inode.
// you must have the inode lock.
void
iput(struct inode *ip)
{
  if(ip->count < 1 || ip->busy != 1)
    panic("iput");

  if((ip->count == 1) && (ip->nlink == 0)) {
    itrunc(ip);
    ifree(ip);
  }

  acquire(&inode_table_lock);

  ip->count -= 1;
  ip->busy = 0;
  wakeup(ip);

  release(&inode_table_lock);
}

void
idecref(struct inode *ip)
{
  ilock(ip);
  iput(ip);
}

void
iincref(struct inode *ip)
{
  ilock(ip);
  ip->count++;
  iunlock(ip);
}

void
stati(struct inode *ip, struct stat *st)
{
  st->st_dev = ip->dev;
  st->st_ino = ip->inum;
  st->st_type = ip->type;
  st->st_nlink = ip->nlink;
  st->st_size = ip->size;
}

#define min(a, b) ((a) < (b) ? (a) : (b))

int
readi(struct inode *ip, char *dst, uint off, uint n)
{
  uint target = n, n1;
  struct buf *bp;

  if(ip->type == T_DEV) {
    if(ip->major < 0 || ip->major >= NDEV || !devsw[ip->major].d_read)
      return -1;
    return devsw[ip->major].d_read(ip->minor, dst, n);
  }

  while(n > 0 && off < ip->size){
    bp = bread(ip->dev, bmap(ip, off / BSIZE));
    n1 = min(n, ip->size - off);
    n1 = min(n1, BSIZE - (off % BSIZE));
    memmove(dst, bp->data + (off % BSIZE), n1);
    n -= n1;
    off += n1;
    dst += n1;
    brelse(bp);
  }

  return target - n;
}

static int
newblock(struct inode *ip, uint lbn)
{
  struct buf *inbp;
  uint *inaddrs;
  uint b;

  if(lbn < NDIRECT) {
    if(ip->addrs[lbn] == 0) {
      b = balloc(ip->dev);
      if(b <= 0) return -1;
      ip->addrs[lbn] = b;
    }
  } else {
    if(ip->addrs[INDIRECT] == 0) {
      b = balloc(ip->dev);
      if(b <= 0) return -1;
      ip->addrs[INDIRECT] = b;
    }
    inbp = bread(ip->dev, ip->addrs[INDIRECT]);
    inaddrs = (uint*) inbp->data;
    if(inaddrs[lbn - NDIRECT] == 0) {
      b = balloc(ip->dev);
      if(b <= 0) return -1;
      inaddrs[lbn - NDIRECT] = b;
      bwrite(inbp, ip->addrs[INDIRECT]);
    }
    brelse(inbp);
  }
  return 0;
}

int
writei(struct inode *ip, char *addr, uint off, uint n)
{
  if(ip->type == T_DEV) {
    if(ip->major < 0 || ip->major >= NDEV || !devsw[ip->major].d_write)
      return -1;
    return devsw[ip->major].d_write(ip->minor, addr, n);
  } else if(ip->type == T_FILE || ip->type == T_DIR) {
    struct buf *bp;
    int r = 0;
    int m;
    int lbn;
    while(r < n) {
      lbn = off / BSIZE;
      if(lbn >= MAXFILE) return r;
      if(newblock(ip, lbn) < 0) {
        cprintf("newblock failed\n");
        return r;
      }
      m = min(BSIZE - off % BSIZE, n-r);
      bp = bread(ip->dev, bmap(ip, lbn));
      memmove(bp->data + off % BSIZE, addr, m);
      bwrite(bp, bmap(ip, lbn));
      brelse(bp);
      r += m;
      off += m;
    }
    if(r > 0) {
      if(off > ip->size) {
        if(ip->type == T_DIR) ip->size = ((off / BSIZE) + 1) * BSIZE;
        else ip->size = off;
      }
      iupdate(ip);
    }
    return r;
  } else {
    panic("writei: unknown type");
    return 0;
  }
}

// look up a path name, in one of three modes.
// NAMEI_LOOKUP: return locked target inode.
// NAMEI_CREATE: return locked parent inode.
//   return 0 if name does exist.
//   *ret_last points to last path component (i.e. new file name).
//   *ret_ip points to the the name that did exist, if it did.
//   *ret_ip and *ret_last may be zero even if return value is zero.
// NAMEI_DELETE: return locked parent inode, offset of dirent in *ret_off.
//   return 0 if name doesn't exist.
struct inode*
namei(char *path, int mode, uint *ret_off, char **ret_last, struct inode **ret_ip)
{
  struct inode *dp;
  struct proc *p = curproc[cpu()];
  char *cp = path, *cp1;
  uint off, dev;
  struct buf *bp;
  struct dirent *ep;
  int i, atend;
  unsigned ninum;

  if(ret_off)
    *ret_off = 0xffffffff;
  if(ret_last)
    *ret_last = 0;
  if(ret_ip)
    *ret_ip = 0;

  if(*cp == '/') dp = iget(rootdev, 1);
  else {
    dp = p->cwd;
    iincref(dp);
    ilock(dp);
  }

  while(*cp == '/')
    cp++;

  while(1){
    if(*cp == '\0'){
      if(mode == NAMEI_LOOKUP)
        return dp;
      if(mode == NAMEI_CREATE && ret_ip){
        *ret_ip = dp;
        return 0;
      }
      iput(dp);
      return 0;
    }

    if(dp->type != T_DIR){
      iput(dp);
      return 0;
    }

    for(off = 0; off < dp->size; off += BSIZE){
      bp = bread(dp->dev, bmap(dp, off / BSIZE));
      for(ep = (struct dirent*) bp->data;
          ep < (struct dirent*) (bp->data + BSIZE);
          ep++){
        if(ep->inum == 0)
          continue;
        for(i = 0; i < DIRSIZ && cp[i] != '/' && cp[i]; i++)
          if(cp[i] != ep->name[i])
            break;
        if((cp[i] == '\0' || cp[i] == '/' || i >= DIRSIZ) &&
           (i >= DIRSIZ || ep->name[i] == '\0')){
          while(cp[i] != '\0' && cp[i] != '/')
            i++;
          off += (uchar*)ep - bp->data;
          ninum = ep->inum;
          brelse(bp);
          cp += i;
          goto found;
        }
      }
      brelse(bp);
    }
    atend = 1;
    for(cp1 = cp; *cp1; cp1++)
      if(*cp1 == '/')
        atend = 0;
    if(mode == NAMEI_CREATE && atend){
      if(*cp == '\0'){
        iput(dp);
        return 0;
      }
      *ret_last = cp;
      return dp;
    }

    iput(dp);
    return 0;

  found:
    if(mode == NAMEI_DELETE && *cp == '\0'){
      *ret_off = off;
      return dp;
    }
    dev = dp->dev;
    iput(dp);
    dp = iget(dev, ninum);
    if(dp->type == 0 || dp->nlink < 1)
      panic("namei");
    while(*cp == '/')
      cp++;
  }
}

void
wdir(struct inode *dp, char *name, uint ino)
{
  uint off;
  struct dirent de;
  int i;

  for(off = 0; off < dp->size; off += sizeof(de)){
    if(readi(dp, (char*) &de, off, sizeof(de)) != sizeof(de))
      panic("wdir read");
    if(de.inum == 0)
      break;
  }

  de.inum = ino;
  for(i = 0; i < DIRSIZ && name[i]; i++)
    de.name[i] = name[i];
  for( ; i < DIRSIZ; i++)
    de.name[i] = '\0';

  if(writei(dp, (char*) &de, off, sizeof(de)) != sizeof(de))
    panic("wdir write");
}

struct inode*
mknod(char *cp, short type, short major, short minor)
{
  struct inode *ip, *dp;
  char *last;

  if((dp = namei(cp, NAMEI_CREATE, 0, &last, 0)) == 0)
    return 0;

  ip = mknod1(dp, last, type, major, minor);

  iput(dp);

  return ip;
}

struct inode*
mknod1(struct inode *dp, char *name, short type, short major, short minor)
{
  struct inode *ip;

  ip = ialloc(dp->dev, type);
  if(ip == 0)
    return 0;
  ip->major = major;
  ip->minor = minor;
  ip->size = 0;
  ip->nlink = 1;

  iupdate(ip);  // write new inode to disk

  wdir(dp, name, ip->inum);

  return ip;
}

int
unlink(char *cp)
{
  struct inode *ip, *dp;
  struct dirent de;
  uint off, inum, dev;

  dp = namei(cp, NAMEI_DELETE, &off, 0, 0);
  if(dp == 0)
    return -1;

  dev = dp->dev;

  if(readi(dp, (char*)&de, off, sizeof(de)) != sizeof(de) || de.inum == 0)
    panic("unlink no entry");

  inum = de.inum;

  memset(&de, 0, sizeof(de));
  if(writei(dp, (char*)&de, off, sizeof(de)) != sizeof(de))
    panic("unlink dir write");

  iupdate(dp);
  iput(dp);

  ip = iget(dev, inum);

  if(ip->nlink < 1)
    panic("unlink nlink < 1");

  ip->nlink--;

  iupdate(ip);
  iput(ip);

  return 0;
}

int
link(char *name1, char *name2)
{
  struct inode *ip, *dp;
  char *last;

  if((ip = namei(name1, NAMEI_LOOKUP, 0, 0, 0)) == 0)
    return -1;
  if(ip->type == T_DIR){
    iput(ip);
    return -1;
  }

  iunlock(ip);

  if((dp = namei(name2, NAMEI_CREATE, 0, &last, 0)) == 0) {
    idecref(ip);
    return -1;
  }
  if(dp->dev != ip->dev){
    idecref(ip);
    iput(dp);
    return -1;
  }

  ilock(ip);
  ip->nlink += 1;
  iupdate(ip);

  wdir(dp, last, ip->inum);
  iput(dp);
  iput(ip);

  return 0;
}
