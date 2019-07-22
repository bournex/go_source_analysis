虚拟内存概念介绍

操作系统的内存管理简介

内存管理面临的问题和挑战

内存管理的常见方式

# overview

go的内存管理参考了tcmalloc，tcmalloc是google开源的一款内存管理工具，它比glibc中自带的ptmalloc性能要更高。

在tcmalloc中，对内存进行划分，大小内存需求进行了不同的处理。对于小内存，按照需求量的不同，实际申请空间被对齐到了预先规划好的大小。同时为小内存的申请，增加了线程局部分配器，用于快速无锁分配。

而对于大内存需求，则不提供线程局部分配器。所有请求都统一通过heap来管理。



## ptmalloc  vs  tcmalloc  vs  jemalloc



# 预备知识

## 常用全局变量



# 系统级内存管理

## 原始堆内存的初始化（mallocinit）

操作系统提供的虚拟内存空间，是由寻找空间决定的，在amd64位系统上，有48个bit用于寻址，所以我们的可用地址空间，在1<<48个，这里没有占满64bit，因为48bit已经提供了256TB的虚拟地址寻址空间。这几乎已经足够大部分的系统应用了。

go在mallocinit中初始化了128个arenahint对象，从0xC000000000开始循环128次，令每个arenahint.addr指向1TB空间的起始地址。作为堆内存的寻址指引。所有在应用程序中通过new关键字创建的对象地址，最终都指向大于0xC000000000的地址。

虽然指定的arenahint链表有128TB空间之大，但堆内存的地址不止如此，后面可以看到当128TB空间耗尽后，arenahint链表还会继续扩张。

选择将堆地址放在0xC000000000的位置，注释阐明了原因。一方面方便调试时识别堆内存空间。另一方面，在用大端法表达地址时，C0不是有效的UTF-8字符，可以一定程度上规避对指针类型被错误解释成UTF8字符。

我们且称这部分128TB的内存为堆原始内存。



```go
func mallocinit() {
	...

	// Initialize the heap.
	mheap_.init()
	_g_ := getg()
	_g_.m.mcache = allocmcache()

	if sys.PtrSize == 8 && GOARCH != "wasm" {
    // 64位系统下，获得128个1TB内存块的起始地址
		for i := 0x7f; i >= 0; i-- {
			var p uintptr
			switch {
			case GOARCH == "arm64" && GOOS == "darwin":
				p = uintptr(i)<<40 | uintptrMask&(0x0013<<28)
			case GOARCH == "arm64":
				p = uintptr(i)<<40 | uintptrMask&(0x0040<<32)
			default:
				p = uintptr(i)<<40 | uintptrMask&(0x00c0<<32)
			}
      // 起始地址被保存在arenaHint链表中
			hint := (*arenaHint)(mheap_.arenaHintAlloc.alloc())
			hint.addr = p
			hint.next, mheap_.arenaHints = mheap_.arenaHints, hint
		}
	} else {
		// 32位系统的初始化过程
	}
```



## arenaHints

```go
type arenaHint struct {
	addr uintptr		// 记录了在当前arenaHint的区间内，已经分配到的内存地址位置，addr只会单调增长或降低。
	down bool				// 默认为false，从低地址向高地址增长。
	next *arenaHint	// 下一个arena区间的arenaHint对象
}
```

arenahint指向最原始的虚拟内存空间，runtime.sysAlloc函数分配空间时以heapArenaBytes(64MB)为单位从arenaHint链表头对象开始分配空间。当第一个arenaHint的1TB空间被用完后，将开始使用下一个arenaHint的空间。

【图：arenaHints示意图】

![arenaHints](https://github.com/bournex/go_source_analysis/images/goheap-arenahint.jpg)



## Arena

### arena

arena是tcmalloc中没有的概念，go中基于垃圾回收的考虑，需要对内存进行更加精细化的管理。所以增加了arena的概念。arena中文翻译是竞技场，没什么卵用。

go中将arena定义为一段连续的64MB空间。堆原始内存的分配，提供的接口为系统页数量，实际分配时会对齐到64MB的arena来分配。



### heapArena

```go
type heapArena struct {

	// bitmap是一个byte数组，它用于表达在64MB的arena空间中，是否需要被GC扫描。
	// 其中每2个bit表达arena中的一个PtrSize（64bit）空间。所以bitmap数组长度为2MB。
	// 2bit的地位表示对应的arena中start+N*PtrSize位置开始的8字节上是否有指针。
	// 0表示没有，1表示有。而高bit位为【TODO】
	bitmap [heapArenaBitmapBytes]byte


	// arena被按照page大小划分，在amd64Go中page为8KB，所以pagesPerArena也是8KB。
	// 即arena空间被分为0 - (8192-1)个mspan指针。其中被分配的page，对应在spans中
	// 的mspan	为当前持有该空间的mspan指针。如果一个区域没有被使用，则这个区域的首尾
	// page在spans中的对象指向该mspan。如果arena中一段page序列从来未分配过，则该
	// 区间的page在spans中的mspan指针值为nil。setSpan方法完成了对spans的初始化。
	// 写需要加锁，读不需要加锁
	spans [pagesPerArena]*mspan
}
```

heapArena标识一个arena。整个虚拟内存地址空间，被按照arena管理起来，每64MB对应一个heapArena对象。heapArena对象通过persistentalloc申请，申请的对象被挂到mheap.arenas二级索引下。



```go
type mheap struct {
	// ...
  
	// arenas是一个二维数组，通过这个二维数组，构成了对整个虚拟内存寻址空间的
	// 描述。在64位系统中，arenaL1Bits为0，arenaL2Bits为22（4MB）。
  // 堆中用了4MB的空间，描述了整个虚拟内存的使用情况和包含指针情况。
	arenas [1 << arenaL1Bits]*[1 << arenaL2Bits]*heapArena
  
	// ...
}
```

在mheap类中的arenas成员使用一个二维指针表达了整个虚拟内存空间。并提供了一些方法来操作这个空间。比如根据指针值，计算出其对应的heapArena对象。

![](https://github.com/bournex/go_source_analysis/images/goheap-heapArena.jpg)



### heapArena索引计算

arenas中，arenaL1Bits、arenaL2Bits分别表达了第一维和第二维的bit shift，在64位系统上，arenaL1Bits为0，arenaL2Bits为22。所以最多会有4MB个heapArena对象。由于每个heapArena对象都表达了64MB的虚拟内存空间，所以4MB * 64MB = 1 << 48，arenas就覆盖了整个虚拟内存空间。

对于一个给定的地址p，共有48个bit位为有效地址bit，其前arenaL1Bits位bit作为第一维索引，这里为0。所以前arenaL2Bits位bit作为第二维索引，这里为22。go使用arenaIndex来计算p对应的heapArena对象在arenas二维数组中的位置。

```go
func arenaIndex(p uintptr) arenaIdx {
  // p加上arenaBaseOffset，除以arena的大小，得到一个index值。
	// arenaBaseOffset用于将
	return arenaIdx((p + arenaBaseOffset) / heapArenaBytes)
}

type arenaIdx uint

// 获得前arenaL1Bits位作为L1索引
func (i arenaIdx) l1() uint {
	if arenaL1Bits == 0 {
		return 0
	} else {
		return uint(i) >> arenaL1Shift
	}
}

// 获得从l1 index开始的arenaL2Bits位，作为L2索引
func (i arenaIdx) l2() uint {
	if arenaL1Bits == 0 {
		return uint(i)
	} else {
		return uint(i) & (1<<arenaL2Bits - 1)
	}
}
```

前面我们提到过，系统上寻址空间为48位。而指针变量为64位。

amd在设计64位系统的虚拟内存空间时，对地址定义做了如下限制：即要求指针值的48-63位，必须要与第47位一致，否则访存时会触发异常。（扩展：[虚拟内存地址空间](https://zh.wikipedia.org/wiki/X86-64)）

根据这一规则，go中将index的计算进行了特殊处理，即向地址追加一个0x800000000000的arena基址再计算index，这会产生如下效果：

- 对于小于等于0x7FFFFFFFFFFF的地址，追加arena基址后，获得的arenaIndex位于arenas二维数组的后半部分。
- 对于大于0x7FFFFFFFFFFF的地址，追加arena基址后，触发了地址值的溢出，因此获得的arenaIndex位于arenas二位数组的前半部分。



### 如何判定内存是否在堆上

go堆中的内存，所有已分配的空间是由称作mspan类型的对象来持有并管理的，在原始虚拟内存中，mallocinit初始化时限定了堆内存的绝对分配范围，mheap通过heapArena对所有的堆内存进行了划分，而heapArena中的spans成员又保存了64MB的空间下每个物理页归属的mspan对象。所以只要是有效的堆内存地址，都可以唯一地映射到一个heapArena对象上。也可以直接映射到一个mspan对象上。

go提供了几个方法来查找指针对应的heapArena对象。



```go
func spanOf(p uintptr) *mspan {
	ri := arenaIndex(p)
	if arenaL1Bits == 0 {
		if ri.l2() >= uint(len(mheap_.arenas[0])) {
			return nil
		}
	} else {
		if ri.l1() >= uint(len(mheap_.arenas)) {
			return nil
		}
	}
	l2 := mheap_.arenas[ri.l1()]
	if arenaL1Bits != 0 && l2 == nil {
		return nil
	}
	ha := l2[ri.l2()]
	if ha == nil {
		return nil
	}
	return ha.spans[(p/pageSize)%pagesPerArena]
}

// 如果p不在有效堆内存范围内，则返回nil
func spanOfHeap(p uintptr) *mspan
```

如果一段arena虚拟内存地址没有在go中被分配过，则其地址对应的heapArena指针为空。



## 内存分配核心方法

malloc.go中提供了几个主要的内存分配方法。这些方法实现了对操作系统虚拟内存空间的分配。通常来说这些方法不提供内存的释放。因为在malloc.go基础上，runtime实现了大部分内存的复用。



### 系统级内存分配方法

```go
// 从v开始映射长度为n的内存，底层依赖mmap
func sysMap(v unsafe.Pointer, n uintptr, sysStat *uint64)
// 从v开始保留长度为n的内存，底层依赖mmap
func sysReserve(v unsafe.Pointer, n uintptr) unsafe.Pointer
// 释放从v开始的长度为n的内存，底层依赖munmap
func sysFree(v unsafe.Pointer, n uintptr, sysStat *uint64)
// 申请长度为n的内存，返回系统指定的虚拟内存空间起始地址
func sysAlloc(n uintptr, sysStat *uint64) unsafe.Pointer
// 向系统归还从v开始的指定长度n的内存，底层依赖madvise
func sysUnused(v unsafe.Pointer, n uintptr)
```

go中对系统级内存分配方法进行了二次封装。代码中给出了依赖的底层系统方法。



### 小内存非堆分配方法

全局小内存块申请方法。该方法传入一个需求大小和对齐大小、返回一个指针，指向申请的空间起始地址。其中align必须是2的指数倍且小于pageSize。

persistentalloc在系统栈上调用了persistentalloc1，实际分配动作由persistentalloc1完成。

```go
func persistentalloc1(size, align uintptr, sysStat *uint64) *notInHeap {
	const (
		chunk    = 256 << 10	// 256KB
		maxBlock = 64 << 10 	// 64KB
	)

	// ...将align对齐到2的指数倍

	// 大于64KB直接向操作系统申请
	if size >= maxBlock {
		return (*notInHeap)(sysAlloc(size, sysStat))
	}

	// 获取当前m对象下的persistentAlloc对象，如果当前没有运行在gmp环境下，则使用
	// 全局的persistentAlloc对象分配内存，需要加锁。
	mp := acquirem()
	var persistent *persistentAlloc
	if mp != nil && mp.p != 0 {
		persistent = &mp.p.ptr().palloc
	} else {
		lock(&globalAlloc.mutex)
		persistent = &globalAlloc.persistentAlloc
	}
	persistent.off = round(persistent.off, align)
	if persistent.off+size > chunk || persistent.base == nil {
		// chunk无法满足需求大小或chunk不存在时，则申请新的chunk
		persistent.base = (*notInHeap)(sysAlloc(chunk, &memstats.other_sys))
		if persistent.base == nil {
			if persistent == &globalAlloc.persistentAlloc {
				unlock(&globalAlloc.mutex)
			}
			throw("runtime: cannot allocate memory")
		}
		persistent.off = 0
	}
	// 在预分配的chunk上规划需要的内存，并找到首地址返回，增加base的偏移
	p := persistent.base.add(persistent.off)
	persistent.off += size
	releasem(mp)
	if persistent == &globalAlloc.persistentAlloc {
		unlock(&globalAlloc.mutex)
	}

  ...
	return p
}
```

这段代码申请了一个满足size需求的空间。align会被对齐到2的指数倍。

- 如果size大于64kb，则直接调用系统mmap分配空间。

- 如果size小于64kb，找到当前m对象下的persistentAlloc对象，在其偏移位上分配一个size的空间。并返回。

- 如果persistentAlloc对象已经不足以分配该size的内存。则直接分配一个新的256KB的chunk，令m的persistentAlloc.base指向该chunk。




go运行时有一种叫m的对象，用于表达一个系统工作线程及其状态。为了加速小内存的线程内快速分配，避免因向堆内存申请空间导致的锁竞争，所以为每个m对象指定了一个persistentAlloc，用于加速线程局部小内存分配速度。同时，runtime中也创建了一个全局带锁的persistentAlloc对象，用于go运行时小内存分配。

persistentalloc调用了persistentalloc1方法。

persistentalloc1方法返回的是一个notInHeap指针，表明了该方法申请的内存不是在go划分的原始堆内存以内申请的。实际上由于调用的是sysAlloc方法，所以chunk分配在哪儿、实际内存在什么位置由操作系统决定。

persistentalloc1分配的内存不提供释放方法。这是由于该方法申请的空间在上层大多提供对象池实现，所以总体上来说其已分配的内存是收敛的。

【图：小内存分配的图例】



#### persistentAlloc

```go
type persistentAlloc struct {
	base *notInHeap	// 当前chunk的首地址
	off  uintptr			// 当前chunk上已经分配空间的偏移量
}
```



### 堆原始内存分配器

sysAlloc只有在堆增长（mheap.grow）的时候才会被调用。

```go
func (h *mheap) sysAlloc(n uintptr) (v unsafe.Pointer, size uintptr) {
	// 将需求空间大小对齐到arena大小，即64MB
	n = round(n, heapArenaBytes)

	// Try to grow the heap at a hint address.
	for h.arenaHints != nil {
		hint := h.arenaHints
		p := hint.addr
		if hint.down {
			p -= n
		}

		if p+n < p {
			v = nil
		} else if arenaIndex(p+n-1) >= 1<<arenaBits {
			v = nil
		} else {
			// 预留当前空闲的地址空间
			v = sysReserve(unsafe.Pointer(p), n)
		}
		if p == uintptr(v) {
			// 更新areanHints当前addr的指针位置，用于下次分配。
			if !hint.down {
				p += n
			}
			hint.addr = p
			size = n
			break
		}
		// 如果当前arenaHints下没有找到合适的空间，则查找下一个
		if v != nil {
			sysFree(v, n, nil)
		}
		h.arenaHints = hint.next
		h.arenaHintAlloc.free(unsafe.Pointer(hint))
	}
	if size == 0 {
    // 一般应用程序不会执行到这里，因为这表明256TB的原始堆空间都已经分配殆尽
		// ...
    // 向系统申请任意起始地址的空间，对齐到heapArenaBytes。
		v, size = sysReserveAligned(nil, n, heapArenaBytes)
		if v == nil {
			return nil, 0
		}

		// 创建新的areanHint并添加到列表
		hint := (*arenaHint)(h.arenaHintAlloc.alloc())
		hint.addr, hint.down = uintptr(v), true
		hint.next, mheap_.arenaHints = mheap_.arenaHints, hint
		hint = (*arenaHint)(h.arenaHintAlloc.alloc())
		hint.addr = uintptr(v) + size
		hint.next, mheap_.arenaHints = mheap_.arenaHints, hint
	}

	// ...

	// 将前面预留的空间重新映射回来。
	sysMap(v, size, &memstats.heap_sys)

mapped:
	// Create arena metadata.
	for ri := arenaIndex(uintptr(v)); ri <= arenaIndex(uintptr(v)+size-1); ri++ {
		// ...
		// 更新mheap.arenas对象，增加对当前申请空间的描述heapArena对象。
		atomic.StorepNoWB(unsafe.Pointer(&l2[ri.l2()]), unsafe.Pointer(r))
	}
	...

	return
}
```

sysAlloc通过hints，从原始规划的堆空间申请一段符合需求量n的内存，返回其空间起始地址和实际mmap的内存大小。所有基于堆内存的分配，最终都指向了sysAlloc方法，这也是操作hints表达的堆内存的唯一途径。

【图：堆原始内存分配器的工作方式】



### 定长对象分配器

对于一些运行时常驻内存的，且有缓存的固定长度对象，比如mspan，go中设计了固定长度对象分配器。由于这类内存需求长度固定易于管理，且需求空间较小，所以没有使用堆原始内存分配器。



```go
type fixalloc struct {
  // 固定对象长度
	size   uintptr
  // 
	first  func(arg, p unsafe.Pointer)
	arg    unsafe.Pointer
  // 空闲对象链表指针
	list   *mlink
  // 当前用于分配新对象的chunk块偏移地址
	chunk  uintptr
  // 当前chunk块中剩余未分配的字节数量
	nchunk uint32
  // 当前chunk块中使用中的字节数量，inuse + nchunk = _FixAllocChunk
	inuse  uintptr
	stat   *uint64
  // 是否需要将新申请的对象空间置为0
	zero   bool
}

type mlink struct {
  // 空闲对象链表指针，空闲的对象的内存是随机值，它的地址空间会被临时视为一个mlink对象
  // 用于构建空闲对象内存链表。当一块空闲对象被分配时，mlink空间被覆盖。
	next *mlink
}
```

定长对象分配器不属于基本的内存分配器，但在堆中经常会用到。其内存的分配和释放实现也很简单



```go
func (f *fixalloc) alloc() unsafe.Pointer {
	if f.size == 0 {
		print("runtime: use of FixAlloc_Alloc before FixAlloc_Init\n")
		throw("runtime: internal error")
	}

	if f.list != nil {
    // 优先从空闲对象链表中查找
		v := unsafe.Pointer(f.list)
		f.list = f.list.next
		f.inuse += f.size
		if f.zero {
			memclrNoHeapPointers(v, f.size)
		}
		return v
	}
  // 向系统申请_FixAllocChunk = 16KB的chunk块，用于对象分配。所以fixalloc只能
  // 分配小于16KB的对象内存。
	if uintptr(f.nchunk) < f.size {
		f.chunk = uintptr(persistentalloc(_FixAllocChunk, 0, f.stat))
		f.nchunk = _FixAllocChunk
	}

	v := unsafe.Pointer(f.chunk)
	if f.first != nil {
		f.first(f.arg, v)
	}
	f.chunk = f.chunk + f.size
	f.nchunk -= uint32(f.size)
	f.inuse += f.size
	return v
}

func (f *fixalloc) free(p unsafe.Pointer) {
  // 释放并将归还的内存解释为mlink，加入到空闲链表中
	f.inuse -= f.size
	v := (*mlink)(p)
	v.next = f.list
	f.list = v
}
```



下面的代码说明了堆对象中的6中定长分配器

```go
type mheap struct {
	...
	// mspan对象分配器
	spanalloc             fixalloc
	// mcache对象分配器
	cachealloc            fixalloc
	// treapNode树堆节点对象分配器
	treapalloc            fixalloc
	// TODO
	specialfinalizeralloc fixalloc
	// TODO
	specialprofilealloc   fixalloc
	// arenaHint对象分配器
	arenaHintAlloc        fixalloc
}
```



## summary

malloc.go中实现了大部分从虚拟内存分配空间的方法。后面要说明的mheap相关内存操作，大部分是基于malloc中的方法来实现基本虚拟内存和基本堆内存的分配。

malloc中管理内存的最小单位是arena，虽然局部涉及到了mspan类型，但大部分操作还是基于原始虚拟内存空间的操作。

# mheap

堆作为运行时的动态内存分配器，主要需要解决以下问题：

1. 如何快速有效的响应内存分配的需求。
2. 如果避免内存碎片的产生。

go中通过以下途径，来解决上述问题

1. 缓存已申请但未分配的内存。
2. 将缓存的内存块按照大小分开管理。
3. 将不需要被GC扫描的内存分开管理。
4. 将释放的连续内存合并。
5. 为每个线程建立独立的内存分配器。
6. 使用专用分配器为常用定长对象分配空间。



堆中的内存以物理页（page）为单位进行管理。当前go中定义的物理页大小为8KB

以128个物理页（1MB）为界。

占用物理页大于128个的，按小对象处理

占用物理页大于等于128个的，按大对象处理



## mspan

```go
type mspan struct {
  // 用于构成链表时的链表指针
	next *mspan
	prev *mspan

  // mspan指向的内存空间起始地址，可以通过mspan.base()方法获得
	startAddr uintptr
  // 当前mspan下的内存空间包含多少个系统页
	npages    uintptr

  // 用于栈内存管理TODO
	manualFreeList gclinkptr

	// 当前mspan空闲可分配的对象索引。如果freeindex==nelems，则mspan已无可用空间
	freeindex uintptr
	// 当前mspan能分配的elemsize大小对象的最大数量
	nelems uintptr

	// 作为当前allocBits的一个滑动窗口，用于快速计算当前的可用空间。
  // allocCache的64个bit位，表达了64个slot。初始化为^0。
  // 应用中，go通过德布鲁因算法快速获得当前allocCache中bit为1的最低位，作为
  // freeindex的补充，通过freeindex+lowbit(allocCache)获得当前空闲的slot。
  // 
  // 同时，由于allocCache只能表达64个slot，所以还需要配合allocBits来使用。
	allocCache uint64

	// 当前mspan中slot使用情况汇总。在mspan初始化时按照elemsize申请空间。其每个
  // bit位表达一个slot，如果bit位为1，表明这个slot已经被分配，否则slot空闲。
	allocBits  *gcBits
	gcmarkBits *gcBits

  // GC相关，暂不讨论
	divMul      uint16
	baseMask    uint16
  // 当前mspan上已经分配的对象数量
	allocCount  uint16
  // 当前mspan的spanClass
	spanclass   spanClass
  // 表明当前mspan是否被mcache缓存
	incache     bool
  // mspan状态，共有四种状态
  // _MSpanDead、_MSpanInUse、_MSpanManual、_MSpanFree
	state       mSpanState
  // 从当前mspan分配对象时，是否需要将对象的内存空间初始化为0
	needzero    uint8
	divShift    uint8
	divShift2   uint8
  // mspan能分配的对象大小
	elemsize    uintptr
  // 当前mspan最近一次使用的nano时间戳，用于判定是否需要向系统归还内存
	unusedsince int64
	npreleased  uintptr    // number of pages released to the os
  // 当前mspan指向空间的结束为止+1字节
	limit       uintptr
	speciallock mutex      // guards specials list
	specials    *special   // linked list of special records sorted by offset.
}
```

堆中无论是空闲还是使用中的内存，都可以用mspan对象来表达。mspan中持有系统页整数倍的内存空间。通过一个状态标记来标识当前mspan是被使用中还是空闲中。

mspan中持有的空间，可用来分配一个或多个相同类型的对象空间，elemsize表明了对象类型占用的空间大小。nelems表明了当前mspan下可以分配对象的最大数量。



mspan在mheap、mcentral、mcache中都有缓存，与mcentral、mcache不同的是，在mheap中缓存的mspan对象是以page为单位的，npages表明了mheap中的mspan对象所持有的系统页数量，其spanclass成员为0。



在堆中，mspan会通过一个包含头尾指针的mSpanList和prev、next指针串联成为一个双向链表。

mSpanList结构如下：

```go
type mSpanList struct {
	first *mspan // first span in list, or nil if none
	last  *mspan // last span in list, or nil if none
}
```



## 堆缓存管理

```go
type mheap struct {
  // 用于锁住allspans对象
	lock      mutex
  // 小于等于128个pageSize的空闲mspan链表数组，按照page数量不同而区分保存
	free      [_MaxMHeapList]mSpanList
  // 大于128个pageSize的空闲mspan树堆
	freelarge mTreap
  // 使用中的小于等于128个pageSize的mspan链表数组，按照page数量不同区分保存
	busy      [_MaxMHeapList]mSpanList
  // 使用中的大于128个pageSize的空闲mspan链表，由于总量不会太大，仅使用一个链表保存
	busylarge mSpanList
	...
}
```

mheap中使用4个数据结构表达了当前已经缓存的原始堆内存。

用于分配tiny和small对象空间的mspan，通过free、busy链表数组来记录

用于分配large对象空间的mspan，通过freelarge树堆、busylarge链表来记录



### 小内存

free和busy是一个mSpanList链表数组，数组大小为128，用于分别对应持有1-128个系统页pages空间的mspan对象链表头。free保存了当前处于空闲状态的mspan，而busy保存了当前使用中的mspan。



### 大内存

mheap中建立了一个树堆（[Treap](https://en.wikipedia.org/wiki/Treap)），用于保存空闲的大内存mspan对象。在树堆中每个节点都包含一个mspan指针、一个当前mspan占用的npages数量和一个uint32的优先级priority。其中，page数量作为首要排序key，priority作为次要排序key。Treap中节点的组织需要遵循以下两点原则：

- 左子树下的mspan占用的pages数量小于父节点，右子树下的mspan占用pages数量大于父节点。
- 父节点的priority值是整棵子树中最小的。

priority值在树节点被插入时由当前m对象的fastrand随机种子生成随机数写入。

树堆的旋转，树堆在插入和删除节点时，会通过左右旋转来确保Treap始终满足其原则。

TODO：补充树堆插入查找删除时间空间复杂度



## 堆增长

```go
func (h *mheap) grow(npage uintptr) bool {
	// 调用sysAlloc分配ask大小的空间，实际返回的size对齐到64MB
	ask := npage << _PageShift
	v, size := h.sysAlloc(ask)
	if v == nil {
		return false
	}

	// 这里创建了一个状态为_MSpanInUse状态的mspan，目的是在freeSpanLocked中可以
	// 实现连续mspan的合并
	s := (*mspan)(h.spanalloc.alloc())
	s.init(uintptr(v), size/pageSize)
	h.setSpans(s.base(), s.npages, s)
	atomic.Store(&s.sweepgen, h.sweepgen)
	s.state = _MSpanInUse
	h.pagesInUse += uint64(s.npages)
	h.freeSpanLocked(s, false, true, 0)
	return true
}
```

堆增长函数调用了此前讨论的系统原始内存分配方法mheap.sysAlloc来分配堆内存。将其状态设置为_MSpanInUse，并调用freeSpanLocked将未使用到的空间放到空闲内存池中。

堆的增长会在sysAlloc中对齐到64MB，所以一次申请至少为64MB空间。触发grow的内存分配操作，除返回满足申请的部分外，其余的将被mheap缓存。而下次内存申请时将不再grow，而是优先通过分割缓存中的内存来完成分配。除非因缓存不足而再次触发grow。



## 堆内存分配与释放

堆内存的分配释放主要通过allocSpanLocked和freeSpanLocked完成。

- allocSpanLocked优先从已分配堆内存中进行分配，如果没有合适的堆内存块（mspan），则触发堆增长，并再次申请。
- freeSpanLocked将内存块归还到已分配堆内存队列中。一个额外动作是，通过检查当前mspan空间的和其连续空间的前后mspan是否均处于_MSpanFree状态，来决定是否要合并这些mspan。



### allocSpanLocked（分配）

```go
func (h *mheap) allocSpanLocked(npage uintptr, stat *uint64) *mspan {
	var list *mSpanList
	var s *mspan

	// 如果需求page数量小于128，则优先从mheap.free中获取满足npage数量的mspan
	for i := int(npage); i < len(h.free); i++ {
		list = &h.free[i]
		if !list.isEmpty() {
			s = list.first
			list.remove(s)
			goto HaveSpan
		}
	}
	// 如果npage大于128，则从mheap.freelarge中分配满足npage数量的mspan
	s = h.allocLarge(npage)
	if s == nil {
		// mheap.freelarge中没有满足条件可用的mspan，则扩张堆空间。
		if !h.grow(npage) {
			return nil
		}
		// 再次尝试从堆中申请
		s = h.allocLarge(npage)
		if s == nil {
			return nil
		}
	}

HaveSpan:
	// ...

	if s.npages > npage {
		// 如果实际分配的空间大于需求的空间，则使用多余出来的空间构造出一个新的mspan
		// 将新的mspan归还给堆
		t := (*mspan)(h.spanalloc.alloc())
		t.init(s.base()+npage<<_PageShift, s.npages-npage)
		s.npages = npage
		h.setSpan(t.base()-1, s)
		h.setSpan(t.base(), t)
		h.setSpan(t.base()+t.npages*pageSize-1, t)
		t.needzero = s.needzero
		s.state = _MSpanManual // prevent coalescing with s
		t.state = _MSpanManual
		h.freeSpanLocked(t, false, false, s.unusedsince)
		s.state = _MSpanFree
	}
	s.unusedsince = 0

	// 更新heapArena
	h.setSpans(s.base(), npage, s)

	// ...
	return s
}
```

在堆中的所有mspan对象，都是通过allocSpanLocked创建的。allocSpanLocked仅会返回满足需求页数量的内存空间，如果查找到堆中的mspan拥有的系统页数量超过需求量，多余的系统页将被额外新创建的mspan持有，并写回堆缓存中。



### freeSpanLocked（释放）

```go
func (h *mheap) freeSpanLocked(s *mspan, acctinuse, acctidle bool, unusedsince int64) {
  // ...

	// 将mspan从使用中列表脱链，修改状态为空闲状态
	s.state = _MSpanFree
	if s.inList() {
		h.busyList(s.npages).remove(s)
	}

	// 如果与s连续内存的低地址方向也是一个已分配的空闲的mspan，则合并s和before
	// 两个mspan
	if before := spanOf(s.base() - 1); before != nil && before.state == _MSpanFree {
		s.startAddr = before.startAddr
		s.npages += before.npages
		s.npreleased = before.npreleased
		s.needzero |= before.needzero
		h.setSpan(before.base(), s)

		// 从空闲列表中移除before mspan
		if h.isLargeSpan(before.npages) {
			h.freelarge.removeSpan(before)
		} else {
			h.freeList(before.npages).remove(before)
		}
    // 释放before mspan对象，归还到spanalloc固定对象分配器中
		before.state = _MSpanDead
		h.spanalloc.free(unsafe.Pointer(before))
	}

	// 与before一样
	if after := spanOf(s.base() + s.npages*pageSize); after != nil && after.state == _MSpanFree {
		s.npages += after.npages
		s.npreleased += after.npreleased
		s.needzero |= after.needzero
		h.setSpan(s.base()+s.npages*pageSize-1, s)
		if h.isLargeSpan(after.npages) {
			h.freelarge.removeSpan(after)
		} else {
			h.freeList(after.npages).remove(after)
		}
    // 释放before mspan对象，归还到spanalloc固定对象分配器中
		after.state = _MSpanDead
		h.spanalloc.free(unsafe.Pointer(after))
	}

	// 将前后合并后的s放到空闲mspan列表中。
	if h.isLargeSpan(s.npages) {
    // 如果是大于1MB的mspan，则插入到树堆中
		h.freelarge.insert(s)
	} else {
		// 如果是小于1MB的mspan，则插入到指定page数量的mspan链表中
		h.freeList(s.npages).insert(s)
	}
}
```

在busy列表中查找使用中的mspan，时间复杂度为常数级。



## 基于缓存的分配器

### sizeClass

在内存管理中，大块内存比较容易管理，小块内存则是产生内存碎片的祸根。如果任由小块内存在系统中随机分配，最终可能会出现无法申请连续大块内存的情况。

go中为了避免内存碎片，加速小内存的分配效率。对小内存进行了更细粒度的划分。

go将小于等于32KB的内存，划分了67个级别，称之为sizeClass。每个级别对应的内存块大小保存在class_to_size数组中。以class_to_size[4]为例，其值为32，即如果我们为一个大小为18字节的对象分配空间时，实际runtime中，会对齐到class_to_size[4]的32字节来申请空间。

这种对小内存的管理方式，为我们实现不同大小对象之间无干扰的分配提供了可能。代价则是会损失一部分的空间。在sizeclasses.go的注释中给出了各个sizeClass可能浪费的最大空间。

至于为什么是67，这可能是相对准备32768个不同大小的分配器 和 过分离散的class导致的性能降低之间的一个折中。

```go
const (
	_MaxSmallSize   = 32768
	smallSizeDiv    = 8
	smallSizeMax    = 1024
	largeSizeDiv    = 128
	_NumSizeClasses = 67
)

// 67种sizeclass对应的分配空间大小
var class_to_size = [_NumSizeClasses]uint16{...}
// 67种sizeclass对应的分配器应持有的系统页数量，被mcentral使用
var class_to_allocnpages = [_NumSizeClasses]uint8{...}
// 小于1024字节的任意空间大小对应的sizeclass
var size_to_class8 = [smallSizeMax/smallSizeDiv + 1]uint8{...}
// 大于1024、小于32768字节的任意空间大小对应的sizeclass
var size_to_class128 = [(_MaxSmallSize-smallSizeMax)/largeSizeDiv + 1]uint8{...}
```



### spanClass

spanClass是sizeClass的一种特殊表达，它将sizeClass的索引（即0-66）左移一位，低位用于表达noscan标记。如果最低位为1，表示不需要GC扫描该spanClass下的对象。

go在编译期间，可以通过代码的AST分析可以得出内存分配的类型，以及类型中是否包含指针类型。这是对象是否需要被GC扫描的重要依据。当runtime进行内存分配时，可以通过将需要扫描的和不需要扫描的对象分开管理申请和释放。提升无指针类型申请和释放的效率。



### mcentral

mheap中声明了一个带有mcentral对象的数组。

```go
type mheap struct {
	// mcentral对象数组
	central [numSpanClasses]struct {
		// mcentral对象
		mcentral mcentral
		// 用于对齐的padding块
		pad      [sys.CacheLineSize - unsafe.Sizeof(mcentral{})%sys.CacheLineSize]byte
	}
}
```

这里总共定义了67<<1个mcentral对象，用于分配小对象内存。mcentral定义如下



```go
type mcentral struct {
	// 互斥锁，用于保护nonempty和empty链表
	lock      mutex
	// 当前mcentral对象的spanClass
	spanclass spanClass
	// 空闲mspan链表
	nonempty  mSpanList
	// 使用中mspan链表
	empty     mSpanList
	// 
	nmalloc uint64
}
```

中央分配器比较简单，它包含当前mcentral的spanclass，以及空闲和使用中的mspan链表。

empty中保存了完全未被使用的mspan，nonempty中保存了使用中的mspan，一个使用中的mspan，仍然可能有未被分配的空间可以被利用。



### mcache

```go
type mcache struct {
	next_sample int32   	// GC相关，先不看；trigger heap sample after allocating this many bytes
	local_scan  uintptr 	// GC相关，先不看；bytes of scannable heap allocated
	
  tiny             uintptr	// 当前tiny块的起始地址
  tinyoffset       uintptr	// 当前tiny块已经分配到的位置
  local_tinyallocs uintptr // 当前已经分配的tiny块数量；number of tiny allocs not counted in other stats
  // The rest is not accessed on every malloc.
  alloc [numSpanClasses]*mspan 			//  spans to allocate from, indexed by spanClass

  stackcache [_NumStackOrders]stackfreelist

  // Local allocator stats, flushed during GC.
  local_largefree  uintptr                  // bytes freed for large objects (>maxsmallsize)
  local_nlargefree uintptr                  // number of frees for large objects (>maxsmallsize)
  local_nsmallfree [_NumSizeClasses]uintptr // number of frees for small objects (<=maxsmallsize)
}
```



## new关键字

### newobject

### newarray

### mallocgc



# summary

## 大对象内存分配

调用链分析

## 小对象内存分配

调用链分析

# 其他

## 内存用量统计

## 德布鲁因序列

德布鲁因序列描述了这样一个二进制环形串，设串的长度为2的n次方，那么串中任意连续的n个二进制bit序列，都不一样。go中应用德布鲁因序列，主要用于找出bitmask形式的无符号整数中，哪些位为1。为了表示一个uint64的位，这里n设为6，即64。预定义一个数组，用于表示德布鲁因子序列对应的bit位。下面的代码展示了go中德布鲁因索引的计算方式：

```go
const deBruijn64 = 0x0218a392cd3d5dbf	//0000001000011000101000111001001011001101001111010101110110111111
func Ctz64(x uint64) int {
	x &= -x                      // isolate low-order bit
	y := x * deBruijn64 >> 58    // extract part of deBruijn sequence
	i := int(deBruijnIdx64[y])   // convert to bit index
	z := int((x - 1) >> 57 & 64) // adjustment if zero
	return i + z
}
```

https://en.wikipedia.org/wiki/De_Bruijn_sequence
http://supertech.csail.mit.edu/papers/debruijn.pdf

## fastrand

```go
func fastrand() uint32 {
	mp := getg().m
	// Implement xorshift64+: 2 32-bit xorshift sequences added together.
	// Shift triplet [17,7,16] was calculated as indicated in Marsaglia's
	// Xorshift paper: https://www.jstatsoft.org/article/view/v008i14/xorshift.pdf
	// This generator passes the SmallCrush suite, part of TestU01 framework:
	// http://simul.iro.umontreal.ca/testu01/tu01.html
	s1, s0 := mp.fastrand[0], mp.fastrand[1]
	s1 ^= s1 << 17
	s1 = s1 ^ s0 ^ s1>>7 ^ s0>>16
	mp.fastrand[0], mp.fastrand[1] = s0, s1
	return s0 + s1
}
```

## GC初探

# 引用

[https://github.com/qyuhen/book/blob/master/Go%201.5%20%E6%BA%90%E7%A0%81%E5%89%96%E6%9E%90.pdf](https://github.com/qyuhen/book/blob/master/Go 1.5 源码剖析.pdf)