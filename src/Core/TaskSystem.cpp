#include "Core/TaskSystem.h"
#include <mutex>

namespace mf {

Task::Task(std::string name, WorkFunc work)
    : m_name(std::move(name)), m_work(std::move(work)) {}

void Task::run() {
    if (m_status.load() != Status::Pending) return;
    m_status.store(Status::Running);
    try {
        m_work(this);
        if (!m_cancelled.load()) {
            m_status.store(Status::Completed);
            m_progress.store(1.0f);
        } else {
            m_status.store(Status::Cancelled);
        }
    } catch (...) {
        m_status.store(Status::Failed);
    }
}

const std::string& Task::message() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_message;
}

void Task::setMessage(std::string msg) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_message = std::move(msg);
}

#ifndef MF_HAS_TBB
TaskSystem::TaskSystem() {
    unsigned int n = std::thread::hardware_concurrency();
    if (n == 0) n = 4;
    for (unsigned int i = 0; i < n; ++i) {
        m_workers.emplace_back(&TaskSystem::workerLoop, this);
    }
}

TaskSystem::~TaskSystem() {
    {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        m_stop = true;
    }
    m_cv.notify_all();
    for (auto& t : m_workers) {
        if (t.joinable()) t.join();
    }
}

void TaskSystem::workerLoop() {
    while (true) {
        std::shared_ptr<Task> task;
        {
            std::unique_lock<std::mutex> lock(m_queueMutex);
            m_cv.wait(lock, [this] { return m_stop || !m_queue.empty(); });
            if (m_stop && m_queue.empty()) return;
            task = std::move(m_queue.front());
            m_queue.pop();
        }
        if (task) task->run();
    }
}
#else
TaskSystem::TaskSystem() = default;
TaskSystem::~TaskSystem() = default;
#endif

TaskSystem& TaskSystem::instance() {
    static TaskSystem instance;
    return instance;
}

void TaskSystem::submit(std::shared_ptr<Task> task) {
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_tasks.push_back(task);
    }
#ifdef MF_HAS_TBB
    m_group.run([task]() { task->run(); });
#else
    {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        m_queue.push(task);
    }
    m_cv.notify_one();
#endif
}

void TaskSystem::waitAll() {
#ifdef MF_HAS_TBB
    m_group.wait();
#else
    while (true) {
        std::unique_lock<std::mutex> lock(m_queueMutex);
        if (m_queue.empty()) break;
        lock.unlock();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    for (auto& t : m_workers) {
        // Wait for active workers to finish
    }
#endif
}

std::vector<std::shared_ptr<Task>> TaskSystem::tasks() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_tasks;
}

} // namespace mf
