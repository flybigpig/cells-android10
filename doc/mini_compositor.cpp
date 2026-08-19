// 迷你图层合成器 —— 综合小项目（串起 cpp_learning_plan.md 全部 12 个模块）
//
// 编译：g++ -std=c++17 mini_compositor.cpp -o mini_compositor && ./mini_compositor
//
// 它把 SurfaceFlinger 的核心数据结构简化成玩具版：
//   - Layer 抽象基类（纯虚 compose()，模块 5），派生 BufferLayer / ColorLayer（模块 1/4/5）
//   - 用 std::vector<std::shared_ptr<Layer>> 管理图层栈（模块 6/7）
//   - Display 用 unique_ptr 持有后端 buffer + atomic<bool> 标记脏（模块 6/12）
//   - 用 Lambda 写「按 Z 序排序 + 遍历合成」的回调（模块 9）
//   - 成员用初始化列表 + =delete 拷贝（模块 4）
//
// 注意：这是教学示意，不是真实 SurfaceFlinger（真实代码有 HWC/GL/VSync 等）。

#include <algorithm>
#include <atomic>
#include <iostream>
#include <memory>
#include <string>
#include <vector>


//#include "SF启动链与mini_compositor映射"

// ---------- 模块 5：纯虚接口 + 多态 ----------
// Layer 抽象基类，定义「图层该提供什么能力」
class Layer {
public:
    int z = 0;                 // Z 序，越大越靠上
    std::string name;

    Layer(std::string n, int z_) : name(std::move(n)), z(z_) {}  // 模块 4：初始化列表 + std::move
    virtual ~Layer() = default;                                  // 模块 5：基类虚析构

    // 纯虚函数：具体图层各自实现如何「合成」
    virtual void compose() const = 0;

    // 模块 4：禁止拷贝（图层不应被浅拷贝，避免双释放）
    Layer(const Layer&) = delete;
    Layer& operator=(const Layer&) = delete;
};

// 模块 1/4/5：BufferLayer —— 带图形缓冲的图层
class BufferLayer : public Layer {
public:
    BufferLayer(std::string n, int z_) : Layer(std::move(n), z_) {}
    void compose() const override {  // 模块 5：override 覆盖虚函数
        std::cout << "  [BufferLayer] " << name << " z=" << z
                  << "  -> GL draw buffer\n";
    }
};

// 模块 1/4/5：ColorLayer —— 纯色图层
class ColorLayer : public Layer {
    uint32_t color;  // 0xRRGGBB
public:
    ColorLayer(std::string n, int z_, uint32_t c)
        : Layer(std::move(n), z_), color(c) {}
    void compose() const override {
        std::cout << "  [ColorLayer ] " << name << " z=" << z
                  << "  -> fill color 0x" << std::hex << color << std::dec << "\n";
    }
};

// ---------- 模块 6/12：Display 持有后端 + 原子脏标记 ----------
class Display {
    // 模块 6：unique_ptr 独占后端 buffer（这里用 string 假装一块显存）
    std::unique_ptr<std::string> mBackBuffer;
    // 模块 12：atomic<bool> 标记「需要重绘」
    std::atomic<bool> mDirty{false};

public:
    // 模块 4：初始化列表装配
    explicit Display(std::string bufferName)
        : mBackBuffer(std::make_unique<std::string>(std::move(bufferName))) {
        mDirty = true;
    }

    void markDirty() { mDirty = true; }                 // 原子写
    bool consumeDirty() { return mDirty.exchange(false); }  // 模块 12：原子取清零

    void present() const {
        std::cout << "[Display] present backbuffer='" << *mBackBuffer << "'\n";
    }

    // Display 独占资源，删拷贝
    Display(const Display&) = delete;
    Display& operator=(const Display&) = delete;
    // 允许移动（模块 10）
    Display(Display&&) = default;
    Display& operator=(Display&&) = default;
};

// ---------- 模块 6/7：图层栈用 shared_ptr 管理 ----------
// 模块 7：vector 存 shared_ptr；模块 9：Lambda 做 Z 序排序
void compositeFrame(std::vector<std::shared_ptr<Layer>>& layers) {
    std::cout << "[Compositor] sorting by Z...\n";
    // 模块 9：Lambda 比较器，按 z 升序（小的在底下）
    std::sort(layers.begin(), layers.end(),
              [](const std::shared_ptr<Layer>& a, const std::shared_ptr<Layer>& b) {
                  return a->z < b->z;
              });

    std::cout << "[Compositor] drawing bottom -> top:\n";
    // 模块 7：范围 for + auto
    for (const auto& layer : layers) {
        layer->compose();   // 模块 5：多态调用，自动派发到 Buffer/Color
    }
}

int main() {
    std::cout << "=== mini compositor ===\n";

    // 模块 6：make_shared 管理图层，可被多处引用（这里图层栈 + 临时引用）
    auto bg = std::make_shared<ColorLayer>("background", 0, 0x000000);
    auto app = std::make_shared<BufferLayer>("app-window", 10);
    auto dim = std::make_shared<ColorLayer>("dim-layer", 20, 0x22000000);

    // 模块 7：vector 图层栈
    std::vector<std::shared_ptr<Layer>> stack;
    stack.emplace_back(bg);    // 模块 7：emplace_back 原地构造，无临时对象
    stack.emplace_back(app);
    stack.emplace_back(dim);

    // 模块 6：验证引用计数（图层栈持有 1 份，下面 alias 再 +1）
    auto alias = app;  // shared_ptr 拷贝，use_count=2
    std::cout << "[main] app use_count=" << app.use_count() << "\n";  // 2

    Display screen("framebuffer-0");
    screen.markDirty();

    // 模块 12：原子脏标记，消费一次
    if (screen.consumeDirty()) {
        compositeFrame(stack);  // 模块 9：Lambda 排序 + 模块 5 多态合成
        screen.present();
    }

    // 模块 6：app 离开作用域，alias 仍持有；stack 析构后计数归零自动 delete
    std::cout << "[main] after stack, app use_count=" << app.use_count() << "\n";  // 1
    return 0;
}
