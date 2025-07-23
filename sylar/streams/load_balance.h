#ifndef __SYLAR_STREAMS_SOCKET_STREAM_POOL_H__
#define __SYLAR_STREAMS_SOCKET_STREAM_POOL_H__

#include "sylar/streams/socket_stream.h"
#include "sylar/mutex.h"
#include "sylar/util.h"
#include "sylar/streams/service_discovery.h"
#include <vector>
#include <string>
#include <unordered_map>

// 负载均衡本质上是让客户端请求能够智能地分配给多个后端服务节点，
// 避免单点瓶颈或过载，提升系统整体性能和容错能力。在结合 ZooKeeper 的场景中，
// ZooKeeper作为服务注册中心，会维护服务节点的列表，
// 负载均衡模块则根据这些节点信息和统计数据，动态调整请求路由策略，
// 保证请求在健康且可用的节点间合理分配，提升分布式系统的稳定性和扩展性。

namespace sylar {

class HolderStatsSet;
/**
 * @brief 用于记录某个连接（或任务单元）在负载均衡过程中的运行统计数据。
 * 
 * 该类提供了一组线程安全的计数器接口，记录连接使用次数、请求执行中数量、
 * 超时数量、成功/失败次数等信息。负载均衡策略（如公平调度）可基于这些指标
 * 评估连接的健康程度和权重，从而实现动态调度。
 */
class HolderStats {
    friend class HolderStatsSet;
public:
    /// 获取累计使用时间（单位：毫秒）
    uint32_t getUsedTime() const { return m_usedTime; }

    /// 获取总请求次数
    uint32_t getTotal() const { return m_total; }

    /// 获取当前正在处理的请求数
    uint32_t getDoing() const { return m_doing; }

    /// 获取请求超时次数
    uint32_t getTimeouts() const { return m_timeouts; }

    /// 获取成功完成的请求数
    uint32_t getOks() const { return m_oks; }

    /// 获取错误请求数
    uint32_t getErrs() const { return m_errs; }
    
    /// 增加使用时间（通常由外部统计调用时长后填充）
    uint32_t incUsedTime(uint32_t v) { return sylar::Atomic::addFetch(m_usedTime, v); }

    /// 增加总请求数
    uint32_t incTotal(uint32_t v) { return sylar::Atomic::addFetch(m_total, v); }

    /// 增加正在处理的请求数
    uint32_t incDoing(uint32_t v) { return sylar::Atomic::addFetch(m_doing, v); }

    /// 增加超时请求数
    uint32_t incTimeouts(uint32_t v) { return sylar::Atomic::addFetch(m_timeouts, v); }

    /// 增加成功请求数
    uint32_t incOks(uint32_t v) { return sylar::Atomic::addFetch(m_oks, v); }

    /// 增加错误请求数
    uint32_t incErrs(uint32_t v) { return sylar::Atomic::addFetch(m_errs, v); }

    /// 减少正在处理的请求数（请求完成时调用）
    uint32_t decDoing(uint32_t v) { return sylar::Atomic::subFetch(m_doing, v); }

    // 清空所有统计数据
    void clear();

    /**
     * @brief 计算当前统计数据对应的权重值
     * 
     * 权重越高，说明该连接越“健康”，越倾向于被负载均衡器选中。
     * 具体计算方式可考虑成功率、并发量等因素。
     * 
     * @param rate 额外的缩放因子（默认为1.0）
     * @return float 当前权重值
     */
    float getWeight(float rate = 1.0f);
    
    /**
     * @brief 将统计数据转为可读字符串（用于日志打印）
     * 
     * 示例输出：
     * used=1234ms total=100 doing=2 ok=90 err=5 timeout=5
     */
    std::string toString();    

private:
    // 连接：一个客户端与某个远程服务端建立的 SocketStream（TCP连接）对象。
    //      “连接”指的是你从 Zookeeper 获取到的某个服务节点地址（比如某个 RPC Server），然后你通过这个地址建立的 TCP 连接（也就是 SocketStream）。
    //      不是指连接 Zookeeper 本身的连接（比如用 zookeeper_init() 创建的那种）。
    // 你启动了两个 RPC 服务实例，它们在 Zookeeper 中注册成：
    // /service/user_rpc/instance1 = 192.168.1.100:9000
    // /service/user_rpc/instance2 = 192.168.1.101:9000
    // 客户端从 Zookeeper 获取这两个地址后，建立两个 Socket 连接：
    // SocketStream("192.168.1.100", 9000)
    // SocketStream("192.168.1.101", 9000)
    // 每个 SocketStream 都有一个对应的 HolderStats，用于记录：
    //  当前是否在忙（m_doing）
    //  成功率（m_oks / m_total）
    //  是否响应慢（m_usedTime）.....

    // 记录该连接累计处理请求所花费的总耗时，单位通常为ms
    uint32_t m_usedTime = 0;
    // 从程序启动或统计开始以来，该连接接收过的总请求数
    uint32_t m_total = 0;
    // 该连接正在处理但还未完成的请求数量（即并发度）
    uint32_t m_doing = 0;
    // 记录该连接上发生的请求超时数量
    uint32_t m_timeouts = 0;
    // 记录成功完成并得到正常响应的请求数
    uint32_t m_oks = 0;
    // 记录返回错误结果的请求数
    uint32_t m_errs = 0;
};



// HolderStatsSet它不是管理多个连接，而是：
// 管理一个连接在不同时间段内的运行状态统计信息（HolderStats）的集合。
// 也就是说，你可以把它想象成：一个连接的“历史状态快照合集”,用来追踪这个连接过去一段时间内的表现
// 为什么要管理多个时间点的统计？
//      在负载均衡场景下，你不能只看连接当前的一点数据，那样不够稳定，容易波动大。
//      举个例子：
//          现在连接正好比较忙，恰巧出现了几次超时或错误 → 直接根据当前瞬间数据来判断会误判连接质量差。
//          过去一段时间里连接一直表现很好，偶尔有一次小故障，也不该马上被剔除。
// 什么是时间窗口？
//      时间窗口，就是把时间划分成一个个小段，每段记录这段时间内的统计数据。
//      比如：
//          每秒统计一次
//          一共维护 5 个时间段的统计（最近 5 秒）
//          那么你就有 5 份数据，分别对应过去 1 秒，2 秒，3 秒……直到 5 秒前的状态。
// 什么是滑动窗口？
//      滑动窗口，是时间窗口的动态移动：
//      时间一直往前走，比如从秒 100 到秒 101，
//      这时候窗口也“滑动”了，过去第 6 秒的统计被丢弃，第 1 秒的统计变成最新。
//      通过滑动，统计数据能实时反映最新的状态变化。
/**
 * @class HolderStatsSet
 * @brief 管理同一连接在多个连续时间段内的统计数据集合，实现滑动时间窗口统计机制。
 * 
 * 该类维护一个固定大小的 HolderStats 容器（m_stats），
 * 每个 HolderStats 代表一个时间段（通常是秒级别）的统计信息，
 * 通过时间窗口滑动实现对连接状态的动态监控和权重计算。
 * 
 * 主要用途是在负载均衡时，根据历史和当前统计数据综合评估连接健康度，
 * 平滑波动，提升调度稳定性和响应灵敏度。
 */
class HolderStatsSet {
public:
    /**
     * @brief 构造函数，初始化时间窗口数量。
     * @param size 时间窗口数量，默认为5，表示维护最近5个时间段的数据。
     * 
     * 时间窗口数决定了统计的历史跨度和粒度。
     */
    HolderStatsSet(uint32_t size = 5);

    /**
     * @brief 获取当前时间对应的 HolderStats 统计对象。
     * @return 当前时间窗口对应的 HolderStats 引用。
     * 
     * 根据传入的时间判断是否需要滑动窗口更新（清理过期窗口），
     * 返回对应时间段的统计数据，支持对该时间段内的数据进行统计和修改。
     */
    HolderStats& get(const uint32_t& now = time(0));

    /**
     * @brief 计算当前时间点的连接权重。
     * @return 连接权重的浮点值。
     * 
     * 基于多个时间窗口内的统计数据综合计算权重，
     * 以平滑连接的动态表现，辅助负载均衡策略做出合理选择。
     */
    float getWeight(const uint32_t& now = time(0));

    /**
     * @brief 获取所有时间窗口的统计数据总和。
     * @return 累积的 HolderStats 对象，包含所有时间段的统计汇总。
     * 
     * 用于获取连接在整个统计周期内的整体表现。
     */
    HolderStats getTotal();

private:
    /**
     * @brief 内部初始化与滑动窗口处理函数。
     * 
     * 用于初始化时间窗口数据，
     * 并根据时间推进，滑动窗口，清理过期统计，保证统计数据的时效性。
     */
    void init(const uint32_t& now);

private:
    uint32_t m_lastUpdateTime = 0;         ///< 上一次更新时间，单位秒。用于判断是否需要滑动窗口。
    std::vector<HolderStats> m_stats;      ///< 持有多个时间窗口的统计数据，每个元素对应一个时间段。
};


/**
 * @class LoadBalanceItem
 * @brief 表示一个可参与负载均衡的连接项（即一个服务节点的抽象）
 * 
 * 一个 LoadBalanceItem 通常对应一个可连接的远程服务节点（如某个 RPC 实例），
 * 它内部持有一个 `SocketStream::ptr` 指向具体的连接对象，
 * 并通过 `HolderStatsSet` 来维护该连接的统计信息，用于健康状态评估与调度权重计算。
 * 
 * 此类是负载均衡的基础单元，可被 RoundRobin、Weighted、Fair 等策略管理。
 */
class LoadBalanceItem {
public:
    typedef std::shared_ptr<LoadBalanceItem> ptr;
    virtual ~LoadBalanceItem() {}

    // 获取连接对应的SockerStream
    SocketStream::ptr getStream() const { return m_stream; }

    // 设置连接对象
    void setStream(SocketStream::ptr v) { m_stream = v; }

    // 设置唯一标识id，通常是服务节点id，来自于服务发现
    void setId(uint64_t v) { m_id = v; }

    // 获取连接唯一id
    uint64_t getId() const { return m_id; }

    /**
     * @brief 获取指定时间点对应的 HolderStats（统计窗口）
     * @param now 当前时间戳（秒），默认为系统当前时间
     * @return 对应时间窗口的统计对象（可修改）
     */
    HolderStats& get(const uint32_t& now = time(0));

    /**
     * @brief 获取封装流对象的派生类指针（如获取为 SSLSocketStream）
     * @tparam T 派生类型
     * @return 如果类型匹配，返回 dynamic_pointer_cast<T> 后的指针
     */
    template<class T>
    std::shared_ptr<T> getStreamAs() {
        return std::dynamic_pointer_cast<T>(m_stream);
    }

    /**
     * @brief 获取该连接的预设静态权重（非运行时评分）
     * @return 权重值，默认是 0
     */
    virtual int32_t getWeight() { return m_weight; }
    
    /**
     * @brief 设置预设静态权重（可用于 Weighted 负载均衡）
     */
    void setWeight(int32_t v) { m_weight = v; }
    
    /**
     * @brief 判断该连接是否可用（如 socket 是否断开、超时、异常等）
     * 默认实现是基于 `SocketStream::isConnected()`，子类可重写。
     * @return 是否有效
     */
    virtual bool isValid();
    
    // 关闭连接，释放资源
    void close();

    std::string toString();


protected:
    uint64_t m_id = 0;                       ///< 唯一标识ID，一般来自服务发现
    SocketStream::ptr m_stream;              ///< 封装的 socket 流连接对象
    int32_t m_weight = 0;                    ///< 预设静态权重（用于 Weighted 策略）
    HolderStatsSet m_stats;                  ///< 多时间段的动态统计信息（用于动态权重评估）
};


/**
 * @brief ILoadBalance 是所有负载均衡策略的抽象接口类
 * 
 * 该接口定义了一个核心功能：根据某种策略，从多个候选连接中选择一个最合适的连接（即 LoadBalanceItem）。
 * 
 * 所有的具体负载均衡实现（如轮询、加权、最优等）都应继承该接口并实现 `get()` 方法。
 * 
 * 该类配合 ServiceDiscovery（服务发现）与 SDLoadBalance（集成管理器）使用，
 * 为 RPC 调用、Socket 分发等场景提供灵活的负载选择机制。
 */
class ILoadBalance {
public:
    // 支持的负载均衡策略枚举
    enum Type {
        ROUNDROBIN = 1, ///< 轮询负载均衡（Round Robin）
        WEIGHT     = 2, ///< 加权负载均衡（按静态 weight 字段选择）
        FAIR       = 3  ///< 公平调度（根据动态统计如延迟、成功率等实时权重选择）
    };

    // 负载均衡失败时的错误码
    enum Error {
        NO_SERVICE    = -101, ///< 没有可用的服务（服务发现失败或为空）
        NO_CONNECTION = -102  ///< 找不到可用连接（可能都无效或都断开）
    };

    typedef std::shared_ptr<ILoadBalance> ptr;

    virtual ~ILoadBalance() {}

    /**
     * @brief 获取一个可用的负载均衡连接项
     * 
     * @param v 输入参数（通常为 hash 或请求 ID），用于 hash 或其他策略使用；
     *          对于 RoundRobin/Fair 负载可能忽略此参数。
     * 
     * @return LoadBalanceItem::ptr 返回一个连接节点（可能是 socket、rpc 等）；
     *         返回空时通常表示负载失败，调用方需要容错处理。
     */
    virtual LoadBalanceItem::ptr get(uint64_t v = -1) = 0;    
};

/**
 * @brief LoadBalance 是 ILoadBalance 接口的基础实现类，封装了负载均衡公共逻辑
 * 
 * 该类负责管理一组连接项（LoadBalanceItem），提供线程安全的注册、移除、更新和查询操作。
 * 其本身并不定义具体的负载策略，而是将选择逻辑交由子类（如 RoundRobinLoadBalance、WeightLoadBalance 等）实现。
 * 
 * 所有具体的负载均衡策略都应继承自该类，并实现纯虚函数 initNolock() 和重载 get() 方法。
 * 
 * 设计模式：Template Method（模板方法模式）
 */
class LoadBalance : public ILoadBalance {
public:
    typedef sylar::RWMutex RWMutexType;
    typedef std::shared_ptr<LoadBalance> ptr;

    // 向负载均衡池中添加一个连接项
    void add(LoadBalanceItem::ptr v);
    // 从负载均衡池中移除一个连接项
    void del(LoadBalanceItem::ptr v);
    // 替换当前所有连接项（全量覆盖）
    void set(const std::vector<LoadBalanceItem::ptr>& vs);
    // 根据id查询对应连接项
    LoadBalanceItem::ptr getById(uint64_t id);
    /**
     * @brief 更新连接池：应用新增和删除的节点
     * 
     * @param adds 要添加的连接项（以 ID 为 key）
     * @param dels 接收被删除的旧连接项（以 ID 为 key）
     */
    void update(const std::unordered_map<uint64_t, LoadBalanceItem::ptr>& adds,
                std::unordered_map<uint64_t, LoadBalanceItem::ptr>& dels);
    // 初始化连接项的缓存，通常在第一次get()前执行
    void init();
    // 获取当前负载池的状态信息（字符串格式）
    std::string statusString(const std::string& prefix);

protected:
    // 子类实现的初始化逻辑
    // 及时更新内部负载均衡结构
    virtual void initNolock() = 0;
    // 内部监查是否需要重新初始化
    void checkInit();

protected:
    RWMutexType m_mutex; ///< 用于保护 m_datas 的读写并发访问
    std::unordered_map<uint64_t, LoadBalanceItem::ptr> m_datas; ///< 原始连接项数据（以 ID 映射）
    uint64_t m_lastInitTime = 0; ///< 上次初始化的时间戳（秒），用于懒加载策略
};


// 基于轮询算法的负载均衡器实现（Round Robin）
// 继承自抽象基类 LoadBalance，重写了调度策略。
// 该类内部维护一个连接列表 m_items，并在每次调度时依次选取下一个节点。
// 特点：对所有连接节点平均使用，适用于性能相近的后端。
class RoundRobinLoadBalance : public LoadBalance {
public:
    typedef std::shared_ptr<RoundRobinLoadBalance> ptr;

    // 获取一个可用的连接项（LoadBalanceItem）
    // 如果当前连接池为空，则返回 nullptr。
    // 参数 v 用于控制初始轮询起点：
    //   - 若 v == (uint64_t)-1，则使用随机起点（rand()）实现经典轮询；
    //   - 否则以 v 为起点，支持基于哈希的一致轮询（如 hash(IP) % N）。
    //
    // 调度过程：从起点开始顺序遍历 m_items，返回第一个有效的连接（isValid() 为 true）。
    // 若没有任何可用连接，则返回 nullptr。
    virtual LoadBalanceItem::ptr get(uint64_t v = -1) override;

protected:
    // 初始化内部连接列表（m_items）
    // 会将父类中的 m_datas 拷贝到 m_items 中，按顺序存储，
    // 供 get() 做轮询使用。
    virtual void initNolock() override;

protected:
    // 存储参与轮询调度的连接项，按顺序排列
    std::vector<LoadBalanceItem::ptr> m_items;
};


// 加权负载均衡类（WeightLoadBalance）
// 继承自基础 LoadBalance 类，实现了一种静态加权调度策略。
// 每个连接项（LoadBalanceItem）会设置一个静态权重值（weight），代表其负载能力。
// 内部根据这些权重生成一个加权索引表（m_weights），从而按比例分配请求流量。
class WeightLoadBalance : public LoadBalance {
public:
    typedef std::shared_ptr<WeightLoadBalance> ptr;

    // 获取一个可用的连接项（LoadBalanceItem）
    // 参数 v 用于控制选择的索引下标：
    //   - 若 v == -1，则使用随机数作为索引；
    //   - 否则将 v 映射到权重索引数组（m_weights）上，确保分布稳定。
    virtual LoadBalanceItem::ptr get(uint64_t v = -1) override;
    
    // 工具函数：将当前对象转换为 FairLoadBalanceItem 类型（如支持动态权重策略时）
    // 注意：返回的是内部连接项中第一个可以强转为 FairLoadBalanceItem 的对象。
    FairLoadBalanceItem::ptr getAsFair();

protected:
    // 初始化函数（重写自 LoadBalance）
    // 从基类 m_datas 中提取所有有效连接项（isValid() == true），
    // 构造加权连接数组（m_items）以及权重映射表（m_weights）。
    // m_weights 数组的每一项存储的是对应连接项在 m_items 中的索引。
    //
    // 举例：若有3个连接项，权重为 3, 2, 1
    // 则 m_weights 可能为：[0,0,0,1,1,2]，用于按比例分配请求。
    virtual void initNolock() override;
    
private:
    // 辅助函数：根据传入的 v 值计算一个索引位置
    // 从 m_weights 中取出位置对应的连接项索引，返回最终选择的连接项在 m_items 中的位置。
    // 如果 v == -1 则使用随机数；否则支持一致性或 hash 映射。
    int32_t getIdx(uint64_t v = -1);
    
protected:
    // 存储所有有效连接项，顺序与 m_weights 中的索引一一对应
    std::vector<LoadBalanceItem::ptr> m_items;
    
private:
    // 加权映射数组：
    // 每个元素表示一个连接项在 m_items 中的下标，数量由连接项的权重决定。
    // 实现“权重越高，被选中次数越多”的调度策略。
    std::vector<int64_t> m_weights;        
};

// FairLoadBalanceItem 表示一个支持“公平调度”的负载均衡项（即一个后端连接）
//
// 相较于基类 LoadBalanceItem，它重写了 getWeight() 方法，
// 使用动态统计信息（通过 HolderStatsSet）来实时计算节点的权重，
// 以便更公平地调度表现更好的节点（如响应更快、成功率更高的连接）
//
// 适用于“动态调度策略”场景，比如：
//   - 节点延迟差异显著，不能用静态 weight 静态调度
//   - 需要实时剔除异常节点或优先使用表现好的节点
//
// 成员函数：
// - getWeight(): 返回当前节点的动态权重，越高表示性能越好
// - clear(): 重置内部的统计信息（通常用于窗口滑动或节点重置场景）
//
class FairLoadBalanceItem : public LoadBalanceItem {
public:
    typedef std::shared_ptr<FairLoadBalanceItem> ptr;

    // 清空统计数据（HolderStatsSet）以重置该连接的状态
    void clear();

    // 获取当前动态权重，通常基于 HolderStatsSet::getWeight()
    // 该权重反映了该连接的响应时间、成功率、并发数、错误率等实时表现
    virtual int32_t getWeight() override;
};



/**
 * @brief SDLoadBalance（Service Discovery LoadBalance）
 *        服务发现驱动的负载均衡管理器
 *
 *        该类负责对接服务发现模块（IServiceDiscovery），
 *        根据其提供的服务列表变更，自动创建、更新对应的负载均衡对象（LoadBalance），
 *        并支持多种负载均衡策略（轮询、加权、公平）。
 *
 *        内部按 "domain/service" 二维结构组织多个 LoadBalance 实例，
 *        并通过回调机制创建每个连接对应的 SocketStream。
 *
 *        用法示例：
 *          auto lb = sdLoadBalancer->get("default", "user_service");
 *          auto stream = lb->get()->getStream();
 */
class SDLoadBalance {
public:
    typedef std::shared_ptr<SDLoadBalance> ptr;

    /// 用于构建 SocketStream 的用户自定义回调（如创建 TCP、SSL 或其他自定义连接）
    typedef std::function<SocketStream::ptr(ServiceItemInfo::ptr)> stream_callback;

    typedef sylar::RWMutex RWMutexType;

    SDLoadBalance(IServiceDiscovery::ptr sd);
    virtual ~SDLoadBalance() {}

    // 启动服务监听，订阅所有服务变化
    virtual void start();
    // 停止服务监听
    virtual void stop();

    /// 获取当前的连接构造回调
    stream_callback getCb() const { return m_cb; }
    /// 设置连接构造回调（用户必须提供）
    void setCb(stream_callback v) { m_cb = v; }

    /**
     * @brief 获取某个 service 的负载均衡对象
     * @param domain 服务所属域（如 default）
     * @param service 服务名（如 user_rpc）
     * @param auto_create 若不存在是否自动创建 LoadBalance 实例（默认 false）
     * @return 对应的 LoadBalance 实例指针
     */
    LoadBalance::ptr get(const std::string& domain, const std::string& service, bool auto_create = false);

    /**
     * @brief 初始化每个服务的调度策略配置（如 "default/user_rpc" -> FAIR）
     * @param confs 配置 map：domain -> service -> type 名称字符串（如 ROUNDROBIN、FAIR）
     */
    void initConf(const std::unordered_map<std::string,std::unordered_map<std::string, std::string> >& confs);

    /// 获取当前所有服务状态（用于打印）
    std::string statusString();

private:
    /**
     * @brief 服务变更回调（由 IServiceDiscovery 通知触发）
     * @param domain 所属域名
     * @param service 服务名
     * @param old_value 旧的服务实例映射（id -> ServiceItemInfo）
     * @param new_value 新的服务实例映射
     */
    void onServiceChange(const std::string& domain, const std::string& service,
                         const std::unordered_map<uint64_t, ServiceItemInfo::ptr>& old_value,
                         const std::unordered_map<uint64_t, ServiceItemInfo::ptr>& new_value);
    
    /// 获取某个服务的调度策略类型（如果未设置，则使用默认策略）
    ILoadBalance::Type getType(const std::string& domain, const std::string& service);

    /// 工厂方法：根据策略类型创建对应的 LoadBalance 实例
    LoadBalance::ptr createLoadBalance(ILoadBalance::Type type);

    /// 工厂方法：根据策略类型创建 LoadBalanceItem（不同策略返回不同子类）
    LoadBalanceItem::ptr createLoadBalanceItem(ILoadBalance::Type type);

protected:
    RWMutexType m_mutex;
    /// 服务发现模块（用于注册监听并获取服务实例）
    IServiceDiscovery::ptr m_sd;

    /// 所有服务的负载均衡数据：domain -> service -> LoadBalance
    std::unordered_map<std::string, std::unordered_map<std::string, LoadBalance::ptr>> m_datas;
    /// 每个服务设置的调度类型：domain -> service -> type（如 ROUNDROBIN）
    std::unordered_map<std::string, std::unordered_map<std::string, ILoadBalance::Type>> m_types;

    /// 默认调度策略类型（如果某个服务未设置，默认采用 FAIR 公平调度）
    ILoadBalance::Type m_defaultType = ILoadBalance::FAIR;

    /// 连接构造回调（用于根据 ServiceItemInfo 构建实际连接）
    stream_callback m_cb;

};

}

#endif