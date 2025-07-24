#include "daemon.h"
#include "sylar/log.h"
#include "sylar/config.h"
#include <time.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

namespace sylar {

static sylar::Logger::ptr g_logger = SYLAR_LOG_NAME("system");
// 重启间隔配置：主进程（子进程）异常退出后，父进程在重新启动主进程之前等待的时间。
static sylar::ConfigVar<uint32_t>::ptr g_daemon_restart_interval
	= sylar::Config::Lookup("daemon.restart_interval", (uint32_t)5, "daemon restart interval");

std::string ProcessInfo::toString() const {
    std::stringstream ss;
    ss << "[ProcessInfo parent_id=" << parent_id
        << " main_id=" << main_id
        << " parent_start_time=" << sylar::Time2Str(parent_start_time)
        << " main_start_time=" << sylar::Time2Str(main_start_time)
        << " restart_count=" << restart_count << "]";
    return ss.str();
}

// 实际启动业务逻辑的函数，完成主进程业务代码的执行
static int real_start(int argc, char** argv, std::function<int(int argc, char** argv)> main_cb) {
    ProcessInfoMgr::GetInstance()->main_id = getpid();
    ProcessInfoMgr::GetInstance()->main_start_time = time(0);
    // 用户自定义的主逻辑函数，相当于 main() 的主体内容；
    return main_cb(argc, argv);
}

// 守护进程管理逻辑的实现，负责守护进程环境设置、创建子进程（主进程）、管理子进程的生命周期与重启。
// 它的主要任务是将当前进程变为守护进程，并不断监控和重启主进程（子进程）。
static int real_daemon(int argc, char** argv, std::function<int(int argc, char** argv)> main_cb) {
    // 将当前进程变为一个守护进程  
    // #include <unistd.h> 
    daemon(1, 0);
    ProcessInfoMgr::GetInstance()->parent_id = getpid();
    ProcessInfoMgr::GetInstance()->parent_start_time = time(0);
    // 父进程进入一个无限循环：用于反复创建并管理子进程(主进程)---实现崩溃自动启动
    while (true) {
        pid_t pid = fork();
        // fork()  在子进程中返回0  父进程中返回子进程PID
        if (pid == 0) {
            ProcessInfoMgr::GetInstance()->main_id = getpid();
            ProcessInfoMgr::GetInstance()->main_start_time = time(0);
            SYLAR_LOG_INFO(g_logger) << "process start pid=" << getpid();
            return real_start(argc, argv, main_cb);
        }
        else if (pid < 0) {
            SYLAR_LOG_ERROR(g_logger) << "fork fail return=" << pid
                << " errno=" << errno << " errstr=" << strerror(errno);
            return -1;
        }
        else {    // 父进程(守护进程)进入此分支，负责控制子进程(主进程)
            int status = 0;
            // waitpid阻塞等待子进程(主进程退出)，并获取其退出状态保存在status中
            waitpid(pid, &status, 0);
            // 如果子进程非正常退出
            if (status) {
                // 如果是被 kill -9 杀死，退出监控循环，不再重启。
                if (status == 9) {
                    SYLAR_LOG_INFO(g_logger) << "killed";
                    break;
                }
                else {  // 其他退出情况（如崩溃、异常）打印错误日志，继续进入while(true)准备重启子进程
                    SYLAR_LOG_ERROR(g_logger) << "child crash pid=" << pid
                        << " status=" << status;
                }
            }
            else {   // 子进程正常退出
                SYLAR_LOG_INFO(g_logger) << "child finished pid=" << pid;
                break;  // 正常退出，不再守护重启                
            }
            // 如果没有break  说明需要重启子进程
            ProcessInfoMgr::GetInstance()->restart_count += 1;
            sleep(g_daemon_restart_interval->getValue());
        }
    }
    return 0;
}


// 对外暴露的启动接口，负责判断是否以守护进程方式启动，并调用不同的启动流程。
int start_daemon(int argc, char** argv, std::function<int(int argc, char** argv)> main_cb, bool is_daemon) {
    if (!is_daemon) {
        ProcessInfoMgr::GetInstance()->parent_id = getid();
        ProcessInfoMgr::GetInstance()->parent_start_time = time(0);
        return real_start(argc, argv, main_cb);
    }
    return real_daemon(argc, argv, main_cb);
}

}