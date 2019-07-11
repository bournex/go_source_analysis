### stack

```go
type stack struct {
	lo uintptr	// 指向栈内存的低地址
	hi uintptr	// 指向栈内存的高地址。栈大小是hi-lo+1
}
```

stack结构比较简单，它只简单描述stack连续内存空间的起止地址。

stackpoolalloc调用mheap的allocManual，每次申请32KB的空间。

stack的order，有四种order，2KB，4KB，8KB，16KB



### stack.go
stack.go中有三个全局变量

- stackpool - 以order为索引的mSpanList数组，用于缓存小于32KB的栈内存。
- stackpoolmu - stackpool的锁
- stackLarge - 用于缓存大于32KB的栈空间，匿名结构对象。对于大于32KB的栈空间对象，使用page数量的log2作为数组索引范围，值为mSpanList。

定义如下

```go
var stackpool [_NumStackOrders]mSpanList
var stackpoolmu mutex

// Global pool of large stack spans.
var stackLarge struct {
	lock mutex
	free [heapAddrBits - pageShift]mSpanList // free lists by log_2(s.npages)
}
```



### go伪寄存器

FP: Frame pointer: arguments and locals.

PC: Program counter: jumps and branches.

SB: Static base pointer: global symbols.

SP: Stack pointer: top of stack.



### go栈帧

了解过栈原理的同学，可能对栈结构比较熟悉，但是go中的栈处理方式稍有区别。所以要理解后面的知识，要先理解go的栈帧。栈帧结构如下：



+——————————— FP（高地址）

|   return address

+——————————— 

|   parent caller BP

+———————————  <- BP，SP

|   local variables

+——————————— 

|   callee needed args

+——————————— <- Physical SP（system register，increace automatically）（低地址）

