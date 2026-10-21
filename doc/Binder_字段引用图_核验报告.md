# Binder_字段引用图.png 逐字段核验报告

> 核验日期：2026-10-16 | 核验基线：cells-android10（android-10.0.0_r33 / kernel 4.9.200）本地实测
> 核验对象：`doc/Binder_字段引用图.png`（三栏：① libbinder ② uapi ABI ③ 内核驱动内部）
> 结论：**图整体框架与引用关系正确（值相等/指针/回填三类箭头语义均成立），但发现 2 处硬错误、3 处精度问题**，逐条列于下。

---

## 一、结论速览

| 编号 | 位置 | 图中标注 | 实测结论 |
|---|---|---|---|
| E1 | ① IPCThreadState | `mIsServing (status_t)` | ❌ **Android 10 无此成员**（Android 11+ 才引入） |
| E2 | ③ binder_transaction | `sender_uid (kuid_t)` | ❌ **本仓无此字段**，只有 `sender_euid`（sender_uid 上游 5.x 才加入） |
| E3 | ③ binder_buffer | `data (uint8[])` | ❌ 字段名错，实际是 `void __user *user_data`；且漏 `extra_buffers_size` |
| E4 | ② flat_binder_object | `cookie (__u64)` | ⚠️ 实际是 `binder_uintptr_t`（ILP32 下为 __u32） |
| E5 | ③ binder_ref | `desc / strong / weak` 平铺 | ⚠️ 实际嵌套在 `struct binder_ref_data data` 内 |
| E6 | ① BBinder | `mExtras (extras)` | ⚠️ 实际是 `std::atomic<Extras*>`（Android 10 改原子） |

其余字段全部核验通过（✓），见第二节逐栏表。

---

## 二、逐栏核验表（✓ 项全部带实测行号）

### ① 用户态 libbinder（frameworks/native/libs/binder）

| 图中字段 | 源码实际 | 行号 | 结论 |
|---|---|---|---|
| Parcel.mData (uint8) | `uint8_t* mData` | include/binder/Parcel.h L478 | ✓ |
| Parcel.mDataSize (size_t) | `size_t mDataSize` | L479 | ✓ |
| Parcel.mObjects (binder_size_t) | `binder_size_t* mObjects` | L482 | ✓ |
| Parcel.mObjectsSize | `size_t mObjectsSize` | L483 | ✓ |
| Parcel.mOwner (release_func) | `release_func mOwner` | L495 | ✓ |
| Parcel.mAllowFds | `bool mAllowFds` | L493 | ✓ |
| IPCThreadState.mOut / mIn (Parcel) | `Parcel mOut / mIn` | IPCThreadState.h L180 / L179 | ✓ |
| IPCThreadState.mProcess | `const sp<ProcessState> mProcess` | L174 | ✓ |
| IPCThreadState.mCallingPid | `pid_t mCallingPid` | L182 | ✓ |
| IPCThreadState.mCallingUid | `uid_t mCallingUid` | L184 | ✓ |
| IPCThreadState.mIsServing | **不存在**（私有区全成员已核对，无此字段） | — | ❌ E1 |
| ProcessState.mDriverFD | `int mDriverFD` | ProcessState.h L113 | ✓ |
| ProcessState.mVMStart | `void* mVMStart` | L114 | ✓ |
| ProcessState.mHandleToObject | `Vector<handle_entry> mHandleToObject` | L128 | ✓ |
| ProcessState.mMaxThreads | `size_t mMaxThreads` | L122 | ✓ |
| ProcessState.mThreadPoolSeq | `volatile int32_t mThreadPoolSeq` | L141 | ✓ |
| BpBinder.mHandle | `const int32_t mHandle` | BpBinder.h L118 | ✓ |
| BpBinder.mAlive | `volatile int32_t mAlive` | L130 | ✓ |
| BpBinder.mObitsSent | `volatile int32_t mObitsSent` | L131 | ✓ |
| BpBinder.mObituaries | `Vector<Obituary>* mObituaries` | L132 | ✓ |
| BBinder.mExtras | `std::atomic<Extras*> mExtras` | Binder.h L84 | ⚠️ E6 |
| BBinder.mReserved0 | `void* mReserved0` | Binder.h L85 | ✓ |

### ② 内核 ABI 线格式（kernel/include/uapi/linux/android/binder.h）

| 图中字段 | 源码实际 | 行号 | 结论 |
|---|---|---|---|
| binder_write_read 六字段（write/read 的 size、consumed、buffer） | 类型 `binder_size_t` / `binder_uintptr_t` 全对 | L223–L228 | ✓ |
| transaction_data.target.handle (binder_uintptr_t) | union { handle; ptr }，均 binder_uintptr_t | L299–L305 | ✓ |
| transaction_data.code / flags (__u32) | `__u32 code / flags` | L307 / L308 | ✓ |
| transaction_data.sender_pid / sender_euid | `__kernel_pid_t` / `__kernel_uid_t` | L309–L310 | ✓ |
| transaction_data.data_size / offsets_size | `binder_size_t` | L311–L312 | ✓ |
| transaction_data.data.ptr.buffer / offsets | `binder_uintptr_t` | L315–L316 | ✓ |
| flat.hdr.type (__u32) | `struct binder_object_header { __u32 type }` | L112–L113 | ✓ |
| flat.flags (__u32) | `__u32 flags` | L125 | ✓ |
| flat.binder / handle（union） | `binder_uintptr_t binder` / `__u32 handle` | L129–L130 | ✓ |
| flat.cookie (__u64) | **`binder_uintptr_t cookie`**（ILP32 下 = __u32） | L134 | ⚠️ E4 |
| ★32/64 位观察：binder_size_t/binder_uintptr_t LP64=__u64, ILP32=__u32 | `#if defined(__64BIT)` 分支实测 | L101–L105 | ✓ |

### ③ 内核驱动内部（kernel/drivers/android/binder.c + binder_alloc.h）

| 图中字段 | 源码实际 | 行号 | 结论 |
|---|---|---|---|
| binder_proc（每进程一份）.threads (rb_root) | `struct rb_root threads` | binder.c L649 | ✓ |
| .nodes / .refs_by_desc (rb_root) | 同类型 | L653 / L655 | ✓ |
| .alloc | `struct binder_alloc alloc` | L701 | ✓ |
| .todo (list_head) / .pid | `list_head todo` / `int pid` | L686 / L669 | ✓ |
| binder_ref.desc / strong / weak | **嵌套在 `struct binder_ref_data data` 内**（ref 平铺只有 proc/node/death + rb 节点） | binder_ref_data L524–L529；binder_ref L548–L560 | ⚠️ E5 |
| binder_ref.node / proc / death | `binder_node *node` / `binder_proc *proc` / `binder_ref_death *death` | L556 / L555 / L559 | ✓ |
| binder_node.ptr / cookie | `binder_uintptr_t ptr / cookie` | L472 / L478 | ✓ |
| binder_node.proc / refs / async_todo | `binder_proc *proc` / `hlist_head refs` / `list_head async_todo` | L457 / L465 / L499 | ✓（refs 为 hlist_head） |
| binder_ref_death.work / cookie | `binder_work work` / `binder_uintptr_t cookie` | L503 / L518 | ✓ |
| binder_thread（每线程一份）.transaction_stack / looper / proc | `binder_transaction *transaction_stack` / `int looper` / `binder_proc *proc` | L783 / L777 / L761 | ✓ |
| binder_transaction.code / flags | `unsigned int code / flags` | L818 / L819 | ✓ |
| binder_transaction.sender_uid (kuid_t) | **不存在**；只有 `kuid_t sender_euid` | L823 | ❌ E2 |
| binder_transaction.to_proc / to_thread / buffer | 指针类型全对 | L813 / L814 / L817 | ✓ |
| binder_buffer.data (uint8[]) | **实际是 `void __user *user_data`**（4.9 已改为用户态指针，无 `data` 字段）；且图漏 `size_t extra_buffers_size` | binder_alloc.h L62 / L61 | ❌ E3 |
| binder_buffer.data_size / offsets_size / transaction / target_node | 全对 | L59–L60 / L55 / L56 | ✓ |
| binder_alloc.buffer (void __user*) / buffers (list_head) / free_buffers (rb_root) / vma | 全对；另补 `allocated_buffers` (rb_root) L116 | binder_alloc.h L113 / L114 / L115 / L110 | ✓ |

### 引用关系（箭头语义）核验

- `Parcel.mData ↔ binder_write_read.write_buffer`（值相等）：✓ talkWithDriver 把 mOut 数据指针填进 write_buffer。
- `BpBinder::transact → BC_TRANSACTION → tr->target.handle → binder_ref.desc`（返回值校验，经 rb-tree refs_by_desc 查 ref）：✓。
- `binder_transaction_data.data.ptr.buffer → binder_buffer`（数据拷贝，一次跨进程）：✓ copy_from_user 直达目标进程 mmap 区。
- `BR_TRANSACTION/BR_REPLY 由驱动回填、用户态不可构造`：✓。
- `IPCThreadState.mCallingPid/mCallingUid ← transaction_data.sender_pid/sender_euid`：✓ executeCommand 回填路径。

---

## 三、图上建议的修正文字（可直接改图）

1. **E1**：删掉 ① 栏 `mIsServing (status_t)`——Android 10 的 IPCThreadState 没有该成员（它是 Android 11 `isServing` 区分引入的）。
2. **E2**：③ 栏 binder_transaction 删 `sender_uid`，保留 `sender_euid (kuid_t)`；可补注 `binder_uintptr_t security_ctx`（L824，本仓带 txn_security_ctx backport）。
3. **E3**：③ 栏 binder_buffer 的 `data (uint8[])` 改为 `user_data (void __user*)`，并补 `extra_buffers_size (size_t)`。
4. **E4**：② 栏 flat_binder_object 的 cookie 类型 `__u64` 改为 `binder_uintptr_t`（与 32/64 位观察自洽）。
5. **E5**：③ 栏 binder_ref 画一层嵌套：`data (binder_ref_data){ debug_id, desc, strong, weak }` 外层再挂 `node / proc / death`。
6. **E6**：① 栏 BBinder.mExtras 类型标 `std::atomic<Extras*>`。

---

## 四、踩坑提示（读图时的三个误区）

1. **不要把 uapi 的 `binder_uintptr_t` 等同 `__u64`**：它是"按 ABI 位宽变化"的类型，ILP32 下是 `__u32`（L101–L105），这正是 32/64 位进程混合通信的兼容设计。
2. **内核结构体本仓是 4.9+backport 形态**：`binder_ref_data`/`spinlock_t lock`/`security_ctx`/`acontext[MAX_CONTEXT]` 都是 cells 树带的高版本 backport 或本地改造，对照上游 4.9 原始文件会有差异。
3. **binder_buffer 没有 `data` 成员**：老内核（3.x）曾有 `uint8_t *data`，4.9 已改为 `user_data`（用户态地址指针）；buffer 的"内容"在 mmap 区里，`binder_buffer` 只是记账头。
