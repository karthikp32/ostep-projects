#ifndef _PTABLE_H_
#define _PTABLE_H_

struct processTable {
  struct spinlock lock;
  struct proc proc[NPROC];
} ;
