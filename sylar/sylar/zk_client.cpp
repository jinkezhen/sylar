#include "zh_client.h"

namespace sylar {

const int ZKClient::EventType::CREATED = ZOO_CREATED_EVENT;
const int ZKClient::EventType::DELETED = ZOO_DELETED_EVENT;
const int ZKClient::EventType::CHANGED = ZOO_CHANGED_EVENT;
const int ZKClient::EventType::CHILD   = ZOO_CHILD_EVENT;
const int ZKClient::EventType::SESSION = ZOO_SESSION_EVENT;
const int ZKClient::EventType::NOWATCHING = ZOO_NOTWATCHING_EVENT;

const int ZKClient::FlagsType::EPHEMERAL = ZOO_EPHEMERAL;
const int ZKClient::FlagsType::SEQUENCE  = ZOO_SEQUENCE;
const int ZKClient::FlagsType::CONTAINER = ZOO_CONTAINER;

const int ZKClient::StateType::EXPIRED_SESSION = ZOO_EXPIRED_SESSION_STATE;
const int ZKClient::StateType::AUTH_FAILED = ZOO_AUTH_FAILED_STATE;
const int ZKClient::StateType::CONNECTING = ZOO_CONNECTING_STATE;
const int ZKClient::StateType::ASSOCIATING = ZOO_ASSOCIATING_STATE;
const int ZKClient::StateType::CONNECTED = ZOO_CONNECTED_STATE;
const int ZKClient::StateType::READONLY = ZOO_READONLY_STATE;
const int ZKClient::StateType::NOTCONNECTED = ZOO_NOTCONNECTED_STATE;

ZKClient::ZKClient()
    : m_handle(nullptr),
      m_recvTimeout(0) {
}

ZKClient::~ZKClient() {
    if (m_handle) {
        close();
    }
}

// void *watcherCtx来自于zookeeper初始化时传入的第5个参数
void ZKClient::OnWatcher(zhandle_t *zh, int type, int stat, const char* path, void *watcherCtx) {
    ZKClient* client = (ZKClient*)watcherCtx;
    clinet->m_watcherCb(type, stat, path);
}

bool ZKClient::reconnect() {
    if (m_handle) {
        zookeeper_close(m_handle);
    }
    // zhandle_t* zookeeper_init2(
    //     const char *host,             /* ZooKeeper服务器地址列表，格式如 "ip:port,ip:port" */
    //     watcher_fn watcher,           /* 事件回调函数指针，用于接收ZooKeeper异步事件通知 */
    //     int recv_timeout,             /* 会话超时时间，单位毫秒 */
    //     const clientid_t *clientid,   /* 客户端ID，通常传nullptr使用默认会话 */
    //     void *context,                /* 用户自定义上下文指针，回调函数中传递该指针 */
    //     int flags,                    /* 额外标志，通常传0 */
    //     zoo_log_stream_t log_stream   /* 日志输出接口，用于自定义日志回调 */
    // );
    m_handle = zookeeper_init2(m_hosts.c_str(), &ZKClient::OnWatcher, m_recvTimeout, nullptr, this, 0, m_logCb);
    return m_handle != nullptr;
}

bool ZKClient::init(const std::string& hosts, int recv_timeout, watcher_callback cb, log_callback lcb) {
    if (m_handle) {
        return true;
    }
    m_hosts = hosts;
    m_recvTimeout = recv_timeout;
    m_watcherCb = std::bind(cb, std::placeholders::_1,
                                std::placeholders::_2,
                                std::placeholders::_3,
                                shared_from_this());
    m_logCb = lcb;
    m_handle = zookeeper_init2(m_hosts.c_str(), &ZKClient::OnWatcher, m_recvTimeout, nullptr, this, 0, m_logCb);
    return m_handle != nullptr;
}

int32_t ZKClient::setServers(const std::string& hosts) {
    auto rt = zoo_set_servers(m_handle, hosts.c_str());
    if (rt == 0) {
        m_hosts = hosts;
    }
    return rt;
}

int32_t ZKClient::create(const std::string& path, const std::string& val, std::string& new_path, const struct ACL_vector* acl, int flags) {
    // 调用ZooKeeper C库函数，在服务器上创建一个节点
    // 参数说明：
    // m_handle        - ZooKeeper客户端连接句柄
    // path.c_str()    - 要创建节点的路径（如 "/app/config"）
    // val.c_str()     - 节点存储的数据内容
    // val.size()      - 数据长度
    // acl             - 节点访问控制列表（权限设置）
    // flags           - 节点类型标志（如临时节点、顺序节点）
    // &new_path[0]    - 用于输出新创建节点的实际路径（写入缓冲区）
    // new_path.size() - 输出缓冲区大小，需保证足够大
    return zoo_create(m_handle, path.c_str(), val.c_str(), val.size(), acl, flags, &new_path[0], new_path.size());    
}

// /**
//  * @brief 检查指定路径的节点是否存在
//  * 
//  * @param path   要检测的节点路径（如 "/app/config"）
//  * @param watch  是否注册 watcher，节点状态变化时触发事件通知
//  * @param stat   输出参数，返回节点的状态信息，传 nullptr 表示不需要
//  * 
//  * @return 返回码：
//  *         ZOK (0) 表示节点存在且调用成功
//  *         ZNONODE (-101) 表示节点不存在
//  *         其他错误码表示调用失败
//  */
// int32_t zoo_exists(
//     zhandle_t *zh,          // ZooKeeper连接句柄
//     const char *path,       // 要检查的节点路径
//     int watch,              // 是否注册 watcher（1 表示注册，0 表示不注册）
//     struct Stat *stat       // 输出节点状态结构体指针，允许为 nullptr
// );

// watch参数的介绍
// 这个是在调用 API（例如 zoo_exists()、zoo_get()、zoo_get_children()）时，传入 watch=1 表示“在该节点上注册一个 watcher”。
// 它告诉 ZooKeeper 服务端：
// “请在此节点上给这个客户端注册一个 watcher，一旦该节点发生变化，请调用该客户端之前初始化时指定的 watcher 回调函数。”
// 也就是说，watch=1 是动作，是请求服务端帮你“装上”监听器。
// 注意:当你为某个节点注册了 watcher 后，一旦该节点发生变化（如创建、删除、修改、子节点变化等），
//      这个 watcher 会被触发通知客户端，同时这个 watcher 就自动失效了。
//      失效后，如果你想继续监听该节点的后续变化，必须重新注册 watcher。          
int32_t ZKClient::exists(const std::string& path, bool watch, Stat* stat) {
    // 调用ZooKeeper C库函数检测节点是否存在
    // 传入客户端句柄m_handle，路径path，是否监听watch，状态stat指针
    return zoo_exists(m_handle, path.c_str(), watch, stat);
}

/**
 * int zoo_delete(zhandle_t *zh, const char *path, int version);
 *
 * @brief 删除 ZooKeeper 上指定路径的节点。
 *
 * @param zh        ZooKeeper 客户端句柄，由 zookeeper_init() 返回。
 * @param path      需要删除的节点路径。
 * @param version   版本号，用于乐观锁控制。
 *                  -1 表示忽略版本检查，直接删除最新版本节点。
 *                  其他非负整数表示仅当节点版本与该值匹配时才删除，否则失败。
 *
 * @return 返回状态码：
 *         ZOK (0) 表示删除成功，
 *         其他值表示失败，参见 ZooKeeper 错误码定义。
 */
int32_t ZKClient::del(const std::string& path, int version) {
    return zoo_delete(m_handle, path.c_str(), version);
}

//从 ZooKeeper 服务器上读取指定节点 (path) 的数据内容，
//并将数据拷贝到用户传入的缓冲区 (buffer) 中，同时可以选择注册一个 watcher 监听该节点的数据变化。
// int zoo_get(zhandle_t *zh, const char *path, int watch,
//             char *buffer, int *buffer_len, Stat *stat);
// zh：ZooKeeper客户端句柄，初始化时获得。
// path：要读取数据的节点路径。
// watch：是否注册watcher，1表示注册，0表示不注册。
// buffer：用于存放节点数据的缓冲区。
// buffer_len：传入缓冲区大小，返回时为实际数据长度。
// stat：可选，指向Stat结构体，用于返回节点元数据。
int32_t ZKClient::get(const std::string& path, std::string& val, bool watch, Stat* stat) {
    int len = val.size();
    int32_t rt = zoo_get(m_handle, path.c_str(), watch, &val[0], &len, stat);
    if (rt == ZOK) {
        val.resize(len);
    }
    return rt;
}

// ZOO_CONFIG_NODE 是 ZooKeeper 预定义的一个路径，通常是 /zookeeper/config，用于存储 ZooKeeper 集群的配置信息。
int32_t ZKClient::getConfig(std::string& val, bool watch, Stat* stat) {
    return get(ZOO_CONFIG_NODE, val, watch, stat);
}

/**
 * int zoo_set2(zhandle_t *zh, const char *path, const char *buffer, int buflen,
 *              int version, Stat *stat);
 *
 * @brief 设置指定节点的内容（更新/覆盖数据）。
 *
 * @param zh       ZooKeeper客户端句柄。
 * @param path     要设置数据的节点路径。
 * @param buffer   新的数据内容指针。
 * @param buflen   数据长度（字节数）。
 * @param version  版本号，用于乐观锁控制。
 *                 -1 表示忽略版本，直接写入最新数据。
 *                 其他值表示仅当节点版本匹配时才更新，否则失败。
 * @param stat     可选参数，调用成功时返回更新后的节点元信息。
 *
 * @return 返回状态码：
 *         ZOK(0) 表示成功，
 *         其他值表示失败（比如版本不匹配、节点不存在等）。
 */
int32_t ZKClient::set(const std::string& path, const std::string& val, int version, Stat* stat) {
    return zoo_set2(m_handle, path.c_str(), val.c_str(), val.size(), version, stat);
}

/**
 * int zoo_get_children2(zhandle_t *zh, const char *path, int watch,
 *                       struct String_vector *strings, Stat *stat);
 *
 * @brief 获取指定节点的所有子节点名称列表，并返回该节点的状态信息。
 *
 * @param zh       ZooKeeper客户端句柄，由 zookeeper_init() 返回。
 * @param path     要获取子节点列表的节点路径。
 * @param watch    是否注册 watcher，1 表示注册，0 表示不注册。
 * @param strings  用于存放返回的子节点名称列表（String_vector 类型）。
 * @param stat     用于返回指定节点的状态信息（Stat 类型）,stat参数返回的是传入的path节点本身的状态信息。
 *
 * @return 返回状态码：
 *         ZOK (0) 表示成功，
 *         其他值表示失败。
 *
 * @note 该函数返回指定节点的直接子节点，不包含更深层的后代节点。
 */
int32_t ZKClient::getChildren(const std::string& path, std::vector<std::string>& val, bool watch, Stat* stat) {
    String_vector strings;
    Stat tmp;
    if(stat == nullptr) {
        stat = &tmp;  // 如果外部没有传入状态指针，则使用临时变量保存
    }
    // 调用 zoo_get_children2 获取子节点及状态信息
    // strings.data[0] == "192.168.1.100:8080"
    // strings.data[1] == "192.168.1.101:8080"
    // strings.data[2] == "192.168.1.102:8080"
    int32_t rt = zoo_get_children2(m_handle, path.c_str(), watch, &strings, stat);
    if(rt == ZOK) {
        // 将 C 风格的 String_vector 转存到 std::vector<std::string>
        for(int32_t i = 0; i < strings.count; ++i) {
            val.push_back(strings.data[i]);
        }
        // 释放 ZooKeeper 分配的 String_vector 内存
        deallocate_String_vector(&strings);
    }
    return rt;
}



int32_t ZKClient::close() {
    m_watcherCb = nullptr;
    int32_t rt = ZOK;
    if(m_handle) {
        rt = zookeeper_close(m_handle);
        m_handle = nullptr;
    }
    return rt;
}

/**
 * const char *zoo_get_current_server(zhandle_t *zh);
 *
 * @brief 获取当前客户端连接的 ZooKeeper 服务器地址（IP:端口格式）。
 *
 * @param zh  ZooKeeper 客户端句柄，由 zookeeper_init() 返回。
 *
 * @return 返回一个指向字符串的指针，表示当前连接的服务器地址。
 *         如果没有连接服务器或连接不可用，返回 nullptr。
 *
 * @note 返回的字符串指针指向 ZooKeeper 客户端内部数据，调用者不应释放该指针。
 */
std::string ZKClient::getCurrentServer() {
    auto rt = zoo_get_current_server(m_handle);
    return rt == nullptr ? "" : rt;
}

/**
 * int zoo_state(zhandle_t *zh);
 *
 * @brief 获取 ZooKeeper 客户端当前连接的状态。
 *
 * @param zh  ZooKeeper 客户端句柄，由 zookeeper_init() 返回。
 *
 * @return 返回当前连接状态的整数值，状态码定义包括但不限于：
 *         - ZOO_EXPIRED_SESSION_STATE：会话过期
 *         - ZOO_AUTH_FAILED_STATE：认证失败
 *         - ZOO_CONNECTING_STATE：正在连接
 *         - ZOO_ASSOCIATING_STATE：关联中
 *         - ZOO_CONNECTED_STATE：已连接
 *         - ZOO_READONLY_STATE：只读连接
 *         - ZOO_NOTCONNECTED_STATE：未连接
 *
 * 具体状态码可参考 ZooKeeper 的状态定义。
 */
int32_t ZKClient::getState() {
    return zoo_state(m_handle);
}



}