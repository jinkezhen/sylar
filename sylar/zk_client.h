#ifndef __SYLAR_ZK_CLIENT_H__      // 头文件保护，防止重复包含
#define __SYLAR_ZK_CLIENT_H__

#include <memory>
#include <functional>
#include <string>
#include <stdint.h>
#include <vector>

#ifndef THREADED
#define THREADED
#endif
// ↑↑↑ 这个宏定义是为了确保 zookeeper C 客户端库在多线程模式下工作。
// 如果不定义 THREADED，ZooKeeper 可能只初始化单线程客户端（非线程安全）。
// ZooKeeper 的头文件 <zookeeper/zookeeper.h> 中根据该宏决定是否启用多线程支持。

#include <zookeeper/zookeeper.h>


// Zookeeper 的“节点”本质上是一个树形数据结构中的“ZNode”，
// 每个 ZNode 都有一个路径标识，看起来像文件路径，但它还可以存储数据、支持监听，是一种比文件路径更“活”的结构。


namespace sylar {

/**
 * @brief ZooKeeper 客户端封装类
 */
class ZKClient : public std::enable_shared_from_this<ZKClient> {
public:
    // 事件类型，对应 zookeeper.h 中的 ZOO_* 宏
    class EventType {
    public:
        static const int CREATED;       // 节点被创建事件
        static const int DELETED;       // 节点被删除事件
        static const int CHANGED;       // 节点数据被改变事件
        static const int CHILD;         // 子节点变更事件
        static const int SESSION;       // 会话相关事件
        static const int NOWATCHING;    // watcher 被取消事件
    };

    // 创建节点时的可选标志位（组合使用）
    class FlagsType {
    public:
        static const int EPHEMERAL;     // 临时节点（会话结束后删除）
        static const int SEQUENCE;      // 顺序节点（自动添加编号后缀）
        static const int CONTAINER;     // 容器节点（仅在非空时保留）
    };

    // 客户端连接状态类型
    class StateType {
    public:
        static const int EXPIRED_SESSION;   // 会话过期
        static const int AUTH_FAILED;       // 权限验证失败
        static const int CONNECTING;        // 正在连接中
        static const int ASSOCIATING;       // 会话关联中
        static const int CONNECTED;         // 已连接
        static const int READONLY;          // 只读模式
        static const int NOTCONNECTED;      // 未连接
    };

    typedef std::shared_ptr<ZKClient> ptr;

    // 监听器回调类型，type为事件类型，stat为状态，path为触发路径
    typedef std::function<void(int type, int stat, const std::string& path, ZKClient::ptr)> watcher_callback;

    // 日志输出回调函数类型
    typedef void(*log_callback)(const char *message);

    ZKClient();  ///< 构造函数
    ~ZKClient(); ///< 析构函数，通常会释放句柄

    /**
     * @brief 初始化 ZooKeeper 客户端连接
     * @param hosts zookeeper 服务器列表，格式 ip:port[,ip:port]
     * @param recv_timeout 接收超时时间（ms）
     * @param cb watcher 回调函数
     * @param lcb 日志回调函数（可选）
     * @return 是否初始化成功
     */
    bool init(const std::string& hosts, int recv_timeout, watcher_callback cb, log_callback lcb = nullptr);

    // 设置服务器地址
    int32_t setServers(const std::string& hosts);

    // 创建节点
    int32_t create(const std::string& path, const std::string& val, std::string& new_path,
                   const struct ACL_vector* acl = &ZOO_OPEN_ACL_UNSAFE,
                   int flags = 0);

    // 判断节点是否存在，可选设置 watch
    int32_t exists(const std::string& path, bool watch, Stat* stat = nullptr);

    // 删除节点
    int32_t del(const std::string& path, int version = -1);

    // 获取节点内容
    int32_t get(const std::string& path, std::string& val, bool watch, Stat* stat = nullptr);

    // 获取配置（ZooKeeper ensemble 模式支持配置节点）
    int32_t getConfig(std::string& val, bool watch, Stat* stat = nullptr);

    // 设置节点值
    int32_t set(const std::string& path, const std::string& val, int version = -1, Stat* stat = nullptr);

    // 获取子节点列表
    int32_t getChildren(const std::string& path, std::vector<std::string>& val, bool watch, Stat* stat = nullptr);

    // 主动关闭连接
    int32_t close();

    // 获取当前连接状态
    int32_t getState();

    // 获取当前连接使用的服务器
    std::string getCurrentServer();

    // 重新连接
    bool reconnect();

private:
    // 静态全局 watcher 回调，用于接收来自 ZooKeeper 的事件通知
    static void OnWatcher(zhandle_t *zh, int type, int stat, const char *path, void *watcherCtx);

    // 实际绑定给客户端的 watcher 回调
    typedef std::function<void(int type, int stat, const std::string& path)> watcher_callback2;

private:
    zhandle_t* m_handle;              ///< ZooKeeper 原始连接句柄
    std::string m_hosts;             ///< 当前连接的服务器地址列表
    watcher_callback2 m_watcherCb;   ///< 内部 watcher 事件处理回调
    log_callback m_logCb;            ///< 日志输出回调
    int32_t m_recvTimeout;           ///< 接收超时时间（毫秒）
};

} // namespace sylar

#endif // __SYLAR_ZK_CLIENT_H__
