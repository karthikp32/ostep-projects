sheet1: left

# "left" and "right" specify which page of a two-page spread a file
# must start on.  "left" means that a file must start on the first of
# the two page.  "right" means it must start on the second of the two
# pages.  The file may start in either column.
#
# "even" and "odd" specify which column a file must start on.  "even"
# means it must start in the left of the two columns.  "odd" means it
# must start in the right of the two columns.

# types.h either
# param.h either
# defs.h either
# x86.h either
# asm.h either
# mmu.h either
# elf.h either
# mp.h either

even: bootasm.S  # mild preference
even: bootother.S  # mild preference
even: bootmain.S  # mild preference
even: main.c
# mp.c don't care at all
# even: initcode.S
# odd: init.c

# spinlock.h either
left: spinlock.c  # mild preference
even: proc.h  # mild preference

# goal is to have two action-packed 2-page spreads,
# one with
#     allocproc userinit growproc fork
# and another with
#     scheduler sched yield forkret sleep wakeup1 wakeup
right: proc.c   # VERY important

# setjmp.S either
# vm.c either
# kalloc.c either

# syscall.h either
# trapasm.S either
# traps.h either
# even: trap.c
# vectors.pl either
# syscall.c either
# sysproc.c either

# buf.h either
# dev.h either
# fcntl.h either
# stat.h either
# file.h either
# fs.h either
# fsvar.h either
left: ide.c
# odd: bio.c
odd: fs.c   # VERY important
# file.c either
# exec.c either
# sysfile.c either

# even: pipe.c  # mild preference
# string.c either
left: kbd.h
even: console.c
odd: sh.c
