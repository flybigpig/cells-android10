# Android Binder 内核与 Native 层源码会话导出

> 导出时间：2026-09-03
> 涉及代码路径：
> - `kernel/drivers/android/binder.c`（内核驱动）
> - `frameworks/native/libs/binder/IPCThreadState.cpp`（Native 用户态）
> - `frameworks/native/cmds/servicemanager/`（servicemanager 简化 C 实现）
> - `uapi/linux/android/binder.h`（ABI 结构体定义）

---

## 1. `hlist_add_head(&binder_device->hlist, &binder_devices)` 添加后的链表结构

`hlist_add_head` 执行后，会把当前 `binder_device` 的侵入式节点插入到全局链表 `binder_devices` 的**头部**。

### 两个核心结构

hlist 是侵入式哈希链表，头节点只有一个指针，普通节点用二级指针 `pprev` 反向链接：

```c
struct hlist_head {
    struct hlist_node *first;   // static HLIST_HEAD(binder_devices); 初值 first=NULL
};

struct hlist_node {
    struct hlist_node *next;    // 单向 next
    struct hlist_node **pprev;  // 二级指针，指向"上一个节点的 next 字段"或"头节点的 first 字段"
};
```

`binder_device` 内嵌 `struct hlist_node hlist;`，节点依附在 `binder_device` 结构体内，不单独分配内存（"侵入式"含义）。

### 内核 hlist_add_head 实现逻辑

```c
n->next      = h->first;        // 新节点的 next = 原来的首节点
if (h->first)
    h->first->pprev = &n->next; // 原来首节点的 pprev 改为指向新节点的 next
h->first     = n;               // 头节点的 first 指向新节点
n->pprev     = &h->first;       // 新节点的 pprev 指向头节点的 first 字段
```

### 单次插入形态

以注册名为 "binder" 的设备为例：

```
binder_devices.first
        │
        ▼
   ┌────────────────────────┐
   │ binder_device(binder)   │
   │  .hlist.next  = NULL     │
   │  .hlist.pprev ──────────┘ (指向 &binder_devices.first)
   └────────────────────────┘
```

### 注册循环导致顺序反转

`binder_init` 中按 `binder_devices_param`（默认 `binder,hwbinder,vndbinder`）以逗号分割，依次调用 `init_binder_device`，每次末尾都 `hlist_add_head`。由于每次插到头部，链表顺序与注册顺序**完全相反**——最后注册的设备位于链表首：

```
binder_devices.first
        │
        ▼
   [vndbinder.hlist] ──next──▶ [hwbinder.hlist] ──next──▶ [binder.hlist] ──next──▶ NULL
        │ pprev                        │ pprev                     │ pprev
        └ &binder_devices.first        └ &vndbinder.hlist.next      └ &hwbinder.hlist.next
```

`hlist_for_each_entry(device, &binder_devices, hlist)` 遍历取到的第一项是 vndbinder。

### 反查结构体

链表中只存 `hlist_node` 成员地址，通过 `container_of`/`hlist_entry` 拿回整个 `binder_device`：

```c
#define hlist_entry(ptr, type, member) container_of(ptr, type, member)
```

卸载时 `hlist_for_each_entry_safe` 遍历 `binder_devices`，对每个 `device` 执行 `misc_deregister` + `hlist_del` + `kfree`。

**要点**：hlist 是双向但不对称的链表（头单指针、节点双指针用二级 `pprev` 省内存）；`hlist_add_head` 恒定插到头部并保证 `pprev` 回指正确；`binder_init` 的循环调用使链表首节点为最后注册设备。

---

## 2. `binder_proc_show` 函数解析

这是 binder 驱动 **debugfs 调试接口** `binder/proc/<pid>` 的 `show` 回调（如 `cat /d/binder/proc/1234`）。

```c
static int binder_proc_show(struct seq_file *m, void *unused) {
    struct binder_proc *itr;
    int pid = (unsigned long) m->private;

    mutex_lock(&binder_procs_lock);
    hlist_for_each_entry(itr, &binder_procs, proc_node)
    {
        if (itr->pid == pid) {
            seq_puts(m, "binder proc state:\n");
            print_binder_proc(m, itr, 1);
        }
    }
    mutex_unlock(&binder_procs_lock);

    return 0;
}
```

### (1) `m->private` 的来源

通过 `BINDER_DEBUG_ENTRY(proc)` 宏自动生成 `binder_proc_open` 与 `binder_proc_fops`。打开文件时：

```c
return single_open(file, binder_proc_show, inode->i_private);  // 把 inode->i_private 透传
```

`inode->i_private` 来自 `binder_open` 创建 debugfs 节点时传入的值：

```c
proc->debugfs_entry = debugfs_create_file(strbuf, 0444,
                          binder_debugfs_dir_entry_proc,
                          (void *)(unsigned long)proc->pid,   // 塞的是 pid
                          &binder_proc_fops);
```

所以 `(unsigned long) m->private` 取回目标进程 pid——这是内核里把整型塞进指针再还原的典型手法。

### (2) 遍历全局进程表 `binder_procs`

`binder_procs` 是 `static HLIST_HEAD(binder_procs)`（86 行），存放所有打开过 binder 设备的进程。`binder_proc` 内嵌 `struct hlist_node proc_node;`（645 行），在 `binder_open` 末尾通过 `hlist_add_head(&proc->proc_node, &binder_procs)` 挂入，release 路径通过 `hlist_del` 摘除。加 `binder_procs_lock` 是为了和 `hlist_add_head`/`hlist_del` 互斥，避免遍历时并发增删导致踩空。

### (3) 按 pid 匹配并输出

命中后调 `print_binder_proc(m, itr, 1)`（第三个参数 `1` 表示打印细节），把该进程的线程池（`threads` 红黑树）、binder 实体（`nodes`）、引用（`refs_by_desc`/`refs_by_node`）、待处理事务、已分配 buffer 等全部 dump 出来。

### 要点

- 纯只读**自省/调试**路径，与 `binder_ioctl`/`binder_mmap`/`binder_transaction` 等通信路径隔离，不改任何 binder 状态。
- `hlist_for_each_entry` 线性扫描，复杂度 O(n)。进程极多时 `cat /d/binder/proc/<pid>` 会全表扫一遍才命中。
- `single_open` + `show` 使整份输出由单次 `show` 产生，第二个参数 `void *unused` 即迭代位置，单页模式下恒为 NULL，未使用。

---

## 3. `binder_state_show`

是 `cat /d/binder/state`（debugfs 节点 `state`）的回调，相当于 `binder_proc_show` 的**全量版**：不按 pid 过滤，一次性 dump 出所有 binder 进程状态，外加"死亡节点(dead nodes)"清单。

### (1) 死亡节点段（spinlock + 临时引用保护）

```c
spin_lock(&binder_dead_nodes_lock);
if (!hlist_empty(&binder_dead_nodes))
    seq_puts(m, "dead nodes:\n");
hlist_for_each_entry(node, &binder_dead_nodes, dead_node) {
    node->tmp_refs++;                 // 取临时引用钉住节点
    spin_unlock(&binder_dead_nodes_lock);
    if (last_node) binder_put_node(last_node);
    binder_node_lock(node);
    print_binder_node_nilocked(m, node);
    binder_node_unlock(node);
    last_node = node;
    spin_lock(&binder_dead_nodes_lock);
}
spin_unlock(&binder_dead_nodes_lock);
if (last_node) binder_put_node(last_node);
```

`binder_dead_nodes` 存放已无宿主进程、但仍有在途引用/事务的 `binder_node`，受 `binder_dead_nodes_lock`（自旋锁）保护。加锁手法：遍历中每打印一个节点前**先释放自旋锁再打印，打印完重新加锁**——因为 `print_binder_node_nilocked` 向 seq_file 输出可能触发睡眠（copy_to_user），自旋锁内不能睡眠。解锁期间节点可能被并发释放，于是用 `node->tmp_refs++` 拿临时引用钉住，打印完通过 `binder_put_node(last_node)` 释放上一个节点的临时引用（交错释放）。

### (2) 全量进程段（与 `binder_proc_show` 同构但不过滤）

```c
mutex_lock(&binder_procs_lock);
hlist_for_each_entry(proc, &binder_procs, proc_node)
    print_binder_proc(m, proc, 1);
mutex_unlock(&binder_procs_lock);
```

与 `binder_proc_show` 用同一个 `binder_procs` 全局链表、`binder_procs_lock`（互斥锁）、内嵌节点 `proc_node`，唯一区别是去掉了 `if (itr->pid == pid)` 过滤。

### 要点

- 锁顺序：先持 `binder_dead_nodes_lock`（自旋锁），**完全释放后**再持 `binder_procs_lock`（互斥锁），二者不嵌套，避免死锁与锁序反转。
- 输出更全：`dead nodes:` 段 + 所有进程完整状态；`binder_proc_show` 只有单个 pid 的 `binder proc state:` 段。
- 同系列还有 `binder_stats_show`（统计计数）、`binder_transactions_show`（仅事务），均由 `BINDER_DEBUG_ENTRY` 宏生成，经 `single_open` 把 `inode->i_private` 透传。

---

## 4. `IPCThreadState::mCallingUid`

`IPCThreadState::mCallingUid` 是 Android Native (C++) 层 **Binder 调用方身份（UID）的线程级缓存**，是 Java 侧 `Binder.getCallingUid()` 在 Native 层的真正数据源。

### (1) 声明与线程归属

```cpp
// IPCThreadState.h:182-184
pid_t   mCallingPid;
const char* mCallingSid;
uid_t   mCallingUid;
```

`IPCThreadState` 是**每线程一个实例**：`self()` 通过 `pthread_key_t gTLS` 做线程局部存储，每个 binder 线程首次调用 `self()` 时 `new IPCThreadState` 并 `pthread_setspecific(gTLS, this)`。因此 `mCallingUid` 是**线程私有**的——binder 线程池里每个线程处理来自不同客户端的请求，身份按线程隔离。

### (2) 值的来源与生命周期

**初始值**：构造函数末尾调用 `clearCaller()`：

```cpp
void IPCThreadState::clearCaller() {
    mCallingPid = getpid();
    mCallingSid = nullptr;
    mCallingUid = getuid();   // 默认是"我自己"的 uid
}
```

**进入一次 binder 调用时被内核覆盖**：在 `executeCommand` 处理 `BR_TRANSACTION` / `BR_TRANSACTION_SEC_CTX` 时：

```cpp
mCallingPid  = tr.sender_pid;     // 内核填充，不可伪造
mCallingUid  = tr.sender_euid;    // 内核填充
mCallingSid  = reinterpret_cast<const char*>(tr_secctx.secctx);
```

`tr.sender_euid`（及 `sender_pid`）由 **binder 驱动在内核态填入**——驱动在 `binder_transaction` 投递时把发送端真实 `euid` 写进事务结构。用户态调用方无法伪造，是整个 Binder 权限校验的信任锚点。

**事务结束后还原**：

```cpp
mCallingPid  = origPid;
mCallingSid  = origSid;
mCallingUid  = origUid;
```

### (3) 显式清除 / 还原

与 Java `Binder.clearCallingIdentity()` / `restoreCallingIdentity()` 对应：

```cpp
int64_t IPCThreadState::clearCallingIdentity() {
    int64_t token = ((int64_t)mCallingUid<<32) | mCallingPid;
    clearCaller();        // 把 mCallingUid 改回 getuid()
    return token;
}

void IPCThreadState::restoreCallingIdentity(int64_t token) {
    mCallingUid = (int)(token>>32);
    mCallingSid = nullptr;
    mCallingPid = (int)token;
}
```

服务端替调用方干活时，常先 `clearCallingIdentity` 暂存并清除身份，干完再 `restore`。注意 `restore` 无法恢复 `mCallingSid`。

### (4) 读取入口

```cpp
uid_t IPCThreadState::getCallingUid() const { return mCallingUid; }
```

Java 侧 `Binder.getCallingUid()` 经 JNI 最终调到它。

---

## 5. `binder_transaction_data` 数据的组装

`binder_transaction_data` 是 Binder 跨 `ioctl` 边界的**信封结构体**。它的"组装"发生在三段不同位置：发送方 libbinder 装配、内核接收后一次拷贝落盘、内核投递给接收方时重新装配。

### (1) 结构本身（ABI 契约）

- `union { size_t handle; void* ptr; } target` —— 发送方填 `handle`，接收方收到时内核改写成 `ptr` 与 `cookie`。
- `void* cookie`：接收方用来定位 BBinder 对象。
- `uint32_t code`：方法号。
- `uint32_t flags`：含 `TF_ONE_WAY`/`TF_STATUS_CODE`/`TF_ACCEPT_FDS` 等。
- `pid_t sender_pid; uid_t sender_euid`：**发送方一律填 0**，由内核投递时权威填充。
- `size_t data_size; size_t offsets_size`：payload 大小与 Binder 对象偏移表大小。
- `union { struct { void* buffer; void* offsets; } ptr; } data`：发送方 `ptr.buffer/offsets` 指向本进程 Parcel 数据；内核投递时改为指向**接收方共享内存**里的地址。

### (2) 发送方装配：`IPCThreadState::writeTransactionData`

```cpp
tr.target.ptr   = 0;          // 清零，避免栈残留
tr.target.handle= handle;      // 目标服务句柄
tr.code         = code;
tr.flags        = binderFlags;
tr.cookie       = 0;
tr.sender_pid   = 0;          // 身份留空，内核会填
tr.sender_euid  = 0;
tr.data_size      = data.ipcDataSize();
tr.data.ptr.buffer= data.ipcData();
tr.offsets_size   = data.ipcObjectsCount()*sizeof(binder_size_t);
tr.data.ptr.offsets = data.ipcObjects();
```

随后 `tr` 连同命令码 `BC_TRANSACTION` 写入 `mOut`，经 `talkWithDriver` → `binder_ioctl` 发往内核。

### (3) 内核接收并落地（一次拷贝）

`binder_ioctl`→`binder_thread_write` 命中 `BC_TRANSACTION`/`BC_REPLY`：

```c
struct binder_transaction_data tr;
if (copy_from_user(&tr, ptr, sizeof(tr))) return -EFAULT;
binder_transaction(proc, thread, &tr, cmd == BC_REPLY, 0);
```

`binder_transaction()` 用 `tr->data.ptr.buffer / data_size / offsets_size` 找到发送方 Parcel 缓冲区，校验合法性，然后**把数据拷贝进目标进程在 `binder_mmap` 映射的共享内存**（即 `binder_alloc` 分配的 `binder_buffer`）——这就是 Binder "一次拷贝"的发生点。信息固化进：

- `struct binder_transaction *t`：`t->code`、`t->flags`、`t->to_proc/to_thread`、`t->from`，以及**权威身份** `t->sender_euid = task_euid(proc->tsk)`（内核取真实 euid）。
- `struct binder_buffer`：承载 `data_size / offsets_size / user_data`（目标地址空间映射地址）。

### (4) 内核投递时重新装配（接收方视角）

`binder_thread_read` 取出事务工作项，重建 `binder_transaction_data_secctx tr`：

```c
trd->target.ptr = target_node->ptr;   // 把 handle 翻译成 node 指针
trd->cookie     = target_node->cookie;
cmd = BR_TRANSACTION;
trd->code  = t->code;
trd->flags = t->flags;
trd->sender_euid = from_kuid(current_user_ns(), t->sender_euid);  // 权威身份
trd->sender_pid = t_from ? task_tgid_nr_ns(...) : 0;
trd->data_size      = t->buffer->data_size;
trd->offsets_size   = t->buffer->offsets_size;
trd->data.ptr.buffer = (uintptr_t) t->buffer->user_data;          // 接收方自己共享内存
trd->data.ptr.offsets= trd->data.ptr.buffer + ALIGN(t->buffer->data_size, sizeof(void*));
tr.secctx = t->security_ctx;
if (t->security_ctx) cmd = BR_TRANSACTION_SEC_CTX;
```

`copy_to_user` 把信封拷回接收方读缓冲区。接收方 libbrinder 在 `executeCommand` 里 `mIn.read(&tr, sizeof(tr))`，再用 `tr.data.ptr.buffer / data_size / offsets` 调 `buffer.ipcSetDataReference` 包成 Parcel 交给 `onTransact`——正是 `mCallingPid = tr.sender_pid; mCallingUid = tr.sender_euid` 被写上的地方。

---

## 6. `binder_transaction_data.target.handle` 赋值

`binder_transaction_data.target.handle` 是发送方用来指明"我要找哪个服务"的整数句柄，由同一个 `union` 字段在不同阶段赋予完全不同含义。

### (1) 字段本质：`union` 双义

```c
union {
    __u32       handle;   // 发送方填：服务引用描述符（整数）
    binder_uintptr_t ptr; // 接收方收：内核填的目标 node 指针
} target;
void *cookie;             // 接收方收：node 关联的 BBinder 对象指针
```

### (2) 发送方赋值（libbinder）

```cpp
tr.target.ptr   = 0;
tr.target.handle= handle;       // transact(handle, code, data) 传入的整数
tr.cookie       = 0;
```

`handle` 来自服务查询：客户端 `getService` 后，libbinder 把返回的引用以 `binder_ref.desc`（描述符）缓存，后续 `transact` 用它。

### (3) handle 值的来源（内核分配）

`handle` 本质是发送进程 `refs_by_desc`（红黑树，键 `desc`）的键。服务对外发布后，别的进程首次拿到它时内核建立 `binder_ref` 并分配 `desc`：

```c
new_ref->data.desc = (node == context->binder_context_mgr_node) ? 0 : 1;
for (... refs_by_desc 升序 ...)
    if (ref->data.desc > new_ref->data.desc) break;
    new_ref->data.desc = ref->data.desc + 1;   // 取比现有最大描述符 +1
```

servicemanager 固定为 `0`，其余服务从 `1` 起递增。这个 `desc` 即 `tr->target.handle` 的值。

### (4) 内核解析（binder_transaction）

```c
if (tr->target.handle) {                       // handle 非 0：普通服务引用
    ref = binder_get_ref_olocked(proc, tr->target.handle, true);  // 按 desc 查 refs_by_desc
    if (ref)
        target_node = binder_get_node_refs_for_txn(ref->node, &target_proc, ...);
    else {  // 句柄无效
        return_error = BR_FAILED_REPLY;
    }
} else {                                       // handle == 0：=servicemanager
    target_node = context->binder_context_mgr_node;
    ...
}
```

解析链路：`tr->target.handle`（整数）→ `binder_get_ref_olocked(proc, handle)` 找到 `binder_ref` → `ref->node` 拿到 `binder_node` → `binder_get_node_refs_for_txn` 取回 `target_node` 与 `target_proc`。

### (5) 投递时 handle 被改写（接收方视角）

```c
trd->target.ptr = target_node->ptr;   // 指向目标 binder_node
trd->cookie     = target_node->cookie;
cmd = BR_TRANSACTION;
```

接收方读到的是 `tr.target.ptr` 与 `tr.cookie`，用它定位 BBinder 并调 `onTransact`——`handle` 整数在接收侧已不复存在。

---

## 7. 内核的 `binder_node` 是从哪里拿的

`binder_node` 是内核里代表"一个 Binder 服务对象（BBinder）"的**唯一内核对象**，始终住在**拥有该服务的那个进程**的 `proc->nodes` 红黑树里。客户端拿到的只是 `binder_ref`（带整数 `desc`/句柄），而 `ref->node` 用指针指向那个真正属于服务进程的 `binder_node`。

### (1) 它存哪、以什么为键

`binder_proc` 里有红黑树 `nodes`（按指针排序）。`binder_node` 索引键是 `node->ptr = fp->binder`（服务在自己进程里的本地 BBinder 指针值）。插入在 `binder_init_node_ilocked`，遍历 `proc->nodes` 按 `ptr` 比较，没有则 `rb_link_node`+`rb_insert_color` 插入，键是进程内指针值，故同一服务对象在本进程只会有一个 node。

### (2) 它从哪被创建（两种来源）

第一种（最常见）：进程**首次把本地 Binder 实体发出去**时，遇到 `flat_binder_object` 里 `BINDER_TYPE_BINDER`，调 `binder_translate_binder`：

```c
struct binder_proc *proc = thread->proc;          // 发送/拥有方
node = binder_get_node(proc, fp->binder);         // 先按本地指针查本进程 nodes 树
if (!node)
    node = binder_new_node(proc, fp);             // 查不到就建
```

`binder_new_node` 把 node 建在**服务对象自己的进程**里，随后在接收方进程建 `binder_ref` 让对方拿句柄——node 只有一个，ref 每进程一份。

第二种：servicemanager 注册自身。`binder_ioctl_set_ctx_mgr` 中：

```c
new_node = binder_new_node(proc, fbo);            // 在 servicemanager 进程建 node
context->binder_context_mgr_node = new_node;      // 记为该 context 的 manager 节点
```

这就是 `handle == 0` 时 `context->binder_context_mgr_node` 的来源。

### (3) 事务时它从哪"拿来"（解析路径）

回顾 `tr->target.handle` 解析：`binder_get_ref_olocked(proc, handle)` 在**发送方进程** `refs_by_desc` 树找到 `binder_ref`，随即 `target_node = binder_get_node_refs_for_txn(ref->node, ...)`——`ref->node` 指针**直接指向拥有方进程的那个 binder_node**。所以"拿到 node"不是新建也不是拷贝，而是顺着本进程 ref 指针解引用。

对于内嵌的 `BINDER_TYPE_HANDLE` 对象，有 `binder_get_node_from_ref`：

```c
ref = binder_get_ref_olocked(proc, desc, need_strong_ref);  // 按 desc 查本进程 ref
node = ref->node;                                           // 指针直取目标 node
binder_inc_node_tmpref(node);                               // 临时钉住，防并发释放
```

### (4) 关键结论

`binder_node` **不会**在每个进程各存一份，也**不会**随 transaction 被拷贝。它只存在于服务拥有方进程的 `proc->nodes` 红黑树（键为本地指针）；所有客户端进程只是在自己 `proc` 里各持 `binder_ref`，`ref->node` 指针指向同一个内核 node。当最后一个引用和本地 ref 释放时，才由 `binder_free_ref`→`binder_free_node` 真正 `kfree`。

---

## 8. 内核 IPCThreadState 处理数据过程

`IPCThreadState` **不在内核里**，它是 Native 层 libbinder 的**用户态**类。真正内核处理在 `binder.c` 的 `binder_ioctl` → `binder_thread_write` / `binder_transaction` / `binder_thread_read`。一次同步调用是 **用户态 → 内核态 → 用户态** 的往返。

### 阶段一：客户端 IPCThreadState 装配并发出（用户态）

`BpBinder::transact()` → `IPCThreadState::transact()` → `writeTransactionData(BC_TRANSACTION, ...)` 把 Parcel 包成 `binder_transaction_data tr`（`tr.target.handle = handle`、payload 指向本地 Parcel 缓冲、`sender_pid/euid` 置 0），写入 `mOut`。接着 `waitForResponse` 调 `talkWithDriver(true)`，打包 `binder_write_read bwr`（`write_buffer = mOut`、`read_buffer = mIn`），执行 `ioctl(mProcess->mDriverFD, BINDER_WRITE_READ, &bwr)` 陷入内核。

### 阶段二：内核接收与落地（binder.c）

`binder_ioctl` 命中 `BINDER_WRITE_READ` → `binder_thread_write`。遇 `BC_TRANSACTION` 执行 `copy_from_user(&tr, ptr, sizeof(tr))` 把信封从用户态拷进内核，再调 `binder_transaction(proc, thread, &tr, false, 0)`：

- **解析目标**：`tr->target.handle` → `binder_ref` → `ref->node` 定位 `target_proc/target_node`。
- **分配与一次拷贝**：在 `target_proc` 已 `mmap` 共享内存里 `binder_alloc` 分配 `binder_buffer`，`copy_from_user` 把 Parcel 数据拷进目标进程共享内存。
- **Binder 对象翻译**：遍历 `flat_binder_object`，`BINDER_TYPE_BINDER` 改写成 `BINDER_TYPE_HANDLE`（在服务进程建 node、接收进程建 ref 给句柄）。
- **入队**：构造 `binder_transaction *t`，记录 `t->code/t->flags/t->sender_euid/t->to_proc/to_thread/t->from`，挂 `BINDER_WORK_TRANSACTION` 工作项，加入目标线程/进程 todo 队列并唤醒。

### 阶段三：服务端 IPCThreadState 收数据并执行（用户态）

服务端 `joinThreadPool` → `getAndExecuteCommand` → `talkWithDriver` → `ioctl`。内核 `binder_thread_read` 取出 `BINDER_WORK_TRANSACTION`，重建 `binder_transaction_data_secctx tr`（`target.ptr/cookie`、`sender_euid`、`data.ptr.buffer` 指向接收方共享内存）。回到用户态 `executeCommand(BR_TRANSACTION)`：`mIn.read(&tr, ...)`，写身份 `mCallingPid = tr.sender_pid; mCallingUid = tr.sender_euid`，用 `tr.data` 重建 Parcel，调 `obj->onTransact(tr.code, data, &reply, tr.flags)`。

### 阶段四：回包（非 one-way）

`sendReply` → `writeTransactionData(BC_REPLY)` → `ioctl`。内核 `binder_thread_write` 命中 `BC_REPLY` 再进 `binder_transaction(reply=true)`，用 `t->from` 找回等待的客户端线程，把 reply 拷进客户端共享内存、入队 `BR_REPLY` 并唤醒。客户端 `waitForResponse` 收到 `BR_REPLY`，`reply->ipcSetDataReference(...)` 还原 Parcel，并把 `mCallingUid = origUid` 还原身份。

---

## 9. 补充各个阶段核心方法

**阶段一：客户端组装与发送（用户态）**
- `IPCThreadState::transact` (663) — 调用入口，`writeTransactionData` + `waitForResponse`。
- `IPCThreadState::writeTransactionData` (1058) — 装配 `binder_transaction_data` 写入 `mOut`。
- `IPCThreadState::waitForResponse` (857) — 循环读响应，遇 `BR_REPLY` 收尾。
- `IPCThreadState::talkWithDriver` (955) — 打包 `binder_write_read bwr`，`ioctl(BINDER_WRITE_READ)`。

**阶段二：内核接收入口（binder.c）**
- `binder_ioctl` (5291) — 总入口，`BINDER_WRITE_READ` 转发 `binder_ioctl_write_read`。
- `binder_ioctl_write_read` (5117) — 写路径调 `binder_thread_write`、读路径调 `binder_thread_read`。
- `binder_get_thread` (4971) — 取或创建本线程 `binder_thread`，加入 `proc->threads`。
- `binder_thread_write` (3912) — 遍历命令流，`case BC_TRANSACTION`/`BC_REPLY` (4161-4162) 调 `binder_transaction`。

**阶段三：事务核心处理（binder.c 心脏）**
- `binder_transaction` (3201) — target 解析、buffer 分配与一次拷贝、对象翻译、入队唤醒。
  - `binder_get_ref_olocked` / `binder_get_node_refs_for_txn` — handle → ref → node。
  - `binder_alloc` 系列（`binder_alloc.c`）— 在目标进程 `mmap` 区分配 `binder_buffer`，完成一次拷贝。
  - `binder_translate_binder` (2791) / `binder_translate_handle` — 改写 `flat_binder_object`，建立 node/ref。
  - `binder_proc_transaction` (3108) — 投递到目标进程/线程并唤醒。
  - `binder_transaction_priority` (1400) — 优先级继承。
  - `binder_transaction_buffer_release` (2629) — 异常/释放路径清理。

**阶段四：服务端接收与执行（用户态）**
- `IPCThreadState::joinThreadPool` (588) — 线程池主循环，`getAndExecuteCommand`。
- `IPCThreadState::getAndExecuteCommand` (495) — `talkWithDriver` 取数据，再 `executeCommand`。
- `IPCThreadState::executeCommand` (1101) — `BR_TRANSACTION_SEC_CTX`/`BR_TRANSACTION` 取 `tr`、写身份、重建 Parcel、调 `BBinder::onTransact`。
- `BBinder::onTransact` — 进入业务服务实现。

**阶段五：回包路径（reply）**
- 服务端：`onTransact` 返回后 `sendReply` (842) → `writeTransactionData(BC_REPLY)` → `talkWithDriver` → `ioctl`。
- 内核：`binder_thread_write` 的 `case BC_REPLY` → `binder_transaction(reply=true)`，用 `t->from` 找回客户端线程。
- 客户端：`waitForResponse` 收到 `BR_REPLY` 还原 Parcel，还原 `mCallingUid`。

---

## 10. `binder_proc_transaction` (3108) 把事务投递到目标进程

`binder_proc_transaction`（3108）是 `binder_transaction` 在"解析目标、分配 buffer、一次拷贝、对象翻译"**全部做完之后**调用的"最终投递"函数——不碰数据本身，只决定事务挂到目标进程哪个队列并唤醒处理线程。

### 入参

```c
static bool binder_proc_transaction(struct binder_transaction *t,
                                    struct binder_proc *proc,
                                    struct binder_thread *thread)
```

`t` 已组装好；`proc` 是目标进程；`thread` 是**期望处理的线程，可为 NULL**。

### 目标线程选择（同步 vs 单向）

```c
struct binder_node *node = t->buffer->target_node;
bool oneway = !!(t->flags & TF_ONE_WAY);
if (oneway) {
    BUG_ON(thread);
    if (node->has_async_transaction) {
        pending_async = true;
    } else {
        node->has_async_transaction = true;   // 按 node 串行化 oneway
    }
}
binder_inner_proc_lock(proc);
if (proc->is_dead || (thread && thread->is_dead))
    return false;                              // 目标进程/线程已死
if (!thread && !pending_async)
    thread = binder_select_thread_ilocked(proc);  // 挑空闲线程
```

### 入队（核心行 3144）

```c
if (thread) {
    binder_transaction_priority(thread->task, t, node_prio, node->inherit_rt);
    binder_enqueue_thread_work_ilocked(thread, &t->work);       // 3144 挂线程 todo
} else if (!pending_async) {
    binder_enqueue_work_ilocked(&t->work, &proc->todo);         // 挂进程级 todo
} else {
    binder_enqueue_work_ilocked(&t->work, &node->async_todo);   // 挂 node 异步队列
}
if (!pending_async)
    binder_wakeup_thread_ilocked(proc, thread, !oneway);        // 唤醒
```

### `_ilocked` 与公开版区别

`binder_enqueue_thread_work`（1057）是加锁包装，内部 `binder_inner_proc_lock` 后调 `binder_enqueue_thread_work_ilocked`（1042，核心是 `list_add_tail(&work->entry, &thread->todo)`）。`binder_proc_transaction` 已持 `binder_inner_proc_lock(proc)`，故直接调 `_ilocked` 避免重复加锁。

### 唤醒与后续

`binder_wakeup_thread_ilocked` 唤醒阻塞在 `binder_thread_read` 的 binder 线程，它从 `thread->todo` 取出 `BINDER_WORK_TRANSACTION`，重建 `binder_transaction_data` 并 `copy_to_user`，回到 `executeCommand(BR_TRANSACTION)` 执行 `onTransact`。

---

## 11. `binder_parse(bs, 0, readbuf, bwr.read_consumed, func)` with `func = svcmgr_handler`

`binder_parse` / `binder_loop` / `svcmgr_handler` 位于 `frameworks/native/cmds/servicemanager/`，是 **servicemanager 自己用的简化 C 版 binder 客户端**，不走 libutils，与 `IPCThreadState`（libbinder C++ 路径）是两套实现。

### 第 1 步：`binder_parse` 还原数据

`binder_loop`（455 行）`for(;;)` 里 `ioctl` 唤醒后，`binder_parse(bs, 0, readbuf, bwr.read_consumed, func)`（485）。命中 `BR_TRANSACTION`：

```c
memcpy(&txn, ptr, sizeof(...));
bio_init_from_txn(&msg, &txn.transaction_data);  // 把 data.ptr.buffer 包成 binder_io
bio_init(&reply, rdata, ...);
res = func(bs, &txn, &msg, &reply);              // = svcmgr_handler
```

`bio_init_from_txn` 零拷贝让 handler 直接读共享内存 payload。

### 第 2 步：进入 `svcmgr_handler`

`svcmgr_handler`（256 行）先校验 `txn->target.ptr == BINDER_SERVICE_MANAGER`（274），读 `strict_policy` 和 `svcmgr_id` 校验，再按 `txn->code` 分派：

- `SVC_MGR_GET_SERVICE` / `SVC_MGR_CHECK_SERVICE`：`bio_get_string16(msg, &len)` 取服务名 → `do_find_service` 在 `svclist` 链表 `find_svc`，过 SELinux `svc_can_find`，命中返回 `si->handle`；`bio_put_ref(reply, handle)` 把 handle 写成 `BINDER_TYPE_HANDLE`。
- `SVC_MGR_ADD_SERVICE`：`bio_get_ref(msg)` 取 handle，`bio_get_uint32` 读 `allow_isolated` → `do_add_service` 查 SELinux `svc_can_register`，把服务名+handle 组 `struct svcinfo` 头插 `svclist`，并 `binder_link_to_death` 注册死亡通知。
- `SVC_MGR_LIST_SERVICES`：遍历 `svclist` 返回列表。

### 第 3 步：回到 `binder_parse` 发回回复

```c
if (txn.transaction_data.flags & TF_ONE_WAY)
    binder_free_buffer(bs, txn.transaction_data.data.ptr.buffer);  // oneway：只释放
else
    binder_send_reply(bs, &reply, txn.transaction_data.data.ptr.buffer, res);  // 同步：回包
```

`binder_send_reply` 把 reply 连同 `BC_FREE_BUFFER` 打包成 `BC_REPLY`，`ioctl` 写回内核。内核 `binder_thread_write` 收 `BC_REPLY` 走 `binder_transaction(reply=true)`，用 `t->from` 找回客户端线程。reply 里 handle 经 `binder_translate_handle` 翻译成客户端视角的句柄，客户端收到 `BR_REPLY` 后包成 `BpBinder`，`getService` 据此返回代理。

### 第 4 步：回到 `binder_loop` 继续

`binder_parse` 返回 1，`binder_loop` 判断正常则回到 `for(;;)` 顶部再次 `ioctl` 阻塞等待。servicemanager 单线程、串行处理所有查/注册请求。

---

## 12. `binder_proc_transaction` 把事务投递到目标服务进程

**准确**。`binder_proc_transaction(t, target_proc, target_thread)` 的 `target_proc` 正是**目标服务进程**的内核 `binder_proc`。

### 调用点：target_proc 来源

`binder_transaction` 尾部三分支都调 `binder_proc_transaction`，第二参数都是 `target_proc`：

```c
} else if (!(t->flags & TF_ONE_WAY)) {
    if (!binder_proc_transaction(t, target_proc, target_thread)) { ... }   // 3810
} else {
    if (!binder_proc_transaction(t, target_proc, NULL)) { ... }            // 3820
}
```

`target_proc` 在解析目标阶段由 `tr->target.handle` → `binder_get_ref_olocked` → `ref->node` → `binder_get_node_refs_for_txn(ref->node, &target_proc, ...)` 算出。因 `binder_node` 始终住在服务对象拥有方的 `proc->nodes`，`target_node->proc` 即服务进程内核 `proc`。

### 函数内部确确实实投进 target_proc

- 选线程：`thread = binder_select_thread_ilocked(proc)`（3139）在目标服务进程 `threads` 红黑树挑空闲线程。
- 判死：`proc->is_dead` 检查**目标服务进程**是否活着，死了返回 `false` → `BR_DEAD_REPLY`。
- 入队：有线程→挂 `thread->todo`；无线程→挂 `proc->todo`。
- 唤醒：`binder_wakeup_thread_ilocked(proc, thread, !oneway)` 唤醒目标服务进程上阻塞的 binder 线程。

此后由该进程 `binder_thread_read` 取出、重建信封、`copy_to_user` 回用户态，落到 `onTransact`。

### 语义

- `target.handle == 0` → `target_proc` = servicemanager → 投给 servicemanager。
- `target.handle` 是业务服务句柄 → `target_proc` = 业务服务进程 → 投给业务服务。
- 两者走同一函数、同样三步（选线程/判死/入队+唤醒），区别只是 `proc` 指向谁。

---

## 13. 内核 binder 只是作为一个数据中转？（辨析）

**在数据面上基本对，但在控制面上严重低估。**

### "中转/搬运"对的部分（数据平面）

- 一次拷贝把发送方 Parcel 数据拷进**接收方**已 `mmap` 共享内存，`binder_transaction` 里 `binder_alloc` + `copy_from_user`。不长期持有数据，完成即交还。
- `binder_transaction_data` 只是跨 `ioctl` 临时信封，落地拆成 `binder_transaction` + `binder_buffer`，投递时重建信封——本质是"把数据从发送进程护送到接收进程"。

### 不只是中转的部分（控制平面）

1. **身份仲裁（安全锚点）**：`sender_pid`/`sender_euid` 由内核从发送进程 `task` 取 `task_euid(proc->tsk)` 强制填入，用户态填的 0 被覆盖。这是 `mCallingUid` 不可伪造来源，是整个 Android 权限模型的信任基础。哑管道做不到"替接收方确认对方真实身份"。

2. **引用与对象模型**：维护 `binder_node`（服务进程侧唯一对象）和 `binder_ref`（每客户端一份、带 `desc` 句柄）映射，负责 handle 递增分配、`flat_binder_object` 的 `BINDER_TYPE_BINDER ↔ BINDER_TYPE_HANDLE` 翻译、跨进程引用计数、强/弱引用。这是一整套**跨进程对象引用系统**。

3. **线程调度与优先级继承**：`binder_select_thread_ilocked` 挑空闲线程、`binder_transaction_priority` 做优先级继承、`binder_wakeup_thread_ilocked` 唤醒，把事务路由到正确服务线程并管理调度优先级。

4. **同步/阻塞语义**：同步调用客户端在 `waitForResponse` 阻塞，内核经 `t->from` 把回复精准投递回当初等待的线程，并延迟 `TRANSACTION_COMPLETE`（3805 行，降延迟）；oneway 按 node 串行化（`has_async_transaction`/`async_todo`）。完整请求-响应时序编排，非无状态转发。

5. **生命周期与死亡通知**：目标进程死，`binder_proc_transaction` 判 `proc->is_dead` 返回 `false` → `BR_DEAD_REPLY`；`binder_dead_nodes` 收集悬空 node；`binder_link_to_death` 让 servicemanager 在服务崩溃时清理 `svclist`。

### 结论

"内核 binder 只是数据中转"只覆盖**数据平面**——它确实高效搬运 Parcel 字节（一次拷贝、共享内存）。但 binder 区别于普通管道/socket 的，是整套**控制平面**：身份注入、引用对象模型、线程调度与优先级继承、请求-响应时序、死亡与存活管理。它是**带可信身份和对象语义的 IPC 运行时/经纪人（broker）**，而非哑管道。前面逐层拆的 `mCallingUid`（身份）、`target.handle`/`binder_node`（引用）、`binder_proc_transaction`（调度投递）恰好分别落在控制平面不同切面——若它只是中转，这些机制都无存在必要。

---

## 附：关键源码位置速查

| 主题 | 文件:行号 |
|------|-----------|
| `hlist_add_head` 插入设备链 | `binder.c` `init_binder_device` 末尾；`binder_devices` 85 行 |
| `binder_proc_show` | `binder.c` BINDER_DEBUG_ENTRY(proc) 生成，96-112 行 |
| `binder_state_show` | `binder.c` 6290-6305 行附近 |
| `mCallingUid` 赋值 | `IPCThreadState.cpp` 1222 行（`mCallingUid = tr.sender_euid`） |
| `writeTransactionData` 装配信封 | `IPCThreadState.cpp` 1058 行 |
| `binder_transaction` 核心 | `binder.c` 3201 行 |
| `target.handle` 解析 | `binder.c` 3313-3339 行 |
| `binder_node` 创建 | `binder.c` `binder_translate_binder` 2801、`binder_ioctl_set_ctx_mgr` 5213 |
| `binder_proc_transaction` | `binder.c` 3108 行 |
| servicemanager 循环 | `servicemanager/binder.c` `binder_loop` 455、`binder_parse` 252、`service_manager.c` `svcmgr_handler` 256 |
