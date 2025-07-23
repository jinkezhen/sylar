#ifndef __SYLAR_NS_NAME_SERVER_MODULE_H__
#define __SYLAR_NS_NAME_SERVER_MODULE_H__

#include "sylar/module.h"
#include "ns_protocol.h"


namespace sylar {
namespace ns {

class NameServerModule;
/**
 * @brief 表示一个已注册的客户端节点及其所注册的服务信息。
 *
 * NSClientInfo 封装了一个客户端节点的元信息，包括：
 *  - 基础节点信息（IP、端口、权重）由 `NSNode` 表示；
 *  - 注册的服务清单：即该客户端在哪些域名下注册了哪些命令（如 REGISTER、QUERY 等）。
 *
 * 这个类通常用于 name_server 侧，用来记录当前有哪些客户端节点接入了注册中心、
 * 并提供了哪些域名与命令的服务。
 *
 * 注意：此类不对外暴露修改接口，只允许 `NameServerModule` 友元类访问和修改，
 * 所以外部代码只能通过 `NameServerModule` 对它进行创建和管理。
 */

class NSClientInfo {
    friend class NameServerModule;
public:
    typedef std::shared_ptr<NSClientInfo> ptr;
private:
    /**
     * @brief 客户端节点的基础信息。
     *
     * `NSNode` 中包含了该节点的 IP、端口、权重，以及唯一 ID（由 IP + 端口生成）。
     * 它是客户端节点的标识数据。
     */
    NSNode::ptr m_node;

    /**
     * @brief 域名 -> 命令集合 的映射。
     *
     * 表示该客户端在什么域名下注册了哪些命令。
     *
     * 例如：
     * {
     *   "sylar.top" => {REGISTER, QUERY},
     *   "example.com" => {QUERY}
     * }
     *
     * 用于后续注销、查询等操作时快速定位该节点在哪些服务下起作用。
     */
    std::map<std::string, std::set<uint32_t>> m_domain2cmds;
};


/**
 * @class NameServerModule
 * @brief 基于 Rock 协议实现的服务注册中心模块，提供节点注册、查询、心跳维护、连接管理等功能。
 *
 * NameServerModule 是整个分布式服务的核心控制模块，作为一个模块插件被加载，
 * 它继承自 sylar::RockModule，具备处理基于 Rock 协议的请求能力。
 *
 * 功能涵盖：
 *   - 处理客户端节点的注册（REGISTER）、查询（QUERY）、心跳（TICK）等请求；
 *   - 管理每个连接客户端的服务注册信息（IP、端口、命令类型等）；
 *   - 在节点变化时通知关注该域名的客户端（通过 notify 机制）；
 *   - 支持 session 与域名之间的双向绑定与查询；
 *   - 保持内部结构：NSDomainSet（服务组织结构）、m_sessions（连接→客户端映射）、关注域名的映射。
 */
class NameServerModule : public RockModule {
public:
    typedef std::shared_ptr<NameServerModule> ptr;
    NameServerModule();

    // 重写父类接口，处理来自客户端的RockRequest请求
    virtual bool handleRockRequest(sylar::RockRequest::ptr request,
                                   sylar::RockResponse::ptr response,
                                   sylar::RockStream::ptr stream) override;
    // 处理来自客户端的RockNotify消息
    virtual bool handleRockNotify(sylar::RockNotify::ptr notify,
                                  sylar::RockStream::ptr stream) override;

    // 当有客户端连接到本服务节点时，会触发该函数
    virtual bool onConnect(sylar::Stream::ptr stream) override;

    // 连接断开时触发该函数，用于清理 session 信息、取消注册的服务、移除通知监听。
    virtual bool onDisconnect(sylar::Stream::ptr stream) override;

    // 返回当前模块的状态信息
    virtual std::string statusString() override;

private:
    // 处理 REGISTER 命令：接收客户端注册的服务节点信息。
    bool handleRegister(sylar::RockRequest::ptr request,
                        sylar::RockResponse::ptr response,
                        sylar::RockStream::ptr stream);

    // 处理 QUERY 命令：客户端查询某个域名和命令类型下的节点列表。
    bool handleQuery(sylar::RockRequest::ptr request,
                     sylar::RockResponse::ptr response,
                     sylar::RockStream::ptr stream);

    // 处理 TICK 命令：客户端发送的心跳，更新存活状态。
    bool handleTick(sylar::RockRequest::ptr request,
                    sylar::RockResponse::ptr response,
                    sylar::RockStream::ptr stream);

private:
    // 获取指定stream的客户端信息
    NSClinetInfo::ptr get(sylar::RockStream::ptr rs);
    
    //  绑定一个 NSClientInfo 到某个连接（stream）
    void set(sylar::RockStream::ptr rs, NSClientInfo::ptr info);
    
    /**
     * @brief 为指定连接设置它关注的域名列表（用于订阅通知）。
     * @param rs 当前客户端连接
     * @param ds 客户端希望监听的域名列表（如 blog.sylar.top）
     */
    void setQueryDomain(sylar::RockStream::ptr rs, const std::set<std::string>& ds);
    
    /**
     * @brief 向一批关注某些域名的连接发送通知。
     *
     * @param domains 域名集合
     * @param nty 要发送的通知消息内容（NotifyMessage）
     */
    void doNotify(std::set<std::string>& domains,
                  std::shared_ptr<NotifyMessage> nty);
    
    /**
     * @brief 获取所有关注某个域名的客户端连接。
     * @param domain 域名
     * @return set<RockStream::ptr> 所有关联的客户端连接
     */
    std::set<sylar::RockStream::ptr> getStreams(const std::string& domain);

private:
    // 核心数据结构：域名管理器，组织所有域名、命令、节点的三层结构。
    NSDomainSet::ptr m_domains;
    // 会话映射：每个连接对应一个客户端信息（IP/端口/注册内容）
    std::map<sylar::RockStream::ptr, MSClientInfo::ptr> m_sessions;
    // 记录每个连接(客户端)关注了哪些域名，用于推送通知
    std::map<sylar::RockStream::ptr, std::set<std::string>> m_queryDomains;
    // 反向映射：每个域名有哪些连接(客户端)在关注它
    std::map<std::string, std::set<sylar::RockStream::ptr>> m_domainToSessions;

    sylar::RWMutex m_mutex;
};


}
}


#endif