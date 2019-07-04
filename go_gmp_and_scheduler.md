全局变量

allglen - 所有g的数量

allm - 所有的m，这里m是隐式链表，通过m中的alllink字段链接

allp - 所有的p对象指针slice，直接受newprocs和gomaxprocs影响。

allpLock - 用于锁allp的无p环境读和所有的写

gomaxprocs - go最大系统线程数量

newprocs - 与gomaxprocs相同

ncpu - cpu核数量

allgs - 所有的g对象指针slice

allglock - 用于锁allgs

