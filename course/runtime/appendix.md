# Appendix

## GMP数据结构说明

### G

```go
type g struct {
	// Stack parameters.
	// stack describes the actual stack memory: [stack.lo, stack.hi).
	// stackguard0 is the stack pointer compared in the Go stack growth prologue.
	// It is stack.lo+StackGuard normally, but can be StackPreempt to trigger a preemption.
	// stackguard1 is the stack pointer compared in the C stack growth prologue.
	// It is stack.lo+StackGuard on g0 and gsignal stacks.
	// It is ~0 on other goroutine stacks, to trigger a call to morestackc (and crash).
  
  // goroutine栈空间描述
	stack       stack   // offset known to runtime/cgo
  // 
	stackguard0 uintptr // offset known to liblink
	stackguard1 uintptr // offset known to liblink

  // 当前g上的panic链
	_panic         *_panic // innermost panic - offset known to liblink
  // 当前g上的defer链
	_defer         *_defer // innermost defer
  
  // 当前栈持有的m对象指针
	m              *m      // current m; offset known to arm liblink
  // 用于运行上下文切换的gobuf对象，如果当前g非_Grunning状态，则其现场保存于此
	sched          gobuf
	syscallsp      uintptr        // if status==Gsyscall, syscallsp = sched.sp to use during gc
	syscallpc      uintptr        // if status==Gsyscall, syscallpc = sched.pc to use during gc
	stktopsp       uintptr        // expected sp at top of stack, to check in traceback
	param          unsafe.Pointer // passed parameter on wakeup
	atomicstatus   uint32
	stackLock      uint32 // sigprof/scang lock; TODO: fold in to atomicstatus
  // goroutine 全局ID
	goid           int64
	schedlink      guintptr
  // 当前goroutine被挂起的时间，被挂起时g对象在一个gobuf对象中。
	waitsince      int64      // approx time when the g become blocked
  // 当前goroutine被挂起的原因，被挂起时g对象在一个gobuf对象中。
	waitreason     waitReason // if status==Gwaiting
	preempt        bool       // preemption signal, duplicates stackguard0 = stackpreempt
	paniconfault   bool       // panic (instead of crash) on unexpected fault address
	preemptscan    bool       // preempted g does scan for gc
	gcscandone     bool       // g has scanned stack; protected by _Gscan bit in status
	gcscanvalid    bool       // false at start of gc cycle, true if G has not run since last scan; TODO: remove?
	throwsplit     bool       // must not split stack
	raceignore     int8       // ignore race detection events
	sysblocktraced bool       // StartTrace has emitted EvGoInSyscall about this goroutine
	sysexitticks   int64      // cputicks when syscall has returned (for tracing)
	traceseq       uint64     // trace event sequencer
	tracelastp     puintptr   // last P emitted an event for this goroutine
	lockedm        muintptr
	sig            uint32
	writebuf       []byte
	sigcode0       uintptr
	sigcode1       uintptr
	sigpc          uintptr
	gopc           uintptr         // pc of go statement that created this goroutine
	ancestors      *[]ancestorInfo // ancestor information goroutine(s) that created this goroutine (only used if debug.tracebackancestors)
	startpc        uintptr         // pc of goroutine function
	racectx        uintptr
	waiting        *sudog         // sudog structures this g is waiting on (that have a valid elem ptr); in lock order
	cgoCtxt        []uintptr      // cgo traceback context
	labels         unsafe.Pointer // profiler labels
	timer          *timer         // cached timer for time.Sleep
	selectDone     uint32         // are we participating in a select and did someone win the race?

	// Per-G GC state

	// gcAssistBytes is this G's GC assist credit in terms of
	// bytes allocated. If this is positive, then the G has credit
	// to allocate gcAssistBytes bytes without assisting. If this
	// is negative, then the G must correct this by performing
	// scan work. We track this in bytes to make it fast to update
	// and check for debt in the malloc hot path. The assist ratio
	// determines how this corresponds to scan work debt.
	gcAssistBytes int64
}
```

### M

```go
type m struct {
  // 系统g对象
	g0      *g     // goroutine with scheduling stack
	morebuf gobuf  // gobuf arg to morestack
	divmod  uint32 // div/mod denominator for arm - known to liblink

	// Fields not known to debuggers.
	procid        uint64       // for debuggers, but offset not hard-coded
  // 信号g对象
	gsignal       *g           // signal-handling g
	goSigStack    gsignalStack // Go-allocated signal handling stack
	sigmask       sigset       // storage for saved signal mask
	tls           [6]uintptr   // thread-local storage (for x86 extern register)
  // 线程回调函数，在allocm中被设置。进入c代码启动线程后，在mstart1中被调用。
  // 未设定该函数对象的m对象，将通过schedule去执行_Grunnable的g对象。
	mstartfn      func()
  // 当前m指针执行中的用户态g指针
	curg          *g       // current running goroutine
	caughtsig     guintptr // goroutine running during fatal signal
	p             puintptr // attached p for executing go code (nil if not executing go code)
  // m将要运行之前，被指定的可以在运行时使用的p对象。
	nextp         puintptr
	id            int64
	mallocing     int32
	throwing      int32
	preemptoff    string // if != "", keep curg running on this m
	locks         int32
	dying         int32
	profilehz     int32
	helpgc        int32
	spinning      bool // m is out of work and is actively looking for work
	blocked       bool // m is blocked on a note
	inwb          bool // m is executing a write barrier
	newSigstack   bool // minit on C thread called sigaltstack
	printlock     int8
	incgo         bool   // m is executing a cgo call
	freeWait      uint32 // if == 0, safe to free g0 and delete m (atomic)
	fastrand      [2]uint32
	needextram    bool
	traceback     uint8
	ncgocall      uint64      // number of cgo calls in total
	ncgo          int32       // number of cgo calls currently in progress
	cgoCallersUse uint32      // if non-zero, cgoCallers in use temporarily
	cgoCallers    *cgoCallers // cgo traceback if crashing in cgo call
	park          note
	alllink       *m // on allm
	schedlink     muintptr
  // 线程局部无锁内存分配器，用于线程内内存申请
	mcache        *mcache
	lockedg       guintptr
	createstack   [32]uintptr    // stack that created this thread.
	lockedExt     uint32         // tracking for external LockOSThread
	lockedInt     uint32         // tracking for internal lockOSThread
	nextwaitm     muintptr       // next m waiting for lock
	waitunlockf   unsafe.Pointer // todo go func(*g, unsafe.pointer) bool
	waitlock      unsafe.Pointer
	waittraceev   byte
	waittraceskip int
	startingtrace bool
	syscalltick   uint32
	thread        uintptr // thread handle
	freelink      *m      // on sched.freem

	// these are here because they are too large to be on the stack
	// of low-level NOSPLIT functions.
	libcall   libcall
	libcallpc uintptr // for cpu profiler
	libcallsp uintptr
	libcallg  guintptr
	syscall   libcall // stores syscall parameters on windows

	vdsoSP uintptr // SP for traceback while in VDSO call (0 if not in call)
	vdsoPC uintptr // PC for traceback while in VDSO call

	mOS
}
```

### P

```go
type p struct {
	lock mutex

  // p对象id
	id          int32
  // p状态
	status      uint32
  // p对象链表指针，用于在schedt中以链表形式保存未使用的p对象
	link        puintptr
	schedtick   uint32     // incremented on every scheduler call
	syscalltick uint32     // incremented on every system call
	sysmontick  sysmontick // last tick observed by sysmon
  // p对象当前关联的m对象，如果处于_Pidle状态，则p.m为nil。
	m           muintptr
  // 线程无锁内存分配器，m运行时从p获取，用于线程内内存申请
	mcache      *mcache
	racectx     uintptr

	deferpool    [5][]*_defer // pool of available defer structs of different sizes (see panic.go)
	deferpoolbuf [5][32]*_defer

	// goroutine ID缓存，每次从schedt获取16个，goidcacheend为16个的最大值。如果创建g对象时发现goidcache == goidcacheend，则再向schedt申请16个。
	goidcache    uint64
	goidcacheend uint64

  // 无锁_Grunnable状态待运行g对象（_Grunnable）环形队列。
	runqhead uint32
	runqtail uint32
	runq     [256]guintptr
	// runnext, if non-nil, is a runnable G that was ready'd by
	// the current G and should be run next instead of what's in
	// runq if there's time remaining in the running G's time
	// slice. It will inherit the time left in the current time
	// slice. If a set of goroutines is locked in a
	// communicate-and-wait pattern, this schedules that set as a
	// unit and eliminates the (potentially large) scheduling
	// latency that otherwise arises from adding the ready'd
	// goroutines to the end of the run queue.
	runnext guintptr

  // 空闲g（状态为_Gdead）链表头，通过g.schedlink链接起来
	gfree    *g
	gfreecnt int32

  // 空闲sudog对象缓存slice
	sudogcache []*sudog	
	sudogbuf   [128]*sudog

	tracebuf traceBufPtr

	// traceSweep indicates the sweep events should be traced.
	// This is used to defer the sweep start event until a span
	// has actually been swept.
	traceSweep bool
	// traceSwept and traceReclaimed track the number of bytes
	// swept and reclaimed by sweeping in the current sweep loop.
	traceSwept, traceReclaimed uintptr

	palloc persistentAlloc // per-P to avoid mutex

	// Per-P GC state
	gcAssistTime         int64 // Nanoseconds in assistAlloc
	gcFractionalMarkTime int64 // Nanoseconds in fractional mark worker
	gcBgMarkWorker       guintptr
	gcMarkWorkerMode     gcMarkWorkerMode

	// gcMarkWorkerStartTime is the nanotime() at which this mark
	// worker started.
	gcMarkWorkerStartTime int64

	// gcw is this P's GC work buffer cache. The work buffer is
	// filled by write barriers, drained by mutator assists, and
	// disposed on certain GC state transitions.
	gcw gcWork

	// wbBuf is this P's GC write barrier buffer.
	//
	// TODO: Consider caching this in the running G.
	wbBuf wbBuf

	runSafePointFn uint32 // if 1, run sched.safePointFn at next safe point

	pad [sys.CacheLineSize]byte
}
```

### scheduler

```go
type schedt struct {
	// accessed atomically. keep at top to ensure alignment on 32-bit systems.
  // goroutine ID生成器，从0开始递增，p对象每次从这里获取16个id。
	goidgen  uint64
	lastpoll uint64

	lock mutex

	// When increasing nmidle, nmidlelocked, nmsys, or nmfreed, be
	// sure to call checkdead().

  // 空闲的m对象，通过mget/mput方法操作该列表
	midle        muintptr
  // 空闲m对象的数量
	nmidle       int32
	nmidlelocked int32    // number of locked m's waiting for work
  // 递增的计数，用于生成mid，在mcommoninit中赋值给m.id后加1
	mnext        int64
  // 最大m数量，mnext超过此值抛出异常，在schedinit中被设置为1万
	maxmcount    int32
	nmsys        int32    // number of system m's not counted for deadlock
	nmfreed      int64    // cumulative number of freed m's

	ngsys uint32 // number of system goroutines; updated atomically

  // 空闲的p对象(_Pidle)，通过p.link链接，通过pidleput/pidleget操作该列表
	pidle      puintptr
  // 空闲p对象的数量
	npidle     uint32
	nmspinning uint32 // See "Worker thread parking/unparking" comment in proc.go.

  // 全局_Grunnable状态的g对象链表，通过g.schedlink指针链接
	runqhead guintptr
	runqtail guintptr
	runqsize int32

	// 用于操作gfreeStack、gfreeNoStack的锁
	gflock       mutex
  // 用于保存处于_Gdead状态的全局空g对象，分有栈或没栈的g对象，用于快速分配新g
	gfreeStack   *g
	gfreeNoStack *g
	ngfree       int32

  // 用于锁住sudogcache链表
	sudoglock  mutex	
  // 空闲sudog对象缓存链表，通过sudog.next指针链接
	sudogcache *sudog	

	// Central pool of available defer structs of different sizes.
	deferlock mutex
	deferpool [5]*_defer

	// 待释放的m对象，在allocm中被释放
	freem *m

	gcwaiting  uint32 // gc is waiting to run
	stopwait   int32
	stopnote   note
	sysmonwait uint32
	sysmonnote note

	// safepointFn should be called on each P at the next GC
	// safepoint if p.runSafePointFn is set.
	safePointFn   func(*p)
	safePointWait int32
	safePointNote note

	profilehz int32 // cpu profiling rate

  // 最近一次调用procresize的纳秒时间戳，用于统计totaltime
	procresizetime int64
  // 所有p对象累积工作时长，在每次procresize时累加
	totaltime      int64
}
```