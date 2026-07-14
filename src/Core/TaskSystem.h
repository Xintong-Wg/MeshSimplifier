#pragma once

#include <functional>
#include <future>
#include <cstring>
#include <vector>
#include <atomic>
#include <mutex>
#include <thread>
#include <queue>
#include <condition_variable>

#ifdef MF_HAS_TBB
#include <tbb/task_group.h>
#endif

namespace mf {

// ------------------------------------------------------------------
// Async task wrapper with progress reporting
// ------------------------------------------------------------------
class Task {
public:
    enum class Status { Pending, Running, Completed, Failed, Cancelled };

    using WorkFunc = std::function<void(Task*)>;

    Task(std::string name, WorkFunc work);

    void run();
    void cancel() { m_cancelled.store(true); }
    bool isCancelled() const { return m_cancelled.load(); }

    Status status() const { return m_status; }
    const std::string& name() const { return m_name; }
    float progress() const { return m_progress.load(); }
    void setProgress(float p) { m_progress.store(p); }
    const std::string& message() const;
    void setMessage(std::string msg);

private:
    std::string m_name;
    WorkFunc m_work;
    std::atomic<Status> m_status{Status::Pending};
    std::atomic<float> m_progress{0.0f};
    std::atomic<bool> m_cancelled{false};
    mutable std::mutex m_mutex;
    std::string m_message;
};

// ------------------------------------------------------------------
// Thread pool backed by TBB or std::thread
// ------------------------------------------------------------------
class TaskSystem {
public:
    static TaskSystem& instance();

    void submit(std::shared_ptr<Task> task);
    void waitAll();
    std::vector<std::shared_ptr<Task>> tasks() const;

private:
    TaskSystem();
    ~TaskSystem();

#ifdef MF_HAS_TBB
    tbb::task_group m_group;
#else
    void workerLoop();
    std::vector<std::thread> m_workers;
    std::queue<std::shared_ptr<Task>> m_queue;
    std::mutex m_queueMutex;
    std::condition_variable m_cv;
    bool m_stop = false;
#endif

    std::vector<std::shared_ptr<Task>> m_tasks;
    mutable std::mutex m_mutex;
};

} // namespace mf
