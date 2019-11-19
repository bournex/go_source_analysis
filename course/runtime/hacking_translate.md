```
Scheduler structures
====================
The scheduler manages three types of resources that pervade the
runtime: Gs, Ms, and Ps. It's important to understand these even if
you're not working on the scheduler.
```

调度器结构体

调度器管理了运行时的三类资源，G、M和P，即便你不需要理解调度器，理解这三个概念依然是很重要的。

```
Gs, Ms, Ps
----------
A "G" is simply a goroutine. It's represented by type `g`. When a
goroutine exits, its `g` object is returned to a pool of free `g`s and can later be reused for some other goroutine.
```

"G"表示一个goroutine，通过结构体"g"来表达。当goroutine退出时，"g"对象被归还到空闲"g"池，并可以被其他goroutine复用。

```
An "M" is an OS thread that can be executing user Go code, runtime
code, a system call, or be idle. It's represented by type `m`. There
can be any number of Ms at a time since any number of threads may be
blocked in system calls.
```

"M"表示一个系统线程，它可以执行用户代码、运行时代码、系统调用或干脆空闲。定义为结构体"m"。由于线程可能会被系统调用阻塞，所以运行时中可以有任意数量的M。

```
Finally, a "P" represents the resources required to execute user Go
code, such as scheduler and memory allocator state. It's represented
by type `p`. There are exactly `GOMAXPROCS` Ps. A P can be thought of
like a CPU in the OS scheduler and the contents of the `p` type like
per-CPU state. This is a good place to put state that needs to be
sharded for efficiency, but doesn't need to be per-thread or
per-goroutine.
```

最后，"P"表示运行运行用户代码所需要的资源，例如调度器或内存分配器。定义为结构体"p"。"p"的数量与GOMAXPROCS相关。"p"可以被视为