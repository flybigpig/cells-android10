我来把 Binder 数据传输链路上涉及的**所有方法**逐个列出，并逐参数注释。先精确读取这些函数在你仓库里的真实签名。
下面按**调用顺序**把 Binder 数据传输链路上的所有方法列出，每个方法给出你仓库里的真实签名 + 逐参数注释。签名全部来自 `kernel/drivers/android/binder.c` 与 `binder_alloc.c`。

## 一、ioctl 入口层

### `binder_ioctl_write_read`  (binder.c:5117)
```c
static int binder_ioctl_write_read(struct file *filp,
                                   unsigned int cmd, unsigned long arg,
                                   struct binder_thread *thread)
```
| 参数 | 类型 | 含义 |
|------|------|------|
| `filp` | `struct file *` | `/dev/binder` 的文件对象；`filp->private_data` 即当前 `binder_proc` |
| `cmd` | `unsigned int` | ioctl 命令，此处恒为 `BINDER_WRITE_READ` |
| `arg` | `unsigned long` | 用户态 `struct binder_write_read *` 的地址；先 `copy_from_user` 出 `bwr` |
| `thread` | `struct binder_thread *` | 当前发起 ioctl 的 Binder 线程 |

内部按 `bwr.write_size>0` 调 `binder_thread_write`，`bwr.read_size>0` 调 `binder_thread_read`。

## 二、写路径（发送方）

### `binder_thread_write`  (binder.c:3912)
```c
static int binder_thread_write(struct binder_proc *proc,
                               struct binder_thread *thread,
                               binder_uintptr_t binder_buffer, size_t size,
                               binder_size_t *consumed)
```
| 参数 | 类型 | 含义 |
|------|------|------|
| `proc` | `struct binder_proc *` | 发送方进程 |
| `thread` | `struct binder_thread *` | 发送线程 |
| `binder_buffer` | `binder_uintptr_t` | 用户态命令缓冲区首地址（`bwr.write_buffer`），内含 `BC_*` 命令流 |
| `size` | `size_t` | 命令缓冲区总字节数（`bwr.write_size`） |
| `consumed` | `binder_size_t *` | 出参：已消费字节数（`&bwr.write_consumed`），驱动边解析边推进 |

解析到 `BC_TRANSACTION`/`BC_REPLY` 时 `copy_from_user(&tr, ptr, sizeof(tr))`，再调 `binder_transaction`。

### `binder_transaction`  (binder.c:3201) — 核心
```c
static void binder_transaction(struct binder_proc *proc,
                               struct binder_thread *thread,
                               struct binder_transaction_data *tr, int reply,
                               binder_size_t extra_buffers_size)
```
| 参数 | 类型 | 含义 |
|------|------|------|
| `proc` | `struct binder_proc *` | 发送方进程 |
| `thread` | `struct binder_thread *` | 发送线程 |
| `tr` | `struct binder_transaction_data *` | 事务信封：`target.handle`、`code`、`flags`、`data_size`、`offsets_size`、`data.ptr.buffer/offsets` |
| `reply` | `int` | 0=`BC_TRANSACTION` 请求；1=`BC_REPLY` 回复（决定目标是查 handle 还是弹事务栈） |
| `extra_buffers_size` | `binder_size_t` | `BC_TRANSACTION_SG` 场景下 scatter-gather 额外内存总量，一般为 0 |

## 三、缓冲区分配层（binder_alloc.c）

### `binder_alloc_new_buf`  (binder_alloc.c:552) / `_locked`(:378)
```c
struct binder_buffer *binder_alloc_new_buf(struct binder_alloc *alloc,
                                           size_t data_size,
                                           size_t offsets_size,
                                           size_t extra_buffers_size,
                                           int is_async)
```
| 参数 | 类型 | 含义 |
|------|------|------|
| `alloc` | `struct binder_alloc *` | **目标进程**的 mmap 分配器（`&target_proc->alloc`），不是发送方 |
| `data_size` | `size_t` | Parcel 数据 blob 字节数（`tr->data_size`） |
| `offsets_size` | `size_t` | 偏移表字节数（`tr->offsets_size`，64 位下每项 8 字节） |
| `extra_buffers_size` | `size_t` | SG 额外内存 |
| `is_async` | `int` | 是否 oneway；为真则从 `free_async_space`(池/2) 扣额度 |

返回 `binder_buffer *`；无连续空闲块或异步额度不足时返回 `ERR_PTR(-ENOSPC)`（上抛即 `TransactionTooLargeException`）。

### `binder_alloc_copy_user_to_buffer`  (binder_alloc.c:1170) — 唯一一次跨进程拷贝
```c
unsigned long binder_alloc_copy_user_to_buffer(struct binder_alloc *alloc,
                                 struct binder_buffer *buffer,
                                 binder_size_t buffer_offset,
                                 const void __user *from,
                                 size_t bytes)
```
| 参数 | 类型 | 含义 |
|------|------|------|
| `alloc` | `struct binder_alloc *` | 目标进程分配器 |
| `buffer` | `struct binder_buffer *` | `binder_alloc_new_buf` 分配出的目标缓冲 |
| `buffer_offset` | `binder_size_t` | 写入起点在 buffer 内的偏移（数据区从 0，偏移表从 `ALIGN(data_size,8)`） |
| `from` | `const void __user *` | 发送方用户态源指针（`tr->data.ptr.buffer` 或 `.offsets`） |
| `bytes` | `size_t` | 拷贝字节数 |

内部逐页 `binder_alloc_get_page` + `kmap` + `copy_from_user`——这就是 Binder "一次拷贝"的落点。

### `binder_alloc_copy_to_buffer`  (:1239) / `binder_alloc_copy_from_buffer`  (:1249)
```c
void binder_alloc_copy_to_buffer(struct binder_alloc *alloc, struct binder_buffer *buffer,
                                 binder_size_t buffer_offset, void *src,  size_t bytes);
void binder_alloc_copy_from_buffer(struct binder_alloc *alloc, void *dest,
                                   struct binder_buffer *buffer, binder_size_t buffer_offset, size_t bytes);
```
两者都是**内核↔内核缓冲**拷贝（不跨用户态）：`to_buffer` 把内核 `src` 写进 buffer（如翻译后就地改写对象）；`from_buffer` 从 buffer 读到内核 `dest`（如读偏移表、读对象头）。参数 `src`/`dest` 为内核指针，其余同上。

### `binder_alloc_do_buffer_copy`  (binder_alloc.c:378) — 上面二者的公共实现
```c
static void binder_alloc_do_buffer_copy(struct binder_alloc *alloc, bool to_buffer,
                                        struct binder_buffer *buffer, binder_size_t buffer_offset,
                                        void *ptr, size_t bytes)
```
`to_buffer=true` 为写入、`false` 为读出；`ptr` 是内核侧源/目的；用 `kmap_atomic`+`memcpy` 逐页搬运。

### `binder_alloc_get_page`  (binder_alloc.c:1141)
```c
static struct page *binder_alloc_get_page(struct binder_alloc *alloc, struct binder_buffer *buffer,
                                          binder_size_t buffer_offset, pgoff_t *pgoffp)
```
把 "buffer + 偏移" 换算成物理 `struct page *`，`pgoffp` 出参返回页内偏移，供上面几个拷贝函数逐页操作。

## 四、对象翻译层（偏移表遍历时逐个调用）

### `binder_get_object`  (binder.c:2460)
```c
static size_t binder_get_object(struct binder_proc *proc, struct binder_buffer *buffer,
                                unsigned long offset, struct binder_object *object)
```
| 参数 | 类型 | 含义 |
|------|------|------|
| `proc` | `struct binder_proc *` | 目标进程 |
| `buffer` | `struct binder_buffer *` | 事务缓冲 |
| `offset` | `unsigned long` | 对象在数据 blob 中的字节偏移（从偏移表读出的 `object_offset`） |
| `object` | `struct binder_object *` | 出参：解析后的对象联合体 |

返回对象字节数（按 `hdr->type` 定：BINDER=24、FD、PTR、FDA 各不同）；非法/越界返回 0。

### `binder_translate_binder`  (binder.c:2791)
```c
static int binder_translate_binder(struct flat_binder_object *fp,
                                   struct binder_transaction *t,
                                   struct binder_thread *thread)
```
| 参数 | 类型 | 含义 |
|------|------|------|
| `fp` | `struct flat_binder_object *` | 指向缓冲内该对象；`type=BINDER_TYPE_BINDER`，`fp->binder` 是发送方本地指针 |
| `t` | `struct binder_transaction *` | 当前事务（含 `t->to_proc` 目标进程） |
| `thread` | `struct binder_thread *` | 发送线程（`thread->proc` 为源进程） |

在目标进程建 `binder_ref` 拿 handle，把 `fp` **就地改写**为 `BINDER_TYPE_HANDLE + handle`。

### `binder_translate_handle`  (binder.c:2843)
参数同上，处理 `type=BINDER_TYPE_HANDLE`。若 handle 指向的 node 属主就是目标进程，则**还原回实体** `BINDER_TYPE_BINDER`；否则在目标进程建新 ref。

### `binder_translate_fd`  (binder.c:2911)
```c
static int binder_translate_fd(int fd, struct binder_transaction *t,
                               struct binder_thread *thread,
                               struct binder_transaction *in_reply_to)
```
| 参数 | 类型 | 含义 |
|------|------|------|
| `fd` | `int` | 发送方进程里的源文件描述符 |
| `t` | `struct binder_transaction *` | 当前事务，取 `t->to_proc` 作安装目标 |
| `thread` | `struct binder_thread *` | 发送线程 |
| `in_reply_to` | `struct binder_transaction *` | 若是回复路径的原事务，用于出错时定位；请求路径为 NULL |

`fget` 源 fd → 目标进程 `fd_install` 新 fd，两者共享同一 `struct file`。

### `binder_translate_fd_array`  (binder.c:2968)
```c
static int binder_translate_fd_array(struct binder_fd_array_object *fda,
                                     struct binder_buffer_object *parent,
                                     struct binder_transaction *t,
                                     struct binder_thread *thread,
                                     struct binder_transaction *in_reply_to)
```
| 参数 | 类型 | 含义 |
|------|------|------|
| `fda` | `struct binder_fd_array_object *` | fd 数组描述符，含数组内 fd 个数与在 parent 中的偏移 |
| `parent` | `struct binder_buffer_object *` | 承载该 fd 数组的父 PTR 缓冲对象 |
| `t`/`thread`/`in_reply_to` | 同 `binder_translate_fd` | 逐个对数组里每个 fd 调翻译 |

## 五、投递与读路径（接收方）

### `binder_proc_transaction`  (binder.c:3108)
```c
static bool binder_proc_transaction(struct binder_transaction *t,
                                    struct binder_proc *proc,
                                    struct binder_thread *thread)
```
| 参数 | 类型 | 含义 |
|------|------|------|
| `t` | `struct binder_transaction *` | 已填好的事务（`t->buffer` 指向目标缓冲） |
| `proc` | `struct binder_proc *` | 目标进程 |
| `thread` | `struct binder_thread *` | 选定的目标线程；为 NULL 则丢进 `proc->todo` 由空闲线程抢 |

把 `t->work` 入 `todo` 并 `binder_wakeup_thread_ilocked` 唤醒；返回是否成功排队。

### `binder_thread_read`  (binder.c:4490)
```c
static int binder_thread_read(struct binder_proc *proc,
                              struct binder_thread *thread,
                              binder_uintptr_t binder_buffer, size_t size,
                              binder_size_t *consumed, int non_block)
```
| 参数 | 类型 | 含义 |
|------|------|------|
| `proc` | `struct binder_proc *` | 接收方进程 |
| `thread` | `struct binder_thread *` | 接收线程 |
| `binder_buffer` | `binder_uintptr_t` | 用户态读缓冲首地址（`bwr.read_buffer`），驱动往这里写 `BR_*` |
| `size` | `size_t` | 读缓冲容量（`bwr.read_size`） |
| `consumed` | `binder_size_t *` | 出参：已写入字节数（`&bwr.read_consumed`） |
| `non_block` | `int` | 是否非阻塞（`O_NONBLOCK`），阻塞时无事务则睡在 `thread->wait` |

从 `todo` 取 `binder_transaction`，构造 `binder_transaction_data` 信封（关键：`data.ptr.buffer = t->buffer->user_data`，即接收方 mmap 地址），`put_user(BR_TRANSACTION)` + `copy_to_user(信封)`——**只回写 64B 信封，payload 已在共享页**。

---

## 全链路调用顺序速查

```
ioctl(BINDER_WRITE_READ)
└─ binder_ioctl_write_read(filp, cmd, arg, thread)
   ├─ binder_thread_write(proc, thread, write_buffer, write_size, &write_consumed)   // 发送方
   │  └─ binder_transaction(proc, thread, tr, reply, extra_buffers_size)
   │     ├─ binder_alloc_new_buf(&target->alloc, data_size, offsets_size, extra, is_async)
   │     ├─ binder_alloc_copy_user_to_buffer(&target->alloc, buf, 0, tr->data.ptr.buffer, data_size)  // 唯一跨进程拷贝
   │     ├─ binder_alloc_copy_user_to_buffer(...offsets...)
   │     ├─ 循环: binder_alloc_copy_from_buffer(读 object_offset)
   │     │        binder_get_object(target, buf, offset, &object)
   │     │        binder_translate_binder / _handle / _fd / _fd_array(...)
   │     │        binder_alloc_copy_to_buffer(写回改写后的对象)
   │     └─ binder_proc_transaction(t, target_proc, target_thread)
   └─ binder_thread_read(proc, thread, read_buffer, read_size, &read_consumed, non_block)  // 接收方
      └─ 构造 BR_TRANSACTION 信封 → put_user + copy_to_user
```

底层拷贝三兄弟 `binder_alloc_copy_user_to_buffer` / `_to_buffer` / `_from_buffer` 最终都落到 `binder_alloc_do_buffer_copy` + `binder_alloc_get_page` 逐页 `kmap` 搬运。

需要的话，我可以把这份"方法+参数注释"整理成一节追加进 `Binder数据结构传输示意流程分析.md`（第八节：核心函数参数速查）。



先确认仓库里关键结构体和函数签名，确保假数据走查与真实代码一致。
让我确认读路径里信封构造和回复路径的关键代码段。
下面用一套**假数据**把"客户端 → 服务端 → 客户端"整条同步调用链路走一遍。所有函数名、字段都对应你仓库里的 `binder.c` / `binder_alloc.c`(行号已标注)。

---

## 0. 角色与假地址约定

| 项 | 值 |
|----|----|
| 客户端进程 App | `pid=10001`,`binder_proc A`,mmap 区 `0x7f00000000` |
| 服务端进程 Server | `pid=1000`,`binder_proc B`,mmap 区 `0x7fb0000000` |
| 客户端持有的服务端句柄 | `handle=1`(对应服务端的 `binder_node`,`debug_id=7`) |
| 客户端传的回调 Binder 实体本地指针 | `0x7f12340000`(实体属于客户端自己) |
| 客户端想带过去的 fd | `fd=5`(客户端打开的一个文件) |
| 调用方式 | 同步(`flags=0`,非 `TF_ONE_WAY`),`code=1` |

**客户端要发的 Parcel 内容**(放在它自己的用户态缓冲区 `0x7f00001000`):
```
数据区 (data_size=48, 对齐到 8):
  [0..3]   int32  = 42          // 请求 id
  [4..7]   int32  = 5           // 字符串长度
  [8..12]  "hello"              // 5 字节 + 3 字节填充
  [16..39] flat_binder_object   // 回调实体, type=BINDER_TYPE_BINDER, binder=0x7f12340000
  [40..47] binder_fd_object     // type=BINDER_TYPE_FD, fd=5
偏移表 (offsets_size=16, 每项 8 字节, 放在 0x7f00001100):
  [0]=16   // 指向 flat_binder_object
  [1]=40   // 指向 binder_fd_object
```

客户端在 `IPCThreadState` 里把这些写进 `bwr.write_buffer`:`BC_TRANSACTION` + 一个 `binder_transaction_data tr`:
```
tr.target.handle = 1
tr.code          = 1
tr.flags         = 0
tr.data_size     = 48
tr.offsets_size  = 16
tr.data.ptr.buffer  = 0x7f00001000
tr.data.ptr.offsets = 0x7f00001100
```

---

## 1. 客户端 ioctl 下行(binder.c:5117 → 3912 → 4161)

`binder_ioctl_write_read` 先 `copy_from_user` 出 `bwr`,因 `write_size>0` 调 `binder_thread_write(A, thread, 0x..., write_size, &write_consumed)`。

解析到 `BC_TRANSACTION`(binder.c:4161)时:
```c
copy_from_user(&tr, ptr, sizeof(tr));   // 把上面那张信封拷进内核
ptr += sizeof(tr);
binder_transaction(proc, thread, &tr, 0 /*reply=0*/, 0 /*extra*/);
```

---

## 2. 核心:`binder_transaction`(binder.c:3201)

**(a) 找目标。** 用 `tr->target.handle=1` 在客户端 refs 里查到 `ref→node(debug_id=7)`,拿到 `target_proc = B`(pid 1000)、`target_node`。

**(b) 分配目标进程缓冲区**(binder_alloc.c:552):
```c
binder_alloc_new_buf(&B->alloc, data_size=48, offsets_size=16, extra=0, is_async=0)
```
在**服务端** mmap 区里切出一块,假设返回 `t->buffer->user_data = 0x7fb0002000`。数据区 `[0..47]`,偏移表区 `[48..63]`。

**(c) 一次拷贝**(binder_alloc.c:1170)——这是唯一的跨进程拷贝:
```c
binder_alloc_copy_user_to_buffer(&B->alloc, buf, 0,   from=0x7f00001000, 48);  // 数据
binder_alloc_copy_user_to_buffer(&B->alloc, buf, 48,  from=0x7f00001100, 16);  // 偏移表
```
底层 `kmap` 目标页 + `copy_from_user`,把客户端用户态内容拷进服务端 mmap 页。

**(d) 偏移表遍历 + 对象翻译**(循环见 binder.c:3592 附近)。逐条读偏移表 `[16, 40]`:

- **offset=16** → `binder_get_object` 读出 `flat_binder_object`(type=`BINDER_TYPE_BINDER`, `binder=0x7f12340000`)。
  调 `binder_translate_binder`(binder.c:2791):回调实体属于客户端(发送方),于是在**目标进程 B** 里建一个 `binder_ref` 指向它,得到 `handle=3`,然后**就地改写**缓冲里的对象为 `BINDER_TYPE_HANDLE` + `handle=3`,并 `binder_alloc_copy_to_buffer` 写回。

- **offset=40** → 读出 `binder_fd_object`(`fd=5`)。
  调 `binder_translate_fd`(binder.c:2911):`fget(5)` 在客户端拿到 `struct file`,再到服务端 `fd_install` 一个新 fd = `9`(两者共享同一 file)。把对象里的 `fd` 字段就地改写为 `9`。

**(e) 投递**(binder.c:3108):
```c
binder_proc_transaction(t, B /*目标进程*/, target_thread);
```
把 `t->work` 挂到服务端线程/进程的 `todo` 队列,`binder_wakeup_thread_ilocked` 唤醒服务端正在 `read` 的线程。

> 同步调用下,客户端此时不会立即返回用户态去干活,而是进入 `binder_thread_read` 等待自己的 `BR_TRANSACTION_COMPLETE`(见第 5 步)。

---

## 3. 服务端读:`binder_thread_read`(binder.c:4490)

被唤醒后,服务端线程从 `todo` 取出 `t`,构造**只读 64 字节信封** `BR_TRANSACTION`(关键在 binder.c:4766 / 4789):
```c
cmd = BR_TRANSACTION;
trd->code            = 1;
trd->flags           = 0;
trd->target.ptr      = node->ptr;        // 服务端本地 service 指针
trd->cookie          = node->cookie;
trd->data.ptr.buffer  = (uintptr_t)t->buffer->user_data;   // = 0x7fb0002000
trd->data.ptr.offsets = 0x7fb0002000 + ALIGN(48,8) = 0x7fb0002030;
put_user(BR_TRANSACTION, ...);
copy_to_user(trd, 64字节);                // 只回写信封!
```
注意:`trd->data.ptr.buffer` 直接是**服务端自己的 mmap 地址 0x7fb0002000**,payload 已经在共享页里,无需再拷。

**服务端用户态读到的 Parcel**(映射自 0x7fb0002000):
```
int32 = 42
string = "hello"
Binder 对象 → 现在是 BINDER_TYPE_HANDLE, handle=3  (即客户端的回调)
fd 对象 → fd=9  (客户端的 fd=5 已被跨进程复制成 9)
```
服务端执行业务逻辑(比如查到名字),准备 reply:`int32 result = 99`。

---

## 4. 服务端回包:BC_REPLY

服务端把 reply 写进 `bwr.write_buffer`:`BC_REPLY` + `tr_reply{ data_size=4, offsets_size=0, data.ptr.buffer = 服务端 reply blob }`。

再次进入 `binder_transaction`(这次 `reply=1`):通过服务端 `transaction_stack` 上的 `in_reply_to` 找到原事务 `t`,其 `t->from` 指向**客户端线程**,于是 `target_proc = A`。`binder_alloc_new_buf(&A->alloc, 4, 0, 0, 0)` 在客户端 mmap 区切出一块(如 `0x7f00002000`),`binder_alloc_copy_user_to_buffer` 把 `99` 拷进去(又一次"一次拷贝"),无对象需翻译,`binder_proc_transaction(t, A, client_thread)` 唤醒客户端。

---

## 5. 客户端读回:BR_TRANSACTION_COMPLETE → BR_REPLY

客户端线程在 `binder_thread_read` 里:
1. 先收到自己这次发送的 **`BR_TRANSACTION_COMPLETE`**(同步事务的完成确认),`consumed` 推进,循环再次 `read`。
2. 阻塞等待中,服务端回包到达,收到 **`BR_REPLY`**(binder.c:4770 分支):信封里 `trd->data.ptr.buffer = 0x7f00002000`(客户端自己的 mmap),`data_size=4`。
3. 客户端用户态从 `0x7f00002000` 读出 `int32 = 99`,整个调用返回。

---

## 6. 一句话总结拷贝次数

整条链路**只有两次 `copy_from_user`**(客户端发 payload、服务端发 reply 各一次),都落在 `binder_alloc_copy_user_to_buffer`;服务端读请求、客户端读回复都是**零拷贝**直接读各自 mmap 页;对象的"跨进程"靠的是驱动在缓冲里**就地改写**(实体指针↔handle、fd 重编号),而不是复制对象本身。FD 的"复制"是内核 `struct file` 引用计数 + 目标进程新 fd 编号,不走内存拷贝。

需要的话,我可以把这份假数据走查整理成 `Binder数据结构传输示意流程分析.md` 的新增章节(第九节:一次同步调用的假数据全链路走查)。