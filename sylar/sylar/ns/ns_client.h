#ifndef __SYLAR_NS_NS_CLIENT_H__
#define __SYLAR_NS_NS_CLIENT_H__

#include "sylar/rock/rock_stream.h"
#include "ns_protocol.h"

namespace sylar {
namespace ns {

/**
 * @brief NameServer 客户端类
 * 
 * 该类用于与服务注册中心（NameServer）建立连接，维护查询域名列表，
 * 支持主动查询服务节点、自动接收变更通知，并在本地维护最新的服务节点信息。
 */
class NSClient : public RockConnection {
public:
    typedef std::shared_ptr<NSClient> ptr;

    NSClient();
    ~NSClient();

    // 获取当前客户端正在查询到所有域名
    const std::set<std::string>& getQueryDomains();

    // 设置当前要查询的所有域名
    void setQueryDomains(const std::set<std::string>& v);

    // 添加一个需要查询的域名
    void addQueryDomain(const std::string& domain);

    // 删除一个已查询的域名
    void delQueryDomain(const std::string& domain);

    // 判断是否已包含某个查询域名
    bool hasQueryDomain(const std::string& domain);

    // 向服务中心发送 QueryRequest，获取指定域名下的所有命令-节点映射
    RockRequest::ptr query();

    // 初始化客户端（建立连接、启动定时器等）
    void init();

    // 反初始化客户端（关闭连接、清理状态）
    void uninit();

    // 获取当前客户端缓存的所有域名信息（NSDomainSet）
    NSDomainSet::ptr getDomains() const { return m_domains;}

private:
    // onQueryDomainChange() 是在 m_queryDomains（查询域名集合）发生增删改时触发的回调，
    // 用于主动刷新服务节点信息，比如立即重新向 NameServer 发起查询请求或重设定时器。
    void onQueryDomainChange();

    // 当连接成功建立时的回调
    bool onConnect(sylar::AsyncSocketStream::ptr stream);

    // 当连接断开时的回调
    void onDisconnect(sylar::AsyncSocketStream::ptr stream);

    // 当收到服务中心的通知消息（RockNotify）时的回调处理函数
    bool onNotify(sylar::RockNotify::ptr notify, sylar::RockStream::ptr stream);

    // 定时器回调函数，可能用于周期性查询、心跳发送或连接检测
    void onTimer();

private:
    sylar::RWMutex m_mutex;

    // 当前查询的域名集合：存储了客户端想向NameServer查询哪些服务
    std::set<std::string> m_queryDomains;
    // 本地缓存的服务节点信息：存储了从NameServer查询得到的结果，是查询到的 服务-命令-节点信息 的本地缓存。
    NSDomainSet::ptr m_domains;

    // 请求序列号，用于标识请求/响应
    uint32_t m_sn = 0;
    // 定时器：用于周期性任务
    sylar::Timer::ptr m_timer;
};


}
}


#endif