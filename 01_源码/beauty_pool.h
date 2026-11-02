#pragma once
// beauty_pool —— 进程级持久线程池（只服务「按行/按列切块」这一种并行）
//
// 为什么需要它：磨皮的合成段一帧要跑 21 趟按行循环（3 个盒式 × 3 次迭代 ×
// 2 个方向 + 3 趟单程）。原来的写法是**每一趟都 new 一组 std::thread 再 join**，
// 1080p + 8 线程时一帧就是 168 次线程创建/销毁。
//
// 实测（06_离线验证\功耗_20261030\spawn_bench.exe，本机 Core Ultra 7 255HX）：
//     8 线程 0.290 ms/批 × 21 趟 = **6.08 ms/帧**
// 而整帧磨皮本身只要 18.28 ms —— 三分之一的算力花在建线程上。
//
// 改成持久池之后，线程在第一次用到时创建、之后只在条件变量上睡觉；
// 每一趟只付「一次 notify + 一次等待」的代价（微秒级）。
//
// 线程数语义与原来的 effectiveThreads 保持一致：**n 是总执行体数，调用线程
// 自己算一个**，所以池里只放 n-1 个 worker。
//
// 线程安全：run() 内部串行化，允许被多个线程同时调用（会排队），但正常情况
// 只有渲染线程在用。回调 fn 只在该调用期间被引用，返回后立即失效。

#include <functional>

namespace beautycore {

// 把 [y0, y1) 切成不超过 n 块并并行执行 f(begin, end)。
// n <= 1 或区间太小时直接在调用线程里跑完，不进池。
void parallelRows(int y0, int y1, int n, const std::function<void(int, int)>& f);

// 关闭池并 join 所有 worker。插件卸载 / 引擎 release 时调用；
// 不调用也不会崩（析构里会兜底），但 DLL 卸载时线程还活着是未定义行为。
void poolShutdown();

// 预建 n 个执行体（同上语义：n 含调用线程）。模块加载时调一次，
// 首帧就不用再付线程创建的代价（实测 7 个线程约 0.3 ms + 首次栈缺页）。
// 只是把线程提前建好，不改变任何计算结果。
void poolReserve(int n);

// 诊断：当前池里活着的 worker 数（日志/基准用）
int poolWorkerCount();

}  // namespace beautycore
