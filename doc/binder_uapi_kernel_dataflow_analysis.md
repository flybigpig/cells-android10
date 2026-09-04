# Binder UAPI / 内核目标进程数据传递过程 与 核心方法注释分析

> 源码基线:kernel 4.9.200(AOSP android-10.0.0_r33,高通平台)
> 涉及文件:
> - `kernel/include/uapi/linux/android/binder.h`(542 行,用户态 UAPI 协议)
> - `kernel/drivers/android/binder.c`(6569 行,驱动主逻辑,含 cells 定制)
> - `kernel/drivers/android/binder_alloc.c`(1258 行,事务缓冲区分配/拷贝)

---

## 1. Binder 是什么

Binder 是 Android 独占的进程间通信(IPC)机制,基于 **内核态一个字符设备驱动**(/dev/binder)+ 内存映射 + 线程间唤醒实现。与管道/套接字最大的不同:

1. **一次数据拷贝**:发送进程的用户态数据,由内核直接拷入 **目标进程映射的缓冲区**,目标进程直接读自己的地址空间,全程只拷贝一次(传统 IPC 要拷两次:内核→发送方、内核→接收方)。
2. **面向对象语义**:传输的不是裸字节流,而是带"对象扁平化 + 指针修复(handle/fd 翻译)"的事务(transaction),支持跨进程传递 Binder 对象与文件描述符。
3. **线程模型**:服务端线程通过 `BC_ENTER_LOOPER` 进入循环,内核把到达的事务精确投递给目标进程的某个线程并唤醒它。

术语速记:
- **进程 (proc)** = 打开 /dev/binder 的一个进程,内含一块 mmap 的缓冲区与红黑树(节点/引用/线程)。
- **节点 (node)** = 一个 Binder 服务对象在驱动中的实体(服务端视角,有 ptr+cookie)。
- **引用 (ref)** = 客户端持有的该对象的"句柄 (handle)"(客户端视角,是一个 32 位整数)。
- **事务 (transaction)** = 一次 IPC 调用,单向(oneway)或同步(需要 reply)。
- **BC_xxx** = 用户→驱动命令(binder command),**BR_xxx** = 驱动→用户返回命令(binder return)。

---

## 2. UAPI 层解析(kernel/include/uapi/linux/android/binder.h)

### 2.1 地址类型随 32/64 位切换

```c
#ifdef BINDER_IPC_32BIT
typedef __u32 binder_size_t;      /* 长度:32 位进程下用 32 位 */
typedef __u32 binder_uintptr_t;   /* 地址:32 位进程下用 32 位 */
#else
typedef __u64 binder_size_t;
typedef __u64 binder_uintptr_t;
#endif
```

驱动通过 compat 层自动适配 64 位内核上的 32 位客户端,协议版本 `BINDER_CURRENT_PROTOCOL_VERSION` 32 位下为 7、64 位下为 8(binder.h L239/241)。

### 2.2 binder_write_read —— 一次 ioctl 的读写载体(L222)

```c
struct binder_write_read {
    binder_size_t  write_size;     /* 要写给驱动的命令区字节数 */
    binder_size_t  write_consumed; /* 驱动已消费的字节数(回写) */
    binder_uintptr_t write_buffer; /* 命令区地址:BC_ 命令流 */
    binder_size_t  read_size;      /* 要读回的命令区容量 */
    binder_size_t  read_consumed;  /* 驱动已填充的字节数(回写) */
    binder_uintptr_t read_buffer;  /* 返回区地址:BR_ 命令流 */
};
```

**一个 `BINDER_WRITE_READ` ioctl 同时干两件事**:先处理 write 区的 BC_ 命令(发事务/回复/引用计数…),再从 read 区收取驱动投递的 BR_ 命令(收到事务/回复/complete…)。write/read 区都是"命令流"(多段命令连续排布)。

### 2.3 binder_transaction_data —— 一次调用的描述(L299)

```c
struct binder_transaction_data {
    union {
        __u32           handle;  /* 客户端 BC_TRANSACTION 用的目标句柄 */
        binder_uintptr_t ptr;    /* 服务端 BR_TRANSACTION 收到的本地对象指针 */
    } target;
    binder_uintptr_t cookie;     /* 服务端对象 cookie */
    __u32           code;        /* 方法号,如 FIRST_CALL_TRANSACTION+0 */
    __u32           flags;       /* TF_ONE_WAY 等 */
    pid_t           sender_pid;  /* 驱动填写:发送者 pid */
    uid_t           sender_euid; /* 驱动填写:发送者 euid */
    binder_size_t   data_size;   /* 主数据字节数 */
    binder_size_t   offsets_size;/* 偏移数组字节数 */
    union {
        struct {
            binder_uintptr_t buffer;  /* 主数据地址 */
            binder_uintptr_t offsets; /* 指向其中 flat_binder_object 的偏移数组 */
        } ptr;
        __u8 buf[8];             /* 小数据内联 */
    } data;
};
```

要点:**payload = 主数据区 + 偏移数组**。偏移数组里的每个元素指向主数据中一个被扁平化的对象(handle/binder/fd/缓冲区),驱动靠它找到这些对象并逐个"翻译/修复"。

### 2.4 可传输对象(扁平化头 + 内容)

所有对象以 `binder_object_header{ __u32 type; }` 开头(binder.h L112):

| 类型 | 结构 | 含义 | 驱动动作 |
|---|---|---|---|
| `BINDER_TYPE_BINDER` / `WEAK_BINDER` | `flat_binder_object` | 发送方自己的服务对象(node) | 翻译成目标进程视角的 handle |
| `BINDER_TYPE_HANDLE` / `WEAK_HANDLE` | `flat_binder_object` | 发送方持有的远程句柄 | 翻译成目标进程视角的 node 或新 handle |
| `BINDER_TYPE_FD` | `binder_fd_object` | 单个文件描述符 | 在目标进程里 dup 出新的 fd |
| `BINDER_TYPE_PTR` | `binder_buffer_object` | 内嵌大缓冲区 | 原样拷入目标缓冲区并修复内部指针 |
| `BINDER_TYPE_FDA` | `binder_fd_array_object` | 缓冲区里的一批 fd(native_handle_t) | 逐个 dup 并回填 |

`flat_binder_object`(L123)中 `binder/cookie` 是"本地对象(发送方 node 地址+服务端 cookie)",`handle` 是"远程对象(客户端句柄)"——**同一 union,收发两侧语义相反**,这正是驱动要翻译的原因。

### 2.5 ioctl 命令表(L266)

```c
BINDER_WRITE_READ        _IOWR('b', 1, ...)  /* 唯一高频入口 */
BINDER_SET_MAX_THREADS   _IOW('b', 5, ...)   /* 进程最大线程数 */
BINDER_SET_CONTEXT_MGR   _IOW('b', 7, ...)   /* 注册自己为 context manager(服务总管) */
BINDER_THREAD_EXIT       _IOW('b', 8, ...)
BINDER_VERSION           _IOWR('b', 9, ...)
BINDER_SET_CONTEXT_MGR_EXT _IOW('b', 13, ...)
```

### 2.6 BC_ / BR_ 命令协议

用户→驱动(`enum binder_driver_command_protocol`,L458)关键项:
- `BC_TRANSACTION` / `BC_REPLY`(带 `_SG` 变体支持 extra buffers):**发起一次调用/回复**,后随 `binder_transaction_data`。
- `BC_FREE_BUFFER`:目标进程用完收到的数据后,把缓冲区还给驱动。
- `BC_ENTER_LOOPER` / `BC_REGISTER_LOOPER`:线程进入 binder 循环(主线程用前者,spawn 出来的用后者)。
- `BC_ACQUIRE`/`BC_RELEASE`/`BC_INCREFS`/`BC_DECREFS`:引用计数管理。

驱动→用户(`enum binder_driver_return_protocol`,L365)关键项:
- `BR_TRANSACTION` / `BR_REPLY`:驱动把到达的事务投递给目标进程线程(带完整 `binder_transaction_data`)。
- `BR_TRANSACTION_COMPLETE`:发送侧收到,表示你的 BC_TRANSACTION/BC_REPLY 已被驱动受理。
- `BR_SPAWN_LOOPER`:驱动提示"没有空闲线程了,请再开一个线程"。
- `BR_DEAD_REPLY`/`BR_FAILED_REPLY`:目标已死 / 发送失败。
- `BR_DEAD_BINDER`:你监视的 binder 对象死亡通知。

---

## 3. 驱动内数据传递全流程(目标进程视角)

以最常见的 **同步调用**(客户端 → 服务端)为例,数据从客户端用户态到达服务端用户态要经过下面 7 步(行号均为 binder.c):

```
客户端进程                          内核驱动                              服务端进程
   |  ioctl(BINDER_WRITE_READ)         |                                      |
   |  write区: BC_TRANSACTION ──────> |                                      |
   |        (binder_ioctl_write_read, L5117)                                  |
   |                                   | binder_thread_write (L3912)          |
   |                                   |   case BC_TRANSACTION (L4161)        |
   |                                   |   └─ binder_transaction (L3201)      |
   |                                   |      ① 解析目标句柄→target_proc     |
   |                                   |      ② binder_alloc_new_buf:         |
   |                                   |         在【目标进程】映射区分配缓冲 |
   |                                   |      ③ copy_user_to_buffer:         |
   |                                   |         【发送方数据一次拷入目标缓冲】|
   |                                   |      ④ 逐对象翻译(handle/fd/ptr)    |
   |                                   |      ⑤ binder_proc_transaction:     |
   |                                   |         事务入【目标线程】todo 队列  |
   |                                   |         并 wake_up 目标线程          |
   |                                   |                                      |
   |   read区: BR_TRANSACTION_COMPLETE |                                      |
   | <──────────────────────────────── |                                      |
   |                                   |                                      | ioctl(BINDER_WRITE_READ)
   |                                   |   binder_thread_read (L4490)         | <─── (被唤醒)
   |                                   |   ⑥ 出队 BINDER_WORK_TRANSACTION     |
   |                                   |   ⑦ 填 binder_transaction_data,      |
   |                                   |      copy_to_user → BR_TRANSACTION ──┘
   |                                   |      (数据本身不拷:目标进程直接读     |
   |                                   |       自己 mmap 的 buffer,仅拷描述)   |
```

### 3.1 入口:binder_ioctl_write_read(L5117)

```c
copy_from_user(&bwr, ubuf, sizeof(bwr));        /* 取回 write/read 描述 */
if (bwr.write_size > 0)
    ret = binder_thread_write(proc, thread,     /* ① 先执行用户命令(发送侧) */
                              bwr.write_buffer, bwr.write_size,
                              &bwr.write_consumed);
if (bwr.read_size > 0)
    ret = binder_thread_read(proc, thread,      /* ② 再收取驱动命令(接收侧) */
                             bwr.read_buffer, bwr.read_size,
                             &bwr.read_consumed, O_NONBLOCK);
copy_to_user(ubuf, &bwr, sizeof(bwr));          /* 回写 consumed,供下次续传 */
```

写与读共用同一 ioctl 是 Binder 的精髓:**发送者写完立刻读(等回复/收 complete);接收者平时只读(被唤醒后拿事务)**。

### 3.2 发送侧命令解析:binder_thread_write 中 BC_TRANSACTION(L4161)

```c
case BC_TRANSACTION:
case BC_REPLY: {
    struct binder_transaction_data tr;
    if (copy_from_user(&tr, ptr, sizeof(tr)))   /* 只拷"描述",不是数据 */
        return -EFAULT;
    ptr += sizeof(tr);
    binder_transaction(proc, thread, &tr,
                       cmd == BC_REPLY, 0);      /* 真正干活的全在这 */
    break;
}
```

### 3.3 核心:binder_transaction(L3201)—— 数据如何进入"目标进程"

该函数约 700 行,分五段(行号对应 4.2 节详注):

1. **L3237-3408 定位目标**:reply 走 `thread->transaction_stack` 找回发起线程;非 reply 按 `tr->target.handle` 查 ref → node → `target_proc`/`target_thread`;handle=0 表示打给 context manager。
2. **L3414-3431 分配内核对象**:`struct binder_transaction`(记录 from/to/code/flags/优先级)与 `tcomplete`(事务受理通知)。
3. **L3498-3560 【关键】在目标进程缓冲区分配并拷贝数据**:
   - `t->buffer = binder_alloc_new_buf(&target_proc->alloc, ...)` —— **缓冲分配在目标进程的 mmap 区域里**;
   - `binder_alloc_copy_user_to_buffer(...)` 把发送方用户态 `data` 与 `offsets` **一次拷入目标进程的这块缓冲**。
4. **L3578-3777 逐对象翻译**:沿偏移数组遍历每个扁平化对象,handle→node/handle、binder→handle、fd→dup、ptr→原样拷入+指针修复(详见 4.3)。
5. **L3778-3822 入队与唤醒**:填好 `BINDER_WORK_TRANSACTION` 后:
   - reply 路径:直接 `binder_enqueue_thread_work_ilocked(target_thread, &t->work)` + `wake_up_interruptible_sync`(L3790-3792);
   - 同步请求:入**目标线程** todo,`t->need_reply = 1` 压入自己线程的事务栈(L3805-3808),再 `binder_proc_transaction(...)`(L3810);
   - oneway:入目标进程 todo/节点 async_todo(L3820)。

### 3.4 入队唤醒:binder_proc_transaction(L3108)

```c
if (!thread && !pending_async)
    thread = binder_select_thread_ilocked(proc);   /* 从目标进程空闲线程里挑一个 */
if (thread)
    binder_enqueue_thread_work_ilocked(thread, &t->work); /* 优先入线程队列 */
else if (!pending_async)
    binder_enqueue_work_ilocked(&t->work, &proc->todo);   /* 否则入进程队列 */
...
if (!pending_async)
    binder_wakeup_thread_ilocked(proc, thread, !oneway);  /* 唤醒目标线程 */
```

### 3.5 接收侧:binder_thread_read(L4490)投递事务

目标进程某线程 ioctl 进入 read,`binder_wait_for_work`(L4459)睡在 `thread->wait`;被唤醒后:

1. 优先取 `thread->todo`,其次取 `proc->todo`(L4556-4560);
2. 出队 `BINDER_WORK_TRANSACTION`(L4574-4583);
3. **填 `binder_transaction_data` 描述**(L4756-4798):`target.ptr/cookie` = 本地 node 地址/服务端 cookie → `cmd = BR_TRANSACTION`;无 node(即 reply)则 `cmd = BR_REPLY`;`data.ptr.buffer/offsets` = **目标进程映射区里的地址**(数据早已在那里,L4789-4792);
4. `put_user(cmd)` + `copy_to_user(ptr, &tr, trsize)`(L4799-4818):**只把这份"描述"拷给用户态**;
5. 同步事务(非 BR_REPLY、非 oneway)把自己压入该线程事务栈等回复(L4837-4842),否则 `binder_free_transaction` 收尾。

> 为什么高效:第 3.3-③ 步已把数据**物理拷到目标进程的缓冲区**(该缓冲在目标进程地址空间有映射),所以 3.5-④ 只需再拷几十字节的结构体描述,**真正的业务数据零二次拷贝**。

### 3.6 返回与释放

- 服务端处理完调 `BC_REPLY`,走与 3.3 相同的 binder_transaction(reply=1)把结果送回客户端原线程;
- 客户端 read 收到 `BR_REPLY`,用完数据后发 `BC_FREE_BUFFER`(L4089)归还缓冲;
- 同步链路上每端都会收到 `BR_TRANSACTION_COMPLETE` 表示受理完成。

---

## 4. 核心方法注释分析(按调用链排列)

### 4.1 binder_ioctl_write_read — L5117(见 3.1)
读改写一体入口;`_IOC_SIZE(cmd)` 校验参数大小;write 出错直接短路,read 出错仍回写 consumed。

### 4.2 binder_transaction — L3201,全程注释

```c
/* L3237:reply? 从本线程 transaction_stack 顶部取 in_reply_to(必须指向本线程), */
/*       并从其 to_thread 找回等待回复的目标线程(L3268 binder_get_txn_from_and_acq_inner) */
if (reply) { ... }

/* L3291(L3311 为无 CONFIG_DRV_NS 时):非 reply,按 handle 解析目标 */
/*   handle>0   : binder_get_ref_olocked → ref->node → binder_get_node_refs_for_txn  */
/*                得到 target_node + target_proc(并临时 +tmp_ref 保活)             */
/*   handle==0  : 打给 context manager(本 context 的 binder_context_mgr_node)      */
/* L3293(cells): handle >= INIT_OTHER_CONTEXT_MGR_HANDLE(100000000) 时,            */
/*                从 proc->acontext[handle-100000000] 取【其它 cell】的 context     */
/*                manager 节点 —— 支持跨虚拟 cell 直接呼叫(L3311 为定制分支)         */

/* L3365:security_binder_transaction —— SELinux 校验发送方能否访问目标进程 */

/* L3414:L3414 分配 t;L3424 分配 tcomplete(两个独立内核对象) */

/* L3454-3471:非 oneway 同步事务记 from=thread,并继承/设定目标默认优先级 */

/* L3498 缓冲分配 —— 注意作用在 target_proc 的 alloc 上: */
t->buffer = binder_alloc_new_buf(&target_proc->alloc, tr->data_size,
                                 tr->offsets_size, extra_buffers_size,
                                 !reply && (t->flags & TF_ONE_WAY));

/* L3530/L3545 数据拷贝:发送方用户态 → 目标进程缓冲(两次:data 与 offsets) */
if (binder_alloc_copy_user_to_buffer(&target_proc->alloc, t->buffer, 0,
        (const void __user *)(uintptr_t)tr->data.ptr.buffer, tr->data_size)) ...

/* L3585 起:沿偏移数组循环,逐个翻译扁平化对象 */
for (buffer_offset = off_start_offset; buffer_offset < off_end_offset;
     buffer_offset += sizeof(binder_size_t)) {
    ...
    switch (hdr->type) {
    case BINDER_TYPE_BINDER/WEAK_BINDER: binder_translate_binder(...); break;
    case BINDER_TYPE_HANDLE/WEAK_HANDLE: binder_translate_handle(...); break;
    case BINDER_TYPE_FD:                 binder_translate_fd(...);     break;
    case BINDER_TYPE_FDA:                binder_translate_fd_array(...); break;
    case BINDER_TYPE_PTR:  /* 内嵌缓冲:再拷一次进 sg 区并修复指针 */
        binder_alloc_copy_user_to_buffer(&target_proc->alloc, t->buffer,
                                         sg_buf_offset, bp->buffer, bp->length);
        bp->buffer = t->buffer->user_data + sg_buf_offset;  /* 指针修复 */
        ...
    }
}

/* L3778-3822 收尾入队:reply→target_thread;同步→thread 栈+binder_proc_transaction; */
/*               oneway→proc/async_todo;最后 wake_up / 记日志(smp_wmb+debug_id_done) */
```

### 4.3 三个翻译函数(数据中嵌入对象的关键)

**binder_translate_binder — L2791(发送方把自己的服务对象传出去)**
- 发送方传 `BINDER_TYPE_BINDER`(node 实体)时,驱动在发送方 proc 查/建 node(L2800-2805);
- 在**目标进程**里为其建立 ref(`binder_inc_ref_for_node`,L2819,会向目标线程 todo 塞 INCREFS 工作项);
- **改写对象**:类型改成 `BINDER_TYPE_HANDLE`,`fp->handle = rdata.desc`(目标进程视角的新句柄),`fp->binder/cookie = 0`(L2825-2831)。→ 目标进程拿到的是自己能用的句柄。

**binder_translate_handle — L2843(把收到的句柄继续转发)**
- 发送方传 `BINDER_TYPE_HANDLE` 时,驱动反查其指向的 node(L2852);
- 若 **node 的宿主就是目标进程**(句柄绕一圈传回服务端):改回 `BINDER_TYPE_BINDER`,填 `node->ptr/cookie`,让服务端直接认领自己的对象(L2865-2884);
- 否则:在目标进程建新 ref,填新的 desc(L2889-2897),handle 继续以句柄形态传递。

**binder_translate_fd — L2911 / binder_translate_fd_array — L2968**
- 从发送方文件表取 file,在**目标进程**文件表里分配新 fd(`task_get_unused_fd_flags`+`fd_install`),回填 `fp->fd = target_fd`(L3661)。native_handle_t 场景走 FDA 批量翻译。

### 4.4 缓冲分配:binder_alloc_new_buf — binder_alloc.c L552
- 在**目标进程** alloc 的空闲红黑树中按 best-fit 找块(`binder_alloc_new_buf_locked`,L378),失败时按需逐页映射(`binder_update_page_range` L187 分配 struct page 并建页表);
- 返回的 `binder_buffer` 的 `user_data` 是目标进程映射区里的虚拟地址,同时该页有内核映射,供驱动直接 `kmap` 写入;
- async(oneway)事务从 `free_async_space` 配额中扣减,防异步风暴。

### 4.5 拷贝三兄弟(binder_alloc.c)
- `binder_alloc_copy_user_to_buffer`(L1169):用户态 → 目标缓冲。按页 `kmap` + `copy_from_user`,**跨页分片循环拷贝**(L1179-1198)。
- `binder_alloc_copy_to_buffer` / `binder_alloc_copy_from_buffer`(L1239/L1249):内核态源 → 目标缓冲 / 读回,内部走 `binder_alloc_do_buffer_copy`(L1201,`kmap_atomic`+memcpy)。
- 安全性:`check_buffer`(L1109)校验越界/对齐/缓冲状态,防止内核读写被释放或用户越权的缓冲。

### 4.6 binder_proc_transaction — L3108(见 3.4)
- 目标 node 锁内取优先级;oneway 且 node 已有 async 事务在排队 → 挂 `node->async_todo` 并**不唤醒**(L3121-3128,L3148);
- 空闲线程挑选 `binder_select_thread_ilocked`;入队后 `binder_wakeup_thread_ilocked`(同步事务要求目标线程必须在 looper 中)。

### 4.7 binder_thread_read — L4490(见 3.5)要点注释

```c
/* L4513:判断是否可处理进程级工作(线程不在同步事务栈中且已进 looper) */
wait_for_proc_work = binder_available_for_proc_work_ilocked(thread);
/* L4537:阻塞等待:优先睡在 thread->wait;进程级工作则加入 proc->waiting_threads */
ret = binder_wait_for_work(thread, wait_for_proc_work);
...
/* L4579:出队的是 BINDER_WORK_TRANSACTION → 取回 t,下面对话填 BR 描述 */
case BINDER_WORK_TRANSACTION:
    t = container_of(w, struct binder_transaction, work); break;
...
/* L4760:L4766 BR_TRANSACTION vs BR_REPLY 判定 */
trd->target.ptr  = target_node->ptr;      /* 服务端看到自己的对象 */
trd->cookie      = target_node->cookie;
cmd = BR_TRANSACTION;                      /* 有 target_node → 新请求 */
/* L4787-4792:关键 —— 数据地址直接给目标进程映射区地址 */
trd->data.ptr.buffer  = (uintptr_t)t->buffer->user_data;
trd->data.ptr.offsets = trd->data.ptr.buffer
                        + ALIGN(t->buffer->data_size, sizeof(void *));
/* L4799/L4809:只拷命令号 + 描述结构体给用户态(真正的数据已就位) */
put_user(cmd, ...);  copy_to_user(ptr, &tr, trsize);
/* L4837:同步事务挂栈等回复;reply/oneway 直接释放 */
```

### 4.8 binder_wait_for_work — L4459

```c
for (;;) {
    prepare_to_wait(&thread->wait, &wait, TASK_INTERRUPTIBLE); /* 挂等待队列 */
    if (binder_has_work_ilocked(thread, do_proc_work)) break;   /* 已有活则退出 */
    if (do_proc_work) list_add(&thread->waiting_thread_node, &proc->waiting_threads);
    binder_inner_proc_unlock(proc);
    schedule();                                                  /* 真正睡眠 */
    ...
}
```

被 `binder_proc_transaction` 的 `binder_wakeup_thread_ilocked` 唤醒 → 返回 0 → 进入出队投递循环。

### 4.9 事务栈(transaction_stack)与同步语义
- 每个线程维护 `transaction_stack`,同步事务在 **发送线程** 压栈(`t->from_parent = thread->transaction_stack; thread->transaction_stack = t;` L3807-3808),在 **目标线程** 收下后也压栈(L4839-4841);
- `BC_REPLY` 到达时,L3237 从栈顶找回 in_reply_to、校验归属,回复完成后 `binder_pop_transaction_ilocked` + `binder_free_transaction(in_reply_to)`(L3789/L3794)弹栈回收 —— 保证同步调用严格配对、栈式嵌套。

---

## 5. cells 定制点(CONFIG_DRV_NS,本仓库独有)

本内核为 cells 多开加了**驱动级命名空间**扩展(binder.c L292-350 等):

- `DEFINE_DRV_NS_INFO(binder)` + `binder_ns_ops`:`binder_ns_create/release` —— 每个虚拟 cell 一个 binder 驱动命名空间实例;
- `current_drv_ns_cell_index()`(L333):通过命名空间 tag("cell1"…"cellN",`init_drv_ns` 为宿主 index 0)反查当前 cell 序号;
- `INIT_OTHER_CONTEXT_MGR_HANDLE = 100000000`(L331):**句柄编号预留段**,>= 该值的 handle 代表"打到第 N 个 cell 的 context manager" —— 见 binder_transaction L3293-3311 定制分支:`proc->acontext[handle - INIT_OTHER_CONTEXT_MGR_HANDLE]->binder_context_mgr_node`,即宿主进程可以直接向虚拟 cell 内的服务总管发起事务;
- 配套:每个 cell 有自己的 context(带独立 context_mgr_node_lock/binder_context_mgr_node),跨 cell 引用计数与节点管理各自隔离。

这意味着 cells 的"虚拟手机"不仅用户态隔离,连 binder 服务注册表(context manager)都在内核层按 cell 切分,并允许宿主 → cell 的定向调用。

---

## 6. 总结

| 阶段 | 关键函数 | 做什么 |
|---|---|---|
| 入口 | `binder_ioctl_write_read` L5117 | 读写一体;先执行 BC 再收取 BR |
| 发送 | `binder_thread_write` → `binder_transaction` L3912/L3201 | 解析目标、分配目标缓冲、拷数据、翻译对象、入队唤醒 |
| 分配 | `binder_alloc_new_buf` L552(alloc.c) | 在目标进程映射区分配缓冲(按页补页) |
| 拷贝 | `binder_alloc_copy_user_to_buffer` L1169 | 发送方用户态 → 目标缓冲,仅一次 |
| 翻译 | `binder_translate_binder/handle/fd/fd_array` L2791/2843/2911/2968 | handle↔node 互转、fd dup |
| 入队 | `binder_proc_transaction` L3108 | 事务入目标线程 todo + 唤醒 |
| 接收 | `binder_thread_read` L4490 | 出队、填 BR_TRANSACTION 描述、copy_to_user |
| 等待 | `binder_wait_for_work` L4459 | 线程睡眠等待被唤醒 |
| 归还 | `BC_FREE_BUFFER` L4089 | 目标进程释放缓冲区 |

**一句话回答"目标进程数据传递过程"**:客户端一次 ioctl 把 `BC_TRANSACTION` 描述交给驱动,驱动在目标进程的 mmap 缓冲区里分配空间、把发送方用户态数据**直接拷入**(唯一一次大拷贝),逐对象翻译好 handle/fd,把事务挂到目标线程的 todo 队列并唤醒它;目标线程下一次 ioctl 的 read 阶段醒来,把已就位数据的"描述"(BR_TRANSACTION)拷回用户态即可直接使用 —— 传统 IPC 的第二次拷贝被"目标进程自己映射缓冲区"这一设计消除。

---

## 7. 过程代码逐步精解(全程真实代码 + 逐行注释)

> 本节把上面第 3、4 节的概述**落到代码行**,按一次同步调用从客户端用户态走到服务端用户态的**真实执行顺序**,逐段贴出本仓库 binder.c / binder_alloc.c 的实际代码并逐行注释。标注 "Lxxxx" 均为 `kernel/drivers/android/binder.c`(alloc 相关注明 `binder_alloc.c`)中的行号。为节省篇幅,每段只截取与本步骤直接相关的代码,省略部分用 `/* ... */` 标明。

### 7.0 前置:用户态做了什么(1 分钟背景)

用户态由 `frameworks/native/libs/binder/` 封装:

1. **进程初始化** `ProcessState::self()`:打开 `/dev/binder`,并 `mmap(NULL, MAP_SIZE, PROT_READ, MAP_PRIVATE, fd, 0)` —— 映射**一块只读缓冲区**,这就是内核向本进程"投递数据"的落点(binder_mmap 见 L5488)。
2. **每次调用** `IPCThreadState::transact()`:`writeTransactionData()` 把 `binder_transaction_data` 填进本线程的 **mOut** 缓冲区(含 Parcel 数据指针/offsets),然后 `talkWithDriver()` 发起 `ioctl(fd, BINDER_WRITE_READ, &bwr)`,`bwr.write_buffer=mOut.data()`,`bwr.read_buffer=mIn.data()`。
3. **服务端线程** 则在 `IPCThreadState::joinThreadPool()` 里循环 `talkWithDriver()`——只 read 不收 write,靠内核唤醒取事务。

下面从内核 `binder_ioctl` 开始。

### 7.1 第一步:ioctl 分发到 binder_ioctl(L5291)

```c
static long binder_ioctl(struct file *filp, unsigned int cmd, unsigned long arg) {
    int ret;
    struct binder_proc *proc = filp->private_data;   /* open 时挂到 file 上的进程对象 */
    struct binder_thread *thread;
    unsigned int size = _IOC_SIZE(cmd);
    void __user *ubuf = (void __user *)arg;

    binder_selftest_alloc(&proc->alloc);            /* 自测钩子(发行版为空) */
    trace_binder_ioctl(cmd, arg);

    ret = wait_event_interruptible(binder_user_error_wait,
                                   binder_stop_on_user_error < 2); /* 驱动出错停机开关 */
    if (ret) goto err_unlocked;

    thread = binder_get_thread(proc);               /* 按 current->pid 在 proc->threads 红黑树
                                                     * 找本线程;找不到则新建 binder_thread,
                                                     * 并把 looper 状态清 0 */
    if (thread == NULL) { ret = -ENOMEM; goto err; }

    switch (cmd) {
        case BINDER_WRITE_READ:                      /* ★ 99% 的调用走这里 */
            ret = binder_ioctl_write_read(filp, cmd, arg, thread);
            if (ret) goto err;
            break;
        case BINDER_SET_MAX_THREADS: { ... }         /* 配最大线程数 */
        case BINDER_SET_CONTEXT_MGR:
        case BINDER_SET_CONTEXT_MGR_EXT: ...         /* 注册 context manager */
        case BINDER_THREAD_EXIT: ...                 /* 线程退出清理 */
        case BINDER_VERSION: ...                     /* 协议版本协商 */
        ...
    }
    ret = 0;
err:
    if (thread) thread->looper_need_return = false;
    ...
    return ret;
}
```

要点:`binder_get_thread(proc)` 把"当前是哪个线程在 ioctl"登记进内核(proc->threads 红黑树按 tid 索引)——之后所有 todo 队列、唤醒都围绕这个 `binder_thread` 进行。

### 7.2 第二步:binder_ioctl_write_read 拆包(L5117)

```c
static int binder_ioctl_write_read(struct file *filp, unsigned int cmd,
                                   unsigned long arg, struct binder_thread *thread) {
    int ret = 0;
    struct binder_proc *proc = filp->private_data;
    struct binder_write_read bwr;

    if (copy_from_user(&bwr, ubuf, sizeof(bwr))) { ret = -EFAULT; goto out; }
    /* bwr = { write_size, write_buffer, read_size, read_buffer, ... } */

    if (bwr.write_size > 0) {
        /* ① 先执行用户写下来的 BC_ 命令流(客户端在这里发事务) */
        ret = binder_thread_write(proc, thread, bwr.write_buffer,
                                  bwr.write_size, &bwr.write_consumed);
        if (ret < 0) { ...; goto out; }              /* write 失败直接返回 */
    }
    if (bwr.read_size > 0) {
        /* ② 再尝试收取驱动要投递给本线程的 BR_ 命令流
         *   (客户端等 reply/complete;服务端等新事务) */
        ret = binder_thread_read(proc, thread, bwr.read_buffer, bwr.read_size,
                                 &bwr.read_consumed, filp->f_flags & O_NONBLOCK);
        binder_inner_proc_lock(proc);
        if (!binder_worklist_empty_ilocked(&proc->todo))
            binder_wakeup_proc_ilocked(proc);        /* 进程队列还有活 → 叫醒别的线程 */
        binder_inner_proc_unlock(proc);
        if (ret < 0) { ...; goto out; }
    }
    /* 把 consumed 写回用户态:用户态据此知道命令流推进了多少,
     * 下轮 ioctl 从上次断点继续(命令流可跨多次 ioctl 分批消费) */
    if (copy_to_user(ubuf, &bwr, sizeof(bwr))) { ret = -EFAULT; goto out; }
out:
    return ret;
}
```

### 7.3 第三步:命令流循环 binder_thread_write(L3912)

```c
static int binder_thread_write(struct binder_proc *proc,
                               struct binder_thread *thread,
                               binder_uintptr_t binder_buffer, size_t size,
                               binder_size_t *consumed) {
    uint32_t cmd;
    void __user *buffer = (void __user *)(uintptr_t)binder_buffer;
    void __user *ptr = buffer + *consumed;            /* 从上次断点继续 */
    void __user *end = buffer + size;

    /* 只要没消费完、且本线程没有挂起的错误,就逐条执行 BC_ 命令 */
    while (ptr < end && thread->return_error.cmd == BR_OK) {
        int ret;

        if (get_user(cmd, (uint32_t __user *)ptr)) return -EFAULT; /* 取命令字 */
        ptr += sizeof(uint32_t);                                    /* 跳过命令字 */
        trace_binder_command(cmd);
        if (_IOC_NR(cmd) < ARRAY_SIZE(binder_stats.bc)) { ... }     /* 统计 */
        switch (cmd) {
            case BC_INCREFS: case BC_ACQUIRE:
            case BC_RELEASE: case BC_DECREFS: { ... }   /* 引用计数命令 */
            ...
            case BC_FREE_BUFFER: { ... }                /* 见 7.9 */
            case BC_TRANSACTION:
            case BC_REPLY: {
                struct binder_transaction_data tr;

                if (copy_from_user(&tr, ptr, sizeof(tr))) return -EFAULT;
                ptr += sizeof(tr);
                /* ★ 真正干活的地方:reply=1 表示 BC_REPLY,否则是 BC_TRANSACTION */
                binder_transaction(proc, thread, &tr, cmd == BC_REPLY, 0);
                break;
            }
            ...
        }
    }
    *consumed = ptr - buffer;
    ...
}
```

注意:**这里只拷了 40/64 字节的 `binder_transaction_data` 描述**,业务数据(tr.data.ptr.buffer 指向的 Parcel 内容)还在发送方用户态,由 binder_transaction 内部按需再读。

### 7.4 第四步(上):binder_transaction 前半段 —— 解析"目标是谁"(L3201)

```c
static void binder_transaction(struct binder_proc *proc,
                               struct binder_thread *thread,
                               struct binder_transaction_data *tr, int reply,
                               binder_size_t extra_buffers_size) {
    ...
    struct binder_proc   *target_proc = NULL;    /* 目标进程(要往它缓冲区拷数据) */
    struct binder_thread *target_thread = NULL;  /* 目标线程(同步事务必须指定) */
    struct binder_node   *target_node = NULL;    /* 目标服务对象 */
    struct binder_transaction *in_reply_to = NULL;
    ...
    if (reply) {
        /* ── BC_REPLY:回复栈顶那个等我回复的事务 ── */
        binder_inner_proc_lock(proc);
        in_reply_to = thread->transaction_stack;      /* 本线程栈顶 */
        if (in_reply_to == NULL) { ... BR_FAILED_REPLY; goto err_empty_call_stack; }
        if (in_reply_to->to_thread != thread) { ... } /* 校验归属 */
        thread->transaction_stack = in_reply_to->to_parent;   /* 弹栈 */
        binder_inner_proc_unlock(proc);
        target_thread = binder_get_txn_from_and_acq_inner(in_reply_to);
        /* ↑ 从被回复的事务里取回"发起线程"(from),并 +tmp_ref 保活 */
        if (target_thread == NULL) { ... BR_DEAD_REPLY; goto err_dead_binder; }
        ...
        target_proc = target_thread->proc;            /* 回复目标是发起进程 */
        atomic_inc(&target_proc->tmp_ref);            /* 进程保活 */
        binder_inner_proc_unlock(target_thread->proc);
    } else {
        /* ── BC_TRANSACTION:按 handle 查目标 ── */
        if (tr->target.handle) {                      /* handle>0:普通远程对象 */
            struct binder_ref *ref;

            binder_proc_lock(proc);
            /* 本进程 refs 红黑树按 handle(desc) 找 ref */
            ref = binder_get_ref_olocked(proc, tr->target.handle, true);
            if (ref) {
                /* 从 ref 反查 node,再取 node 的宿主进程为 target_proc;
                 * 同时给 node/proc 加 tmp_ref 保活,防目标刚好在退出 */
                target_node = binder_get_node_refs_for_txn(ref->node,
                                                           &target_proc,
                                                           &return_error);
            } else { ... BR_FAILED_REPLY; }
            binder_proc_unlock(proc);
        } else {
            /* handle==0:打给 context manager(0 号服务总管) */
            mutex_lock(&context->context_mgr_node_lock);
            target_node = context->binder_context_mgr_node;
            if (target_node)
                target_node = binder_get_node_refs_for_txn(target_node,
                                                           &target_proc,
                                                           &return_error);
            else
                return_error = BR_DEAD_REPLY;
            mutex_unlock(&context->context_mgr_node_lock);
            ...
        }
        if (!target_node) { ...; goto err_dead_binder; }

        /* SELinux 安全校验:发送进程能否对目标进程发起事务 */
        if (security_binder_transaction(proc->tsk, target_proc->tsk) < 0) {
            ... BR_FAILED_REPLY/-EPERM; goto err_invalid_target_handle;
        }

        /* 同步事务(非 oneway):尝试找到目标进程里"正在等我这条链路回复"的线程,
         * 以便把新事务派给同一个线程(避免死锁/乱序) */
        binder_inner_proc_lock(proc);
        if (!(tr->flags & TF_ONE_WAY) && thread->transaction_stack) {
            struct binder_transaction *tmp = thread->transaction_stack;
            ... /* 沿 from_parent 向上找 from->proc == target_proc 的线程 */
        }
        binder_inner_proc_unlock(proc);
    }
    /* 到这里 target_proc 已确定;target_thread 可能为 NULL(oneway) */
```

关键点:回复(reply)的"目标进程"就是**发起者的进程**;请求的"目标进程"由 **handle → ref → node → node->proc** 三级索引解出。

### 7.5 第四步(下):分配内核事务对象并填充元信息(L3414-3471)

```c
    /* 内核侧事务描述对象 */
    t = kzalloc(sizeof(*t), GFP_KERNEL);            /* binder_transaction */
    if (t == NULL) { ... -ENOMEM; goto err_alloc_t_failed; }
    binder_stats_created(BINDER_STAT_TRANSACTION);
    spin_lock_init(&t->lock);

    tcomplete = kzalloc(sizeof(*tcomplete), GFP_KERNEL); /* binder_work(受理通知) */
    if (tcomplete == NULL) { ...; goto err_alloc_tcomplete_failed; }
    binder_stats_created(BINDER_STAT_TRANSACTION_COMPLETE);

    t->debug_id = t_debug_id;                       /* 每事务唯一 id,用于日志/dumpsys */
    ...
    if (!reply && !(tr->flags & TF_ONE_WAY))
        t->from = thread;                           /* 同步请求:记住发起线程 */
    else
        t->from = NULL;                             /* reply/oneway 不需要回程信息 */
    t->sender_euid = task_euid(proc->tsk);
    t->to_proc   = target_proc;                     /* ★ 数据要拷进这个进程 */
    t->to_thread = target_thread;                   /* ★ 事务要交给这个线程 */
    t->code  = tr->code;                            /* 方法号 */
    t->flags = tr->flags;
    /* 优先级继承:同步事务继承调用者策略,否则用目标进程默认优先级 */
    if (!(t->flags & TF_ONE_WAY) && binder_supported_policy(current->policy)) {
        t->priority.sched_policy = current->policy;
        t->priority.prio = current->normal_prio;
    } else {
        t->priority = target_proc->default_priority;
    }
    ...
    trace_binder_transaction(reply, t, target_node);
```

### 7.6 第五步(核心!):在【目标进程】里分配事务缓冲区(L3498)

```c
    /* ★★★ 分配动作作用在 target_proc->alloc 上:
     * 也就是说缓冲区是从"目标进程 mmap 的那块地址空间"里切出来的。
     * 大小 = data_size(主数据) + offsets_size(偏移数组) + extra(如安全上下文),
     * 最后一项 is_async:oneway 事务要占用独立的异步配额。 */
    t->buffer = binder_alloc_new_buf(&target_proc->alloc, tr->data_size,
                                     tr->offsets_size, extra_buffers_size,
                                     !reply && (t->flags & TF_ONE_WAY));
    if (IS_ERR(t->buffer)) {
        return_error_param = PTR_ERR(t->buffer);
        /* -ESRCH = 目标进程 vma 已没了(正在退出)→ 报 BR_DEAD_REPLY */
        return_error = return_error_param == -ESRCH ?
                       BR_DEAD_REPLY : BR_FAILED_REPLY;
        ...
        goto err_binder_alloc_buf_failed;
    }
    t->buffer->debug_id = t_debug_id;
    t->buffer->transaction = t;                 /* 缓冲 ↔ 事务互相索引 */
    t->buffer->target_node = target_node;
    trace_binder_transaction_alloc_buf(t->buffer);
```

展开分配内部 `binder_alloc_new_buf_locked`(binder_alloc.c L378):

```c
    /* alloc->free_buffers 是空闲块组成的红黑树(按块大小/地址排序) */
    struct rb_node *n = alloc->free_buffers.rb_node;
    ...
    if (alloc->vma == NULL) return ERR_PTR(-ESRCH);  /* 目标进程没 mmap → 已死 */

    data_offsets_size = ALIGN(data_size, sizeof(void *)) +
                        ALIGN(offsets_size, sizeof(void *));
    /* (整数溢出/对齐合法性校验省略) */
    size = data_offsets_size + ALIGN(extra_buffers_size, sizeof(void *));

    /* oneway 事务受 free_async_space 配额限制,防止异步风暴吃光映射区 */
    if (is_async && alloc->free_async_space < size + sizeof(struct binder_buffer))
        return ERR_PTR(-ENOSPC);

    /* best-fit:在空闲红黑树里找 ≥ size 的最小块 */
    while (n) {
        buffer = rb_entry(n, struct binder_buffer, rb_node);
        buffer_size = binder_alloc_buffer_size(alloc, buffer);
        if (size < buffer_size) { best_fit = n; n = n->rb_left; }
        else if (size > buffer_size) n = n->rb_right;
        else { best_fit = n; break; }
    }
    if (best_fit == NULL) { ...; return ERR_PTR(-ENOSPC); } /* 地址空间耗尽 */

    /* 若该块还没映射物理页(首次用到的页)→ 逐页补页:
     * alloc_page 分配物理页 + vm_insert_page 映射进【目标进程】用户态 vma。
     * 内核侧则通过 alloc->pages[] 持有 struct page 指针,之后 kmap 即可写入。 */
    end_page_addr = PAGE_ALIGN((uintptr_t)buffer->user_data + size);
    ret = binder_update_page_range(alloc, 1,
                                   PAGE_ALIGN((uintptr_t)buffer->user_data),
                                   end_page_addr);
    if (ret) return ERR_PTR(ret);

    if (buffer_size != size) {               /* 大块切出小块后,剩余部分重新入空闲树 */
        new_buffer = kzalloc(sizeof(*buffer), GFP_KERNEL);
        new_buffer->user_data = (u8 __user *)buffer->user_data + size;
        list_add(&new_buffer->entry, &buffer->entry);
        new_buffer->free = 1;
        binder_insert_free_buffer(alloc, new_buffer);
    }

    rb_erase(best_fit, &alloc->free_buffers);  /* 从空闲树摘下 */
    buffer->free = 0;
    buffer->allow_user_free = 0;
    binder_insert_allocated_buffer_locked(alloc, buffer);
    buffer->data_size = data_size;             /* 记录各段大小(接收侧据此定位) */
    buffer->offsets_size = offsets_size;
    buffer->async_transaction = is_async;
    buffer->extra_buffers_size = extra_buffers_size;
    if (is_async) alloc->free_async_space -= size + sizeof(struct binder_buffer);
    return buffer;                             /* 返回的 buffer->user_data 是
                                                * 目标进程用户态虚拟地址! */
```

> **关键认知**:`binder_buffer.user_data` 是 **目标进程地址空间** 里的虚拟地址(它同时被目标进程的 mmap 和内核页表引用)。后续内核 kmap 写它、目标进程直接读它,天然一致 —— 这就是"拷进目标进程的缓冲区"的含义。

### 7.7 第六步:把发送方数据拷进目标缓冲(L3530-3560)

```c
    /* ① 拷贝主数据区:发送方用户态 tr.data.ptr.buffer → 目标缓冲偏移 0 */
    if (binder_alloc_copy_user_to_buffer(
            &target_proc->alloc, t->buffer, 0,
            (const void __user *)(uintptr_t)tr->data.ptr.buffer,
            tr->data_size)) {
        ... -EFAULT; goto err_copy_data_failed;
    }
    /* ② 拷贝偏移数组:紧跟在主数据之后(对齐到指针大小) */
    if (binder_alloc_copy_user_to_buffer(
            &target_proc->alloc, t->buffer,
            ALIGN(tr->data_size, sizeof(void *)),
            (const void __user *)(uintptr_t)tr->data.ptr.offsets,
            tr->offsets_size)) {
        ... -EFAULT; goto err_copy_data_failed;
    }
```

展开 `binder_alloc_copy_user_to_buffer`(binder_alloc.c L1169)的**逐页分片拷贝**:

```c
unsigned long
binder_alloc_copy_user_to_buffer(struct binder_alloc *alloc,
                                 struct binder_buffer *buffer,
                                 binder_size_t buffer_offset,
                                 const void __user *from, size_t bytes)
{
    if (!check_buffer(alloc, buffer, buffer_offset, bytes))  /* 越界/对齐/占用校验 */
        return bytes;                                        /* 非法 → 返回未拷字节数 */

    while (bytes) {
        unsigned long size;
        unsigned long ret;
        struct page *page;
        pgoff_t pgoff;
        void *kptr;

        page = binder_alloc_get_page(alloc, buffer, buffer_offset, &pgoff);
        /* ↑ 由 buffer->user_data + buffer_offset 算出落在第几页、页内偏移 */
        size = min_t(size_t, bytes, PAGE_SIZE - pgoff);      /* 本页最多拷多少 */
        kptr = kmap(page) + pgoff;                           /* 内核映射该页 */
        ret = copy_from_user(kptr, from, size);              /* ★ 用户→内核一次拷贝 */
        kunmap(page);
        if (ret) return bytes - size + ret;                  /* 失败返回剩余量 */
        bytes -= size;
        from += size;
        buffer_offset += size;                               /* 推进到下一页继续 */
    }
    return 0;                                                /* 全拷完 */
}
```

这段代码是"一次拷贝"的真正实现:**从发送方用户态读,直接写进目标进程缓冲对应的物理页**(该页同时也映射在目标进程用户态)。跨页的数据在循环里逐页 kmap 处理。

### 7.8 第七步:逐对象翻译(handle/binder/fd/内嵌缓冲)(L3585-3777)

数据拷完后,驱动要"看懂"里面嵌入的对象,按偏移数组逐个处理:

```c
    /* 布局:[主数据 data_size][offsets 数组 offsets_size][extra sg 区 extra_buffers_size] */
    off_start_offset = ALIGN(tr->data_size, sizeof(void *));  /* offsets 起始 */
    buffer_offset   = off_start_offset;
    off_end_offset  = off_start_offset + tr->offsets_size;    /* offsets 结束 */
    sg_buf_offset   = ALIGN(off_end_offset, sizeof(void *));  /* sg 区起始 */
    sg_buf_end_offset = sg_buf_offset + extra_buffers_size - ALIGN(secctx_sz, sizeof(u64));

    off_min = 0;   /* 偏移必须单调递增,防止恶意乱序 */
    for (buffer_offset = off_start_offset; buffer_offset < off_end_offset;
         buffer_offset += sizeof(binder_size_t)) {
        struct binder_object_header *hdr;
        size_t object_size;
        struct binder_object object;
        binder_size_t object_offset;

        /* 读出偏移数组中的一个值 → object_offset(指向主数据里某个对象) */
        binder_alloc_copy_from_buffer(&target_proc->alloc, &object_offset,
                                      t->buffer, buffer_offset,
                                      sizeof(object_offset));
        /* 校验并取回对象头(object_offset 处必须是合法对象,且不小于 off_min) */
        object_size = binder_get_object(target_proc, t->buffer,
                                        object_offset, &object);
        if (object_size == 0 || object_offset < off_min) { ... -EINVAL; goto err_bad_offset; }

        hdr = &object.hdr;
        off_min = object_offset + object_size;
        switch (hdr->type) {          /* 按对象类型分流翻译 */
            case BINDER_TYPE_BINDER:
            case BINDER_TYPE_WEAK_BINDER: {
                struct flat_binder_object *fp = to_flat_binder_object(hdr);
                /* 发送方把自己的服务对象(node)传出去:
                 * 在目标进程建 ref,并把对象改写成目标进程视角的 handle */
                ret = binder_translate_binder(fp, t, thread);
                ...
                binder_alloc_copy_to_buffer(&target_proc->alloc, t->buffer,
                                            object_offset, fp, sizeof(*fp)); /* 写回 */
            } break;
            case BINDER_TYPE_HANDLE:
            case BINDER_TYPE_WEAK_HANDLE: {
                /* 发送方转发的远程句柄 → 目标进程视角的 node 或新 handle */
                ret = binder_translate_handle(fp, t, thread);
                ...
                binder_alloc_copy_to_buffer(...);   /* 翻译结果写回缓冲 */
            } break;
            case BINDER_TYPE_FD: {
                /* 单个 fd:在目标进程文件表里 dup 一个 */
                int target_fd = binder_translate_fd(fp->fd, t, thread, in_reply_to);
                ...
                fp->pad_binder = 0;
                fp->fd = target_fd;                 /* 回填新 fd 号 */
                binder_alloc_copy_to_buffer(...);
            } break;
            case BINDER_TYPE_FDA: { ... binder_translate_fd_array(...); } break;
            case BINDER_TYPE_PTR: {
                /* 内嵌大缓冲:拷进 sg 区,并把对象里的指针改成目标进程地址 */
                if (binder_alloc_copy_user_to_buffer(&target_proc->alloc, t->buffer,
                                                     sg_buf_offset,
                                                     (const void __user *)(uintptr_t)bp->buffer,
                                                     bp->length)) { ... -EFAULT; }
                /* ★ 指针修复:让目标进程里的这个指针指向【目标进程】的缓冲地址 */
                bp->buffer = (uintptr_t)t->buffer->user_data + sg_buf_offset;
                sg_buf_offset += ALIGN(bp->length, sizeof(u64));
                ...
            } break;
            default: ... -EINVAL; goto err_bad_object_type;
        }
    }
```

逐个翻译时若出错,`goto err_*` 会统一走 `binder_transaction_buffer_release()` + `binder_alloc_free_buf()` 把已拷数据全部回滚释放(L3846-3852),保证不留半截事务。

翻译细节(与第 4.3 节呼应,这里给出 ref 建立的真实代码 `binder_inc_ref_for_node`,L2206):

```c
static int binder_inc_ref_for_node(struct binder_proc *proc,
                                   struct binder_node *node,
                                   bool strong,
                                   struct list_head *target_list,
                                   struct binder_ref_data *rdata) {
    struct binder_ref *ref;
    struct binder_ref *new_ref = NULL;

    binder_proc_lock(proc);
    ref = binder_get_ref_for_node_olocked(proc, node, NULL);  /* 目标进程里已有 ref? */
    if (!ref) {                          /* 没有 → 造一个(带 desc = 递增的句柄号) */
        binder_proc_unlock(proc);
        new_ref = kzalloc(sizeof(*ref), GFP_KERNEL);
        if (!new_ref) return -ENOMEM;
        binder_proc_lock(proc);
        ref = binder_get_ref_for_node_olocked(proc, node, new_ref);
    }
    ret = binder_inc_ref_olocked(ref, strong, target_list);
    /* ↑ 计数 +1;若这是本进程第一次接触该 node,
     *   会向 target_list(调用线程的 todo)塞 BINDER_WORK 通知,
     *   稍后由驱动向用户态发 BR_ACQUIRE/BR_INCREFS 让用户态补计数 */
    *rdata = ref->data;                  /* 把新 desc(handle)带回给调用者 */
    binder_proc_unlock(proc);
    if (new_ref && ref != new_ref) kfree(new_ref);  /* 并发下别人先建了 → 释放多余的 */
    return ret;
}
```

### 7.9 第八步:入队 —— 把事务交给目标线程并唤醒(L3778-3822)

翻译全部成功后,按三类路径收尾:

```c
    tcomplete->type = BINDER_WORK_TRANSACTION_COMPLETE;   /* 受理通知(发给发送方) */
    t->work.type    = BINDER_WORK_TRANSACTION;            /* 事务工作项(给目标方) */

    if (reply) {
        /* ── BC_REPLY:回复直接送回发起线程 ── */
        binder_enqueue_thread_work(thread, tcomplete);    /* 发送方收 complete */
        binder_inner_proc_lock(target_proc);
        if (target_thread->is_dead) { ...; goto err_dead_proc_or_thread; }
        BUG_ON(t->buffer->async_transaction != 0);
        binder_pop_transaction_ilocked(target_thread, in_reply_to); /* 弹发起线程栈 */
        binder_enqueue_thread_work_ilocked(target_thread, &t->work);/* 事务入发起线程 todo */
        binder_inner_proc_unlock(target_proc);
        wake_up_interruptible_sync(&target_thread->wait); /* ★ 同步唤醒发起线程 */
        binder_restore_priority(current, in_reply_to->saved_priority);
        binder_free_transaction(in_reply_to);             /* 旧事务(等回复的那个)使命完成 */
    } else if (!(t->flags & TF_ONE_WAY)) {
        /* ── 同步请求 ── */
        BUG_ON(t->buffer->async_transaction != 0);
        binder_inner_proc_lock(proc);
        /*
         * Defer the TRANSACTION_COMPLETE, so we don't return to
         * userspace immediately; this allows the target process to
         * immediately start processing this transaction, reducing
         * latency. We will then return the TRANSACTION_COMPLETE when
         * the target replies (or there is an error).
         */
        binder_enqueue_deferred_thread_work_ilocked(thread, tcomplete);
        /* ↑ complete 延迟入队(见 L3798-3804 注释):等目标回 reply 时才发,
         *  让目标进程立刻开始处理,降低时延 */
        t->need_reply = 1;
        /* 发送线程事务栈压栈:栈上保存"我在等谁的回复" */
        t->from_parent = thread->transaction_stack;
        thread->transaction_stack = t;
        binder_inner_proc_unlock(proc);
        /* ★ 把事务送进目标进程/线程,并唤醒目标线程 */
        if (!binder_proc_transaction(t, target_proc, target_thread)) {
            binder_inner_proc_lock(proc);
            binder_pop_transaction_ilocked(thread, t);
            binder_inner_proc_unlock(proc);
            goto err_dead_proc_or_thread;      /* 目标已死 → 回滚 */
        }
    } else {
        /* ── oneway:无回复,直接投递 ── */
        binder_enqueue_thread_work(thread, tcomplete);    /* complete 立刻发 */
        if (!binder_proc_transaction(t, target_proc, NULL))
            goto err_dead_proc_or_thread;
    }
    ...
    /* 收尾:解 tmp_ref、写日志(smp_wmb + debug_id_done) */
```

展开 `binder_proc_transaction`(L3108)——"挑线程 + 入队 + 唤醒"三合一:

```c
static bool binder_proc_transaction(struct binder_transaction *t,
                                    struct binder_proc *proc,
                                    struct binder_thread *thread) {
    struct binder_node *node = t->buffer->target_node;
    bool oneway = !!(t->flags & TF_ONE_WAY);
    bool pending_async = false;

    binder_node_lock(node);
    ...
    if (oneway) {
        BUG_ON(thread);
        if (node->has_async_transaction) pending_async = true; /* 该 node 已有异步事务在跑 */
        else node->has_async_transaction = true;
    }

    binder_inner_proc_lock(proc);
    if (proc->is_dead || (thread && thread->is_dead)) { ...; return false; }

    if (!thread && !pending_async)
        /* 没指定线程 → 从 proc->waiting_threads 队头挑一个正在等待的空闲线程 */
        thread = binder_select_thread_ilocked(proc);

    if (thread) {
        binder_transaction_priority(thread->task, t, node_prio, node->inherit_rt);
        binder_enqueue_thread_work_ilocked(thread, &t->work); /* 入【线程】todo */
    } else if (!pending_async) {
        binder_enqueue_work_ilocked(&t->work, &proc->todo);   /* 入【进程】todo */
    } else {
        binder_enqueue_work_ilocked(&t->work, &node->async_todo); /* 挂 node 异步队列 */
    }

    if (!pending_async)
        binder_wakeup_thread_ilocked(proc, thread, !oneway /* sync */);
    /* ↑ 有线程 → wake_up_interruptible[_sync](&thread->wait);
     *  无线程(全忙/epoll)→ binder_wakeup_poll_threads_ilocked 扫全部线程唤醒 */

    binder_inner_proc_unlock(proc);
    binder_node_unlock(node);
    return true;
}
```

### 7.10 第九步(接收侧):目标线程醒来,在 binder_thread_read 里取事务(L4490-4846)

目标进程的服务线程一直在 `joinThreadPool()` 循环里 ioctl read;上一步的 wake_up 让它从 `binder_wait_for_work`(L4459,睡在 `thread->wait` 上)返回:

```c
static int binder_thread_read(struct binder_proc *proc,
                              struct binder_thread *thread,
                              binder_uintptr_t binder_buffer, size_t size,
                              binder_size_t *consumed, int non_block) {
    void __user *buffer = (void __user *)(uintptr_t)binder_buffer;
    void __user *ptr = buffer + *consumed;
    void __user *end = buffer + size;
    int ret = 0;
    int wait_for_proc_work;

    if (*consumed == 0) {
        /* 每条命令流开头先塞一个 BR_NOOP 占位(见下) */
        if (put_user(BR_NOOP, (uint32_t __user *)ptr)) return -EFAULT;
        ptr += sizeof(uint32_t);
    }

retry:
    binder_inner_proc_lock(proc);
    /* 本线程能否处理"进程级"工作?
     * 条件:不在同步事务栈上(不能一边等回复一边接新活)、todo 空、
     *      且已 BC_ENTER_LOOPER/BC_REGISTER_LOOPER */
    wait_for_proc_work = binder_available_for_proc_work_ilocked(thread);
    binder_inner_proc_unlock(proc);

    thread->looper |= BINDER_LOOPER_STATE_WAITING;

    if (non_block) {
        if (!binder_has_work(thread, wait_for_proc_work)) ret = -EAGAIN;
    } else {
        /* ★ 阻塞等活:优先只等自己的 todo;等进程级工作时把自己挂进
         *   proc->waiting_threads(这样 binder_select_thread_ilocked 才挑得到我) */
        ret = binder_wait_for_work(thread, wait_for_proc_work);
    }
    thread->looper &= ~BINDER_LOOPER_STATE_WAITING;
    if (ret) return ret;

    while (1) {          /* 出队循环:一次 read 尽量多取几条命令 */
        uint32_t cmd;
        struct binder_transaction_data_secctx tr;
        struct binder_transaction_data *trd = &tr.transaction_data;
        struct binder_work *w = NULL;
        struct list_head *list = NULL;
        struct binder_transaction *t = NULL;
        size_t trsize = sizeof(*trd);

        binder_inner_proc_lock(proc);
        /* 优先取本线程 todo;否则(允许时)取进程 todo */
        if (!binder_worklist_empty_ilocked(&thread->todo)) list = &thread->todo;
        else if (!binder_worklist_empty_ilocked(&proc->todo) && wait_for_proc_work)
            list = &proc->todo;
        else { ...; if (ptr - buffer == 4 && !thread->looper_need_return) goto retry; break; }

        if (end - ptr < sizeof(tr) + 4) { ...; break; }  /* 用户 read 缓冲不够 → 下轮再来 */

        w = binder_dequeue_work_head_ilocked(list);      /* 出队一个工作项 */
        ...

        switch (w->type) {
            case BINDER_WORK_TRANSACTION: {              /* ★ 我们等的就是它 */
                binder_inner_proc_unlock(proc);
                t = container_of(w, struct binder_transaction, work);
            } break;
            case BINDER_WORK_TRANSACTION_COMPLETE: {     /* 发送方收受理通知 */
                cmd = BR_TRANSACTION_COMPLETE;
                put_user(cmd, (uint32_t __user *)ptr); ptr += 4;
                kfree(w); ...
            } break;
            ...
        }

        if (!t) continue;                                /* 不是事务就处理下一条 */

        BUG_ON(t->buffer == NULL);
        /* ── 填写要拷给用户态的 binder_transaction_data ── */
        if (t->buffer->target_node) {                    /* 有 target_node → 新请求 */
            struct binder_node *target_node = t->buffer->target_node;
            trd->target.ptr   = target_node->ptr;        /* 服务端看到的是自己的对象 */
            trd->cookie       = target_node->cookie;
            ...
            cmd = BR_TRANSACTION;
        } else {                                         /* 无 node → 这是 reply */
            trd->target.ptr = 0; trd->cookie = 0;
            cmd = BR_REPLY;
        }
        trd->code = t->code;
        trd->flags = t->flags;
        trd->sender_euid = from_kuid(current_user_ns(), t->sender_euid);
        t_from = binder_get_txn_from(t);                 /* 取发送线程(同步请求才有) */
        if (t_from) trd->sender_pid = task_tgid_nr_ns(sender, ...);  /* 填 sender_pid */
        ...
        trd->data_size = t->buffer->data_size;
        trd->offsets_size = t->buffer->offsets_size;
        /* ★★★ 数据地址直接给【目标进程自己的映射区地址】—— 数据早已就位,无需再拷 */
        trd->data.ptr.buffer = (uintptr_t)t->buffer->user_data;
        trd->data.ptr.offsets = trd->data.ptr.buffer +
                                ALIGN(t->buffer->data_size, sizeof(void *));
        tr.secctx = t->security_ctx;
        if (t->security_ctx) { cmd = BR_TRANSACTION_SEC_CTX; trsize = sizeof(tr); }

        /* 拷给用户态:先命令字,再结构体描述(几十字节,不含业务数据) */
        if (put_user(cmd, (uint32_t __user *)ptr)) { ...; return -EFAULT; }
        ptr += sizeof(uint32_t);
        if (copy_to_user(ptr, &tr, trsize)) { ...; return -EFAULT; }
        ptr += trsize;

        t->buffer->allow_user_free = 1;   /* 现在允许目标进程 BC_FREE_BUFFER 归还 */
        if (cmd != BR_REPLY && !(t->flags & TF_ONE_WAY)) {
            /* 同步请求:在【目标线程】也压栈,等服务端代码调 BC_REPLY 时弹栈 */
            binder_inner_proc_lock(thread->proc);
            t->to_parent = thread->transaction_stack;
            t->to_thread = thread;
            thread->transaction_stack = t;
            binder_inner_proc_unlock(thread->proc);
        } else {
            binder_free_transaction(t);   /* reply/oneway:事务使命完成,回收 */
        }
        break;
    }

done:
    *consumed = ptr - buffer;
    /* 若目标进程空闲线程不足且没到 max_threads → 追加 BR_SPAWN_LOOPER,
     * 让用户态再开一条线程进 looper(线程池扩容) */
    if (proc->requested_threads == 0 &&
        list_empty(&thread->proc->waiting_threads) &&
        proc->requested_threads_started < proc->max_threads &&
        (thread->looper & (BINDER_LOOPER_STATE_REGISTERED | BINDER_LOOPER_STATE_ENTERED))) {
        proc->requested_threads++;
        put_user(BR_SPAWN_LOOPER, (uint32_t __user *)buffer); ...
    }
    return 0;
}
```

**接收侧零拷贝真相**:业务数据在 7.7 就被拷进目标进程映射区;这里 `put_user` + `copy_to_user` 只搬运命令字与 `binder_transaction_data` 描述(几十字节),然后用户态 `Parcel::ipcSetDataReference` 直接拿 `data.ptr.buffer` 指向自己地址空间里的真实数据解析。

### 7.11 收尾:回复(BC_REPLY)、弹栈、释放缓冲

1. **服务端处理完** → 调 `BC_REPLY`:走 7.4-7.9 同一条 binder_transaction 但 reply=1,目标被解析为"发起线程";数据拷进**发起进程**的缓冲并唤醒它(L3781-3794)。
2. **发起线程 read 醒来**收 `BR_REPLY`(7.10 无 target_node 分支)。
3. **弹栈回收**(同步配对的核心,见 L2237 / L3789 / L3794):

```c
/* binder_pop_transaction_ilocked(L2237):把目标线程栈顶的"等回复事务"弹掉 */
static void binder_pop_transaction_ilocked(struct binder_thread *target_thread,
                                           struct binder_transaction *t) {
    BUG_ON(!target_thread);
    BUG_ON(target_thread->transaction_stack != t);
    BUG_ON(target_thread->transaction_stack->from != target_thread);
    target_thread->transaction_stack =
            target_thread->transaction_stack->from_parent;  /* 恢复上一层 */
    t->from = NULL;
}
/* 之后 binder_free_transaction(in_reply_to)(L2349):置 buffer->transaction=NULL,
 * 释放内核事务对象 —— 注意此时缓冲还没还,等目标进程 BC_FREE_BUFFER */
```

4. **归还缓冲**:使用方(通常是服务端处理完请求后、或客户端读完 reply 后)发 `BC_FREE_BUFFER`(L4089):

```c
case BC_FREE_BUFFER: {
    binder_uintptr_t data_ptr;
    struct binder_buffer *buffer;
    if (get_user(data_ptr, (binder_uintptr_t __user *)ptr)) return -EFAULT;
    ptr += sizeof(binder_uintptr_t);

    buffer = binder_alloc_prepare_to_free(&proc->alloc, data_ptr); /* 按 user_data 找缓冲 */
    ...
    binder_inner_proc_lock(proc);
    if (buffer->transaction) {              /* 切断缓冲↔事务的互指 */
        buffer->transaction->buffer = NULL;
        buffer->transaction = NULL;
    }
    binder_inner_proc_unlock(proc);
    if (buffer->async_transaction && buffer->target_node) {
        /* oneway 缓冲归还时,若 node 的 async_todo 还排着队 → 挪一个出来继续跑 */
        ... binder_enqueue_work_ilocked(w, &proc->todo); binder_wakeup_proc_ilocked(proc);
    }
    binder_transaction_buffer_release(proc, buffer, 0, false); /* 释放翻译时加的引用/fd */
    binder_alloc_free_buf(&proc->alloc, buffer);               /* 归还空闲树 + 可选释放物理页 */
    break;
}
```

`binder_alloc_free_buf` → `binder_free_buf_locked`(binder_alloc.c L626):async 配额加回、相邻空闲块合并、页若独享则 `binder_update_page_range(alloc, 0, ...)` 还物理页(进 LRU 缓存,非立即释放)。

### 7.12 全链路代码地图(行号速查)

| 阶段 | 函数(行号) | 干了什么 |
|---|---|---|
| 入口 | `binder_ioctl`(binder.c 5291)→ `binder_ioctl_write_read`(5117) | ioctl 分发;读写拆包 |
| 命令循环 | `binder_thread_write`(3912)→ `BC_TRANSACTION`(4161) | 逐条执行 BC_ |
| 主流程 | `binder_transaction`(3201) | 定位目标→分配缓冲→拷贝→翻译→入队 |
| 分配 | `binder_alloc_new_buf`→`_locked`(alloc.c 552/378)→`binder_update_page_range`(187) | 目标进程映射区 best-fit + 逐页补页 |
| 拷贝 | `binder_alloc_copy_user_to_buffer`(alloc.c 1169) | 发送方用户态→目标缓冲(逐页 kmap) |
| 翻译 | `binder_translate_binder/handle/fd/fd_array`(2791/2843/2911/2968)、`binder_inc_ref_for_node`(2206) | 对象改写 + fd dup |
| 入队唤醒 | `binder_proc_transaction`(3108)→`binder_select_thread_ilocked`(1236)→`binder_wakeup_thread_ilocked`(1267) | 挑线程、入 todo、唤醒 |
| 接收 | `binder_thread_read`(4490)→`binder_wait_for_work`(4459) | 睡眠→出队→填 BR_TRANSACTION→拷贝描述 |
| 弹栈 | `binder_pop_transaction_ilocked`(2237)、`binder_free_transaction`(2349) | 同步配对回收 |
| 归还 | `BC_FREE_BUFFER`(4089)→`binder_alloc_free_buf`(alloc.c 689) | 缓冲区回到空闲树 |

---

## 8. binder_transaction 全函数结构总览(L3201-3910,约 710 行)

前面第 7 章按"调用链"横向切开了 binder_transaction;本节把整个函数**纵向摊开**,让你看到一次事务在驱动里从进入到退出(含所有错误路径)的完整骨架。

### 8.1 函数分段一览(正常路径)

```c
static void binder_transaction(struct binder_proc *proc,
                               struct binder_thread *thread,
                               struct binder_transaction_data *tr, int reply,
                               binder_size_t extra_buffers_size)
{
    /* ========== 段 A:L3201-3236 准备 ========== */
    /* 局部变量:target_proc/target_thread/target_node、in_reply_to、
     * return_error(出错码)/return_error_param/return_error_line(出错定位)、
     * 各 offset 游标(数据区/偏移区/sg 区)、secctx 指针
     * → 先把事务记进内核环形日志 binder_transaction_log(出错时便于 dumpsys 排查) */

    /* ========== 段 B:L3237-3411 定位目标 ========== */
    if (reply) {
        /* BC_REPLY:① 取本线程 transaction_stack 栈顶 = in_reply_to(必须存在且指向本线程)
         *          ② 弹栈(thread->transaction_stack = in_reply_to->to_parent)
         *          ③ binder_get_txn_from_and_acq_inner 取回发起线程并 +tmp_ref 保活
         *          ④ 校验发起线程的栈顶仍是 in_reply_to(防乱序)
         *          ⑤ target_proc = 发起线程的 proc */
    } else {
        /* BC_TRANSACTION:handle>0 → ref → node → target_proc(handle==0 → context mgr;
         * CONFIG_DRV_NS 下 handle>=100000000 → 其它 cell 的 context mgr)
         * SELinux 校验 security_binder_transaction()
         * 同步且本线程有事务栈 → 沿 from_parent 找"目标进程里正在等回复的线程",
         *   把 target_thread 定为它(保证嵌套调用发回同一线程) */
    }
    /* 收尾:记录 e->to_proc/e->to_thread;解 tmpref */

    /* ========== 段 C:L3414-3431 分配内核对象 ========== */
    t = kzalloc(sizeof(*t), GFP_KERNEL);          /* binder_transaction(核心对象) */
    tcomplete = kzalloc(sizeof(*tcomplete), GFP_KERNEL); /* binder_work(受理通知) */

    /* ========== 段 D:L3433-3496 填元信息 ========== */
    /* debug_id、from(仅同步非 oneway)、sender_euid、to_proc/to_thread、
     * code/flags、优先级继承/目标默认优先级;
     * 目标 node 要求安全上下文时:security_task_getsecid → 取 secctx 追加进 extra 区 */

    /* ========== 段 E:L3498-3528 分配目标进程缓冲 ========== */
    t->buffer = binder_alloc_new_buf(&target_proc->alloc, ...);  /* ★ 见 7.6 */
    if (secctx) binder_alloc_copy_to_buffer(..., secctx);        /* 安全上下文拷入 */
    t->buffer->transaction = t;  t->buffer->target_node = target_node;

    /* ========== 段 F:L3530-3560 拷入用户数据 ========== */
    /* copy data → 偏移 0;copy offsets → data 之后(见 7.7) */
    /* 大小/对齐校验(offsets_size % 8、extra_buffers_size % 8) */

    /* ========== 段 G:L3578-3777 逐对象翻译 ========== */
    /* 沿偏移数组循环:get_object → switch(type) →
     *   translate_binder / translate_handle / translate_fd /
     *   translate_fd_array / BINDER_TYPE_PTR 内嵌缓冲拷贝+指针修复(见 7.8) */

    /* ========== 段 H:L3778-3834 收尾入队(见 7.9) ========== */
    if (reply)        { /* complete 发给发送方;事务入发起线程并同步唤醒;free in_reply_to */ }
    else if (同步)     { /* complete 延迟;本线程压栈;binder_proc_transaction 投递 */ }
    else /* oneway */ { /* complete 立即发;binder_proc_transaction(thread=NULL) 投递 */ }
    /* 解 tmpref、smp_wmb + 写日志 debug_id_done */
    return;
```

### 8.2 错误处理骨架(同一函数内的回滚路径)

binder_transaction 用了 **"一处收尾、多个 goto 标签"** 的错误架构:任何一个失败点 `goto err_xxx`,最终都汇聚到函数尾部的统一清理块:

```c
    /* ========== 统一错误收尾 L3836-3910 ========== */
    err_dead_proc_or_thread:
    return_error = BR_DEAD_REPLY;
    binder_dequeue_work(proc, tcomplete);        /* 撤销已入队的 complete 通知 */
    err_translate_failed:
    err_bad_object_type:
    err_bad_offset:
    err_bad_parent:
    err_copy_data_failed:
    /* ★ ① 回滚翻译副作用:释放已建立的对象引用/dup 的 fd */
    binder_transaction_buffer_release(target_proc, t->buffer,
                                      buffer_offset, true);   /* is_failure=true:
                                      * 只回滚到失败位置 buffer_offset 为止 */
    if (target_node) binder_dec_node_tmpref(target_node);
    target_node = NULL;
    t->buffer->transaction = NULL;
    /* ② 归还目标进程里已分配的缓冲 */
    binder_alloc_free_buf(&target_proc->alloc, t->buffer);
    err_binder_alloc_buf_failed:
    err_bad_extra_size:
    if (secctx) security_release_secctx(secctx, secctx_sz);
    err_get_secctx_failed:
    kfree(tcomplete);                           /* ③ 释放内核对象 */
    binder_stats_deleted(BINDER_STAT_TRANSACTION_COMPLETE);
    err_alloc_tcomplete_failed:
    kfree(t);
    binder_stats_deleted(BINDER_STAT_TRANSACTION);
    err_alloc_t_failed:
    err_bad_call_stack:
    err_empty_call_stack:
    err_dead_binder:
    err_invalid_target_handle:
    if (target_thread) binder_thread_dec_tmpref(target_thread);  /* ④ 解保活引用 */
    if (target_proc)   binder_proc_dec_tmpref(target_proc);
    if (target_node)   { binder_dec_node(target_node, 1, 0); binder_dec_node_tmpref(target_node); }
    /* 写失败日志(failed 环形日志)+ 设置线程 return_error */

    BUG_ON(thread->return_error.cmd != BR_OK);
    if (in_reply_to) {
        /* reply 路径失败:不仅要报错,还得把栈上等回复的事务逐级“送失败回复” */
        binder_restore_priority(current, in_reply_to->saved_priority);
        thread->return_error.cmd = BR_TRANSACTION_COMPLETE;
        binder_enqueue_thread_work(thread, &thread->return_error.work);
        binder_send_failed_reply(in_reply_to, return_error);   /* ★ 见 8.3 */
    } else {
        thread->return_error.cmd = return_error;  /* 非 reply:错误直接回给当前线程 */
        binder_enqueue_thread_work(thread, &thread->return_error.work);
    }
}
```

**各 goto 标签含义速查**(全部指向上面的清理块):

| 标签(失败点) | 触发场景 |
|---|---|
| `err_empty_call_stack` | reply 时本线程事务栈为空(没有可回复的事务) |
| `err_bad_call_stack` | 事务栈顶不属于本线程 / 新事务破坏了栈结构 |
| `err_dead_binder` | 目标 handle 无效、目标 node 已死(BR_DEAD_REPLY) |
| `err_invalid_target_handle` | SELinux 拒绝 / 自呼叫 context mgr / handle 非法 |
| `err_alloc_t_failed` / `err_alloc_tcomplete_failed` | kzalloc 失败(内存不足) |
| `err_get_secctx_failed` / `err_bad_extra_size` | 安全上下文获取失败 / extra 大小溢出 |
| `err_binder_alloc_buf_failed` | 目标进程缓冲分配失败(-ESRCH → BR_DEAD_REPLY) |
| `err_copy_data_failed` | 拷贝用户数据/内嵌缓冲失败(-EFAULT) |
| `err_bad_offset` / `err_bad_parent` | 偏移数组非法 / parent 校验失败 |
| `err_translate_failed` / `err_bad_object_type` | 对象翻译失败 / 未知对象类型 |
| `err_dead_proc_or_thread` | 入队时发现目标进程/线程已死 |

> 错误处理的设计要点:**先回滚副作用(引用/fd)、再还缓冲、再释放内核对象、最后处理"等回复的栈"**,顺序严格,保证任何失败点退出后内核状态一致、不留悬挂事务。

### 8.3 错误回滚链详解(三个关键辅助函数)

错误收尾块调用三个函数完成清理,逐个展开:

**① `binder_transaction_buffer_release`(L2629)—— 回滚"翻译副作用"**

```c
static void binder_transaction_buffer_release(struct binder_proc *proc,
                                              struct binder_buffer *buffer,
                                              binder_size_t failed_at,
                                              bool is_failure) {
    if (buffer->target_node)
        binder_dec_node(buffer->target_node, 1, 0);   /* 归还事务对 node 的强引用 */

    off_start_offset = ALIGN(buffer->data_size, sizeof(void *));
    /* is_failure 时只回滚到失败点 failed_at 为止(之前翻译成功的才需要撤销) */
    off_end_offset = is_failure ? failed_at :
                     off_start_offset + buffer->offsets_size;
    for (buffer_offset = off_start_offset; buffer_offset < off_end_offset;
         buffer_offset += sizeof(binder_size_t)) {
        ... /* 取对象头 → 按类型撤销翻译时做的工作 */
        switch (hdr->type) {
        case BINDER_TYPE_BINDER / WEAK_BINDER:
            /* 撤销 translate_binder:发送方 node 计数 -1 */
            binder_dec_node(node, strong, 0); binder_put_node(node); break;
        case BINDER_TYPE_HANDLE / WEAK_HANDLE:
            /* 撤销 translate_handle:目标进程 ref 计数 -1 */
            binder_dec_ref_for_handle(proc, fp->handle, strong, &rdata); break;
        case BINDER_TYPE_FD:
            /* 撤销 translate_fd:关掉 dup 出来的 fd */
            if (failed_at) task_close_fd(proc, fp->fd); break;
        case BINDER_TYPE_FDA:
            /* 撤销 fd 数组:逐个 task_close_fd(按 fda->parent_offset 定位) */ ...
        case BINDER_TYPE_PTR:
            /* 内嵌缓冲无副作用,随整块缓冲释放而清理 */ break;
        }
    }
    ...
}
```

> 作用:翻译阶段每成功一个对象都"欠"了一笔账(新建的 ref、dup 的 fd、node 计数)。出错时若不撤销,目标进程会**泄漏句柄引用和 fd**——这是 binder 内存泄漏/句柄耗尽类 bug 的根源,所以回滚必须精确到 `failed_at` 位置。

**② `binder_cleanup_transaction`(L2436)—— 未投递事务的清理入口**

```c
static void binder_cleanup_transaction(struct binder_transaction *t,
                                       const char *reason,
                                       uint32_t error_code) {
    if (t->buffer->target_node && !(t->flags & TF_ONE_WAY)) {
        /* 同步事务且目标还存在 → 必须给发起线程一个"失败回复",
         * 否则发起线程会永远阻塞在等 reply 上 */
        binder_send_failed_reply(t, error_code);
    } else {
        /* oneway 或目标已死 → 直接释放事务对象 */
        binder_free_transaction(t);
    }
}
```

调用点:目标进程死亡时 `binder_release_work` 对滞留事务逐个 cleanup;`binder_thread_read` 中 put_user/copy_to_user 失败也会 cleanup。

**③ `binder_send_failed_reply`(L2374)—— 沿事务栈逐级回送失败回复**

```c
static void binder_send_failed_reply(struct binder_transaction *t,
                                     uint32_t error_code) {
    BUG_ON(t->flags & TF_ONE_WAY);
    while (1) {                                    /* 循环:可能嵌套多层同步调用 */
        target_thread = binder_get_txn_from_and_acq_inner(t); /* 取发起线程 */
        if (target_thread) {
            binder_pop_transaction_ilocked(target_thread, t); /* 弹掉它栈顶 */
            if (target_thread->reply_error.cmd == BR_OK) {
                /* 把失败码塞进线程的 reply_error 工作项 */
                target_thread->reply_error.cmd = error_code;
                binder_enqueue_thread_work_ilocked(
                        target_thread, &target_thread->reply_error.work);
                wake_up_interruptible(&target_thread->wait);  /* ★ 唤醒等回复的线程 */
            }
            binder_inner_proc_unlock(target_thread->proc);
            binder_thread_dec_tmpref(target_thread);
            binder_free_transaction(t);            /* 释放本层事务 */
            return;
        }
        /* 发起线程也死了 → 沿 from_parent 向上处理上一层嵌套调用 */
        next = t->from_parent;
        binder_free_transaction(t);
        if (next == NULL) return;                  /* 到栈底,结束 */
        t = next;                                  /* 继续向上回送失败 */
    }
}
```

> 关键场景:**服务端进程崩溃/被杀**时,它栈上所有"等它回复"的同步调用都必须被失败唤醒,否则所有客户端会永久卡死。`binder_send_failed_reply` 正是这个"失败广播"——被唤醒的发起线程在 read 阶段收到 `BR_FAILED_REPLY` / `BR_DEAD_REPLY`,`transact()` 立刻抛 `DeadObjectException` 等错误返回,不会死等。

---

## 9. 其他配套机制(线程/服务注册/引用计数/死亡通知)

### 9.1 线程登记:binder_get_thread(L4931-4989)

驱动需要知道"当前 ioctl 来自进程的哪个线程",才能把事务精确投递给线程、管理线程池:

```c
/* 无锁版本:在 proc->threads 红黑树(按线程 pid 排序)里二分查找 */
static struct binder_thread *binder_get_thread_ilocked(
        struct binder_proc *proc, struct binder_thread *new_thread) {
    struct rb_node **p = &proc->threads.rb_node;
    while (*p) {
        thread = rb_entry(parent, struct binder_thread, rb_node);
        if (current->pid < thread->pid) p = &(*p)->rb_left;
        else if (current->pid > thread->pid) p = &(*p)->rb_right;
        else return thread;                  /* 找到了:老线程 */
    }
    if (!new_thread) return NULL;            /* 没有且没给新对象 → 未登记 */
    /* 找不到 → 用传入的 new_thread 完成初始化并插入红黑树 */
    thread = new_thread;
    thread->proc = proc;
    thread->pid = current->pid;
    get_task_struct(current);
    thread->task = current;                  /* 记住 task_struct,用于唤醒/优先级 */
    atomic_set(&thread->tmp_ref, 0);
    init_waitqueue_head(&thread->wait);      /* ★ 线程睡眠/唤醒的等待队列 */
    INIT_LIST_HEAD(&thread->todo);           /* ★ 本线程的事务队列 */
    rb_link_node(&thread->rb_node, parent, p);
    rb_insert_color(&thread->rb_node, &proc->threads);
    thread->looper_need_return = true;       /* 首轮 ioctl 后需要先回用户态 */
    thread->return_error.work.type = BINDER_WORK_RETURN_ERROR;
    thread->return_error.cmd = BR_OK;        /* 每线程一个 return_error 槽位 */
    thread->reply_error.work.type = BINDER_WORK_RETURN_ERROR;
    thread->reply_error.cmd = BR_OK;         /* 每线程一个 reply_error 槽位 */
    INIT_LIST_HEAD(&new_thread->waiting_thread_node); /* 空闲线程链表节点 */
    return thread;
}

static struct binder_thread *binder_get_thread(struct binder_proc *proc) {
    /* 先查:命中直接返回;未命中:分配新 binder_thread 再插入 */
    binder_inner_proc_lock(proc);
    thread = binder_get_thread_ilocked(proc, NULL);
    binder_inner_proc_unlock(proc);
    if (!thread) {
        new_thread = kzalloc(sizeof(*thread), GFP_KERNEL);
        ...
        thread = binder_get_thread_ilocked(proc, new_thread);
        if (thread != new_thread) kfree(new_thread);  /* 并发:别人先插入了 */
    }
    return thread;
}
```

> binder_thread 是"每线程一套"的关键状态:**todo 队列**(只属于该线程的工作)、**wait 等待队列**(睡眠位置)、**transaction_stack**(该线程发起的同步调用栈)、**两个错误槽位**(return_error 是写命令时的错误、reply_error 是失败回复)。这也是 Binder 能"精确唤醒某一线程"的根基。

### 9.2 服务注册:context manager(谁拥有 handle 0)

Binder 的"服务发现"极其朴素:**第一个调用 `BINDER_SET_CONTEXT_MGR` 的进程成为 context manager(0 号服务总管)**——ServiceManager 就是这样注册的。客户端"找服务"实际是向它发一个 `BC_TRANSACTION(handle=0)` 查询。

```c
static int binder_ioctl_set_ctx_mgr(struct file *filp,
                                    struct flat_binder_object *fbo) {
    struct binder_context *context = proc->context;
    struct binder_node *new_node;

    mutex_lock(&context->context_mgr_node_lock);
    if (context->binder_context_mgr_node) {      /* 每 context 只能有一个总管 */
        ret = -EBUSY; goto out;
    }
    ret = security_binder_set_context_mgr(proc->tsk);  /* SELinux 校验 */
    if (ret < 0) goto out;
    if (uid_valid(context->binder_context_mgr_uid)) {
        if (!uid_eq(context->binder_context_mgr_uid, curr_euid)) {
            ret = -EPERM; goto out;              /* 换 uid 抢注 → 拒绝 */
        }
    } else {
        context->binder_context_mgr_uid = curr_euid; /* 首注者记录 uid */
    }
    new_node = binder_new_node(proc, fbo);       /* 为其建 node(0 号对象) */
    ...
    new_node->local_weak_refs++;
    new_node->local_strong_refs++;               /* 内核替总管持强/弱引用,防被回收 */
    new_node->has_strong_ref = 1;
    new_node->has_weak_ref = 1;
    context->binder_context_mgr_node = new_node; /* ★ 挂到 context 上 */
    ...
}
```

> 结合 5 节:cells 定制的多 context(`proc->acontext[]`)让每个虚拟 cell 拥有**自己的 0 号总管**,宿主用 `handle >= INIT_OTHER_CONTEXT_MGR_HANDLE` 定向打到某 cell 的总管——本质是"给每个 cell 一个独立的服务注册表"。

### 9.3 引用计数命令(BC_INCREFS/ACQUIRE/RELEASE/DECREFS,L3939-4013)

Binder 对象跨进程共享靠内核维护的引用计数,用户态通过四条命令增减(驱动把"计数变化"翻译成 node 或 ref 的引用):

```c
case BC_INCREFS: case BC_ACQUIRE:
case BC_RELEASE: case BC_DECREFS: {
    uint32_t target;
    bool strong = cmd == BC_ACQUIRE || cmd == BC_RELEASE;   /* ACQUIRE/RELEASE 是强引用 */
    bool increment = cmd == BC_INCREFS || cmd == BC_ACQUIRE;

    get_user(target, ...);                    /* 目标:handle */
    ret = -1;
    /* (cells 定制)handle>=100000000 → 其它 cell 的 context manager 节点 */
    ...
    if (increment && !target) {
        /* handle==0:对 context manager 增引用(绑定 ServiceManager) */
        mutex_lock(&context->context_mgr_node_lock);
        ctx_mgr_node = context->binder_context_mgr_node;
        if (ctx_mgr_node)
            ret = binder_inc_ref_for_node(proc, ctx_mgr_node, strong, NULL, &rdata);
        mutex_unlock(&context->context_mgr_node_lock);
    }
    if (ret)
        ret = binder_update_ref_for_handle(proc, target, increment, strong, &rdata);
    /* ↑ 普通 handle:在 proc 的 ref 上增减计数;
     *   计数归零时驱动会回收 ref,并向 node 宿主回发释放通知 */
    ...
}
```

用户态侧这些命令的触发方是 `IPCThreadState` 的 `incStrongHandle/decStrongHandle`(如 Java 层 `BinderProxy` 销毁、`ServiceManager` 获取服务后 ACQUIRE 一次)。计数归零语义:**ref 归零 → 客户端失去该句柄;node 无任何 ref 且无本地引用 → node 被释放,并向所有注册过死亡通知的客户端广播 BR_DEAD_BINDER**(见 9.4)。

### 9.4 死亡通知:linkToDeath 的内核实现(L4211-4360)

客户端 `binderDied()` 回调的背后是这套流程:

```c
case BC_REQUEST_DEATH_NOTIFICATION:
case BC_CLEAR_DEATH_NOTIFICATION: {
    /* 参数:handle(target)+cookie(用户态回调标识) */
    get_user(target, ...); get_user(cookie, ...);
    if (cmd == BC_REQUEST_DEATH_NOTIFICATION) {
        death = kzalloc(sizeof(*death), GFP_KERNEL);   /* 预分配监视记录 */
        if (death == NULL) { ...塞 BR_ERROR; break; }
    }
    binder_proc_lock(proc);
    ref = binder_get_ref_olocked(proc, target, false); /* 找到要监视的句柄 */
    if (ref == NULL) { ...; break; }

    binder_node_lock(ref->node);
    if (cmd == BC_REQUEST_DEATH_NOTIFICATION) {
        if (ref->death) { ...; break; }                /* 一个 ref 只能监视一次 */
        death->cookie = cookie;
        ref->death = death;                            /* 挂到 ref 上 */
        if (ref->node->proc == NULL) {
            /* ★ 目标 node 已死 → 立即投递 BR_DEAD_BINDER */
            ref->death->work.type = BINDER_WORK_DEAD_BINDER;
            binder_enqueue_work_ilocked(&ref->death->work, &proc->todo);
            binder_wakeup_proc_ilocked(proc);
        }
    } else { /* BC_CLEAR_DEATH_NOTIFICATION:撤销监视 */
        death = ref->death;
        if (death->cookie != cookie) { ...; break; }   /* cookie 必须匹配 */
        ref->death = NULL;
        if (list_empty(&death->work.entry)) {
            /* 监视还挂着但没触发过 → 干净撤销 */
            death->work.type = BINDER_WORK_CLEAR_DEATH_NOTIFICATION;
            binder_enqueue_work_ilocked(..., &death->work); ...
        } else {
            /* 已经触发过 BR_DEAD_BINDER 但用户态还没 BC_DEAD_BINDER_DONE 确认
             * → 标记成 DEAD_BINDER_AND_CLEAR,等确认时一并收尾 */
            BUG_ON(death->work.type != BINDER_WORK_DEAD_BINDER);
            death->work.type = BINDER_WORK_DEAD_BINDER_AND_CLEAR;
        }
    }
    ...
}
```

当 node 最终被释放(`binder_node_release` L5663)时,驱动遍历所有引用该 node 的 ref:对每个设了 `ref->death` 的进程入队 `BINDER_WORK_DEAD_BINDER`;目标线程 read 阶段收 `BR_DEAD_BINDER`(L4706-4747)→ 用户态回调 `binderDied` → 回发 `BC_DEAD_BINDER_DONE`(L4337)确认,驱动才释放 death 记录。

### 9.5 线程池:looper 注册与 BR_SPAWN_LOOPER 扩容(L4173-4210)

一个进程能同时处理多少 binder 事务,由"进了 looper 的线程数"决定。用户态线程用三条命令告诉驱动自己的状态:

```c
case BC_REGISTER_LOOPER:                 /* 由驱动要求(收到 BR_SPAWN_LOOPER)新开的线程 */
    if (thread->looper & BINDER_LOOPER_STATE_ENTERED) {
        thread->looper |= BINDER_LOOPER_STATE_INVALID;   /* 重复进入 → 标记非法 */
        ...
    } else if (proc->requested_threads == 0) {
        thread->looper |= BINDER_LOOPER_STATE_INVALID;   /* 没被要求就自己来 → 非法 */
        ...
    } else {
        proc->requested_threads--;              /* 消化一次"扩招名额" */
        proc->requested_threads_started++;
    }
    thread->looper |= BINDER_LOOPER_STATE_REGISTERED;    /* 标记为已注册 looper */
    break;

case BC_ENTER_LOOPER:                      /* 主线程(或池中线程)主动进入循环 */
    if (thread->looper & BINDER_LOOPER_STATE_REGISTERED) {
        thread->looper |= BINDER_LOOPER_STATE_INVALID;   /* 两种进入方式互斥 */
        ...
    }
    thread->looper |= BINDER_LOOPER_STATE_ENTERED;
    break;

case BC_EXIT_LOOPER:                       /* 退出循环 */
    thread->looper |= BINDER_LOOPER_STATE_EXITED;
    break;
```

**扩容逻辑**在 `binder_thread_read` 收尾处(L4852-4868):一次 read 处理完工作后,如果目标进程**一个空闲线程都没有**(`waiting_threads` 空)、已启动线程数还没到 `max_threads`、且当前线程是合格 looper,驱动就在命令流末尾追加一条 `BR_SPAWN_LOOPER`:

```c
done:
    *consumed = ptr - buffer;
    binder_inner_proc_lock(proc);
    if (proc->requested_threads == 0 &&
        list_empty(&thread->proc->waiting_threads) &&      /* 无空闲线程 */
        proc->requested_threads_started < proc->max_threads && /* 未达上限 */
        (thread->looper & (BINDER_LOOPER_STATE_REGISTERED | BINDER_LOOPER_STATE_ENTERED))) {
        proc->requested_threads++;                         /* 记一笔"扩招名额" */
        binder_inner_proc_unlock(proc);
        if (put_user(BR_SPAWN_LOOPER, (uint32_t __user *)buffer)) return -EFAULT;
        ...
    } else
        binder_inner_proc_unlock(proc);
```

用户态收到 `BR_SPAWN_LOOPER` 后 `pthread_create` 新线程 → 新线程进 `joinThreadPool` → 先发 `BC_REGISTER_LOOPER`(消费名额)再循环 read —— 这就是 binder 线程池**按需自动扩容**的内核驱动逻辑(上限由 `BINDER_SET_MAX_THREADS` 设定,一般 15/16)。配合 7.10 的 `binder_select_thread_ilocked`:新事务到达时从 `waiting_threads` 队头挑空闲线程;挑不到就入 `proc->todo` 并依赖这次扩容机制产生新线程。

### 9.6 用户态调用链与 BC_/BR_ 命令的对应(衔接第 7.0 节)

| 用户态动作(概念) | 写出的 BC_ | 内核动作 | 收回的 BR_ |
|---|---|---|---|
| `BpBinder::transact()` 发起同步调用 | `BC_TRANSACTION` | 7.4-7.9 全流程 | `BR_TRANSACTION_COMPLETE` → 等待 `BR_REPLY` |
| 服务端 `BBinder` 处理完 `onTransact` 后回复 | `BC_REPLY` | 反向 binder_transaction(reply=1) | `BR_TRANSACTION_COMPLETE` |
| oneway 调用(`FLAG_ONEWAY`) | `BC_TRANSACTION`+TF_ONE_WAY | 不压同步栈、直接投递 | `BR_TRANSACTION_COMPLETE`(立即) |
| 用完收到的 Parcel | `BC_FREE_BUFFER` | 归还缓冲(见 7.11) | — |
| `ProcessState::self()` 后主线程 `joinThreadPool` | `BC_ENTER_LOOPER` | 登记 looper(9.5) | `BR_TRANSACTION`… |
| 池中线程进入循环 | `BC_REGISTER_LOOPER` | 消化扩招名额 | `BR_SPAWN_LOOPER`(需要时) |
| `linkToDeath(recipient, cookie)` | `BC_REQUEST_DEATH_NOTIFICATION` | 挂 ref->death(9.4) | `BR_DEAD_BINDER` → 回调后回 `BC_DEAD_BINDER_DONE` |
| `unlinkToDeath` | `BC_CLEAR_DEATH_NOTIFICATION` | 撤销监视 | `BR_CLEAR_DEATH_NOTIFICATION_DONE` |
| Java 层 `BinderProxy` 生命周期 | `BC_ACQUIRE`/`BC_RELEASE` 等 | 引用计数(9.3) | `BR_ACQUIRE`/`BR_RELEASE` 等(node 侧通知) |
| `ServiceManager.addService()` 的注册 | `BINDER_SET_CONTEXT_MGR` ioctl | context mgr 登记(9.2) | — |

> 内存记账惯例:每次 ioctl 后用户态检查 `bwr.write_consumed/read_consumed`;若命令流没消费完(如 read 缓冲已满),**用户态会立刻用剩余空间再发一次 ioctl 续传**,直到命令流清空——这就是 `binder_ioctl_write_read` 返回后用户态循环的由来,保证内核入队的工作最终都能被取走。









