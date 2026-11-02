// beauty_pool.cpp —— 持久线程池的实现。设计说明见 beauty_pool.h。
//
// 取块协议（一个「代」= 一次 parallelRows 调用）：
//   run()      置 job/cursor/y1/chunk，busy = 块数，gen++，notify_all；
//   worker     等 gen 变化 → 循环「锁内取一块、锁外算」，每算完一块 busy--；
//   run()      自己也取块（调用线程是第一个执行体），然后等 busy == 0。
//
// 这样做的两个好处：
//   · 块数而不是线程数计忙：某个 worker 迟迟没被唤醒时，其它执行体会把它的块
//     领走，不会出现「等了半天只跑了一半」；
//   · job 指针在 busy==0 之前一直有效，worker 只要在锁内确认 cursor < y1 就
//     一定还在本次调用之内，不存在悬空回调。

#include "beauty_pool.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

namespace beautycore {
namespace {

class Pool {
public:
    static Pool& inst()
    {
        // 函数内静态：C++11 起初始化线程安全，且**不会**在 DLL 卸载前被销毁
        // （进程退出时才析构）—— 比全局对象安全，卸载顺序不受它影响。
        static Pool p;
        return p;
    }

    void run(int y0, int y1, int n, const std::function<void(int, int)>& f)
    {
        const int rows = y1 - y0;
        if (rows <= 0) return;
        if (n <= 1 || rows < 2) {
            f(y0, y1);
            return;
        }
        int nt = std::min(n, rows);

#ifdef BEAUTY_POOL_LEGACY_SPAWN
        // ------------------------------------------------------------------
        // A/B 专用分支：原样复刻 3.1.20 的写法（每次调用新建一组 std::thread
        // 再 join）。只在离线对照 DLL 里编译，插件正常构建永远走下面的池。
        // ------------------------------------------------------------------
        {
            std::vector<std::thread> th;
            const int per = (rows + nt - 1) / nt;
            for (int i = 0; i < nt; i++) {
                const int b = i * per;
                if (b >= rows) break;
                int e = b + per;
                if (e > rows) e = rows;
                th.emplace_back([&f, y0, b, e] { f(y0 + b, y0 + e); });
            }
            for (size_t i = 0; i < th.size(); i++) th[i].join();
        }
        return;
#endif

        // 并发调用排队：正常只有一个调用者，加锁几乎无成本（未争用）。
        std::lock_guard<std::mutex> serialize(run_);

        ensureWorkers(nt);

        {
            std::lock_guard<std::mutex> lk(m_);
            job_ = &f;
            y1_ = y1;
            cursor_ = y0;
            chunk_ = (rows + nt - 1) / nt;
            busy_.store(nt, std::memory_order_relaxed);
            ++gen_;
        }
        cvJob_.notify_all();

        // 调用线程也干活，干完自己的再一起等
        takeAndRun();
        {
            std::unique_lock<std::mutex> lk(m_);
            cvDone_.wait(lk, [this] {
                return busy_.load(std::memory_order_acquire) == 0;
            });
            job_ = nullptr;   // 本代结束：之后醒来的 worker 会立刻退出去等下一代
        }
    }

    void shutdown()
    {
        // 持 run_：万一有别的线程正在 run() 里算，等它跑完再拆，不能拆正在用的池。
        std::lock_guard<std::mutex> serialize(run_);
        {
            std::lock_guard<std::mutex> lk(m_);
            if (th_.empty()) return;      // 没建过线程，什么都不用做
            quit_ = true;
        }
        cvJob_.notify_all();
        for (size_t i = 0; i < th_.size(); i++)
            if (th_[i].joinable()) th_[i].join();
        th_.clear();
        // quit_ 复位：池是进程级单例，插件被卸载再加载（OBS 热重载场景）后
        // 还要能重新建线程。不清零的话新 worker 一进循环就看到 quit_，立刻退出，
        // 而 run() 又永远等不到块被执行 —— 死锁。
        std::lock_guard<std::mutex> lk(m_);
        quit_ = false;
    }

    int workerCount() const { return (int)th_.size(); }

    // 预建 worker（模块加载时用）。持 run_ 与 ensureWorkers 同一条串行化路径。
    void reserve(int n)
    {
        std::lock_guard<std::mutex> serialize(run_);
        ensureWorkers(n);
    }

    ~Pool() { shutdown(); }

private:
    void ensureWorkers(int nt)
    {
        // 调用线程占一个执行体，池里只放 nt-1 个
        const size_t want = (nt > 1) ? (size_t)(nt - 1) : 0;
        while (th_.size() < want) {
            // 起点必须**在创建时**取：如果让线程自己进去才读 gen_，它会读到
            // 已经递增过的值（本代），于是「等新一代」永远等不到 —— 实测直接
            // 死锁。这里读 gen_ 与下面的 gen_++ 都在 run_ 里，中间不会有人改。
            unsigned long long base;
            {
                std::lock_guard<std::mutex> lk(m_);
                base = gen_;
            }
            th_.emplace_back([this, base] { worker(base); });
        }
    }

    // 锁内取一块，锁外算；没有可取的块就返回 false
    bool takeAndRun()
    {
        const std::function<void(int, int)>* job = nullptr;
        int b = 0, e = 0;
        {
            std::lock_guard<std::mutex> lk(m_);
            if (!job_ || cursor_ >= y1_) return false;
            job = job_;
            b = cursor_;
            e = std::min(cursor_ + chunk_, y1_);
            cursor_ = e;
        }
        (*job)(b, e);
        {
            std::lock_guard<std::mutex> lk(m_);
            if (busy_.fetch_sub(1, std::memory_order_acq_rel) == 1)
                cvDone_.notify_one();
        }
        return true;
    }

    void worker(unsigned long long seen)
    {
        for (;;) {
            {
                std::unique_lock<std::mutex> lk(m_);
                cvJob_.wait(lk, [this, seen] { return quit_ || gen_ != seen; });
                if (quit_) return;
                seen = gen_;
            }
            while (takeAndRun()) {
                // 同一代里继续领块，直到没有剩余
            }
        }
    }

    std::mutex run_;                  // 串行化并发的 parallelRows 调用
    std::mutex m_;
    std::condition_variable cvJob_;
    std::condition_variable cvDone_;
    const std::function<void(int, int)>* job_ = nullptr;
    int y1_ = 0;
    int cursor_ = 0;
    int chunk_ = 1;
    std::atomic<int> busy_{0};
    unsigned long long gen_ = 0;
    bool quit_ = false;
    std::vector<std::thread> th_;
};

}  // namespace

void parallelRows(int y0, int y1, int n, const std::function<void(int, int)>& f)
{
    Pool::inst().run(y0, y1, n, f);
}

void poolShutdown() { Pool::inst().shutdown(); }

void poolReserve(int n)
{
    if (n <= 1) return;
    Pool::inst().reserve(n);
}

int poolWorkerCount() { return Pool::inst().workerCount(); }

}  // namespace beautycore
