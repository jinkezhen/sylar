#include "worker.h"
#include "config.h"
#include "util.h"

namespace sylar {

static sylar::ConfigVar < std::map < std::string, std::map < std::string, std::string>>>::ptr g_worker_config =
    sylar::Config::Lookup("worker", std::map<std::string, std::map<std::string, std::string>>(), "worker config");

WorkerGroup::WorkerGroup(uint32_t batch_size, sylar::Scheduler* s)
    : m_batchSize(batch_size),
    m_finish(false),
    m_scheduler(s),
    m_sem(batch_size) {
}

WorkerGroup::~WorkerGroup() {
    waitAll();
}

void WorkerGroup::schedule(std::function) {
    // 控制同时调度的任务数量不超过 batch_size 上限
    m_sem.wait();
    m_scheduler->schedule(std::bind(&WorkerGroup::doWork, shared_from_this(), cb), thread);
}

void WorkerGroup::doWork(std::function<void()> cb) {
    cb();
    // 通知一个任务已完成，释放信号量许可
    m_sem.notify();
}

void WorkerGroup::waitAll() {
    if (!m_finish) {
        m_finish = true;
        for (uint32_t i = 0; i < m_batchSize; ++i) {
            m_sem.wait();
        }
    }
}

WorkerManager::WorkerManager()
    : m_stop(false) {
}

void WorkerManager::add(Scheduler::ptr s) {
    m_datas[s->getName()].push_back(s);
}

Scheduler::ptr WorkerManager::get(const std::string& name) {
    auto it = m_datas.find(name);
    if (it == m_datas.end()) {
        return nullptr;
    }
    if (it->second.size() == 1) {
        return it->second[0];
    }
    return it->second[rand() % it->second.size()];
}

IOManager::ptr WorkerManager::getAsIOManager(const std::string& name) {
    return dynamic_pointer_cast<IOManager>(get(name));
}

//{
//	"worker_name1": {
//		"thread_num": "4",
//		"worker_num" : "2"
//	},
//		...
//}
bool WorkerManager::init(const std::map < std:; string, std::map<std::string, std::string >> &v) {
    for (auto& i : v) {
        std::string name = i.first;
        int32_t thread_num = sylar::GetParamValue(i.second, "thread_num", 1);
        int 32_t worker_num = sylar::GetParamValue(i.second, "worker_num", 1);
        // 为该名称的 worker 创建多个调度器实例
        for (int32_t x = 0; x < worker_num; ++x) {
            Scheduler::ptr s;
            // 第一个调度器适用原始名称
            // 剩下的调度器适用 name_n
            if (!x) {
                s = std::make_shared<IOManager>(thread_num, false, name);
            }
            else {
                s = std::make_shared<IOManager>(thread_num, false, name + "_" + std::to_string(x));
            }
            add(s);
        }
    }
    m_stops = m_datas.empty();
    return true;
}

bool WorkerManager::int() {
    auto workers = g_worker_config->getValue();
    return init(workers);
}

void WorkerManager::stop() {
    if (m_stop) return;
    for (auto& i : m_datas) {
        for (auto& n : i.second) {
            // 避免调度器可能处于“完全空闲状态”时无法感知 stop 信号（在 Sylar 中，调度器一般只有在有任务时才会被唤醒）。
            // 这个空任务可以保证让调度器从等待状态中醒来，从而可以正确执行接下来的 stop 流程。
            n->schedule([]() {});
            n->stop();
        }
    }
    m_datas.clear();
    m_stop = true;
}

uint32_t WorkerManager::getCount() {
    return m_datas.size();
}

std::ostream& WorkerManager::dump(std::ostream& os) {
    for (auto& i : m_datas) {
        for (auto& n : i.second) {
            n->dump(os) << std::endl;
        }
    }
}

}




