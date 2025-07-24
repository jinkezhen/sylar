#ifndef __SYLAR_ENV_H__
#define __SYLAR_ENV_H__

#include "sylar/singleton.h"
#include "sylar/thread.h"
#include <map>
#include <vector>

// sylar::Env 是 Sylar 框架中的一个 环境管理类，用于封装程序运行时的环境信息、命令行参数解析、环境变量访问、路径处理等功能。
// 它是一个 线程安全的全局环境上下文管理器，通过单例模式（EnvMgr）供全局使用。
namespace sylar {

/**
 * @brief 环境变量与命令行参数管理类
 * 
 * 用于程序启动时初始化环境信息，管理命令行参数、环境变量、路径等信息，
 * 提供线程安全的访问接口，支持路径规范化、帮助信息注册输出等功能。
 */
class Env {
public:
	typedef RWMutex RWMutexType;

    /**
     * @brief 初始化环境（在 main 中调用）
     * @param argc 参数数量
     * @param argv 参数数组
     * @return 是否初始化成功
     */
    bool init(int argc, char** argv);

    // 手动添加一个参数(模拟命令行参数)
    // EnvMgr::GetInstance()->add("config", "/etc/my_server.yaml");
    // 等价于命令行执行了./my_server --config=/etc/my_server.yaml
    void add(const std::string& key, const std::string& val);

    // 判断命令行中是否存在某个参数
    // ./my_server --daemon
    // if (EnvMgr::GetInstance()->has("daemon")) {
    // 如果传了 --daemon 参数，则以守护进程模式启动
    // startAsDaemon();}
    bool has(const std::string& key);

    // 从参数表中移除一个参数
    void del(const std::string& key);

    // 获取参数值
    std::string get(const std::string& key, const std::string& default_value = "");

    // 添加某个参数的帮助说明，用于后续--help显示
    // --help会把所有添加的参数及其说明全部打印出来
    void addHelp(const std::string& key, const std::string& desc);

    // 移除某个参数的帮助说明
    void removeHelp(const std::string& key);

    // 输出所有通过addHelp添加的参数说明，通常用于处理--help的逻辑
    // if (EnvMgr::GetInstance()->has("help")) {
    //      EnvMgr::GetInstance()->printHelp();
    //      return 0;}
    void printHelp();

    /**
         * @brief 获取可执行程序路径（含完整路径）
         */
    const std::string& getExe() const { return m_exe; }

    /**
     * @brief 获取当前工作目录路径
     */
    const std::string& getCwd() const { return m_cwd; }

    // 设置系统环境变量，等价于setenv
    bool setEnv(const std::string& key, const std::string& val);

    //获取系统环境变量，等价于getenv
    std::string getEnv(const std::string& key, const std::string& default_value = "");

    /**
     * @brief 获取绝对路径（相对于可执行文件所在目录）
     * @param path 相对路径
     * @return 绝对路径
     */
    std::string getAbsolutePath(const std::string& path) const;

     /**
     * @brief 获取绝对路径（相对于当前工作目录）
     * @param path 相对路径
     * @return 绝对路径
     */
    std::string getAbsoluteWorkPath(const std::string& path) const;

    /**
     * @brief 获取默认配置文件路径
     * 通常用于加载配置系统的入口配置文件
     */
    std::string getConfigPath();

private:
    RWMutexType m_mutex;  

    // 命令行参数映射
    // 将命令行参数中--xxx=yyy形式解析成key->value形式，存入map
    std::map<std::string, std::string> m_args;
    
    // 参数帮助信息
    std::vector<std::pair<std::string, std::string>> m_helps;

    // 执行程序的命令 
    std::string m_program;
    // 可执行文件完整路径
    std::string m_exe;
    // 当前工作目录路径
    std::string m_cwd;
};

typedef sylar::Singleton<Env> EnvMgr;

}


#endif