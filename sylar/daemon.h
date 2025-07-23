#ifndef __SYLAR_DAEMON_H__
#define __SYLAR_DAEMON_H__

#include <unistd.h>
#include <functional>
#include "sylar/singleton.h"

namespace sylar {

//守护进程是一个 在后台运行的、脱离终端的服务型程序，不随用户登录/退出而结束。
// 守护进程（Daemon Process）是 在操作系统后台运行、不依赖终端控制、通常用于提供服务的长期驻留进程。

//守护进程通常采用“父进程守护 + 子进程运行”结构；
//父进程负责 监控、重启 子进程；
//真正执行服务逻辑的子进程，是你程序的“主力”，所以称为 主进程 更贴切；
//父进程一旦启动守护流程，自己不干事，只是“保姆”。
//父进程 是负责管理守护进程的控制逻辑，主进程 是真正运行核心服务功能的子进程。
struct ProcessInfo {
	// 父进程id
	pid_t parent_id = 0;
	// 主进程id
	pid_t main_id = 0;
	// 父进程启动时间
	uint64_t parent_start_time = 0;
	// 主进程启动时间
	uint64_t main_start_time = 0;
	// 主进程重启的次数
	uint32_t restart_count = 0;

	std::string toString() const;
};

// ProcessInfo 被设计为单例，是为了让程序中的所有模块都能随时全局访问当前守护进程的状态信息（如主进程 PID、启动时间、重启次数等），并保持状态唯一且一致。
typedef sylar::Singleton<ProcessInfo> ProcessInfoMgr;


/**
 * @brief 启动程序可以选择用守护进程的方式
 * @param[in] argc 参数个数
 * @param[in] argv 参数值数组
 * @param[in] main_cb 启动函数
 * @param[in] is_daemon 是否守护进程的方式
 * @return 返回程序的执行结果
 */
int start_daemon(int argc, char** argv, std::function<int(int argc, char** argv)> main_cb, bool is_daemon);

}


#endif