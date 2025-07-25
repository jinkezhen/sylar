/**
 * @file util.h
 * @brief 常用的工具函数 
 * @date 
 * copyright Copyright (c) 2025 kezhen.jin. All right reserved.
 */

#ifndef __SYLAR_UTIL_H__
#define __SYLAR_UTIL_H__

#include <cxxabi.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/syscall.h>
#include <stdio.h>
#include <stdint.h>
#include <vector>
#include <string>
#include <memory>
#include <map>
#include <iomanip>
#include <jsoncpp/json/json.h>
#include <yaml-cpp/yaml.h>
#include <iostream>
#include <google/protobuf/message.h>

namespace sylar {

/**
 * @brief 返回当前线程的id
 */
pid_t GetThreadId();

/**
 * @brief 返回当前的协程id
 */
uint32_t GetFiberId();

/**
 * @brief 获取当前的调用栈
 * @param[out] bt 保存调用栈
 * @param[in] size 最多返回层数
 * @param[in] skip 跳过栈顶的层数
 */
void Backtrack(std::vector<std::string> bt, int size, int skip);

/**
 * @brief 获取当前栈信息的字符串
 * @param[in] size 栈的最大层数
 * @param[in] skip 跳过栈顶的层数
 * @param[in] perfix 栈信息前输出的内容
 */
std::string BacktraceToString(int size = 64, int skip = 2, const std::string& prefix = "");

/**
 * @brief 获取当前时间的毫秒
 */
uint64_t GetCurrentMS();

/**
 * @brief 获取当前时间的微秒
 */
uint64_t GetCurrentUS();

/**
 * @brief 时间转字符串
 */
std::string Time2Str(time_t ts = time(0), const std::string& format = "%Y-%m-%d %H:%M:%S");

/**
 * @brief 字符串转时间
 */
time_t Str2Time(const char* str, const std::string& format = "%Y-%m-%d %H:%M:%S");


/**
 * @brief 所有字母转大写
 */
std::string ToUpper(const std::string& name);

/**
 * @brief 所有字母转小写
 */
std::string ToLower(const std::string& name);

//封装文件系统相关操作的类
//所有方法都声明为静态方法是因为静态方法属于类，而不属于某个具体对象。这个类是个工具类，内部的函数不依赖具体对象，可以通过类型::函数的形式直接调用
class FSUtil {
public:
    //列出目录下所有符合后缀要求的文件，并存入files容器中
    static void ListAllFile(std::vector<std::string>& files, const std::string& path, const std::string& suffix);
    //如果目录不存在则创建目录，返回是否创建成功
    static bool Mkdir(const std::string& dirname);
    //判断进程ip(PID)文件是否存在且进程仍在运行，通常用于检查程序是否已经在运行
    static bool IsRunningPidfile(const std::string& pidfile);
    //删除文件或目录
    static bool Rm(const std::string& path);
    //移动或重命名文件
    static bool Mv(const std::string& from, const std::string& to);
    //获取文件或目录的绝对路径
    static bool Realpath(const std::string& path, std::string& rpath);
    //获取文件所在的目录
    static std::string Dirname(const std::string& filename);
    //获取文件名
    static std::string Basename(const std::string& filename);
    //以特定的格式打开文件进行读取
    static bool OpenForRead(std::ifstream& ifs, const std::string& filename, std::ios_base::openmode mode);
    //以特定的格式打开文件进行写入
    static bool OpenForWrite(std::ofstream& ofs, const std::string& filename, std::ios_base::openmode mode);
    //创建符号链接:符号链接（Symbolic Link）相当于一个指向目标文件或目录的快捷方式，可以像原文件一样访问，但不占用额外存储空间。可以通过to访问到from
    static bool Symlink(const std::string& from, const std::string& to);
    //删除文件:如果exist为false，则只有在文件存在时才能删除，文件不存在时不做任何操作(直接返回true)；exist为true时，文件存在才删除，不存在返回false
    static bool Unlink(const std::string& filename, bool exist = false);
};


/**
 * @brief 从映射m(如std::map<K, std::string>)中查找键k，并尝试将对应的值转换为类型为V
 */
template<class V, class Map, class K>
V GetParamValue(const Map& m, const K& k, const V& def = V()) {
    auto it = m.find(k);
    if (it == m.end()) {
        return def;
    }
    V value;
    std::stringstream ss(it -> second);
    ss >> value;
    if (ss.fail()) return def;
    return value;
}

template<class V, class Map, class K>
bool CheckGetParamValue(const Map& m, const K& k, V& v) {
    auto it = m.find(k);
    if (it == m.end()) return false;
    std::stringstream ss(it -> second);
    if (!(ss >> v)) return false;
    return true;
}

/**
 * @brief 该类提供字符串到基本数据类型的转换工具
 */
class TypeUtil {
    public:
        static int8_t ToChar(const std::string& str);
        static int64_t Atoi(const std::string& str);
        static double Atof(const std::string& str);
        static int8_t ToChar(const char* str);
        static int64_t Atoi(const char* str);
        static double Atof(const char* str);
};

/**
 * @brief 原子操作工具类，封装 GCC 提供的 __sync 系列原子指令。
 * 
 * 该类主要用于实现线程安全的数据操作，适用于无需锁的轻量级并发控制。
 * 
 * 所有操作都基于 GCC 的内建函数，如：
 * - __sync_add_and_fetch
 * - __sync_fetch_and_add
 * - __sync_bool_compare_and_swap 等
 *
 * 使用模板支持任意整数类型的操作。
 */
class Atomic {
public:

    /**
     * @brief 原子加操作：先加后返回
     * @param t 目标变量（必须是 volatile 的引用）
     * @param v 加数，默认值为 1
     * @return 加法后的结果
     */
    template<class T, class S = T>
    static T addFetch(volatile T& t, S v = 1) {
        return __sync_add_and_fetch(&t, (T)v);
    }

    /**
     * @brief 原子减操作：先减后返回
     */
    template<class T, class S = T>
    static T subFetch(volatile T& t, S v = 1) {
        return __sync_sub_and_fetch(&t, (T)v);
    }

    /**
     * @brief 原子按位或：先或后返回
     */
    template<class T, class S>
    static T orFetch(volatile T& t, S v) {
        return __sync_or_and_fetch(&t, (T)v);
    }

    /**
     * @brief 原子按位与：先与后返回
     */
    template<class T, class S>
    static T andFetch(volatile T& t, S v) {
        return __sync_and_and_fetch(&t, (T)v);
    }

    /**
     * @brief 原子按位异或：先异或后返回
     */
    template<class T, class S>
    static T xorFetch(volatile T& t, S v) {
        return __sync_xor_and_fetch(&t, (T)v);
    }

    /**
     * @brief 原子按位与非：先与非后返回
     */
    template<class T, class S>
    static T nandFetch(volatile T& t, S v) {
        return __sync_nand_and_fetch(&t, (T)v);
    }

    /**
     * @brief 原子加操作：先返回后加
     */
    template<class T, class S>
    static T fetchAdd(volatile T& t, S v = 1) {
        return __sync_fetch_and_add(&t, (T)v);
    }

    /**
     * @brief 原子减操作：先返回后减
     */
    template<class T, class S>
    static T fetchSub(volatile T& t, S v = 1) {
        return __sync_fetch_and_sub(&t, (T)v);
    }

    /**
     * @brief 原子按位或：先返回后或
     */
    template<class T, class S>
    static T fetchOr(volatile T& t, S v) {
        return __sync_fetch_and_or(&t, (T)v);
    }

    /**
     * @brief 原子按位与：先返回后与
     */
    template<class T, class S>
    static T fetchAnd(volatile T& t, S v) {
        return __sync_fetch_and_and(&t, (T)v);
    }

    /**
     * @brief 原子按位异或：先返回后异或
     */
    template<class T, class S>
    static T fetchXor(volatile T& t, S v) {
        return __sync_fetch_and_xor(&t, (T)v);
    }

    /**
     * @brief 原子按位与非：先返回后与非
     */
    template<class T, class S>
    static T fetchNand(volatile T& t, S v) {
        return __sync_fetch_and_nand(&t, (T)v);
    }

    /**
     * @brief 原子比较并交换操作（CAS）
     * 
     * 如果 t == old_val，则将 t 设为 new_val，返回 t 原来的值
     * 可用于实现无锁状态切换、资源保护等。
     * 
     * @param t 待修改的变量
     * @param old_val 期望原值
     * @param new_val 替换的新值
     * @return 返回操作前的 t 值（不代表交换成功与否）
     */
    template<class T, class S>
    static T compareAndSwap(volatile T& t, S old_val, S new_val) {
        return __sync_val_compare_and_swap(&t, (T)old_val, (T)new_val);
    }

    /**
     * @brief 原子比较并交换操作（CAS），返回 bool 是否成功
     * 
     * 如果 t == old_val，则将 t 设为 new_val，返回 true，否则返回 false
     * 通常用于锁的实现、状态切换等场景
     */
    template<class T, class S>
    static bool compareAndSwapBool(volatile T& t, S old_val, S new_val) {
        return __sync_bool_compare_and_swap(&t, (T)old_val, (T)new_val);
    }
};


template<class T>
void nop(T*) {}

//释放由new[]分配的数组指针
//int* arr = new int[5];  delete_array(arr);
template<class T>
void delete_array(T* v) {
    if (v) {
        delete[] v;
    }
}

//智能数组指针封装模板类：用于安全管理动态数组
template<class T>
class SharedArray {
public:
    //explicit：防止SharedArray<int> arr = 5;这样的隐式转换
    explicit SharedArray(const uint64_t& size = 0, T* p = 0) 
        :m_size(size), m_ptr(p, delete_array<T>){
    }

    //带自定义删除器的构造函数
    template<class D>
    SharedArray(const uint64_t& size, T* p, D d) : m_size(size), m_ptr(p, d) {
    }

    SharedArray(const SharedArray& s) {
        m_size = s.m_size;
        m_ptr = s.m_ptr;
    }

    SharedArray& operator=(const SharedArray& s) {
        m_size = s.m_size;
        m_ptr = s.m_ptr;
        return *this;
    }

    SharedArray& operator[](std::ptrdiff_t i) const {
        return m_ptr.get()[i];
    }

    //获取原始指针
    T* get() const {
        return m_ptr.get();
    }

    //检查当前SharedArray是否唯一持有数组（即引用计数是否为1）
    bool unique() const {
        return m_ptr.unique();
    }

    //返回当前数组的引用计数
    long use_count() const {
        return m_ptr.use_count();
    }

    void swap(SharedArray& b) {
        std::swap(m_size, b.m_size);
        m_ptr.swap(b.m_ptr);
    }

    bool operator!() const {
        return !m_ptr;
    }

    operator bool() const {
        return !!m_ptr;
    }

    uint64_t size() const {
        return m_size;
    }

    
private:
    //存储的数组的大小
    uint64_t m_size;
    //用来管理动态分配的数组
    std::shared_ptr<T> m_ptr;
};

/**
 * @brief 字符串工具类
 */
class StringUtil {
public:
    //格式化字符串
    static std::string Format(const char* fmt, ...);
    static std::string Formatv(const char* fmt, va_list ap);
    //url编码与解码
    static std::string UrlEncode(const std::string& str, bool space_as_plus = true);
    static std::string UrlDecode(const std::string& str, bool space_as_plus = true);
    //字符串去除空白字符
    static std::string Trim(const std::string& str, const std::string& delimit = " \t\r\n");
    static std::string TrimLeft(const std::string& str, const std::string& delimit = " \t\r\n");
    static std::string TrimRight(const std::string& str, const std::string& delimit = " \t\r\n");
    //字符串转换
    static std::string WStringToString(const std::wstring& ws);
    static std::wstring StringToWString(const std::string& s);
};

std::string GethostName();
std::string GetIPv4();

bool YamlToJson(const YAML::Node& ynode, Json::Value& jnode);
bool JsonToYaml(const Json::Value& jnode, YAML::Node& unode);

//获取c++类型的T的可读类型名
template<class T>
std::string TypeToName() {
    //abi::__cxa_demangle函数的作用，比如我们有一个类A，编译器会将类名进行修饰，
    //所以我们通过typeid().name()得到的类名可能是“1A”
    //而这个函数的作用就是解码这个混淆名称，从而得到“A”
    //函数使用示例：TypeName<int>()
    int status;
    const char* s_name = abi::__cxa_demangle(typeid(T).name(), nullptr, nullptr, &status);
    std::string result = (status == 0) ? s_name : typeid(T).name();
    free(s_name);
    return result;
}

//protobuf数据转为json形式的字符串
std::string PBToJsonString(const google::protobuf::Message& message);

//将一组元素连接成一个字符串并用tag作为分隔符
template<class Iter>  //某个迭代器类型
std::string Join(Iter begin, Iter end, const std::string& tag) {
    std::stringstream ss;
    for (Iter it = begin; it != end; ++it) {
        if (it != begin) {
            ss << tag;
        }
        ss << *it;
    }
    return ss.str();
}

//二分查找
template<class T>
int BinarySearch(const T* arr, int length, const T& v) {
    int mid = 0;
    int begin = 0;
    int end = length - 1;
    while (begin < end) {
        mid = begin + (end - begin) / 2;
        if (v == arr[mid]) {
            return mid;
        } else if (v > arr[mid]) {
            begin = mid + 1; 
        } else {
            end = mid - 1;
        }
    }
    //未找到则返回插入位置,为负值好与找到返回的正值进行区分
    return -(begin + 1);
}

//从输入流is读取size字节的数据，存入data指向的缓冲区
inline bool ReadFixFromStream(std::istream& is, char* data, const uint64_t& size) {
    uint64_t pos = 0;
    while (is && (pos < size)) {
        //从流中读取最多size - pos字节，存入data + pos(即data指针向后偏移pos字节)
        is.read(data + pos, size - pos);
        //.gcpunt返回上一次读取的字节数
        pos += is.gcount();
    }
    return pos == size;
}

//该函数尝试从输入流is读取一个T类型的变量v
template<class T>
bool ReadFromStream(std::istream& is, T& v) {
    return ReadFixFromStream(is, (char*)&v, sizeof(v));
}

//该函数从流中读取多个T类型的元素，并填充到v中
template<class T>
bool ReadFromStream(std::istream& is, std::vector<T>& v) {
    //&v[0]:获取vector的首地址元素
    //(char*)&v[0]时要注意v必须非空
    return ReadFixFromStream(is, (char*)&v[0], sizeof(T) * v.size());
}

//该函数的作用是将v的二进制数据流写入os
template<class T>
bool WriteToStream(std::ostream& os, const T& v) {
    if (!os) return false;
    //将v进行强转，以字节方式方式写入流
    os.write((const char*)&v, sizeof(T));
    //检查流是否可用，如果os进入错误状态（比如磁盘满了）返回false
    return (bool)os;
}

//该函数的作用是将v中的多个T类型元素写入os
template<class T>   //T适用于POD类型：int double
bool WriteToStream(std::ostream& os, const std::vector<T>& v) {
    if (!os) return false;
    os.write((const char*)&v, sizeof(T) * v.size());
    return (bool)os;
}

//该类为限速器，用于限制某个事件（如请求、数据包、任务等）的速率，防止短时间内发生过多操作
class SpeedLimit {
public:
    typedef std::shared_ptr<SpeedLimit> ptr;
    SpeedLimit(uint32_t speed);
    //用于控制请求或任务的执行速率
    void add(uint32_t v);
private:
    uint32_t m_speed;     //限制的速率
    float m_countPerMS;   //每毫秒允许的操作次数

    uint32_t m_curCount;  //当前秒内已经执行的操作次数
    uint32_t m_curSec;    //记录当前的秒数
};

//从istream中读取指定大小的数据到data，如果是ifstream则应该限制速度
bool ReadFixFromStreamWithSpeed(std::istream& is, char* data, const uint64_t& size, const uint64_t speed);
//将data数据写入ostream，如果是ofstram则应该限制速度
bool WriteFixFromStreamWithSpeed(std::ostream& os, const char* data, const uint64_t& size, const uint64_t& speed);

//将一个T类型的变量v写入std::ostream，支持限速
template<class T>
bool WriteToStreamWithSpeed(std::ostream& os, const T& v, const uint64_t& speed = -1) {
    if (os) {
        return WriteFixToStreamWithSpeed(os, (const char*)&v, sizeof(T), speed);
    }
    return false;
}

template<class T>
bool WriteToStreamWithSpeed(std::ostream& os, const std::vector<T>& v, const uint64_t& speed = -1) {
    if (os) {
        return WriteFixToStreamWithSpeed(os, (const char*)&v[0], sizeof(T) * v.size(), speed);
    }
    return false;
}


template<class T>
bool ReadFromStreamWithSpeed(std::istream& is, const std::vector<T>& v,
                            const uint64_t& speed = -1) {
    if(is) {
        return ReadFixFromStreamWithSpeed(is, (char*)&v[0], sizeof(T) * v.size(), speed);
    }
    return false;
}

template<class T>
bool ReadFromStreamWithSpeed(std::istream& is, const T& v,
                            const uint64_t& speed = -1) {
    if(is) {
        return ReadFixFromStreamWithSpeed(is, (char*)&v, sizeof(T), speed);
    }
    return false;
}

std::string Format(const char* fmt, ...);
std::string Formatv(const char* fmt, va_list ap);

template<class T>
void Slice(std::vector<std::vector<T> >& dst, const std::vector<T>& src, size_t size) {
    size_t left = src.size();
    size_t pos = 0;
    while(left > size) {
        std::vector<T> tmp;
        tmp.reserve(size);
        for(size_t i = 0; i < size; ++i) {
            tmp.push_back(src[pos + i]);
        }
        pos += size;
        left -= size;
        dst.push_back(tmp);
    }

    if(left > 0) {
        std::vector<T> tmp;
        tmp.reserve(left);
        for(size_t i = 0; i < left; ++i) {
            tmp.push_back(src[pos + i]);
        }
        dst.push_back(tmp);
    }
}


}

#endif
