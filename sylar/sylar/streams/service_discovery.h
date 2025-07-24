#ifndef __SYLAR_STREAMS_SERVICE_DISCOVERY_H__
#define __SYLAR_STREAMS_SERVICE_DISCOVERY_H__

#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <string>
#include "sylar/mutex.h"
#include "sylar/iomanager.h"
#include "sylar/zk_client.h"

namespace sylar {

// 封装一个服务节点的信息（IP、端口、附加数据），并且给每个节点生成一个唯一ID。
class ServiceItemInfo {
public:
    typedef std::shared_ptr<ServiceItemInfo> ptr;
    static ServiceItemInfo::ptr Create(const std::string& ip_and_port, const std::string& data);

    uint64_t getId() const { return m_id; }
    uint16_t getPort() const { return m_port; }
    const std::string& getIp() const { return m_ip; }
    const std::string& getData() const { return m_data; }

    std::string toString() const;

private:
    uint64_t m_id;         ///< 节点唯一 ID，基于 IP 和端口生成
    uint16_t m_port;       ///< 服务端口号
    std::string m_ip;      ///< 服务 IP 地址
    std::string m_data;    ///< 附加数据（例如元数据、标签等）
};



// domain（域）：是服务的顶级分类，通常表示一个业务模块、应用系统或者组织名称。
// service（服务）：是 domain 下的具体某个微服务的名称。
// | domain         | service           | 作用         |
// | -------------- | ----------------- | ---------- |
// | `order_system` | `order_service`   | 处理订单相关请求   |
// | `order_system` | `payment_service` | 处理支付相关请求   |
// | `user_system`  | `user_service`    | 处理用户管理相关请求 |
// | `user_system`  | `auth_service`    | 处理用户认证相关请求 |
// 对应的 ZooKeeper 路径结构可能是这样：
// /sylar/order_system/order_service/providers
// /sylar/order_system/payment_service/providers
// /sylar/user_system/user_service/providers
// /sylar/user_system/auth_service/providers

// 服务发现：在一个分布式系统中，**自动查找可以调用的服务地址（IP + 端口）**的机制。
// 你有很多服务（用户服务、订单服务、支付服务）部署在不同机器上，服务发现就是帮你找到这些服务的“家”，让其他服务可以访问它们。
// ZooKeeper 的目录结构就像一个文件系统，我们可以用它来注册服务地址、监听服务变化：
// 1. 服务注册（Service Register）
//  比如 订单服务 想加入系统，它会像这样往 ZooKeeper 注册自己：
//  /sylar/order_system/order_service/providers/192.168.1.10:8080
//  这是一个 临时节点，一旦这个服务挂了，ZooKeeper 会自动把它删掉。
// 2. 服务查询（Service Discovery）
//  比如 购物车服务 想调用 订单服务，就来问 ZooKeeper：ZooKeeper，请告诉我 /sylar/order_system/order_service/providers/ 下面都有谁？
//  ZooKeeper 回复：- 192.168.1.10:8080  - 192.168.1.11:8080
//  然后购物车服务就可以从这些地址中任选一个发请求。
// 3. 服务监听（Watch）
//  如果 订单服务 的某个节点宕机，ZooKeeper 会通过 Watch 机制通知 购物车服务，让它更新服务列表。
// 服务发现就是在分布式系统中，自动地知道别的服务在哪里，什么时候上线、什么时候下线，用于实现“服务与服务之间的自动连接”。


// 服务发现的抽象基类。
class IServiceDiscovery {
public:
    typedef std::shared_ptr<IServiceDiscovery> ptr;

    // 回调函数类型：当某个服务的节点发生变化（上线或下线）时触发
    typedef std::function<void(const std::string& domain, const std::string& service, 
                                const std::unordered_map<uint64_t, ServiceItemInfo::ptr>& old_value,
                                const std::unordered_map<uint64_t, ServiceItemInfo::ptr>& new_value) service_callback;
    
    virtual ~IServiceDiscovery() {}

    // 注册当前服务器：将当前 IP:端口 和 附加数据 注册到指定 domain/service 名下
    void registerServer(const std::string& domain, const std::string& service,
                        const std::string& ip_and_port, const std::string& data);

    // 查询某个 domain 下的某个 service 的服务节点信息（会建立监听）
    void queryServer(const std::string& domain, const std::string& service);

    // 获取当前已知的所有服务信息
    void listServer(std::unordered_map<std::string, std::unordered_map<std::string, std::unordered_map<uint64_t, ServiceItemInfo::ptr>>>& infos);

    // 获取注册服务的原始信息（ip_and_port -> data），不包含解析后的 ServiceItemInfo
    void listRegisterServer(std::unordered_map<std::string, std::unordred_map<std::string, std::unordered_map<std::string, std::string>>>& infos);

    // 获取当前设置的服务查询列表（主动监听哪些服务）
    void listQueryServer(std::unordered_map<std::string, std::unordered_set<std::string> >& infos);

    // 启动服务发现逻辑（纯虚函数，由具体子类实现，如 ZKServiceDiscovery）
    virtual void start() = 0;
    // 停止服务发现逻辑
    virtual void stop() = 0;

    // 获取当前设置的服务变化回调函数
    service_callback getServiceCallback() const { return m_cb; }

    // 设置服务变化回调函数，用于通知外部“服务节点变化”
    void setServiceCallback(service_callback v) { m_cb = v; }

    // 设置需要主动查询的服务（通常在系统启动时由配置传入）
    void setQueryServer(const std::unordered_map<std::string, std::unordered_set<std::string>>& v);


protected:
    sylar::RWMutex m_mutex;

    // 存储查询到的服务节点信息：
    // domain -> service -> id -> ServiceItemInfo
    // 表示服务发现系统已知的所有服务节点信息
    std::unordered_map<std::string, std::unordered_map<std::string, std::unordered_map<uint64_t, ServiceItemInfo::ptr>>> m_datas;
    
    // 存储本地注册的服务信息：
    // domain -> service -> ip:port -> data
    // 表示当前服务注册到 ZooKeeper 上的节点信息
    std::unordered_map<std::string, std::unordered_map<std::string, std::unordered_map<std::string, std::string> > > m_registerInfos;        

    // 存储需要监听查询的服务：
    // domain -> [service1, service2, ...]
    // 表示当前客户端主动监听的服务列表
    std::unordered_map<std::string, std::unordered_set<std::string> > m_queryInfos;

    // 外部设置的回调函数，在服务节点变化时触发通知
    service_callback m_cb;
};


// ZKServiceDiscovery 是 IServiceDiscovery 的具体实现类，
// 使用 ZooKeeper 实现服务注册与服务发现机制。
class ZKServiceDiscovery : public IServiceDiscovery,
                           public std::enable_shared_from_this<ZKServiceDiscovery> {
public:
    typedef std::shared_ptr<ZKServiceDiscovery> ptr;

    // // 构造函数，传入 ZooKeeper 服务端地址，如 "127.0.0.1:2181"
    ZKServiceDiscovery(cpnst std::string& hosts);

    // 获取当前服务的自身标识（IP:PORT），用于注册到 ZK 节点名称
    const std::string& getSelfInfo() const { return m_selfInfo; }

    // 设置当前服务的自身标识（IP:PORT）
    void setSelfInfo(const std::string& v) { m_selfInfo = v; }

    // 获取自身携带的数据（服务元信息），注册时写入节点值中
    const std::string& getSelfData() const { return m_selfData; }

    // 设置自身携带的数据
    void setSelfData(const std::string& v) { m_selfData = v; }

    // 启动服务发现组件，连接 ZooKeeper 并注册/监听节点
    virtual void start();

    // 停止服务发现组件，释放连接、取消定时器
    virtual void stop();

private:
    // ZooKeeper Watch 回调事件处理函数（会分派到具体的 onZK* 方法）
    void onWatch(int type, int stat, const std::string& path, ZKClient::ptr client);

    // 处理连接事件（SESSION 连接成功），重新注册和查询所有节点
    void onZKConnect(const std::string& path, ZKClient::ptr client);

    // 处理节点子项变化（CHILD 事件），用于发现服务实例变化
    void onZKChild(const std::string& path, ZKClient::ptr client);

    // 处理节点数据变化（CHANGED 事件），暂时保留未用
    void onZKChanged(const std::string& path, ZKClient::ptr client);
    
    // 处理节点被删除事件（DELETED 事件）
    void onZKDeleted(const std::string& path, ZKClient::ptr client);

    // 处理会话过期事件（EXPIRED_SESSION），重连 ZK 并重新注册
    void onZKExpiredSession(const std::string& path, ZKClient::ptr client);

    // 向 ZooKeeper 注册服务节点（创建临时子节点）
    bool registerInfo(const std::string& domain, const std::string& service, 
                      const std::string& ip_and_port, const std::string& data);

    // 订阅指定 domain/service 的消费者节点，创建并写入自身信息
    // 向 ZooKeeper 注册“我（当前客户端）要消费哪个服务”。
    bool queryInfo(const std::string& domain, const std::string& service);

    // 查询指定服务的 providers 子节点信息，用于发现可用服务节点
    bool queryData(const std::string& domain, const std::string& service);

    // 判断某个节点是否存在，不存在则递归创建路径
    bool existsOrCreate(const std::string& path);

    // 获取某个 providers 路径下的所有子节点（服务节点列表）
    bool getChildren(const std::string& path);


private:
    std::string m_hosts;      // Zookeeper服务端地址
    std::string m_selfInfo;   // 当前客户端标识（IP:PORT）
    std::string m_selfData;   // 当前服务的附加数据 （用于节点值）

    ZKClient::ptr m_client;   // 与Zookeeper通信的客户端封装

    // 在使用 ZooKeeper 做服务注册与发现时：
    // 服务提供方（provider） 会向 ZooKeeper 注册临时节点，代表自己“在线”；
    // 服务调用方（consumer） 会订阅这些节点变化，感知服务上线/下线；
    // 但 ZooKeeper 存在一些 不稳定因素：
    // 会话（session）可能因为网络闪断而过期；
    // 临时节点会自动删除，但你必须重新创建；
    // 子节点变化可能未能触发 Watch（ZooKeeper 的 Watch 是一次性的）；
    // 为了解决这些问题，需要“定期检查+恢复”
    // 1. 重注册（re-register）
    // 服务提供方为了保证自己在 ZooKeeper 中的“存在”：
    // 每隔一段时间会检查 /sylar/domain/service/providers/IP:PORT 是否还存在；
    // 如果发现节点 被 ZooKeeper 删除了（如会话过期），就会 重新注册（re-create）该节点；
    // 保证其他消费者依然能发现它。
    // 重注册 = 再次创建自己的临时节点（IP:PORT）
    // 2. 重查询（re-query）
    // 服务调用方为了保证能持续获得可用服务节点：
    // 会周期性重新去 /sylar/domain/service/providers/ 读取当前所有子节点；
    // 即使 Watch 没有被触发（比如 ZooKeeper 丢了通知），也能恢复对服务的正确感知；
    // 如果发现有新节点加入或旧节点下线，会触发回调更新本地服务缓存。
    // 重查询 = 周期性主动拉取可用服务列表
    // 总结：
    // 重注册 是为了避免服务因 ZooKeeper session 失效而“下线”；
    // 重查询 是为了避免因为 Watch 丢失导致“看不到最新的服务列表”。
    sylar::Timer::ptr m_timer;// 定时器，用于周期性重注册、重查询

    // m_isOnTimer表示当前是否是 定时器触发的操作（比如：定时去重注册、重查询）；
    // 如果为 true，说明当前正在执行的是 定时任务 中的逻辑。                        
    // 这是为了实现 日志抑制（log suppressing） —— 避免定时器触发时产生太多无意义日志。
    bool m_isOnTimer = false;
};


}