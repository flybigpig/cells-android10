好,我们钻进 `binder_alloc.c`。先定位关键函数与上限常量,再读核心逻辑。
发现这个仓库里 `binder_alloc.c` 有两份实现:带中文注释的 `binder_alloc_new_buf_lockeds`(321 行)和原版 `binder_alloc_new_buf_locked`(378 行)。我把它们和 mmap 初始化、上限常量一起读出来。
两份实现并存,我需要确认真正被调用的是哪一份。查一下调用关系。
已经把 `binder_alloc.c` 的分配、回收、上限初始化全部读通了。先说一个**关键事实**避免混淆:仓库里有一份带中文注释的 `binder_alloc_new_buf_lockeds`(321 行),但它**不在调用链上**——`binder_alloc_new_buf`(552 行)实际调用的是 `binder_alloc_new_buf_locked`(561 行)。下面以真正生效的 `binder_alloc_new_buf_locked` 为准讲,注释版是它的"镜像教学稿"。

---

## 一、两个上限分别来自哪里

你问的"4MB / 1MB"其实是**两层不同来源**的天花板,容易混:

- **4MB**:内核驱动在 `binder_mmap` 里硬截的上限(`binder.c`:`if ((vma->vm_end - vma->vm_start) > SZ_4M) vma->vm_end = vma->vm_start + SZ_4M;`)。这是驱动允许单个进程 mmap 的最大空间。
- **1MB(单事务≈上限)**:真正生效的 mmap 大小由**用户态 `ProcessState`** 决定,它请求的是 `MMAP_SIZE = 1MB - 2*PAGE_SIZE`(在 `frameworks/native/libs/binder/ProcessState.cpp`,不在本内核树里)。于是驱动侧拿到的最终大小是 `min(1MB-2页, 4MB) = 1MB-2页`。这个尺寸被写进 `alloc->buffer_size`。
- **512KB(异步硬上限)**:`alloc->free_async_space = alloc->buffer_size / 2`,即异步(oneway)事务最多只能用一半。

所以"单事务 1MB 上限"的本质是:**整个 mmap 缓冲池只有 ~1MB,而一次事务必须在池子里占到一块连续的空闲区间**,它自然不可能超过池子总量。

初始化就在 `binder_alloc_mmap_handler`(744 行),把 mmap 大小记进 `buffer_size`,并建一个覆盖整段的初始空闲块,再把异步额度设成一半:

```709:719:kernel/drivers/android/binder_alloc.c
alloc->buffer_size = vma->vm_end - vma->vm_start;  // 通常为 1MB-2页(用户态请求)
buffer = kzalloc(sizeof(*buffer), GFP_KERNEL);
buffer->user_data = alloc->buffer;
buffer->free = 1;
binder_insert_free_buffer(alloc, buffer);   // 整段作为唯一空闲块入树
alloc->free_async_space = alloc->buffer_size / 2;  // 异步最多用一半 ≈ 512KB
```

---

## 二、空闲块查找:Best-Fit 红黑树

每个 `binder_proc` 的 `binder_alloc` 维护两棵红黑树:`free_buffers`(按地址排序的**空闲**块)和 `allocated_buffers`(已分配块)。分配时走**最佳适应(Best-Fit)**:在空闲树里找"能装下、且尽量小"的块。

`binder_alloc_buffer_size`(66 行)给出某块可用长度(下一块 user_data 减本块,或到池尾)。查找循环在 `binder_alloc_new_buf_locked`(427-441 行):

```427:441:kernel/drivers/android/binder_alloc.c
while (n) {
	buffer = rb_entry(n, struct binder_buffer, rb_node);
	BUG_ON(!buffer->free);
	buffer_size = binder_alloc_buffer_size(alloc, buffer);
	if (size < buffer_size) {
		best_fit = n;       // 当前块够大,先记下来当候选
		n = n->rb_left;     // 仍往左找更小的,力求最贴合
	} else if (size > buffer_size)
		n = n->rb_right;    // 太小,往右找更大的
	else {
		best_fit = n; break; // 完美匹配
	}
}
```

注意:因为树按"地址/大小"组织,best-fit 会一路往左钻(更小且满足),所以**倾向于把碎片留得最小**,而不是随便切大块。这是 Binder 为"长期大量小事务"做的选择(反之首次适应 FCFS 会更快但碎片更大)。

---

## 三、分配成功路径:懒分配物理页 + 切分剩余

找到 `best_fit` 后:

1. **懒映射物理页**:只给"实际用到的那几页"调 `binder_update_page_range`(491 行),而非整块。mmap 本身不分配物理页,这里才真正按需调页。
2. **切分剩余**:若整块比请求大(`buffer_size != size`,496 行),把尾部新切成 `new_buffer` 塞回 `free_buffers` 树(508 行)。这样一块大空闲被切成"已用 + 新空闲"。
3. **挪树**:原块从 `free_buffers` 移除(`rb_erase`,511 行),标记 `free=0`,插入 `allocated_buffers`(514 行)。
4. **扣异步额度**:`if (is_async) alloc->free_async_space -= size + sizeof(struct binder_buffer);`(522-523 行)——每笔异步事务都从那 512KB 额度里扣。

---

## 四、两个"失败返回点"——TransactionTooLargeException 的源头

分配只有两种情况会失败,且**都返回 `-ENOSPC`**:

**(a) 异步额度不足**(416-422 行):oneway 事务但剩余异步空间不够。

```416:422:kernel/drivers/android/binder_alloc.c
if (is_async &&
    alloc->free_async_space < size + sizeof(struct binder_buffer)) {
	binder_alloc_debug(... "no async space left" ...);
	return ERR_PTR(-ENOSPC);
}
```

**(b) 没有足够大的连续空闲块**(442-473 行):`best_fit == NULL`,即整棵空闲树都找不到一块 ≥ `size` 的连续区间。此时驱动还会把已分配/空闲块数、最大块尺寸打印出来,便于你确认是不是"总量够但碎片化导致失败"。

```468:473:kernel/drivers/android/binder_alloc.c
pr_err("%d: binder_alloc_buf size %zd failed, no address space\n",
	alloc->pid, size);
pr_err("allocated: %zd (num: %zd largest: %zd), free: %zd (num: %zd largest: %zd)\n", ...);
return ERR_PTR(-ENOSPC);
```

这个 `-ENOSPC` 正是关键。回到 `binder.c` 的 `binder_transaction`,分配失败时:

```3501:3510:kernel/drivers/android/binder.c
if (IS_ERR(t->buffer)) {
	return_error_param = PTR_ERR(t->buffer);
	return_error = return_error_param == -ESRCH ?
		       BR_DEAD_REPLY : BR_FAILED_REPLY;
	...
	goto err_binder_alloc_buf_failed;
}
```

驱动给客户端回 `BR_FAILED_REPLY`;用户态 `IPCThreadState` 把它转成 `FAILED_TRANSACTION`,再经 `android_util_Binder.cpp` 的 `signalExceptionForError`(错误码 `-ENOSPC` 时)抛成 **`android.os.TransactionTooLargeException`**。链路是:

```
Parcel 太大 / 池子不够连续
  → binder_alloc_new_buf_locked 返回 ERR_PTR(-ENOSPC)
  → binder_transaction 置 BR_FAILED_REPLY
  → IPCThreadState 收到失败
  → Binder.java 抛 TransactionTooLargeException
```

所以"TransactionTooLargeException"不一定是真的超过 1MB,**只要本次分配取不到一块连续空闲就触发**——多笔并发事务把 1MB 池子切得零碎时,一笔中等事务也会中招。

---

## 五、回收与合并:为什么"总量够却仍失败"

事务处理完、用户 `BC_FREE_BUFFER` 后走 `binder_free_buf_locked`(626 行):

```647:653:kernel/drivers/android/binder_alloc.c
if (buffer->async_transaction) {
	alloc->free_async_space += size + sizeof(struct binder_buffer);
	...
}
binder_update_page_range(alloc, 0, ...);  // 释放物理页
```

接着它会尝试和**前后相邻的空闲块合并**(在 `binder_delete_free_buffer` 578 行与 `binder_free_buf_locked` 的合并逻辑里):如果上一块/下一块也是 free 且同页,就把它们合并成一块更大的空闲。合并成功,后面的大事务才分配得出来;**合并不成功(被已分配块隔开)就会留下永久碎片**——这正是"池子总空闲看着够、单笔却分配失败"的根因。

---

## 六、数字小结与规避

- 每进程 mmap 池:`min(用户态请求, 4MB)`,常规 app ≈ `1MB - 2*PAGE_SIZE`。
- 单笔同步事务上限 ≈ 池大小(≈1MB,减去 `binder_buffer` 头与对齐)。
- 单笔异步(oneway)事务硬上限 = `free_async_space = 池/2` ≈ 512KB。
- 触发 `TransactionTooLargeException` 的充分条件:`binder_alloc_new_buf` 返回 `-ENOSPC`(无连续块 或 异步额度不足)。

规避建议:大负载(图片 bitmap、长列表、大 JSON)不要内联进 Parcel,改用 `ParcelFileDescriptor`/`Ashmem`(走 `BINDER_TYPE_FD`,不占这块 1MB 池)或 `Bundle` 拆分;高频 oneway 调用注意 512KB 异步天花板;`dmesg` 里搜 `binder_alloc_buf ... failed, no address space` 能直接看到失败时的碎片分布。

需要的话,下一块我可以接着讲 `binder_update_page_range` 的物理页映射细节(它和 `VM_MIXEDMAP` 共同构成"一次拷贝"的底层),或者拿一个真实的 `TransactionTooLargeException` 栈回推到上述某个失败点。