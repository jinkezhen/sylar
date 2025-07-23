#include "load_balance.h"
#include "sylar/log.h"
#include "sylar/worker.h"
#include "sylar/macro.h"
#include <math.h>

namespace sylar {

static sylar::Logger::ptr g_logger = SYLAR_LOG_NAME("system");

std::string HolderStats::toString() {
    std::stringstream ss;
    ss << "[Stat total=" << m_total
       << " used_time=" << m_usedTime
       << " doing=" << m_doing
       << " timeouts=" << m_timeouts
       << " oks=" << m_oks
       << " errs=" << m_errs
       << " oks_rate=" << (m_total ? (m_oks * 100.0 / m_total) : 0)
       << " errs_rate=" << (m_total ? (m_errs * 100.0 / m_total) : 0)
       << " avg_used=" << (m_oks ? (m_usedTime * 1.0 / m_oks) : 0)
       << " weight=" << getWeight(1)
       << "]";
    return ss.str();    
}

void HolderStats::clear() {
    m_usedTime = 0;
    m_total = 0;
    m_doing = 0;
    m_timeouts = 0;
    m_oks = 0;
    m_errs = 0;
}

float HolderStats::getWeight(float rate) {
    float base = m_total + 20;
    return std::min((m_oks * 1.0 / (m_usedTime + 1)) * 2.0, 50.0)
        * (1 - 4.0 * m_timeouts / base) 
        * (1 - 1 * m_doing / base)
        * (1 - 10.0 * m_errs / base) * rate;
}

HolderStatsSet::HolderStatsSet(uint32_t size) {
    m_stats.resize(size);
}

void HolderStatsSet::init(const uint32_t& now) {
    if (m_lastUpdateTime < now) {
        for (uint32_t t = m_lastUpdateTime + 1, i = 0;
                      t < now && i < m_stats.size();
                      ++t, ++i) {
            m_stats[t % m_stats.size()].clear();
        }
        m_lastUpdateTime = now;
    }
} 

HolderStats& HolderStats::get(const uint32_t& now) {
    init(now);
    return m_stats[now % m_stats.size()];
} 

float HolderStatsSet::getWeight(const uint32_t& now) {
    init(now);
    int v;
    for (int i = 1; i < m_stats.size(); ++i) {
        v += m_stats[(now - i) % m_stats.size()].getWeight(1 - 0.1 * i);
    }
    return v;
}

HolderStats HolderStatsSet::getTotal() {
    HolderStats rt;
    for (auto& i : m_stats) {
#define XX(f) rt.f += i.f
        XX(m_usedTime);
        XX(m_total);
        XX(m_doing);
        XX(m_timeouts);
        XX(m_oks);
        XX(m_errs);    
#undef XX    
    }
    return rt;
}

void LoadBalanceItem::isValid() {
    return m_stream && m_stream->isConnected();
}

// 为什么要“异步”关闭？
// 直接调用 stream->close() 不也可以吗？为什么要扔到线程池？
// 答案是：线程安全 + 避免死锁/阻塞 + 保持 IO 封装一致性
// 原因如下：
// SocketStream 的关闭操作可能涉及 IO 管理器（如 IOManager 的 epoll 注销等）；
// 如果当前线程不是 IOManager 的线程，直接操作会出问题；
// 比如你当前在 service_logic 线程中关闭连接，但连接是在 service_io 注册的 epoll，这就会乱套。
// 防止与读写线程产生竞争条件
// 异步读写可能正在运行中，此时直接关闭连接，容易引发并发错误；
// 所以统一由专属线程（service_io）进行 IO 管理操作更安全。
// 保持封装层次清晰
// 所有 socket 的 IO 操作都应交给 IO 线程完成。
void LoadBalanceItem::close() {
    if (m_stream) {
        auto stream = m_stream;
        sylar::WorkerMgr::GetInstance()->schedule("service_io", [stream]() { 
            stream->close();
        });
    }
}

std::string LoadBalanceItem::toString() {
    std::stringstream ss;
    ss << "[Item id=" << m_id
       << " weight=" << getWeight();
    if(!m_stream) {
        ss << " stream=null";
    } else {
        ss << " stream=[" << m_stream->getRemoteAddressString()
           << " is_connected=" << m_stream->isConnected() << "]";
    }
    ss << m_stats.getTotal().toString() << "]";
    return ss.str();    
}


LoadBalanceItem::ptr LoadBalance::getById(uint64_t id) {
    RWMutexType::ReadLock lock(m_mutex);
    auto it = m_datas.find(id);
    return it == m_datas.end() ? nullptr : it->second;
}

void LoadBalance::add(LoadBalanceItem::ptr v) {
    RWMutexType::WriteLock lock(m_mutex);
    m_datas[v->getId()] = v;
    initNolock();
}

void LoadBalance::del(LoadBalanceItem::ptr v) {
    RWMutexType::WriteLock lock(m_mutex);
   m_datas.erase(v->getId());
    initNolock();
}

void LoadBalance::update(const std::unordered_map<uint64_t, LoadBalanceItem::ptr>& adds,
                            std::unordered_map<uint64_t, LoadBalanceItem::ptr>& dels) {
    RWMutexType::WriteLock lock(m_mutex);
    for (auto& i : dels) {
        auto it = m_datas.find(i.first);
        if (it != m_datas.end()) {
            i.second = it->second;
            m_datas.erase(it);
        }
    }
    for (auto& i : adds) {
        m_datas[i.first] = i.second;
    }
    initNolock();
}

void LoadBalance::set(const std::vector<LoadBalanceItem::ptr>& vs) {
    RWMutexType::WriteLock lock(m_mutex);
    m_datas.clear();
    for(auto& i : vs){
        m_datas[i->getId()] = i;
    }
    initNolock();
}

void LoadBalance::init() {
    RWMutexType::WriteLock lock(m_mutex);
    initNolock();
}

std::string LoadBalance::statusString(const std::string& prefix) {
    RWMutexType::ReadLock lock(m_mutex);
    decltype(m_datas) datas = m_datas;
    lock.unlock();
    std::stringstream ss;
    ss << prefix << "init_time: " << sylar::Time2Str(m_lastInitTime / 1000) << std::endl;
    for(auto& i : datas) {
        ss << prefix << i.second->toString() << std::endl;
    }
    return ss.str();    
}

void LoadBalance::checkInit() {
    uint64_t ts = sylar::GetCurrentMS();
    if (ts - m_lastInitTime > 500) {
        init();
        m_lastInitTime = ts;
    }
}

void RoundRobinLoadBalance::initNolock() {
    decltype(m_items) items;
    for (auto& i : m_datas) {
        if (i.second->isValid()) {
            items.push_back(i.second)
        }
    }
    items.swap(m_items);
}

LoadBalanceItem::ptr RoundRobinLoadBalance::get(uint64_t v) {
    checkInit();
    RWMutexType::ReadLock lock(m_mutex);
    if (m_items.empty()) return nullptr;
    uint32_t r = (v == (uint64_t)-1 ? rand() : v) % m_items.size();
    for (size_t i = 0; i < m_items.size(); ++i) {
        auto& h = m_items[(r + i) % m_items.size()];
        if (h->isValid()) {
            return h;
        }
    }
    return nullptr;
}

FairLoadBalanceItem::ptr WeightLoadBalance::getAsFair() {
    auto item = get();
    if (item) {
        return std::static_pointer_cast<FairLoadBalanceItem>(item);
    }
    return nullptr;
}

void WeightLoadBalance::::initNolock() {
    decltype(m_items) items;
    for (auto& i : m_datas) {
        if (i.second->isValid()) {
            items.push_back(i.second);
        }
    }
    items.swap(m_items);
    // | 随机值范围 | 对应连接项 |
    // | -----     | -----   |
    // | 1\~3      | 第 0 个 |
    // | 4\~8      | 第 1 个 |
    // | 9\~10     | 第 2 个 |
    int total = 0;
    m_weights.resize(m_items.size());
    for (size_t i = 0; i < m_items.size(); ++i) {
        // 此处如果是FairLoadBalanceItem，则会调用其重新的getWeight，重写的getWeight同时考虑了静态权重和动态权重
        total += m_items[i]->getWeight();
        // 根据连接项的权重计算前缀和
        m_weights[i] = total;
    }
}

// 根据传入值 v（或者随机数），在总权重空间 [0, total) 中选择一个落点 dis，然后通过二分查找在 m_weights 中找到第一个 比 dis 大的位置，即对应的节点索引，从而实现按权重比例进行选择。
int32_t WeightLoadBalance::getIdx(uint64_t v) {
    if (m_weights.empty()) {
        return -1;
    }
    int64_t total = *m_weights.rbegin();
    // 生成一个随机落点
    uint64_t dis = (v == (uint64_t)-1 ? rand() : v) % total;
    // m_weights = [10, 30, 60]
    // dis = 25 → it 指向 30，对应索引 1
    auto it = std::upper_bound(m_weights.begin(), m_weights.end(), dis);
    SYLAR_ASSERT(it != m_weights.end());
    return std::distance(m_weights.begin(), it);
}

LoadBalanceItem::ptr WeightLoadBalance::get(uint64_t v) {
    checkInit();
    RWMutexType::ReadLock lock(m_mutex);
    int32_t idx = getIdx();
    if (idx == -1) {
        return nullptr;
    }
    for (size_t i = 0; i < m_items.size(); ++i) {
        auto& h = m_items[(i + idx) % m_items.size()];
        if (h->isValid()) {
            return h;
        }
    }
    return nullptr;
}

int32_t FairLoadBalanceItem::getWeight() {
    int32_t v = m_weight * m_stats.getWeight();
    if (m_stream->isConnected()) {
        return v > 1 ? v : 1;
    }
    return 1;
}

void FairLoadBalanceItem::clear() {
    m_stats.clear();
}

SDLoadBalance::SDLoadBalance(IServiceDiscovery::ptr sd)
    :m_sd(sd) {
}

LoadBalance::ptr SDLoadBalance::get(const std::string& domain, const std::string& service, bool auto_create) {
    do {
        RWMutexType::ReadLock lock(m_mutex);
        auto it = m_datas.find(domain);
        if (it == m_datas.end()) {
            break;
        }
        auto itt = it->second.find(service);
        if (itt == it->second.end()) {
            break;
        }
        return itt->second;
    } while (0);
    if (!auto_create) {
        return nullptr;
    }
    auto type = getType(domain, service);
    auto lb = createLoadBalance(type);
    RWMutexType::WriteLock lock(m_mutex);
    m_datas[domain][service] = lb;
    lock.unlock();
    return lb;
}

ILoadBalance::Type SDLoadBalance::getType(const std::string& domain, const std::string& service) {
    RWMutexType::ReadLock lock(m_mutex);
    auto it = m_types.find(domain);
    if (it == m_types.end()) {
        return m_defaultType;
    }
    auto iit = it.find(service);
    if (iit == m_types.end()) {
        return m_defaultType;
    }
    return iit->second;
}

LoadBalance::ptr createLoadBalance(ILoadBalance::Type type) {
    if (type == ILoadBalance::ROUNDROBIN) {
        return RoundRobinLoadBalance::ptr(new RoundRobinLoadBalance);
    }  else if(type == ILoadBalance::WEIGHT) {
        return WeightLoadBalance::ptr(new WeightLoadBalance);
    } else if(type == ILoadBalance::FAIR) {
        return WeightLoadBalance::ptr(new WeightLoadBalance);
    }
    return nullptr;
}

LoadBalanceItem::ptr SDLoadBalance::createLoadBalanceItem(ILoadBalance::Type type) {
    LoadBalanceItem::ptr item;
    if(type == ILoadBalance::ROUNDROBIN) {
        item.reset(new LoadBalanceItem);
    } else if(type == ILoadBalance::WEIGHT) {
        item.reset(new LoadBalanceItem);
    } else if(type == ILoadBalance::FAIR) {
        item.reset(new FairLoadBalanceItem);
    }
    return item;
}

/// 当某个服务的服务实例列表发生变化时（新增/删除），由服务发现模块调用本函数进行处理。
void SDLoadBalance::onServiceChange(
    const std::string& domain,
    const std::string& service,
    const std::unordered_map<uint64_t, ServiceItemInfo::ptr>& old_value,
    const std::unordered_map<uint64_t, ServiceItemInfo::ptr>& new_value) {
    SYLAR_LOG_INFO(g_logger) << "onServiceChange domain=" << domain
                         << " service=" << service;
    auto type = getType(domain, service);
    auto lb = get(domain, service, true);

    // 表示还未构建连接的服务节点信息（原始数据，来自服务发现）
    std::unordered_map<uint64_t, ServiceItemInfo::ptr> add_values;
    // 表示准备被移除的连接对象（已经构建好连接的，来自负载均衡器）
    std::unordered_map<uint64_t, LoadBalanceItem::ptr> del_infos;

    for (auto& i : old_value) {
        if (new_value.find(i.first) == new_value.end()) {
            del_infos[i.first];
        }
    }
    for (auto& i : new_value) {
        if (old_value.find(i.first) == old_value.end()) {
            add_values.insert(i);
        }
    }

    std::unordered_map<uint64_t, LoadBalanceItem::ptr> add_infos;
    for (auto& i : add_values) {
        // 将 ServiceItemInfo 转换为 SocketStream
        auto stream = m_cb(i.second);
        if (!stream) {
            SYLAR_LOG_ERROR(g_logger) << "create stream fail, " << i.second->toString();
            continue;            
        }
        LoadBalanceItem::ptr lditem = createLoadBalanceItem(type);
        lditem->setId(i.first);
        lditem->setStream(stream);
        lditem->setWeight(10000);

        add_infos[i.first] = lditem;
    }
    lb->update(add_infos, del_infos);
    // 关闭已经被删除的连接
    for (auto& i : del_infos) {
        if (i.second) {
            i.second->close();
        }
    }
}

void SDLoadBalance::start() {
    m_sd->setServiceCallback(std::bind(&SDLoadBalance::onServiceChange, this,
                                        std::placeholders::_1, 
                                        std::placeholders::_2, 
                                        std::placeholders::_3, 
                                        std::placeholders::_4));
    m_sd->start();
}

void SDLoadBalance::stop() {
    m_sd->stop();
}

void SDLoadBalance::initConf(const std::unordered_map<std::string,std::unordered_map<std::string, std::string> >& confs) {
    decltype(m_types) types;
    // query_infos用于记录要监听的服务列表，后面会通知给服务发现组件 m_sd
    std::unordered_map<std::string, std::unordered_set<std::string> > query_infos;
    for (auto& i : confs) {
        for (auto& n : i.second) {
            ILoadBalance::Type t = ILoadBalance::FAIR;
            if (n.second == "round_robin") {
                t = ILoadBalance::ROUNDROBIN;
            } else if (n.second == "weight") {
                t = ILoadBalance::WEIGHT;
            }
            types[i.first][n.first] = t;
            query_infos[i.first].insert(n.first);
        }
    }
    // 通知服务发现：表示“我对这些服务感兴趣，请你 m_sd 帮我监听它们的变更情况”；
    // 通常 m_sd（例如 Zookeeper 实现）内部会在对应路径 /domain/service 上注册监听器；
    // 一旦该服务上线、下线、节点变化，就会触发 onServiceChange() 来更新 LoadBalance。
    // query_infos中就是我们要监测的服务，一旦有服务发生变化，就会触发m_sd注册的服务变回调函数，而这个回调函数是在SDLoadBalance::start()中设置的
    m_sd->setQueryServer(query_infos);
    RWMutexType::WriteLock lock(m_mutex);
    types.swap(m_types);
    lock.unlock();    
}

std::string SDLoadBalance::statusString() {
    RWMutexType::ReadLock lock(m_mutex);
    decltype(m_datas) datas = m_datas;
    lock.unlock();
    std::stringstream ss;
    for(auto& i : datas) {
        ss << i.first << ":" << std::endl;
        for(auto& n : i.second) {
            ss << "\t" << n.first << ":" << std::endl;
            ss << n.second->statusString("\t\t") << std::endl;
        }
    }
    return ss.str();    
}



}
