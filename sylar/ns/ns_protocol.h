#ifndef __SYLAR_NS_NS_PROTOCOL_H__
#define __SYLAR_NS_NS_PROTOCOL_H__

#include <memory>
#include <string>
#include <map>
#include <iostream>
#include <stdint.h>
#include "sylar/mutex.h"
#include "ns_protobuf.pb.h"

namespace sylar {
namespace ns {

// Rock 是传输层协议（比如 RPC 框架）
// ns_protocol.h 是基于 Rock 的 应用层协议设计，定义了“如何通过 Rock 表达注册、查询、心跳”等操作。

// | 类名          | 说明                                |
// | ------------- | --------------------------------- |
// | `NSNode`      | 表示单个节点（ip、port、weight）            |
// | `NSNodeSet`   | 某一类节点集合（如注册节点、黑名单节点）              |
// | `NSDomain`    | 某个域名下的所有节点分类（每个 cmd 对应一个 NodeSet） |
// | `NSDomainSet` | 所有域名下的管理（整个系统的节点结构顶层）             |

// 这种设计形成了一个域名 -> 命令类型 -> 节点 ID 的 3 层树状结构，非常适合注册中心服务或配置中心中对节点进行分类和查询。

//NSDomainSet 是顶级容器，通过 std::map<std::string, NSDomain::ptr> 管理多个 NSDomain 实例，每个 NSDomain 对应一个域名；
//NSDomain 通过 std::map<uint32_t, NSNodeSet::ptr> 管理多个 NSNodeSet 实例，每个 NSNodeSet 对应一个命令类型（如注册、查询）；
//NSNodeSet 通过 std::map<uint64_t, NSNode::ptr> 管理多个 NSNode 实例，每个 NSNode 表示一个节点的 IP、端口和权重等信息。
//所有层级均通过线程安全的读写锁（sylar::RWMutex）保护数据操作，形成从域名 → 命令类型 → 节点 ID 的多级分类管理结构。
//系统实现了对分布式节点的高效分类管理。


// 定义一组预设的命令类型，用于标识网络服务节点管理系统中不同操作的指令。
// 这些命令会配合 .proto 文件中定义的 protobuf 消息（如 ns_protobuf.proto）通过 Rock 协议传输，形成应用层 RPC 协议语义。
enum class NSCommand {
	// 注册节点信息
	// 当新节点加入系统时，通过此命令将节点信息（如 IP、端口、权重）注册到服务管理模块中。
	REGISTER = 0x10001,
	// 查询节点信息
	// 客户端或其他服务需要查询某个节点的详细信息（如 IP、端口、权重）时使用此命令
	QUERY = 0x10002,
	// 设置黑名单
	// 将某个节点加入黑名单（例如因异常行为被禁用），防止其继续参与服务
	SET_BLACKLIST = 0x10003,
	// 查询黑名单
	// 获取当前黑名单中的节点列表，用于监控或告警
	QUERY_BLACKLIST = 0x10004,
	// 心跳
	// 节点定期发送心跳包，表明自身存活状态，防止被误判为失效节点
	TICK = 0x10005
};

// 定义一组预设的通知类型，用于标识系统中需要广播或处理的特定事件或状态变化
enum class NSNotify {
	NODE_CHANGE = 0x10001
};

/**
 * @class NSNode
 * @brief 表示网络服务中的节点信息，用于存储和管理节点的基本属性（如IP、端口、权重）及唯一标识。
 *
 * 该类封装了节点的核心数据，包括IP地址、端口号、权重和唯一ID。通过智能指针（std::shared_ptr）管理实例，
 * 适用于分布式系统中的节点注册、查询、负载均衡等场景。
 */
class NSNode {
public:
    typedef std::shared_ptr<NSNode> ptr;

    /**
     * @brief 构造函数，初始化节点的IP地址、端口和权重。
     *
     * @param ip 节点的IP地址（字符串格式，如 "192.168.1.1"）
     * @param port 节点的端口号（范围 0~65535）
     * @param weight 节点的权重值（用于负载均衡算法，值越大优先级越高）
     */
    NSNode(const std::string& ip, uint16_t port, uint32_t weight);

    const std::string& getIp() const { return m_ip; }

    uint16_t getPort() const { return m_port; }

    uint32_t getWeight() const { return m_weight; }

    void setWeight(uint32_t v) { m_weight = v; }

    uint64_t getId() const { return m_id; }

    /**
     * @brief 根据 IP 和端口生成节点的唯一ID。
     *
     * 该静态方法通过组合 IP 和端口生成一个全局唯一的 64 位 ID。
     *
     * @param ip 节点的IP地址
     * @param port 节点的端口号
     * @return uint64_t 生成的唯一ID
     */
    static uint64_t GetID(const std::string& ip, uint16_t port);

    std::ostream& dump(std::ostream& os, const std::string& prefix = "");

    std::string toString(const std::string& prefix = "");

private:
    /**
     * @var m_id
     * @brief 节点的唯一ID（64位无符号整数）。
     *
     * 由 GetID 方法基于 IP 和端口生成，确保同一网络环境下的全局唯一性。
     */
    uint64_t m_id;

    /**
     * @var m_ip
     * @brief 节点的IP地址（字符串格式）。
     *
     * 创建后不可修改，确保节点身份的稳定性。
     */
    std::string m_ip;

    /**
     * @var m_port
     * @brief 节点的端口号（16位无符号整数）。
     *
     * 创建后不可修改，确保节点服务的稳定性。
     */
    uint16_t m_port;

    /**
     * @var m_weight
     * @brief 节点的权重值（32位无符号整数）。
     *
     * 用于负载均衡算法，值越大表示优先级越高。可动态调整。
     */
    uint32_t m_weight;
};


/**
 * @class NSNodeSet
 * @brief 管理一组 NSNode 节点的集合类，提供线程安全的操作接口。
 *
 * 该类使用 `std::map<uint64_t, NSNode::ptr>` 存储节点，通过读写锁（`sylar::RWMutex`）确保线程安全。
 * 支持节点的添加、删除、查询、遍历等操作，并可关联一个命令类型（`m_cmd`）用于标识集合用途。
 * 适用于分布式系统中的节点管理模块。
 */
class NSNodeSet {
public:
    typedef std::shared_ptr<NSNodeSet> ptr;

    // param cmd 命令类型（如 NSCommand 中的值），用于标识该集合的用途。
    NSNodeSet(uint32_t cmd);

    /**
     * @brief 添加一个节点到集合中。
     *
     * @param info 要添加的 NSNode 实例（智能指针）
     *
     * 注意：若集合中已存在相同 ID 的节点，新节点会覆盖旧节点。
     * 该操作通过读写锁的写锁保护，确保线程安全。
     */
    void add(NSNode::ptr info);

    // 从集合中删除指定 ID 的节点。
    NSNode::ptr del(uint64_t id);

    // 获取指定 ID 的节点。
    NSNode::ptr get(uint64_t id);

    // 获取集合关联的命令类型。
    uint32_t getCmd() const { return m_cmd; }

    // 设置集合关联的命令类型。
    void setCmd(uint32_t v) { m_cmd = v; }

    // 将集合中的所有节点导出到 vector 中。
    void listAll(std::vector<NSNode::ptr>& infos);

    // 将集合信息输出到指定的输出流。
    std::ostream& dump(std::ostream& os, const std::string& prefix = "");

    // 将集合信息转换为字符串表示。
    std::string toString(const std::string& prefix = "");

    // 获取集合中节点的数量。
    size_t size();

private:
    sylar::RWMutex m_mutex;

    /**
     * @var m_cmd
     * @brief 关联的命令类型（如 NSCommand 中的值），用于标识集合的用途。
     *
     * 例如，集合可能用于注册节点（`REGISTER`）、黑名单管理（`SET_BLACKLIST`）等场景。
     */
    uint32_t m_cmd;

    /**
     * @var m_datas
     * @brief 存储节点的 map，键为节点唯一 ID，值为 NSNode 实例（智能指针）。
     *
     * 通过 `uint64_t` ID 作为键，确保节点的快速查找和唯一性。
     */
    std::map<uint64_t, NSNode::ptr> m_datas;
};


/**
 * @class NSDomain
 * @brief 管理与域名关联的节点集合容器，支持按命令类型分类管理节点集合（NSNodeSet）。
 *
 * 该类通过 `std::map<uint32_t, NSNodeSet::ptr>` 存储多个 `NSNodeSet` 实例，每个 `NSNodeSet` 对应一个命令类型（如注册、黑名单）。
 * 提供线程安全的操作接口（通过 `sylar::RWMutex`），适用于分布式系统中按域名隔离的节点管理场景。
 */
class NSDomain {
public:
    typedef std::shared_ptr<NSDomain> ptr;

    /**
     * @brief 构造函数，初始化域名并关联一组节点集合。
     *
     * @param domain 域名，用于标识该域名下的节点管理范围。
     */
    NSDomain(const std::string& domain)
        : m_domain(domain) {}

    // 获取当前域名。
    const std::string& getDomain() const { return m_domain; }

    // 设置新的域名。
    void setDomain(const std::string& v) { m_domain = v; }

    // 添加一个完整的节点集合（NSNodeSet）到当前域名下。
    void add(NSNodeSet::ptr info);

    // 向指定命令类型的集合中添加一个节点。
    void add(uint32_t cmd, NSNode::ptr info);

    /**
     * @brief 删除指定命令类型的节点集合。
     *
     * @param cmd 命令类型（如 `BLACKLIST`）
     *
     * 该操作通过写锁保护，确保线程安全。
     * 删除后，该命令类型对应的所有节点将被清除。
     */
    void del(uint32_t cmd);

    /**
     * @brief 从指定命令类型的集合中删除一个节点。
     *
     * @param cmd 命令类型
     * @param id 要删除的节点的唯一 ID
     * @return NSNode::ptr 被删除的节点（若不存在则返回空指针）
     *
     * 该操作通过写锁保护，确保线程安全。
     */
    NSNode::ptr del(uint32_t cmd, uint64_t id);

    /**
     * @brief 获取指定命令类型的节点集合。
     *
     * @param cmd 命令类型
     * @return NSNodeSet::ptr 对应的节点集合（若不存在则返回空指针）
     *
     * 该操作通过读锁保护，确保线程安全。
     */
    NSNodeSet::ptr get(uint32_t cmd);

    /**
     * @brief 将当前域名下的所有节点集合导出到 vector 中。
     *
     * @param infos 用于存储结果的 vector
     *
     * 该操作通过读锁保护，确保线程安全。
     */
    void listAll(std::vector<NSNodeSet::ptr>& infos);

    std::ostream& dump(std::ostream& os, const std::string& prefix = "");

    std::string toString(const std::string& prefix = "");

    // 获取当前域名下的节点集合总数。
    size_t size();

private:
    /**
     * @var m_domain
     * @brief 关联的域名，用于标识该 NSDomain 管理的节点范围。
     *
     * 注册节点、黑名单节点等均通过此域名隔离。
     */
    std::string m_domain;

    sylar::RWMutex m_mutex;

    /**
     * @var m_datas
     * @brief 存储节点集合的 map，键为命令类型（如 NSCommand 中的值），值为 NSNodeSet 实例。
     *
     * 每个命令类型对应一个 NSNodeSet，用于分类管理节点（如注册、心跳、黑名单）。
     */
    std::map<uint32_t, NSNodeSet::ptr> m_datas;
};


/**
 * @class NSDomainSet
 * @brief 管理一组 NSDomain 域名集合的容器类，支持按域名分类管理节点集合（NSNodeSet）。
 *
 * 该类通过 `std::map<std::string, NSDomain::ptr>` 存储多个 `NSDomain` 实例，每个 `NSDomain` 对应一个域名。
 * 提供线程安全的操作接口（通过 `sylar::RWMutex`），适用于分布式系统中按域名隔离的节点管理场景。
 */
class NSDomainSet {
public:
    typedef std::shared_ptr<NSDomainSet> ptr;

    //向集合中添加一个 NSDomain 实例。
    void add(NSDomain::ptr info);

    //根据域名删除对应的 NSDomain 实例。
    void del(const std::string& domain);

    /**
     * @brief 获取指定域名的 NSDomain 实例。
     *
     * @param domain 目标域名
     * @param auto_create 是否自动创建（若不存在）
     * @return NSDomain::ptr 对应的 NSDomain 实例（若不存在且 auto_create 为 false 则返回空指针）
     *
     * 该操作通过读锁保护，确保线程安全。
     * 若 auto_create 为 true 且域名不存在，则自动创建一个新的 NSDomain 实例并添加到集合中。
     */
    NSDomain::ptr get(const std::string& domain, bool auto_create = false);

    /**
     * @brief 根据域名、命令类型和节点 ID 删除指定节点。
     *
     * @param domain 域名
     * @param cmd 命令类型（如 NSCommand 中的值）
     * @param id 节点的唯一 ID
     *
     * 该操作通过写锁保护，确保线程安全。
     * 首先获取对应的 NSDomain 和 NSNodeSet，再调用 NSNodeSet 的 del 方法。
     */
    void del(const std::string& domain, uint32_t cmd, uint64_t id);

    // 将集合中的所有 NSDomain 实例导出到 vector 中。
    void listAll(std::vector<NSDomain::ptr>& infos);

    // 将集合信息输出到指定的输出流。
    std::ostream& dump(std::ostream& os, const std::string& prefix = "");

    // 将集合信息转换为字符串表示。
    std::string toString(const std::string& prefix = "");

    // 交换两个 NSDomainSet 实例的内容。
    void swap(NSDomainSet& ds);

private:
    sylar::RWMutex m_mutex;

    /**
     * @var m_datas
     * @brief 存储 NSDomain 实例的 map，键为域名（字符串），值为 NSDomain 实例（智能指针）。
     *
     * 每个域名对应一个 NSDomain，用于管理该域名下的节点集合（NSNodeSet）。
     */
    std::map<std::string, NSDomain::ptr> m_datas;
};

}
}

#endif