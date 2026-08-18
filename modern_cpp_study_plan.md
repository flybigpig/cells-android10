# 现代 C++ 语法学习计划（C++11 / 14 / 17 / 20）

目标读者：已懂 C，正在通过本仓库 SurfaceFlinger 源码学习现代 C++。
本文按**标准版本**重新组织语法特性，作为 `cpp_learning_plan.md`（按主题分模块）的补充视角——那篇讲「概念是什么」，这篇讲「哪些特性属于哪个标准、何时引入、现在该用哪个」。

学习方式：每个标准挑「最常用 + 仓库里真会出现」的特性，附一句仓库对照。

---

## 〇、为什么按标准学

Android 10 的 SurfaceFlinger 编译目标通常是 **C++17**（部分模块 C++14/11 混用），所以你读到的代码里：
- C++11 特性（智能指针、Lambda、auto、移动语义、线程）——满地都是，是地基。
- C++14 特性（泛型 Lambda、`make_unique`）——少量但实用。
- C++17 特性（结构化绑定、`if constexpr`、内联变量、`std::optional`、`emplace` 增强）——你已见过 `for (const auto& [token, display] : mDisplays)` 就是 C++17 结构化绑定。
- C++20 特性（协程、概念 Concepts、Ranges、三路比较 `<=>`）——仓库里基本没有，但了解趋势有助于读新代码。

> 记忆：C++11 是「现代化分水岭」，C++14 是小修，C++17 是大补，C++20 是范式升级。

---

## 一、C++11（2011，现代 C++ 起点，必精通）

### 1.1 自动类型推导
- `auto`：编译器推导变量类型（你模块 8 学过）。仓库：`auto layer = make_shared<Layer>(...)`。
- `decltype`：推导表达式类型，用于模板和 `decltype(auto)` 完美转发场景。

### 1.2 智能指针（模块 6 全在此标准）
- `std::unique_ptr` / `shared_ptr` / `weak_ptr`，全部 C++11 引入。
- `std::make_shared` 也是 C++11（注意 `make_unique` 是 C++14）。
- 仓库：`make_shared<Layer>`、`make_shared<FenceTime>`、`weak_ptr` 破环。

### 1.3 移动语义与右值引用（模块 10）
- 右值引用 `T&&`、`std::move`、`std::forward`（完美转发）。
- 移动构造 `T(T&&)`、移动赋值。
- 仓库：`std::move(getVsyncPeriod)` 移入 Lambda、`make_shared<Layer>(..., std::move(args))`。

### 1.4 Lambda 表达式（模块 9）
- `[capture](params) -> ret { body }`，可赋值给 `std::function`。
- 仓库：`makeResyncCallback` 的 `[ptr, getVsyncPeriod = std::move(...)](){}`。

### 1.5 范围 for（模块 7）
- `for (auto& x : container)`。
- 仓库：`for (const auto& [token, display] : mDisplays)`（C++17 才加结构化绑定，但循环本身是 11）。

### 1.6 右值引用外的「统一初始化」
- 花括号初始化 `Type{...}`、`std::initializer_list`。
- 仓库：`std::vector<int> z = {5,1,9,3};`。

### 1.7 `nullptr`
- 替代 `NULL`/`0`，类型是 `std::nullptr_t`，避免重载歧义。

### 1.8 `= default` / `= delete`（模块 4）
- 控制默认/删除特殊成员函数。
- 仓库：`Buffer(const Buffer&) = delete`、`Scheduler` 内部状态禁拷贝。

### 1.9 `override` / `final`（模块 5）
- `override` 显式覆盖虚函数；`final` 禁止进一步覆盖。
- 仓库：`void speak() override`、`Layer::prepareClientLayer` 等虚函数覆盖。

### 1.10 强类型枚举 `enum class`
- 作用域限定 + 不隐式转 int，替代 C 的裸 enum。
- 仓库：`HWComposer::DisplayType`、`VsyncPeriod` 等大量 `enum class`。

### 1.11 线程与原子（模块 12）
- `<thread>`、`<mutex>`、`std::lock_guard`、`<atomic>`、`std::atomic`。
- 仓库：`lock_guard(mStateLock)`、`atomic<bool> mRefreshPending`。

### 1.12 其他常用
- `static_assert` 编译期断言。
- 变长模板 `template<typename... Args>`（SFINAE 基础）。
- `std::array` / `std::tuple` / `std::function` / `std::bind`（模块 9 用过 bind）。

---

## 二、C++14（2014，小修小补）

### 2.1 `std::make_unique`
- 补齐 `make_shared` 的对称（11 只有 `make_shared`）。仓库里 `make_unique` 也常见。

### 2.2 泛型 Lambda
- 参数可用 `auto`：`[](auto x, auto y){ return x+y; }`，无需手敲模板 Lambda。
- 仓库：很多工具 Lambda 直接写 `auto` 参数。

### 2.3 返回类型推导
- 函数 `auto foo() { return expr; }` 让编译器推导返回类型（配合 `decltype(auto)`）。

### 2.4 放宽 `constexpr`
- 11 的 `constexpr` 只能单行 return；14 允许局部变量、循环、条件，更接近「编译期计算」。

### 2.5 二进制字面量 & 数字分隔符
- `0b1010`、`1'000'000`（可读性）。

### 2.6 变量模板
- `template<typename T> constexpr T pi = T(3.14159);`。

---

## 三、C++17（2017，大补，Android 10 主用）

### 3.1 结构化绑定（你见过！）
- `auto [a, b] = pair;`，直接拆出成员。
- 仓库：`for (const auto& [token, display] : mDisplays)` 拆 map 的 key/value。

### 3.2 `if constexpr`
- 编译期条件分支，避免 SFINAE 地狱，写泛型更直观。

### 3.3 内联变量 `inline`
- `inline` 可用于变量，解决头文件里定义全局常量/变量的 ODR 问题（无需再用 `extern` 技巧）。

### 3.4 `std::optional<T>`
- 表示一个「可能有值也可能没有」的结果，替代「用特殊值或输出参数表示失败」。
- 仓库：很多查询函数（如找某图层/某显示）可返回 `optional`。

### 3.5 `std::variant` / `std::any`
- 类型安全的联合体（`variant`）、可存任意类型（`any`）。

### 3.6 折叠表达式（fold expressions）
- 变长模板的 `...` 简化：`(args + ...)` 求和。让可变参数模板更易写。

### 3.7 嵌套命名空间 `namespace A::B::C {}`
- 替代层层 `namespace A { namespace B { ... } }`。

### 3.8 文件系统 `<filesystem>`
- 标准库级路径/目录操作（Android 用得少，NDK 较新版本支持）。

### 3.9 `[[nodiscard]]` / `[[maybe_unused]]` 属性
- `[[nodiscard]]` 标记「返回值不能忽略」（如分配/错误码），忽略即警告。
- 仓库：不少返回错误码或资源的函数加了 `[[nodiscard]]`。

### 3.10 其他
- 保证拷贝消除（RVO 强化，省拷贝更确定）。
- `std::string_view`：只读字符串视图，避免无谓拷贝（性能利器，仓库里日志/解析常见）。
- 并行 STL 算法（`std::execution::par`）。

---

## 四、C++20（2020，范式升级，仓库暂无但值得了解）

### 4.1 概念 Concepts
- 给模板参数加约束：`template<C1 T>`，编译错误更友好，替代 SFINAE。
- 例：`template<std::integral T> T add(T a, T b)`。

### 4.2 范围库 Ranges
- 管道式惰性求值：`auto even = nums | std::views::filter(...)`。取代手写循环 + 算法组合。

### 4.3 协程 Coroutines
- `co_await`/`co_yield`/`co_return`，原生支持异步（对理解 future 的异步合成、调度器演进有用，但 SF 当前未用）。

### 4.4 三路比较运算符 `<=>`
- 「太空船运算符」自动生成 `==`/`<`/`>` 等全部比较，写比较函数极简。

### 4.5 模块 Modules
- 取代 `#include` 的文本替换（更快编译、无宏污染），但生态迁移慢，Android 暂未用。

### 4.6 其他
- `std::span`：连续序列的轻量视图（比 `string_view` 泛化到任意 T）。
- `constexpr` 虚函数、Lambda 捕获 `[=, this]` 修正。
- 原子智能指针 `std::atomic<std::shared_ptr<T>>`（无锁共享指针，对并发场景重要）。

---

## 五、学习路线建议（结合本仓库）

优先级排序（按「仓库出现频率 × 实用度」）：

1. **C++11 全套**（模块 1-12 已覆盖）——地基，必须熟。
2. **C++17 结构化绑定 + optional + string_view + nodiscard**——读 SF 代码常遇到。
3. **C++14 泛型 Lambda + make_unique**——顺手掌握。
4. **C++20 概念/Ranges/协程**——了解即可，等读更新代码或写新模块再用。

### 对照自检清单
- [ ] 看到 `auto` / `for (auto& x : c)` 立刻懂（11）
- [ ] 看到 `make_shared` / `weak_ptr::lock` 立刻懂（11）
- [ ] 看到 `std::move` 知道是转移而非拷贝（11）
- [ ] 看到 Lambda 捕获列表能说出 `[=]`/`[&]`/`[this]`（11）
- [ ] 看到 `override` / `enum class` 知道多态与强枚举（11）
- [ ] 看到 `for (const auto& [k, v] : map)` 知道是结构化绑定（17）
- [ ] 看到 `[[nodiscard]]` / `std::string_view` 知道含义（17）
- [ ] 能说出 Concepts / Ranges / 协程属于 20

### 与 cpp_learning_plan.md 的关系
- `cpp_learning_plan.md`：**按主题**讲概念（类、RAII、智能指针、Lambda…），配 12 个练习。
- 本文：**按标准版本**归类特性，帮你建立「这个语法是哪一年引入的、现在该不该用」的认知。
- 两者互补：先按主题练熟，再按标准查漏补缺。

---

## 六、小练习（按标准）

### C++11 练习：用 `enum class` + `nullptr` + `override` 写一个小状态机
**要求**：定义 `enum class State { Idle, Running, Stopped };`，写一个 `Machine` 类，虚函数 `tick()` 由子类 `TimerMachine` 用 `override` 实现；指针成员初始化用 `nullptr`。

### C++14 练习：泛型 Lambda + make_unique
**要求**：用泛型 Lambda `[](auto a, auto b){ return a + b; }` 分别加 int 和 string；用 `make_unique` 创建一个对象并 `std::move` 给另一个 `unique_ptr`。

### C++17 练习：结构化绑定 + optional + string_view
**要求**：(a) 遍历 `map<int,string>` 用 `[id, name]` 结构化绑定打印；(b) 写函数返回 `std::optional<int>`（查找成功返回値，失败返回 `std::nullopt`）；(c) 用 `std::string_view` 只读解析一个字符串而不拷贝。

### C++20 练习（纸上/新编译器）：Concepts + 三路比较
**要求**：用 `template<std::integral T>` 约束一个 `add` 函数；为一个 `Point` 类写 `auto operator<=>(const Point&) const = default;` 自动获得全部比较运算符。

---

## 七、参考资源
- cppreference.com（最权威的语法/标准查阅站）
- 《Effective Modern C++》（Scott Meyers）——11/14 惯用法必读
- 《C++20 The Complete Guide》（Nicolai Josuttis）——20 新特性
- 编译器支持表：https://en.cppreference.com/w/cpp/compiler_support
