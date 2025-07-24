#include "service_discovery.h"
#include "sylar/log.h"
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>


namespace sylar {

static sylar::Logger::ptr g_logger = SYLAR_LOG_NAME("system");

ServiceItemInfo::ptr ServiceItemInfo::Create(const std::string& ip_and_port, const std::string& data) {
    auot pos = ip_and_port.find(':');
    if (pos == std::string::npos) {
        return nullptr;
    }
    auto ip = ip_and_port.substr(0, pos);
    auto port = sylar::TypeUtil::Atoi(ip_and_port.substr(pos + 1));
    // inet_addr() 是一个 C/C++ 网络编程中常用的函数，用于将 IPv4 点分十进制字符串（如 "192.168.1.1"）转换为网络字节序的 32 位整数（in_addr_t 类型）。
    in_addr_t ip_addr = inet_addr(ip.c_str());
    if (ip_addr == 0) {
        return nullptr;
    }
    ServiceItemInfo::ptr rt(new ServiceItemInfo);
    rt->m_id = ((uint64_t)ip_addr << 32) | port;
    rt->m_ip = ip;
    rt->m_port = port;
    rt->m_data = data;
    return rt;
}

std::string ServiceItemInfo::toString() const {
    std::stringstream ss;
    ss << "[ServiceItemInfo id=" << m_id
       << " ip=" << m_ip
       << " port=" << m_port
       << " data=" << m_data
       << "]";
    return ss.str();
}

void IServiceDiscovery::setQueryServer(const std::unordered_map<std::string, std::unordered_set<std::string> >& v) {
    sylar::RWMutex::WriteLock lock(m_mutex);
    m_queryInfos = v;
}

void IServiceDiscovery::registerServer(const std::string& domain, const std::string& service,
                                       const std::string& ip_and_port, const std::string& data) {
    sylar::RWMutex::WriteLock lock(m_mutex);
    m_registerInfos[domain][service][ip_and_port] = data;
}

void IServiceDiscovery::queryServer(const std::string& domain, const std::string& service) {
    sylar::RWMutex::WriteLock lock(m_mutex);
    m_queryInfos[domain].insert(service);
}

void IServiceDiscovery::listServer(std::unordered_map<std::string, std::unordered_map<std::string
                                   ,std::unordered_map<uint64_t, ServiceItemInfo::ptr> > >& infos) {
    sylar::RWMutex::ReadLock lock(m_mutex);
    infos = m_datas;
}

void IServiceDiscovery::listRegisterServer(std::unordered_map<std::string, std::unordered_map<std::string
                                           ,std::unordered_map<std::string, std::string> > >& infos) {
    sylar::RWMutex::ReadLock lock(m_mutex);
    infos = m_registerInfos;
}

void IServiceDiscovery::listQueryServer(std::unordered_map<std::string
                                        ,std::unordered_set<std::string> >& infos) {
    sylar::RWMutex::ReadLock lock(m_mutex);
    infos = m_queryInfos;
}

ZKServiceDiscovery::ZKServiceDiscovery(const std::string& hosts) 
    : m_hosts(hosts) {
}

// ZKServiceDiscovery 的启动方法，启动 ZooKeeper 客户端、注册回调、启动定时器。
void ZKServiceDiscovery::start() {
    if (m_client) {
        return;
    }
    // 获取当前对象的 shared_ptr，方便后续绑定 this 到 lambda 和回调函数中。
    auto self = shared_from_this();
    m_client.reset(new sylar::ZKClient);
    // 当 ZooKeeper 的连接状态、节点变更等事件发生时，就会回调你绑定的 onWatch() 函数。
    bool b = m_client->init(m_hosts, 6000, std::bind(&ZKServiceDiscovery::onWatch, self,
                            std::placeholders::_1, std::placeholders::_2, std::placeholders::_3, std::placeholders::_4));
    if (!b) {
        SYLAR_LOG_ERROR(g_logger) << "ZKClient init fail, hosts=" << m_hosts;
    }
    // 容错性保障，周期性定时执行，持续刷新连接状态和同步服务数据，
    // 确保在会话断开后也能自动补偿注册和订阅信息。
    // addTimer 第二个参数为 true 表示该定时器循环执行。
    m_timer = sylar::IOManager::GetThis()->addTimer(60 * 1000, [self, this]() {
        m_isOnTimer = true;
        // 刷线连接状态，尝试重新同步数据
        onZKConnect("", m_client);
        m_isOnTimer = false;
    }, true);
}

void ZKServiceDiscovery::stop() {
    if (m_client) {
        m_client->close();
        m_client = nullptr;
    }
    if (m_timer) {
        m_timer->cancel();
        m_timer = nullptr;
    }
}

// 主要作用就是在连接 ZooKeeper 成功之后，把之前注册过的服务重新注册一遍，同时重新设置对感兴趣服务的监听和查询操作。
// 为什么要“重新”呢？因为 ZooKeeper 的临时节点（ephemeral node）在会话断开后就会被自动删除，所以一旦断连再重连，
// 就要手动把状态“补”上。
void ZKServiceDiscovery::onZKConnect() {
    sylar::RWMutex::ReadLock lock(m_mutex);
    auto rinfo = m_registerInfos;
    auto qinfo = m_queryInfos;
    lock.unlock();
    bool ok = true;
    for (auto& i : rinfos) {
        for (auto& x : i.second) {
            for (auto& v : x.second) {
                ok &= registerInfo(i.first, x.first, v.first, v.second);
            }
        }
    }
    if (!ok) {
        SYLAR_LOG_ERROR(g_logger) << "onZKConnect register fail";
    }
    ok = true;
    for (auto& i : qinfo) {
        for (auto& x : i.second) {
            ok &= queryInfo(i.first, x);
        }
    }
    if (!ok) {
        SYLAR_LOG_ERROR(g_logger) << "onZKConnect query fail";
    }
    ok = true;
    for (auto& i : qinfo) {
        for (auto& x : i.second) {
            ok &= queryData(i.first, x);
        }
    }
    if(!ok) {
        SYLAR_LOG_ERROR(g_logger) << "onZKConnect queryData fail";
    }
}

bool ZKServiceDiscovery::existsOrCreate(const std::string& path) {
    int32_t v = m_client->exists(path, false);
    if (v == ZOK) {
        return true;
    } else {
        auto pos = path.find_last_of('/');
        if (pos == std::string::npos) {
            SYLAR_LOG_ERROR(g_logger) << "existsOrCreate invalid path=" << path;
            return false;            
        }
        // path.substr(0, pos) 表示取上一级路径，例如 /a/b/c 传进来，会先尝试去创建 /a/b。
        // 这里递归调用 existsOrCreate() —— 先保证父路径存在。
        // pos == 0 表示是 /a 这种形式，只剩根路径了，无需继续递归。
        if (pos == 0 || existsOrCreate(path.substr(0, pos))) {
            std::string new_val(1024, 0);
            // 当前路径不存在且父路径存在时创建当前路径
            v = m_client->create(path, "", new_val);
        }
    }
    if (!v == ZOK) {
        SYLAR_LOG_ERROR(g_logger) << "create path=" << path << " error:"
            << zerror(v) << " (" << v << ")";
        return false;        
    }
    return false;
}

// 发布者-订阅者 模型
// 区分 providers 和 consumers，是为了明确谁在提供服务、谁在使用服务，方便管理、监控、动态感知和负载控制。
static std::string GetProvidersPath(const std::string& domain, const std::string& service) {
    return "/sylar/" + domain + "/" + service + "/providers";
}

static std::string GetConsumersPath(const std::string& domain, const std::string& service) {
    return "/sylar/" + domain + "/" + service + "/consumers";
}

static std::string GetDomainPath(const std::string& domain) {
    return "/sylar/" + domain;
}

// /sylar/{domain}/{service}/{role}/{instance}
// /sylar/payment/order/providers/192.168.1.10:9000
// v[0] = ""（因为路径以 / 开头）
// v[1] = "sylar"
// v[2] = "payment" ← 这是 domain
// v[3] = "order" ← 这是 service
// v[4] = "providers" 或 "consumers"
bool ParseDomainService(const std::string& path, std::string& domain, std::string& service) {
    auto v = sylar::split(path, '/');
    if (v.size() != 5) {
        return false;
    }
    domain = v[2];
    service = v[3];
    return true;
}

bool ZKServiceDiscovery::registerInfo(const std::string& domain, const std::string& service, 
                                      const std::string& ip_and_port, const std::string& data) {
    std::string path = GetProvidersPath(domain, service);
    bool v = existsOrCreate(path);
    if (!v) {
        SYLAR_LOG_ERROR(g_logger) << "create path=" << path << " fail";
        return false;
    }
    std::string new_val(1024, 0);
    // ZOO_OPEN_ACL_UNSAFE 表示：这个节点的访问权限是“完全开放”，任何客户端都可以读写它，不做权限校验。
    // ZKClient::FlagsType::EPHEMERAL 表示创建临时节点（Zookeeper session 断开时自动删除）
    int32_t rt = m_client->create(path + "/" + ip_and_port, data, new_val, &ZOO_OPEN_ACL_UNSAFE, ZKClient::FlagsType::EPHEMERAL);
    if (rt == ZOK) {
        return true;
    } 
    if (!m_isOnTimer) {
        SYLAR_LOG_ERROR(g_logger) << "create path=" << (path + "/" + ip_and_port) << " fail, error:"
            << zerror(rt) << " (" << rt << ")";        
    }
    return rt == ZNODEEXISTS;
}

bool ZKServiceDiscovery::queryInfo(const std::string& domain, const std::string& service) {
    if (service != "all") {
        std::string path = GetConsumersPath(domain, service);
        bool v = existsOrCreate(path);
        if (!v) {
            SYLAR_LOG_ERROR(g_logger) << "create path=" << path << " fail";
            return false;        
        }
        // m_selfInfo 是本客户端的信息（如 IP+端口），它不能为空，否则我们无法作为消费者“实名”注册。
        if (m_selfInfo.empty()) {
            SYLAR_LOG_ERROR(g_logger) << "queryInfo selfInfo is null";
            return false;            
        }
        std::string new_val(1024, 0);
        // /sylar/mall_service/user_service/consumers/192.168.1.100:8080
        int32_t rt = m_client->create(path + "/" + m_selfInfo, m_selfData, new_val
                                      ,&ZOO_OPEN_ACL_UNSAFE, ZKClient::FlagsType::EPHEMERAL);
        if (rt == ZOK) {
            return true;
        }
        // 如果创建失败，且不是定时器周期性触发
        if(!m_isOnTimer) {
            SYLAR_LOG_ERROR(g_logger) << "create path=" << (path + "/" + m_selfInfo) << " fail, error:"
                << zerror(rt) << " (" << rt << ")";
        }    
        return rt == ZNODEEXISTS;
    } else {
        std::vector<std::string> children;
        m_client->getChildren(GetDomainPath(domain), children, false);
        bool rt = true;
        for (auto& i : children) {
            rt &= queryInfo(domain, i);
        }
        return rt;
    } 
}

bool ZKServiceDiscovery::getChildren(const std::string& path) {
    std::string domain;
    std::string service;
    // 从路径中解析出 domain 和 service，例如：/sylar/mall/user/providers -> mall, user
    if(!ParseDomainService(path, domain, service)) {
        SYLAR_LOG_ERROR(g_logger) << "get_children path=" << path
            << " invalid path";
        return false;
    }
    {
        sylar::RWMutex::ReadLock lock(m_mutex); // 加读锁保护 m_queryInfos 的并发访问
        auto it = m_queryInfos.find(domain);
        // 如果 domain 不存在，说明客户端未声明过对该域的查询兴趣
        if(it == m_queryInfos.end()) {
            SYLAR_LOG_ERROR(g_logger) << "get_children path=" << path
                << " domian=" << domain << " not exists";
            return false;
        }
        // 如果 service 不在查询服务集合中，且也没有 "all"，说明不应该对其监听
        if(it->second.count(service) == 0
                && it->second.count("all") == 0) {
            SYLAR_LOG_ERROR(g_logger) << "get_children path=" << path
                << " service=" << service << " not exists "
                << sylar::Join(it->second.begin(), it->second.end(), ",");
            return false;
        }
    }
    std::vector<std::string> vals;
    // 向 Zookeeper 发起获取子节点请求，同时注册 watcher（true 表示监听变更）
    int32_t v = m_client->getChildren(path, vals, true);
    if(v != ZOK) {
        SYLAR_LOG_ERROR(g_logger) << "get_children path=" << path << " fail, error:"
            << zerror(v) << " (" << v << ")";
        return false;
    }
    // 将子节点名（IP:PORT 格式）转换为 ServiceItemInfo 实例
    std::unordered_map<uint64_t, ServiceItemInfo::ptr> infos;
    for(auto& i : vals) {
        auto info = ServiceItemInfo::Create(i, ""); // data 为空，仅解析 ip 和 port
        if(!info) {
            continue;
        }
        infos[info->getId()] = info;
        SYLAR_LOG_INFO(g_logger) << "domain=" << domain
            << " service=" << service << " info=" << info->toString();
    }
    // 保存旧数据，准备与新数据对比
    auto new_vals = infos;
    {
        sylar::RWMutex::WriteLock lock(m_mutex); // 写锁保护 m_datas 写入
        // 将当前 path 下的服务实例列表更新到 m_datas（domain -> service -> infos）
        m_datas[domain][service].swap(infos); // 旧的值被替换并保留为 infos（传给回调）
    }
    // 触发用户注册的回调函数，传递服务变更信息
    // 外部设置的回调函数，在服务节点变化时触发通知
    m_cb(domain, service, infos, new_vals);
    return true;
}

bool ZKServiceDiscovery::queryData(const std::string& domain, const std::string& service) {
    if (service != "all") {
        std::string path = GetProvidersPath(domain, service);
        return getChildren(path);
    } else {
        std::vector<std::string> children;
        // 获取 /sylar/<domain> 下的所有子目录名，也就是所有服务的名称，例如 "user"、"order"、"product" 等。
        m_client->getChildren(GetDomainPath(domain), children, false);
        bool rt = true;
        //对每一个服务名递归调用 queryData（回到上面具体服务的逻辑），批量获取所有服务的 providers 信息。
        for (auto& i : children) {
            rt &= queryData(domain, i);
        }
        return rt;
    }
}

// 当某个节点的 子节点（children）发生变化 时会触发该函数。
void ZKServiceDiscovery::onZKChild(const std::string& path, ZKClient::ptr client) {
    getChildren(path);
}

void ZKServiceDiscovery::onZKChanged(const std::string& path, ZKClient::ptr client) {
    SYLAR_LOG_INFO(g_logger) << "onZKChanged path=" << path;
}

void ZKServiceDiscovery::onZKDeleted(const std::string& path, ZKClient::ptr client) {
    SYLAR_LOG_INFO(g_logger) << "onZKDeleted path=" << path;
}

void ZKServiceDiscovery::onZKExpiredSession(const std::string& path, ZKClient::ptr client) {
    SYLAR_LOG_INFO(g_logger) << "onZKExpiredSession path=" << path;
    client->reconnect();
}

void ZKServiceDiscovery::onWatch(int type, int stat, const std::string& path, ZKClinet::ptr client) {
    // 正常连接状态
    if (stat == ZKClient::StatType::CONNECTED) {
        if (type == ZKClinet::EventType::SESSION) {
            // 如果事件是“会话事件”（通常是首次连接或重连），调用 onZKConnect：
            return onZKConnect(path, client);
        } else if (type == ZKClient::EventType::CHILD) {
            // 子节点发生变化（如 provider 或 consumer 节点增删），
            // 会触发调用 getChildren(path) 刷新本地服务节点列表。
            return onZKChild(path, client);
        } else if (type == ZKClient::EventType::CHANGED) {
            // 节点数据被修改
            return onZKChanged(path, client);
        } else if (type == ZKClient::EventType::DELETED) {
            return onZKDeleted(path, client);
        }
    } else if (stat == ZKClient::StateType::EXPIRED_SESSION) {
        if (type == ZKClient::EventType::SESSION) {
            return onZKExpiredSession(path, client);
        }
    }
    SYLAR_LOG_ERROR(g_logger) << "onWatch hosts=" << m_hosts
        << " type=" << type << " stat=" << stat
        << " path=" << path << " client=" << client;
}

}