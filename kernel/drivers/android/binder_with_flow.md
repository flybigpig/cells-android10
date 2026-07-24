I'll help you understand how Binder passes data structures with a tracked example. Let me first
explore the codebase and load the relevant Binder skill.
The `binder.md` already gives an excellent overview. The user wants a concrete **data structure
passing trace example**. Let me read the actual structure definitions and the object-translation
code in `binder.c` to build a real, line-referenced trace.
Let me find the actual struct definitions and read the key transaction/translation code sections.
The structs live in the UAPI header. Let me read it along with the key
transaction/object-translation code in `binder.c`.
Now let me read the core `binder_transaction` object-translation and the read-path code, plus the
BC_TRANSACTION entry.
Let me read the write-path entry, the data-buffer copy section, and the read-path `BR_TRANSACTION`
construction to complete the end-to-end trace.
Let me read the buffer-allocation and data-copy portion of `binder_transaction` to complete the
trace.
Let me read the section right after the data copy to confirm how the offsets array is copied into
the target buffer.
我已经把本仓库里 Binder 驱动的数据结构定义和事务流转代码都读完了。下面用一个**完整的端到端跟踪示例**
,演示"一个数据结构如何跨进程传递"。所有引用都对应你 `cells-android10` 仓库里的真实代码。

---

## 一、涉及的核心数据结构(演员表)

一次 Binder 调用能"传递数据结构",靠的是一套**信封 + 内嵌对象 + 偏移表**
的组合。关键定义都在 `kernel/include/uapi/linux/android/binder.h`:

信封: `binder_transaction_data`(每个事务一份,描述"发给谁、什么 code、数据在哪")

```299:332:kernel/include/uapi/linux/android/binder.h
struct binder_transaction_data {
	union {
		__u32	handle;		  /* 目标 handle(BC_TRANSACTION 用) */
		binder_uintptr_t ptr;	  /* 回复时定位原线程 */
	} target;
	
	__u32		code;		  /* 事务命令(对应 AIDL 方法号) */
	__u32	        flags;
	pid_t		sender_pid;
	uid_t		sender_euid;
	binder_size_t	data_size;	  /* 数据字节数 */
	binder_size_t	offsets_size;	  /* 偏移表字节数 */
	union {
		struct {
			binder_uintptr_t buffer;  /* 数据区(用户态指针) */
			binder_uintptr_t offsets; /* 偏移表(用户态指针) */
		} ptr;
		__u8	buf[8];
	} data;
};
```

内嵌对象(可以塞进数据区里的"特殊类型"):这些不是在 `binder_transaction_data` 里,而是**内联在数据 blob
中**,由偏移表定位。

```112:135:kernel/include/uapi/linux/android/binder.h
struct binder_object_header { __u32 type; };   // 所有对象的公共头
struct flat_binder_object {   // 一个 Binder 实体/引用(传 IBinder 时用)
	struct binder_object_header hdr;
	__u32 flags;
	union { binder_uintptr_t binder; __u32 handle; }; // 本地 ptr 或 远端 handle
	binder_uintptr_t cookie;
};
struct binder_fd_object { ... __u32 fd; ... };          // 传文件描述符
struct binder_buffer_object { ... buffer; length; ... };// 传一段额外内存(PTR)
```

偏移表:就是一串 `binder_size_t`(本仓库 64 位下是 8 字节)的数组,每个值是"数据 blob
里第几个字节处内嵌了一个对象"。驱动靠它知道数据区里哪些字节需要"翻译",哪些只是普通数据。

内核侧记账结构(`binder.c`):`binder_proc`(每进程)、`binder_thread`(每线程)、`binder_node`(
服务端实体,带 `ptr/cookie`)、`binder_ref`(客户端引用,`desc` 即 handle)、`binder_buffer`(
目标进程里分配的内核缓冲区)。

---

## 二、一个具体示例(跟踪剧本)

假设客户端 Client(pid=A) 要调用服务端 Server(pid=B) 的方法:
`doWork(int code, String msg, IBinder callback)`

客户端把参数塞进一个 `Parcel`,序列化后数据区大概长这样(data_size = N,offsets_size = 8):

```
数据 blob:
  [0..3]   int code = 7
  [4..K]   String msg 的内联字节
  [P..P+23] flat_binder_object { type=BINDER_TYPE_BINDER,
                                 binder=<BBinder 指针>, cookie=<Binder 对象> }
偏移表 offsets[0] = P   (即上面那个对象在第 P 字节处)
```

### 步骤 1 — 客户端下发 BC_TRANSACTION

`IPCThreadState` 把信封和 `Parcel` 的缓冲区指针填进 `binder_transaction_data`
,通过 `ioctl(BINDER_WRITE_READ)` 发出。驱动在 `binder_thread_write` 里收到:

```4161:4170:kernel/drivers/android/binder.c
case BC_TRANSACTION:
case BC_REPLY: {
	struct binder_transaction_data tr;
	if (copy_from_user(&tr, ptr, sizeof(tr)))
		return -EFAULT;
	ptr += sizeof(tr);
	binder_transaction(proc, thread, &tr, cmd == BC_REPLY, 0);
	break;
}
```

### 步骤 2 — 找目标 + 在目标进程分配缓冲区

`binder_transaction`(入口 `binder.c:3201`)按 `handle` 找到目标 `binder_node`(handle==0 是
servicemanager,见 `3337-3346`),然后在**目标进程**的 `binder_alloc` 里分配一块 `binder_buffer`:

```3498:3511:kernel/drivers/android/binder.c
t->buffer = binder_alloc_new_buf(&target_proc->alloc, tr->data_size,
				 tr->offsets_size, extra_buffers_size,
				 !reply && (t->flags & TF_ONE_WAY));
if (IS_ERR(t->buffer)) { ... return_error = BR_FAILED_REPLY; ... }
```

接着把"数据 blob"和"偏移表"各拷贝一次进目标内核缓冲区(这就是"一次拷贝"的来源,因为目标用户态靠 `mmap`
共享这块物理页直接读):

```3530:3537:kernel/drivers/android/binder.c
if (binder_alloc_copy_user_to_buffer(&target_proc->alloc, t->buffer, 0,
	(const void __user *)(uintptr_t) tr->data.ptr.buffer,
	tr->data_size)) { ... }   // 拷贝数据区
// 偏移表单独拷贝到 ALIGN(data_size) 之后
binder_alloc_copy_user_to_buffer(&target_proc->alloc, t->buffer,
	ALIGN(tr->data_size, sizeof(void *)),
	(const void __user *)(uintptr_t) tr->data.ptr.offsets,
	tr->offsets_size);
```

### 步骤 3 — 遍历偏移表,逐个"翻译"内嵌对象(关键)

驱动扫描偏移表,在 `P` 处发现一个 `BINDER_TYPE_BINDER` 对象,调用 `binder_translate_binder`:

```3613:3629:kernel/drivers/android/binder.c
switch (hdr->type) {
case BINDER_TYPE_BINDER:
case BINDER_TYPE_WEAK_BINDER: {
	struct flat_binder_object *fp;
	fp = to_flat_binder_object(hdr);
	ret = binder_translate_binder(fp, t, thread);
	...
	binder_alloc_copy_to_buffer(&target_proc->alloc, t->buffer,
		object_offset, fp, sizeof(*fp)); // 把改写后的对象写回缓冲区
}
```

`binder_translate_binder` 做了三件事:在 Server 进程里为这个实体新建一个 `binder_ref`,然后把**
原对象就地改写**——类型从 `BINDER_TYPE_BINDER` 变成 `BINDER_TYPE_HANDLE`,`binder` 指针清零,填上
Server 侧的新 handle:

```2819:2831:kernel/drivers/android/binder.c
ret = binder_inc_ref_for_node(target_proc, node, ... , &rdata);
if (fp->hdr.type == BINDER_TYPE_BINDER)
	fp->hdr.type = BINDER_TYPE_HANDLE;
else
	fp->hdr.type = BINDER_TYPE_WEAK_HANDLE;
fp->binder = 0;
fp->handle = rdata.desc;   // Server 看到的 handle
fp->cookie = 0;
```

> 这就是"Binder 实体在 A、代理在 B"的本质:`flat_binder_object` 在跨进程时被驱动原地改写成持有对方
> handle 的版本,新 handle 指向内核里新建的 `binder_ref`。

### 步骤 4 — 投递并唤醒目标线程

驱动把 `BINDER_WORK_TRANSACTION` 挂到 Server 线程的 `todo`,唤醒它(`3778-3799`
,目标 `binder_thread_read` 读出来)。

### 步骤 5 — Server 侧收到 BR_TRANSACTION

Server 线程在 `binder_thread_read` 里取出 work,重组成面向用户态的 `binder_transaction_data`
,下发 `BR_TRANSACTION`:

```4578:4583:kernel/drivers/android/binder.c
case BINDER_WORK_TRANSACTION: {
	binder_inner_proc_unlock(proc);
	t = container_of(w, struct binder_transaction, work);
}
```

`IPCThreadState::executeCommand` 收到后调到 `BBinder::onTransact(code, data, reply)`。此时 Server
从数据区读 `int code`、读 `String msg` —— 这两段就是普通字节,拷贝后已可用;再在偏移 `P`
处 `readStrongBinder()`,拿到一个 `BpBinder(handle=H')`,也就是那个 callback 代理。

### 步骤 6 — 回程(可选)

Server 处理完写 `BC_REPLY`,`binder_transaction(reply=1)` 按 `thread->transaction_stack` 找到最初发起调用的
Client 线程,把结果原路送回,Client 收到 `BR_REPLY`(oneway 则不需要)。

---

## 三、偏移表里可能出现的三种"对象类型"翻译

| 对象类型 | 发送方写入 | 驱动翻译后 | 作用 |
|---|---|---|---|
| `BINDER_TYPE_BINDER` / `WEAK_BINDER` | 实体指针 | 改成 `BINDER_TYPE_HANDLE` + 新 handle | 跨进程传 IBinder(实体→代理) |
| `BINDER_TYPE_FD` | 本进程 fd | 在目标进程装一个新 fd(同文件) | 传 Ashmem/GraphicBuffer/文件 |
| `BINDER_TYPE_PTR` | 一段用户内存的指针+长度 | 把该内存数据 copy 进事务缓冲区 | 传大块二进制/AIDL out 参数 |

`FD` 走 `binder_translate_fd`(`binder.c:3650`),`PTR` 走 `binder_alloc_copy_user_to_buffer`
把指针指向的数据复制到缓冲区(`binder.c:3714` 起)。注意 `binder_buffer_object`
还能设 `BINDER_BUFFER_FLAG_HAS_PARENT`,实现"buffer 里嵌 buffer"的嵌套指针修正。

---

## 四、怎么自己跟踪(实用手段)

1. **ftrace / tracepoint**:驱动在 `binder_trace.h` 里定义了 `binder:binder_transaction`
   、`binder_transaction_alloc_buf`、`binder_transaction_node_to_ref`、`binder_transaction_ref_to_ref`
   等事件。步骤 3 那段改写就对应 `binder_transaction_node_to_ref` 的 tracepoint(`binder.c:2833`)。
   ```
   echo 1 > /sys/kernel/debug/tracing/events/binder/enable
   cat /sys/kernel/debug/tracing/trace_pipe
   ```
2. **debugfs 进程视图**:`cat /sys/kernel/debug/binder/proc/<pid>` 能看到该进程的 nodes /
   refs_by_desc / buffers,直接验证"实体→引用"的 handle 映射。
3. **内核日志**:驱动里大量 `binder_debug(BINDER_DEBUG_TRANSACTION, ...)`(如 `3445`、``)会在 `dmesg`
   打出 `BC_TRANSACTION ... size ...` 这类行,前提开了 `BINDER_DEBUG`。
4. **用户态**:`atrace --list` 里选 `binder` 类别做 systrace,看每次 transaction 的耗时与调用栈;Java
   侧 `android.os.Binder` 的 `transact`/`onTransact` 打好断点即可对照驱动行为。

---

需要的话,我可以再深入任意一块:比如 `binder_alloc.c` 里 `binder_alloc_new_buf` 的**空闲块查找与 4MB
上限/1MB 单事务上限**实现(直接关联 `TransactionTooLargeException`),或者 `CONFIG_DRV_NS` 在本仓库做多
cell 隔离时 handle 翻译怎么切 context。你说一个方向,我继续往下挖。

(注:你当前在 ask 模式,我只能做只读分析。若想把上面这份跟踪示例整理成文档落到 `binder.md`,切到 craft
模式我可以直接帮你补进去。)