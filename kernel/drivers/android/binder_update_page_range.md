好,我们钻到物理页层。先把 `binder_update_page_range` 本身、相关的页分配/释放,以及 `VM_MIXEDMAP` 在 `binder_mmap` 里的设置都读出来。
`VM_MIXEDMAP` 在 `binder_mmap`(5460-5480)里确认了。现在读完整的 `binder_update_page_range`、页面拷贝函数,以及 fault 回调,把"一次拷贝"的底层机制讲透。
你已经看到核心了:`vm_insert_page`(270 行)把内核 `struct page` 直接插进目标进程的用户地址空间。最后读一下 `binder_vm_fault`,它负责用户态读页时 PTE 缺失的补映射,这样"一次拷贝"的闭环就完整了。
已经把物理页层全部读通,而且发现这个 cells 内核和上游 AOSP 在 fault 处理上有一个**关键差异**(下面第 7 点)。下面把 `binder_update_page_range` + `VM_MIXEDMAP` 如何支撑"一次拷贝"讲透。

---

## 一、为什么需要"逐页"管理

Binder 的 1MB 缓冲池不是一次性整块占满物理内存,而是按需逐页分配(懒分配)。原因有二:一是进程刚 mmap 时根本不知道将来会有多大事务;二是池子会被切成很多大小不一的 `binder_buffer`,需要把**离散的物理页**拼成一块连续的用户虚拟地址区间。这就引出 `VM_MIXEDMAP`。

---

## 二、VM_MIXEDMAP 的作用:允许"单页插入"

`binder_mmap` 给 VMA 打上这组标志(`binder.c:5467-5472`):

```5471:5472:kernel/drivers/android/binder.c
vma->vm_flags |= VM_DONTCOPY | VM_MIXEDMAP;
vma->vm_flags &= ~VM_MAYWRITE;
```

- `VM_MIXEDMAP`:告诉内核"这个 VMA 的页由驱动自己管理,逐页用 `vm_insert_page` 插入,且这些页不隶属于同一个文件/匿名对象"。**没有它,内核会拒绝往一个普通 VMA 里插入零散的 `struct page`**——这正是 Binder 需要的:把一块块独立 `alloc_page` 出来的页,映射成一段连续的进程虚拟地址。
- `VM_DONTCOPY`:fork 不继承(避免子进程持有驱动缓冲)。
- 清掉 `VM_MAYWRITE`:用户态不能直接写这些页,所有写入都必须走驱动(保证数据只由内核在事务里填充),这是安全边界。

---

## 三、binder_update_page_range:逐页 alloc_page + vm_insert_page

这是"一次拷贝"的物理基础。参数 `allocate=1` 时,它把 `[start, end)` 范围内的每一页,分配并映射到目标进程用户空间(`binder_alloc.c:187`):

```209:282:kernel/drivers/android/binder_alloc.c
for (page_addr = start; page_addr < end; page_addr += PAGE_SIZE) {
	page = &alloc->pages[(page_addr - alloc->buffer) / PAGE_SIZE];
	if (!page->page_ptr) { need_mm = true; break; }   // 有页还没分配
}
... // 取下 mm 的 mmap_sem 读锁,拿到 vma
for (page_addr = start; page_addr < end; page_addr += PAGE_SIZE) {
	...
	page->page_ptr = alloc_page(GFP_KERNEL | __GFP_HIGHMEM | __GFP_ZERO); // 分配清零页
	...
	user_page_addr = (uintptr_t)page_addr;
	ret = vm_insert_page(vma, user_page_addr, page[0].page_ptr);          // ★ 关键:把该物理页插进用户空间
	...
}
```

关键点:`vm_insert_page(vma, user_page_addr, page_ptr)` 把内核刚 `alloc_page` 出来的那个**物理页**,同时映射到两处:
- 目标进程的**用户虚拟地址** `user_page_addr`(就在 mmap 区内);
- 内核侧则通过 `alloc->pages[index].page_ptr` 持有这个 `struct page`,需要时用 `kmap` 临时映射访问。

**同一块物理内存,拥有"内核映射 + 目标进程用户映射"两份虚拟地址**。这就是 Binder 能省掉第二次拷贝的根本原因。

还有个细节:如果某页 `page->page_ptr` 已经存在(之前分配过、被放到 LRU 缓存里),就走 `list_lru_del` 从 LRU 摘下来直接复用(244-252 行),不必重新 `alloc_page`/`vm_insert_page`——这是性能优化。

---

## 四、"一次拷贝"的真正含义

传统 IPC(pipe/socket)要两次拷贝:发送方用户态 → 内核缓冲(拷贝1) → 接收方用户态(拷贝2)。Binder 只有**一次**:

1. 事务发起时,驱动把发送方 `Parcel` 数据从**发送方用户空间**拷进**目标进程的那块内核页**(`binder.c:3530` 调 `binder_alloc_copy_user_to_buffer`,里面是 `copy_from_user(kptr, from, size)`,`from` 是发送方指针)——这是唯一一次拷贝。
2. 那块内核页,在分配时已经通过 `vm_insert_page` 映射到了**目标进程的用户地址空间**(`t->buffer->user_data`)。所以目标进程的用户态代码(`IPCThreadState`)直接读自己的虚拟地址就能拿到数据,**不需要再拷一次**。

对比链:
```
pipe:   发送用户 ──copy──> 内核页 ──copy──> 接收用户   (2 次)
Binder: 发送用户 ──copy──> 目标内核页(=目标用户映射同一物理页)  (1 次)
```

所以"一次拷贝"= 数据只在发送方用户态→目标物理页之间走一次 `copy_from_user`,之后目标靠共享物理页"零成本"读取。

---

## 五、驱动内部怎么访问这些页:kmap + memcpy

驱动在翻译 `flat_binder_object`、回填改写对象时,也要读改写目标缓冲区,但它**没有目标页的永久内核地址**(因为页可能来自高端内存 `__GFP_HIGHMEM`,没有线性映射)。所以访问走临时映射:

```1186:1191:kernel/drivers/android/binder_alloc.c
page = binder_alloc_get_page(alloc, buffer, buffer_offset, &pgoff);
size = min_t(size_t, bytes, PAGE_SIZE - pgoff);
kptr = kmap(page) + pgoff;
ret = copy_from_user(kptr, from, size);   // 唯一一次拷贝发生在这里
kunmap(page);
```

`binder_alloc_do_buffer_copy`(1201 行)对内核↔缓冲区则用 `kmap_atomic` + `memcpy`。这些都不产生"第二次跨进程拷贝",只是驱动在自己持有的物理页上做内核态操作。

---

## 六、释放不是立刻归还:LRU 缓存 + shrinker

`binder_free_buf_locked` 调 `binder_update_page_range(alloc, 0, ...)` 释放时,**不会立即把页还给伙伴系统**,而是挂到 `binder_alloc_lru`(`binder_alloc.c:289-304` 的 `free_range` 走 `list_lru_add`)。好处:下次分配同一页可直接复用,省掉 `alloc_page`+`vm_insert_page`。

只有在系统内存紧张、shrinker 触发时,才真正回收:

```1011:1024:kernel/drivers/android/binder_alloc.c
if (vma) {
	zap_page_range(vma, page_addr, PAGE_SIZE, NULL);  // 撤掉用户态 PTE
	...
}
__free_page(page->page_ptr);   // 真正归还物理页
page->page_ptr = NULL;
```

`binder_shrink_scan`/`binder_alloc_free_page` 就是内存压力下回收这些缓存页的入口。这解释了为什么第 3 点里"已在 LRU 的页摘下来复用"那段代码存在——它是整套缓存的核心。

---

## 七、本 cells 内核的 fault 处理与上游不同(重要差异)

上游 AOSP 的 `binder_vm_fault` 会在缺页时查 `alloc->pages[pgoff]` 把页重新插回用户空间。但**本仓库(这个 cells 内核)把它简化成了直接 SIGBUS**:

```5449:5451:kernel/drivers/android/binder.c
static int binder_vm_fault(struct vm_area_struct *vma, struct vm_fault *vmf) {
    return VM_FAULT_SIGBUS;
}
```

含义:正常路径下,页在 `binder_update_page_range` 分配时就已经 `vm_insert_page` 插好 PTE,用户态读事务缓冲不会触发缺页;而一旦缓冲被 `binder_free_buf` 释放(甚至被 shrinker `zap_page_range` 撤掉 PTE),任何再访问都直接 SIGBUS——驱动假定用户态不会在缓冲归还后还去碰它。这与上游"缺页自愈"的语义不同,是移植/排障时要注意的点。

---

## 八、把前几讲串起来

- `binder_mmap` 用 `VM_MIXEDMAP` 开了一块 ~1MB(驱动上限 4MB)的、可由驱动逐页填充的用户 VMA;
- `binder_alloc_new_buf_locked` 用 best-fit 在 `free_buffers` 红黑树找连续区间,再调 `binder_update_page_range` 逐页 `alloc_page`+`vm_insert_page`,把同一物理页同时映射给内核和目标进程用户态;
- 事务时数据只 `copy_from_user` 一次进目标内核页,目标进程靠共享物理页零成本读取 → "一次拷贝";
- 回收走 LRU 缓存,内存压力下由 shrinker `zap`+`__free_page` 真正归还。

这一套机制加在一起,就是 Binder 比传统 IPC 少一次拷贝、延迟更低的底层原因。

需要的话,下一步可以讲 `binder_transaction` 里 `binder_buffer` 的"生命周期与引用计数"(谁负责 free、为何 `BC_FREE_BUFFER` 必须由用户态发、以及在 nested/oneway 场景下缓冲何时释放),或者回到你最早关心的 `flat_binder_object` 翻译在页层面的边界情况(比如对象跨越页边界时 `binder_get_object` 的校验)。