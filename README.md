这是一份go源码的阅读笔记。起初在使用go时，希望了解chan的实现原理，看过一些源码和网上的资料，越来越觉得有趣，便开始了深入的了解。
目前读的比较随意，但有一个大概的范围，后面我会把这些点逐渐完善。范围如下：

**runtime**

内存管理

gmp与调度

gc原理

**cmd/compile**

编译器前后端

