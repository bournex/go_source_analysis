虚拟内存概念介绍

操作系统的内存管理简介

内存管理面临的问题和挑战

内存管理的常见方式

# overview

go的内存管理参考了tcmalloc，tcmalloc是google开源的一款内存管理工具，它比glibc中自带的ptmalloc性能要更高。

在tcmalloc中，对内存进行划分，大小内存需求进行了不同的处理。对于小内存，按照需求量的不同，实际申请空间被对齐到了预先规划好的大小。同时为小内存的申请，增加了线程局部分配器，用于快速无锁分配。

而对于大内存需求，则不提供线程局部分配器。所有请求都统一通过heap来管理。

## ptmalloc  vs  tcmalloc

ptmalloc是glibc中默认的内存分配器。

# 原始堆内存

操作系统提供的虚拟内存空间，是由寻找空间决定的，在amd64位系统上，有48个bit用于寻址，所以我们的可用地址空间，在1<<48个，这里没有占满64bit，因为48bit已经提供了256TB的虚拟地址寻址空间。这几乎已经足够大部分的系统应用了。

go中共使用了128TB的内存，用于初始化内存分配池

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
```

## arenaHints

```go
type arenaHint struct {
	addr uintptr		// 记录了在当前arenaHint的区间内，已经分配到的内存地址位置，addr只会单调增长或降低。
	down bool			// 默认为false，从低地址向高地址增长。
	next *arenaHint		// 下一个arena区间的arenaHint对象
}
```


arenahint指向最原始的虚拟内存空间，runtime.sysAlloc函数分配空间时以heapArenaBytes(64MB)为单位从arenaHint链表头对象开始分配空间。当第一个arenaHint的1TB空间被用完后，将开始使用下一个arenaHint的空间。

# mheap

## overview

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
  
	// 状态所有mspan对象的slice，用于gc时快速扫描所有已分配的mspan对象。
	allspans []*mspan // all spans out there
	
  // arena描述，heapArena有两个成员，其中heapArena.bitmap是用于标识一个
  // arena中每一个8byte中是否包含指针，heapArean.spans用于表示当前arena
  // 下的每一个页被哪个mspan对象所占用。
  // arenas是一个二维数组，通过这个二维数组，构成了对整个虚拟内存寻址空间的
  // 描述。在64位系统中，arenaL1Bits为0，arenaL2Bits为22（4MB）。所以
  // 这里总共有，所以堆中用4MB的空间，描述了整个虚拟内存的使用情况和包含指针
  // 情况，bitmap会在gc时起到关键作用。
	arenas [1 << arenaL1Bits]*[1 << arenaL2Bits]*heapArena

	// 整个堆内存空间的索引。
	arenaHints *arenaHint

	// 中央分配器数组，按照sizeClass区分，不同的中央分配器，所能分配的空间大小不同。
  // pad为了将结构体对齐到cpu的cacheline。
	central [numSpanClasses]struct {
		mcentral mcentral
		pad      [sys.CacheLineSize - unsafe.Sizeof(mcentral{})%sys.CacheLineSize]byte
	}

  // 六个常用固定类型对象分配器。
	spanalloc             fixalloc
	cachealloc            fixalloc
	treapalloc            fixalloc
	specialfinalizeralloc fixalloc
	specialprofilealloc   fixalloc
	speciallock           mutex   
	arenaHintAlloc        fixalloc
}
```

## arena

arena是tcmalloc中没有的概念，go中基于垃圾回收的考虑，需要对内存进行更加精细化的管理。所以增加了arena的概念。arena中文翻译是竞技场，没什么卵用。go中将堆内存划分为64MB的块。



## heapArena

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



heapArena标识一个64MB的arena。整个虚拟内存地址空间，被按照arena管理起来，每64MB对应一个heapArena对象。heapArena对象通过persistentalloc申请，申请的对象被挂到mheap.arenas二级索引下。

spans我们稍后再看



## 内存分配核心方法

malloc.go中提供了几个主要的内存分配方法。这些方法实现了对操作系统虚拟内存空间的分配。通常来说这些方法不提供内存的释放。因为在malloc.go基础上的mheap对象实现了大部分内存的复用。



### persistentalloc

全局小内存块申请方法。该方法传入一个需求大小和对齐大小、返回一个指针，指向申请的空间起始地址。其中align必须是2的指数倍且小于pageSize。

persistentalloc在系统栈上调用了persistentalloc1，实际分配动作由persistentalloc1完成。

```go
func persistentalloc1(size, align uintptr, sysStat *uint64) *notInHeap {
	const (
		chunk    = 256 << 10	// 256KB
		maxBlock = 64 << 10 	// 64KB
	)

  // ...将align对齐到2的指数倍

  // 大于64KB直接分配
	if size >= maxBlock {
		return (*notInHeap)(sysAlloc(size, sysStat))
	}

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
  // 在预分配的chunk上规划需要的内存，并找到首地址返回
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

这段代码申请了一个满足size需求的空间。

如果size大于64kb，则直接调用系统mmap分配空间。

如果size小于64kb，找到当前m对象下的persistentAlloc对象，在其偏移位上分配一个size的空间。并返回。

如果persistentAlloc对象已经不足以分配该size的内存。则直接分配一个新的chunk，令m的persistentAlloc.base指向该chunk。



persistentAlloc对象有全局和m局部之分。全局persistentAlloc对象包含一个全局锁，用于没有m的分配场景。

注意persistentalloc1方法返回的是一个notInHeap指针，表明了该方法申请的内存不是在go划分的堆内存以内申请的。实际上chunk分配在哪儿，由于调用的是sysAlloc方法，所以实际内存在什么位置由操作系统指定。

此外，persistentalloc1分配的内存不提供释放方法。这是由于该方法申请的空间在上层大多提供对象池实现，所以总体上来说其已分配的内存是收敛的。

##### persistentAlloc

```go
type persistentAlloc struct {
	base *notInHeap	// 当前chunk的首地址
	off  uintptr			// 当前chunk上已经分配空间的偏移量
}
```



### mheap.sysAlloc

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
    if...{
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
  // ...

	// 检查指针合法性
	{
		var bad string
		p := uintptr(v)
		if p+size < p {
			bad = "region exceeds uintptr range"
		} else if arenaIndex(p) >= 1<<arenaBits {
			bad = "base outside usable address space"
		} else if arenaIndex(p+size-1) >= 1<<arenaBits {
			bad = "end outside usable address space"
		}
		if bad != "" {
			// ...
		}
	}

	if uintptr(v)&(heapArenaBytes-1) != 0 {
		throw("misrounded allocation in sysAlloc")
	}

	// 将前面预留的空间重新映射回来。
	sysMap(v, size, &memstats.heap_sys)

mapped:
	// Create arena metadata.
	for ri := arenaIndex(uintptr(v)); ri <= arenaIndex(uintptr(v)+size-1); ri++ {
		// ...更新mheap.arenas对象，增加对当前申请空间的描述heapArena对象。
		atomic.StorepNoWB(unsafe.Pointer(&l2[ri.l2()]), unsafe.Pointer(r))
	}
  ...

	return
}
```

sysAlloc通过hints，从原始规划的堆空间申请一段符合需求量n的内存，返回其空间起始地址和实际mmap的内存大小。所有基于堆内存的分配，最终都指向了sysAlloc方法，这也是操作hints表达的堆内存的唯一途径。

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
```



#### mallocgc

它是golang中new关键字的实现



##### 堆增长

heap每次grow都会创建一个新的span，新申请空间在heap层对齐到pagesize，到sys层对齐到arena，申请完成后span会记录下来申请内存的起始位置、page数量，状态置为_MSpanInUse。需要注意span没有固定长度，arena有固定长度。heapArena中的spans是一个arenaSize/pageSize数量的mspan列表，也就是说，arena中的每一个page，都应该有一个mspan指针，都需要被一个mspan管理。当然arena中的一段连续空间，可能被同一个mspan管理。对于一次分配正好跨多个arena的情况，arenaIndex解决了多个arena对象获取的索引计算问题。所以一个arena上，最多会有pagesPerArena个有效mspan指针。

对于小对象的分配，优先会使用m中自带的mcache.nextFree来分配，如果mcache中空间不足就会通过mcache.refill来向mcentral申请空闲span。无论是mcache还是mcentral，都是按照spanclass来分类缓存span的。所以此处不用关心分配内存的大小。如果mcentral中也没有足量的mspan了，那么会通过mcentral.grow来直接向堆申请空间。

mheap提供的allocSpanLocked方法首先检查需要多大page数量的空间，如果需要的pages数量小于128个，则在mheap.free中申请，如果在mheap.free中没有找到可用空间，或需要空间大于128个page，则在mtreap中申请空间。如果mtreap中也没有足够的mspan。则调用mheap.grow从原始堆空间申请。注意申请完的堆内存，并不进入到mheap.free或mheap.mtreap，而是构建新的span对象直接返回。由于原始堆中是以64MB的arena为单位申请空间的，需求mspan大小很可能远低于这个值。这种情况下，原始堆内存被分割成两个mspan，一个进入到堆的

### fixalloc

固定长度对象分配器池

```go
type fixalloc struct {
	size   uintptr		// 单个对象大小
	first  func(arg, p unsafe.Pointer) // 回调函数
	arg    unsafe.Pointer	// 回调函数参数指针
	list   *mlink		// fixalloc已经分配的对象的缓存链表，调用fixalloc.free被归还回来的对象，会被加入到list里面。
	chunk  uintptr 		// 对象空间地址空间，大小为nchunk。use uintptr instead of unsafe.Pointer to avoid write barriers
	nchunk uint32		// 固定为_FixAllocChunk，即（16 << 10 = 16KB），一次从系统申请固定nchunk长度的空间
	inuse  uintptr 		// 当前chunk使用中的内存量
	stat   *uint64		// 全局mstats的统计计数变量
	zero   bool 		// 是否需要置0初始化
}
```

用于分配固定长度的对象，在初始化时传入单个对象的大小保存到size成员，此后每次分配内存都按照size来分配。归还回来的对象会以链表节点的形式添加到list链表中。
fixalloc依赖persistentalloc方法实现扩张，每当list中没有空闲对象可以使用时，便调用persistentalloc申请一个16KB的chunk。所以fixalloc申请的对象，也不在堆上管理。

mheap对象中持有六个fixalloc对象，名称和作用如下：
**spanalloc**:用于分配和管理mspan对象
**cachealloc**:用于分配和管理mcache对象
**treapalloc**:用于分配和管理treapNodes对象
**specialfinalizeralloc**:TODO
**specialprofilealloc**:TODO
**arenaHintAlloc**:用于分配和管理arenaHint对象



### mheap again

为了方便介绍堆内存的管理机制，这里需要先简要介绍一下mheap对象中的主要数据成员

```go
//go:notinheap
type mheap struct {
	lock      mutex
	free      [_MaxMHeapList]mSpanList		// 管理小于128个page的空闲mspan链表的数组
	freelarge mTreap					// 管理大于128个page的空闲mspan树堆
	busy      [_MaxMHeapList]mSpanList 	// 管理正在使用中的小于128个page的mspan链表的数组
	busylarge mSpanList                			// 管理正在使用中的大于128个page的mspan链表
	....	
}
```



根据前面叙述，一个mspan可能指向一个或多个page，在mheap中，free成员保存了空闲的、未被使用的mspan，它维护了一个大小为128的mSpanList链表数组。数组中每个成员，对应了拥有其索引值+1个page的mspan链表的头指针。由于堆上内存块申请都是对齐到page的，所以这种对mspan的管理方式，可以让分配器快速找到当前需要page数量的空闲mspan对象。

同理，对于已经被申请使用的mspan，mheap使用busy成员来保存这些mspan。



#### mTreap

```go
//go:notinheap
type mTreap struct {
	treap *treapNode	// 树堆根节点
}

//go:notinheap
type treapNode struct {
	right     *treapNode 	// 右子树；all treapNodes > this treap node
	left      *treapNode 	// 左子树；all treapNodes < this treap node
	parent    *treapNode 	// 父节点；direct parent of this node, nil if root
	npagesKey uintptr    	// 当前节点mspan占用的page数量；number of pages in spanKey, used as primary sort key
	spanKey   *mspan    	// mspan指针；span of size npagesKey, used as secondary sort key
	priority  uint32     	// 平衡系数；random number used by treap algorithm to keep tree probabilistically balanced
}
```



mTreap是一个排序的AVL树。每个节点保存了一个mspan对象。mTreap采用mspan的page数量作为排序依据。左子树为page数量小于根的节点，右子树为page数量大于根的节点。除page数外，如果两个节点的page数量相同，则secondary排序依据是mspan的地址。
树节点还有一个priority属性，是一个随机数生成的平衡系数。mTreap中保证任一节点的priority必然比他的左右子节点要小。这里通过rotateLeft和rotateRight两个函数实现了树的旋转。
mTreap提供一个insert方法用于插入mspan。并提供了两个方法用于获取并删除mspan。其中removeSpan传入一个mspan指针，含有该指针的节点会被从mTreap中移除。而remove则传入一个page数量，用于获取最小可以容纳该page数量的mspan指针，并从mTreap中移除该节点。
以上，mTreap就构成了go中大内存（大于128page）的空闲内存管理。
对于非空闲的大内存，没有使用mTreap，参见mheap中的busylarge，是mspan链表来实现的。

##### 小常识

可以注意到，runtime包中很多对象都包含一个init函数，用于初始化成员变量。但是我们在编码过程中却不需要显式初始化，那是因为go替我们完成了初始化，但是在go语言内部，没有在堆上分配的对象，是不会被初始化的（内存置0），所以很多runtime的结构，都需要这样的显式初始化。

#### mSpanList

```go
func (list *mSpanList) init() {
	list.first = nil
	list.last = nil
}
```

mspan双向链表，提供了insert和insertBack，分别用于插入一个mspan对象到链表头和链表尾。提供了一个remove方法用于移除并返回一个mspan。提供一个takeAll方法用于合并一个mSpanList下的所有mspan到当前链表尾部。以及一个isEmpty的判空方法。
值得注意的是，mSpanList并没有依据mspan指向空间的大小来管理mspan，这是因为使用mSpanList的地方，往往是根据mspan拥有空间的大小来分别创建不同的mSpanList的。


##### mspan的产生

mspan产生于mheap的grow方法。当上层调用allocSpanLocked方法时，是在请求一个满足需求page数量的mspan。mheap会优先在mTreap中查找，如果没有就会grow申请空间，并新建一个mspan指向这个空间。新建的mspan被加入到mTreap中，allocSpanLocked会再次向mTreap申请mspan，这次必然会申请到该刚申请的mspan。
mspan的产生过程详细描述TODO

##### sizeClass

见sizeclasses.go。
在向系统申请空间时，由于对齐的需要，实际申请的空间往往大于我们需要的空间。对于内存管理，大段内存往往是以page来对齐的。但是小内存申请对齐到page浪费太大。应该以更小的单元来申请小内存。所以在以page为单位的mspan基础上，GO将0-32KB的小内存申请进行了划分。所有0-32KB的小内存申请，对齐到了67个离散的class中。
class_to_size定义了这67个class对应的内存分块大小。索引是class，值是size，这里的值是申请小对象的实际申请空间。
内存分配期间，我们持有的往往是大小，而不是sizeClass，所以，还需要将要申请的空间大小，对应到sizeClass上。size_to_class8和size_to_class128两个数组实现了这样的映射。

回顾一下内存分配的大小划分：
tiny - 小于16字节的空间，且没有指针不需要被扫描的
small - 0 - 32KB的空间
large - 大于32KB的空间

所有的tiny、small都是通过mcache对象来分配的，而所有的large都是通过直接在arena上分配新的mspan来实现的

### mheap again & again

```go
type mheap struct {
	allspans []*mspan 		// 所有已经分配的mspan都会通过recordspan方法添加到allspans slice上；all spans out there
	arenaHints *arenaHint	// 前面已经描述过的，用于描述每个最大粒度内存块的对象链表。
	arenas [1 << arenaL1Bits]*[1 << arenaL2Bits]*heapArena	// 描述arena内部结构的对象的二级缓存列表。
	central [numSpanClasses]struct {
		mcentral mcentral
		pad      [sys.CacheLineSize - unsafe.Sizeof(mcentral{})%sys.CacheLineSize]byte
	}					// 按spanClass来划分的中央分配器列表。pad用于对齐到cpu的cacheline
	....
}
```

这里有几个宏需要介绍
numSpanClasses - 这是sizeclass的二倍，注意区分一下sizeclass和spanclass，在sizeclass的基础上，spanclass有分为有指针和无指针的mspan，所以，spanclass数量 = sizeclass数量 << 1，spanclass的最低bit位为1的，表示无指针的mspan，0表示有指针的对象。所以这里central包含了67 << 1 = 134个mcentral对象。有无指针的内存申请分开处理，用于加速GC。
arenaL1Bits、arenaL2Bits，这两个宏用于构建heapArena二级缓存。在64位系统上，arenaL1Bits为0，arenaL2Bits为22。即地址的前0位为L1的索引，地址从L1开始的22位为L2索引。这里有一个概念需要说明，64位系统上，uintptr的寻址空间并不是1<<64个地址，而是1<<48个地址，剩余的高位是不被处理的。所以刨除了L1和L2之后，还剩26个bit位，即64MB，这就是为什么之前在描述heapArena时说，一个heapArena对象描述了64MB的地址空间。还有一个点需要关注的是根据指针计算索引的方法arenaIndex：

```go
func arenaIndex(p uintptr) arenaIdx {
	return arenaIdx((p + arenaBaseOffset) / heapArenaBytes)
}
```

其中arenaBaseOffset是1<<47，是48bit寻址空间的最高位。地址p加上这个offset之后，大于1<<47的高地址落在了L2的低索引位上，小于1<<47的低地址落在了L2的高索引位上。
拥有了以上方法后，我们可以通过一个给定的地址p，很快计算出来它在哪个heapArena对象上，以及他属于哪个mspan。参见spanOf方法

下面看一下mcentral

### mcentral

```go
type mcentral struct {
	lock      mutex			// nonempty和empty链表的锁
	spanclass spanClass	// 当前mcentral的spanClass。
	nonempty  mSpanList 	// 有空闲空间的span链表。 list of spans with a free object, ie a nonempty free list
	empty     mSpanList 		// 没有空闲空间的span链表。list of spans with no free objects (or cached in an mcache)
	nmalloc uint64			// 当前已经分配的对象数量
}
```

对于一个mspan，他单次能分配的空间大小是以其成员elemsize来计算的。而这个值是由mcentral中的spanclass来决定的。
已经被mcache持有的，或已经被完全分配的mspan，被加入到empty链表中，没有被分配的在nonempty中。
当前已经提供的五个方法
init - 初始化
cacheSpan - 获取一个当前sizeclass的空闲mspan，写入到empty中
uncacheSpan - 外部归还一个mspan。写入到nonempty中
freeSpan - 完全释放mspan，即归还到heap，被mcentral释放的mspan，最终归还到了mheap的free链表或freeLarge树堆中。这个函数会在GC阶段被调用。
grow - 向heap申请新的mspan。

这里有个区分：mheap中管理的mspan，是通过占用的page数量来归类的，而mcentral中是通过spanclass来归类的。从函数签名上可以看出来，mheap的相关申请函数，都带有npages参数。而对于小内存的申请，往往是通过spanclass或sizeclass。

### mcache

mcentral是带锁的全局分配器，为了加速分配性能，go为每个m和p创建了一个mcache对象。由于m和p是一个可执行单元，所以这个mcache对象相当于TLS变量，可以无锁使用。

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

### 扩展阅读

#### 德布鲁因序列

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



## 引用

[https://github.com/qyuhen/book/blob/master/Go%201.5%20%E6%BA%90%E7%A0%81%E5%89%96%E6%9E%90.pdf](https://github.com/qyuhen/book/blob/master/Go 1.5 源码剖析.pdf)



配图

1. 虚拟内存分配总览
2. 

【问题】===========================

1. 内存管理主要要解决的问题。一个是内存的快速分配。一个是内存碎片问题的解决。