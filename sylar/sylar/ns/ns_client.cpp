#include "ns_client.h"
#include "sylar/log.h"
#include "sylar/util.h"


namespace sylar {
namespace ns {

static sylar::Logger::ptr g_logger = SYLAR_LOG_NAME("system");

NSClient::NSClient() {
    m_domains.reset(new sylar::ns::NSDomainSet);
}

NSClient::~NSClient() {
    SYLAR_LOG_DEBUG(g_logger) << "NSClient::~NSClient";
}

const std::set<std::string>& NSClient::getQueryDomains() {
    sylar::RWMutex::ReadLock lock(m_mutex);
    return m_queryDomains;
}

void NSClient::setQueryDomains(const std::set<std::string>& v) {
    sylar::RWMutex::WriteLock lock(m_mutex);
    m_queryDomains = v;
    lock.unlock();
    onQueryDomainChange();
}

void NSClient::addQueryDomain(const std::string& domain) {
    sylar::RWMutex::WriteLock lock(m_mutex);
    m_queryDomains.insert(domain);
    lock.unlock();
    onQueryDomainChange();
}

bool NSClient::hasQueryDomain(const std::string& domain) {
    sylar::RWMutex::ReadLock lock(m_mutex);
    return m_queryDomains.count(domain) > 0;
}

void NSClient::delQueryDomain(const std::string& domain) {
    sylar::RWMutex::WriteLock lock(m_mutex);
    m_queryDomains.erase(domain);
    lock.unlock();
    onQueryDomainChange();
}

RockResult::ptr NSClient::query() {
    sylar::RockRequest::ptr req = std::make_shared<sylar::RockRequest>();
    req->setSn(sylar::Atomic::addFetch(m_sn, 1));
    req->setCmd((int)NSCommand::QUERY);
    // 创建一个 protobuf 消息 QueryRequest，用于携带要查询的域名。
    auto data = std::make_shared<sylar::ns::QueryRequest>();

    sylar::RWMutex::ReadLock lock(m_mutex);
    if (m_queryDomains.empty()) {
        return std::make_shared<RockRequest>(0, 0, nullptr, nullptr);
    }
    for (auto& i : m_queryDomains) {
        data->add_domains(i);
    }
    lock.unlock();

    // 把 protobuf 对象 QueryRequest 序列化写入 RockRequest 的 payload(m_body) 中。
    req->setAsPB(*data);
    // 向服务端发送请求并等待响应
    auto rt = request(req, 1000);

    // 处理响应
    do {
        if (!rt->response) {
            SYLAR_LOG_ERROR(g_logger) << "query error result=" << rt->result;
            break;            
        }
        // 将响应数据反序列化为 protobuf 类型 QueryResponse。
        auto rsp = rt->response->getAsPB<sylar::ns::QueryResponse>();
        if (!rsp) {
            SYLAR_LOG_ERROR(g_logger) << "invalid data not QueryResponse";
            break;        
        }
        NSDomainSet::ptr domains(new NSDomainSet);
        for (auto& i : rsp->infos()) {
            if (!hasQueryDomain(i.domain())) {
                continue;
            }
            auto domain = domains->get(i.domain(), true);
            uint32_t cmd = i.cmd();
            for (auto& n : i.nodes()) {
                NSNode::ptr node(new NSNode(n.ip(), n.port(), n.weight()));
                if (!(node->getId() >> 32)) {
                    SYLAR_LOG_ERROR(g_logger) << "invalid node: "
                    << node->toString();
                    continue;       
                }
                domain->add(cmd, node);
            }
        }
        m_domains->swap(*domains);
    } while (false);
    return rt;
}

void NSClient::onQueryDomainChange() {
    if (isConnected()) {
        query();
    }
}

void NSClient::init() {
    auto self = std::dynamic_pointer_cast<NSClient>(shared_from_this());
    setConnectCb(std::bind(&NSClient::onConnect, self, std::placeholders::_1));
    setDisconnectCb(std::bind(&NSClient::onDisconnect, self, std::placeholders::_1));
    setNotifyHandler(std::bind(&NSClient::onNotify, self
                        ,std::placeholders::_1, std::placeholders::_2));
}

void NSClient::uninit() {
    setConnectCb(nullptr);
    setDisconnectCb(nullptr);
    setNotifyHandler(nullptr);

    if(m_timer) {
        m_timer->cancel();
    }
}

bool NSClient::onConnect(sylar::AsyncSocketStream::ptr stream) {
    if (m_timer) {
        m_timer->cancel();
    }
    auto self = std::dynamic_pointer_cast<NSClient>(shared_from_this());
    m_timer = m_iomanager->addTimer(30 * 1000, std::bind(&NSClinet::onTimer, self), true);
    m_iomanager->schedule(std::bind(&NSClinet::query, self));
    return true;
}

void NSClient::onTimer() {
    sylar::RockRequest::ptr req = std::make_shared<sylar::RockRequest>();
    req->setSn(sylar::Atomic::addFetch(m_sn, 1));
    req->setCmd((uint32_t)NSCommand::TICK);
    auto rt = request(req, 1000);
    if (!rt->response) {
        SYLAR_LOG_ERROR(g_logger) << "tick error result=" << rt->result;        
    }
    sleep(1000);
    query();
}

void NSClient::onDisconnect(sylar::AsyncSocketStream::ptr stream) {
}

// 处理服务端发来的通知（Notify），当某些服务节点变更（增加或删除）时，更新本地 m_domains 中的服务节点缓存
bool NSClient::onNotify(sylar::RockNotify::ptr nty, sylar::RockStream::ptr stream) {
    do {
        if (nty->getNotify() == (uint32_t)NSNotify::NODE_CHANGE) {
            auto nm = nty->getAsPB<sylar::ns::NotifyMessage>();
            if (!nm) break;
            for (auto& i : nm->dels()) {
                // 跳过那些客户端没有订阅/关注的域名。
                if (!hasQueryDomain(i.domain())) continue;
                auto domain = m_domains->get(i.domain());
                if (!domain) continue;
                int cmd = i.cmd();
                for (auto& n : i.nodes()) {
                    NSNode::ptr node(new NSNode(n.ip(), n.port(), n.weight()));
                    domain->del(cmd, node->getId());
                }
            }

            for (auto& i : nm->updates()) {
                if (!hasQueryDomain(i.domain())) continue;
                auto domain = m_domains->get(i.domain(), true);
                if (!domain) continue;
                int cmd = i.cmd();
                for (auto& n : i.nodes()) {
                    NSNode::ptr node(new NSNode(n.ip(), n.port(), n.weight()));
                    if (node->getId() >> 32) {
                        domain->add(cmd, node);
                    } else {
                        SYLAR_LOG_ERROR(g_logger) << "invalid node: " << node->toString();                        
                    }
                }
            }
        }
    } while (false);
    return true;
}



}
}