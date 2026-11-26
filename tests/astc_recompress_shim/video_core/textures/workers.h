// test shim: runs queued work at once on the calling thread (the real one is a thread pool; the work items are independent)
#pragma once
#include <functional>
namespace Common {
class ThreadWorker {
public:
    template <class F> void QueueWork(F&& f) { f(); }
    void WaitForRequests() {}
};
}
namespace Tegra::Texture { inline Common::ThreadWorker& GetThreadWorkers() { static Common::ThreadWorker w; return w; } }
