#include "core/worker_pool.h"

#include <algorithm>

#include "platform/crash_log.h"

namespace hailstone::core {

namespace {

thread_local bool tl_on_worker = false;
std::atomic<WorkerPool*> g_workers{nullptr};

} // namespace

struct WorkerPool::Batch {
    void (*call)(void*, std::size_t, std::size_t) = nullptr;
    void* fn = nullptr;
    std::size_t count = 0;
    std::size_t grain = 1;
    std::atomic<std::size_t> next{0};
    std::atomic<bool> failed{false};
    std::exception_ptr error; // the first chunk's that threw (guarded by `failed`)
    std::mutex mutex;
    std::condition_variable done;
    std::size_t helpers = 0; // queued or running helpers not yet finished (guarded by mutex)
};

struct WorkerPool::Task::State {
    std::function<void()> work;
    std::mutex mutex;
    std::condition_variable done_cv;
    bool done = false;    // guarded by mutex
    std::exception_ptr error;
};

WorkerPool::WorkerPool(unsigned workers) {
    threads_.reserve(workers);
    for (unsigned i = 0; i < workers; ++i) threads_.emplace_back([this] { loop(); });
}

WorkerPool::~WorkerPool() {
    {
        std::lock_guard lock(mutex_);
        stopping_ = true;
    }
    wake_.notify_all();
    for (auto& t : threads_) t.join();
    // Tasks never started run here, so every handle's wait() returns.
    for (auto& job : queue_)
        if (job.task) run_task(*job.task);
    if (g_workers.load() == this) g_workers.store(nullptr);
}

bool WorkerPool::on_worker() { return tl_on_worker; }

void WorkerPool::work_on(Batch& batch) {
    for (;;) {
        const std::size_t begin = batch.next.fetch_add(batch.grain);
        if (begin >= batch.count) return;
        const std::size_t end = std::min(batch.count, begin + batch.grain);
        if (batch.failed.load(std::memory_order_relaxed)) continue; // the rest are skipped once one threw
        try {
            batch.call(batch.fn, begin, end);
        } catch (...) {
            bool expected = false;
            if (batch.failed.compare_exchange_strong(expected, true)) batch.error = std::current_exception();
        }
    }
}

void WorkerPool::run_batch(std::size_t count, std::size_t grain, void (*call)(void*, std::size_t, std::size_t),
                           void* fn) {
    Batch batch;
    batch.call = call;
    batch.fn = fn;
    batch.count = count;
    batch.grain = std::max<std::size_t>(grain, 1);
    // Helpers for the chunks beyond the caller's first, no more than there are workers.
    const std::size_t chunks = (count + batch.grain - 1) / batch.grain;
    const std::size_t helpers = std::min<std::size_t>(threads_.size(), chunks > 0 ? chunks - 1 : 0);
    batch.helpers = helpers;
    if (helpers > 0) {
        {
            std::lock_guard lock(mutex_);
            for (std::size_t i = 0; i < helpers; ++i) queue_.push_back({&batch, nullptr});
        }
        if (helpers == 1) wake_.notify_one();
        else wake_.notify_all();
    }
    work_on(batch);
    if (helpers > 0) {
        // The chunks are all taken: helpers no worker has started yet are not needed, and leave the queue.
        std::size_t removed = 0;
        {
            std::lock_guard lock(mutex_);
            auto mine = [&](const Job& j) { return j.batch == &batch; };
            removed = static_cast<std::size_t>(std::count_if(queue_.begin(), queue_.end(), mine));
            std::erase_if(queue_, mine);
        }
        std::unique_lock lock(batch.mutex);
        batch.helpers -= removed;
        batch.done.wait(lock, [&] { return batch.helpers == 0; });
    }
    if (batch.error) std::rethrow_exception(batch.error);
}

void WorkerPool::run_task(Task::State& state) {
    try {
        state.work();
    } catch (...) {
        state.error = std::current_exception();
    }
    state.work = nullptr; // what it held goes before wait() returns
    {
        std::lock_guard lock(state.mutex);
        state.done = true;
    }
    state.done_cv.notify_all();
}

WorkerPool::Task WorkerPool::run(std::function<void()> work) {
    auto state = std::make_shared<Task::State>();
    state->work = std::move(work);
    if (threads_.empty()) {
        run_task(*state);
        return Task(this, std::move(state));
    }
    {
        std::lock_guard lock(mutex_);
        queue_.push_back({nullptr, state});
    }
    wake_.notify_one();
    return Task(this, std::move(state));
}

bool WorkerPool::steal(const Task::State* state) {
    std::lock_guard lock(mutex_);
    auto it = std::find_if(queue_.begin(), queue_.end(), [&](const Job& j) { return j.task.get() == state; });
    if (it == queue_.end()) return false;
    queue_.erase(it);
    return true;
}

void WorkerPool::loop() {
    platform::CrashStack crash_stack;
    tl_on_worker = true;
    std::unique_lock lock(mutex_);
    for (;;) {
        wake_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
        if (stopping_) return;
        Job job = std::move(queue_.front());
        queue_.pop_front();
        lock.unlock();
        if (job.batch) {
            Batch& batch = *job.batch;
            work_on(batch);
            // The caller frees the batch once helpers reaches 0: the count and the notice go under its mutex, which
            // holds the caller back until this thread has let go of the batch for good.
            std::lock_guard batch_lock(batch.mutex);
            --batch.helpers;
            batch.done.notify_all();
        } else if (job.task) {
            run_task(*job.task);
        }
        lock.lock();
    }
}

WorkerPool::Task& WorkerPool::Task::operator=(Task&& other) noexcept {
    if (this != &other) {
        if (state_) {
            try {
                wait();
            } catch (...) {
            }
        }
        pool_ = other.pool_;
        state_ = std::move(other.state_);
        other.pool_ = nullptr;
    }
    return *this;
}

WorkerPool::Task::~Task() {
    if (!state_) return;
    try {
        wait();
    } catch (...) {
    }
}

void WorkerPool::Task::wait() {
    if (!state_) return;
    auto state = std::move(state_);
    if (pool_ && pool_->steal(state.get())) {
        run_task(*state);
    } else {
        std::unique_lock lock(state->mutex);
        state->done_cv.wait(lock, [&] { return state->done; });
    }
    if (state->error) std::rethrow_exception(state->error);
}

unsigned default_worker_count() {
    const unsigned hardware = std::thread::hardware_concurrency();
    if (hardware <= 1) return 0;
    return std::min(hardware - 1, 6u);
}

WorkerPool* workers() { return g_workers.load(std::memory_order_acquire); }

void set_workers(WorkerPool* pool) { g_workers.store(pool, std::memory_order_release); }

} // namespace hailstone::core
