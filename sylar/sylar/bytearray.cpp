#include "bytearray.h"
#include "endian.h"
#include "log.h"

#include <stdexcept>
#include <string.h>
#include <vector>
#include <fstream>
#include <string>
#include <sstream>
#include <iomanip>

namespace sylar {

static sylar::Logger::ptr g_logger = SYLAR_LOG_NAME("system");

ByteArray::Node::Node(size_t s)
    : ptr(new char[s]),
      next(nullptr),
      size(0) {
}

ByteArray::Node::Node() 
    : ptr(nullptr),
      next(nullptr),
      size(0) {
}

ByteArray::Node::~Node() {
    if (ptr) {
        delete[] ptr;
    }
}

ByteArray::ByteArray(size_t base_size) :
    m_baseSize(base_size),
    m_position(0),
    m_capacity(base_size),
    m_size(0),
    m_endian(SYLAR_BIG_ENDIAN),
    m_root(new Node(base_size)),
    m_cur(m_root) {
}

ByteArray::~ByteArray() {
    Node* temp = m_root;
    while (temp) {
        m_cur = temp;
        temp = temp->next;
        delete m_cur;
    }
}

void ByteArray::clear() {
    m_position = m_size = 0;
    m_capacity = m_baseSize;
    Node* temp = m_root->next;
    while (temp) {
        m_cur = temp;
        temp = temp->next;
        delete m_cur;
    }
    m_cur = m_root;
    m_root->next == nullptr;
}

bool ByteArray::isLittleEndian() const {
    return m_endian == SYLAR_LITTLE_ENDIAN;
}

void ByteArray::setIsLittleEndian(bool val) {
    if (val) {
        m_endian = SYLAR_LITTLE_ENDIAN;
    } else {
        m_endian = SYLAR_BIG_ENDIAN;
    }
}

void ByteArray::addCapacity(size_t size) {
    if (size == 0) return;
    size_t old_cap = getCapacity();
    if (old_cap >= size) return;

    //计算缺口大小
    size -= old_cap;
    //计算需要增加多少个节点，×浮点数后向上取整
    size_t count = ceil(1.0 * (size / m_baseSize));

    Node* temp = m_root;
    //找到尾节点
    while (temp->next) {
        temp = temp->next;
    }
    //用于记录新添加节点的第一个
    Node* first = nullptr;
    //添加新节点
    for (size_t i = 0; i < count; ++i) {
        temp -> next = new Node(m_baseSize);
        if (first == nullptr) {
            first = temp -> next;
        }
        temp = temp->next;
        m_capacity += m_baseSize;
    }
    //如果在扩容之前容量为0，那么更新m_cur为扩容后的第一个块
    if (old_cap == 0) {
        m_cur = first;
    }
}

void ByteArray::setPosition(size_t v) {
    if (v > m_capacity) {
        return throw out_of_range("set position out of range");
    }
    m_position = v;
    if (m_position > m_size) {
        m_size = m_position;
    }
    m_cur = m_root;
    while (v > m_cur->size) {
        v -= m_cur->size;
        m_cur = m_cur->next;
    }
    if (v == m_cur->size) {
        m_cur = m_cur->next;
    }
}

//将一段原始内存写入到ByteArray中，并根据需要扩展容量，维护当前位置和总长度等状态
void ByteArray::write(const void* buf, size_t size) {
    addCapacity(size);  // 确保当前 ByteArray 有足够容量容纳 size 字节的数据（可能会扩展内存块）

    // npos 表示当前位置在当前内存块（Node）中的偏移量
    // 比如 m_position = 4100，m_baseSize = 4096，那么在当前块中偏移是 4
    size_t npos = m_position % m_baseSize;

    // ncap 表示当前内存块中从 npos 开始还剩多少可写字节
    // 如果 npos = 4，块大小 = 4096，那么剩余空间 = 4096 - 4 = 4092
    size_t ncap = m_cur->size - npos;

    // bpos 表示源数据（buf）中已经被写入的偏移量
    // 每次 memcpy 时都从 bpos 开始拷贝数据
    size_t bpos = 0;

    // 开始循环写入数据，直到 size 字节写完
    while (size > 0) {
        // 当前块的剩余空间足够容纳 size 个字节，直接写入即可
        if (ncap >= size) {
            // 拷贝 size 字节数据到当前块的空闲位置
            memcpy(m_cur->ptr + npos, (const void*)buf + bpos, size);
            // 如果刚好填满当前块，则移动到下一个块
            if (m_cur->size == (npos + size)) {
                m_cur = m_cur->next;
            }
            // 更新 ByteArray 的写入位置（全局偏移）
            m_position += size;
            // 源缓冲区偏移前进 size 字节
            bpos += size;
            // 写入完毕，退出循环
            size = 0;
        } else {
            // 当前块不够写完 size 字节，只能写 ncap 字节
            memcpy(m_cur->ptr + npos, (const void*)buf + bpos, ncap);  // 拷贝 ncap 字节
            m_position += ncap;  // 写入位置前进 ncap 字节
            size -= ncap;        // 剩余待写数据减少
            bpos += ncap;        // 源缓冲区偏移前进 ncap 字节
            // 移动到下一块继续写入
            m_cur = m_cur->next;
            // 进入新块，偏移从 0 开始
            npos = 0;
            ncap = m_cur->size;  // 新块的总可写空间
        }
    }
    // 只有当你写入的位置超过了“历史上已经写入过的末尾”，才说明你是“新增数据”，
    // 这时候要更新 m_size，否则你只是“在原有位置重写旧数据”。
    if (m_position > m_size) {
        m_size = m_position;
    }
}


//从当前ByteArray读取size大小的数据，存储到buf中
void ByteArray::read(void* buf, size_t size) {
    // 读取前，先检查请求读取的字节数是否超过剩余可读数据大小，防止越界读取
    if (size > getReadSize()) {
        throw std::out_of_range("not enough len");
    }

    // 计算当前读取位置在当前内存块（Node）中的偏移
    // 例如 m_position = 4100，m_baseSize = 4096，npos = 4
    size_t npos = m_position % m_baseSize;

    // 计算当前块从偏移位置开始还能读取多少字节
    // 例如当前块大小为 4096，npos = 4，ncap = 4096 - 4 = 4092
    size_t ncap = m_cur->size - npos;

    // bpos 是目标缓冲区 buf 中的偏移量，表示已经复制了多少数据
    size_t bpos = 0;

    // 循环读取，直到 size 字节全部读取完毕
    while (size > 0) {
        if (ncap >= size) {
            // 当前块剩余可读字节数大于等于所需读取的 size
            // 直接将 size 字节数据从当前块的偏移位置复制到目标缓冲区
            memcpy((char*)buf + bpos, m_cur->ptr + npos, size);
            // 如果正好读取到了当前块的末尾，则将读取指针移动到下一块
            if (m_cur->size == npos + size) {
                m_cur = m_cur->next;
            }
            // 更新全局读取位置，向后移动 size 字节
            m_position += size;
            // 目标缓冲区偏移增加 size
            bpos += size;
            // 读取完成，退出循环
            size = 0;
        } else {
            // 当前块剩余字节不足以满足读取需求，只能先读完当前块剩余数据
            // 将当前块剩余数据复制到目标缓冲区
            memcpy((char*)buf + bpos, m_cur->ptr + npos, ncap);
            // 更新全局读取位置，移动 ncap 字节
            m_position += ncap;
            // 目标缓冲区偏移增加 ncap
            bpos += ncap;
            // 待读取字节数减少 ncap
            size -= ncap;
            // 读取指针移动到下一块，准备继续读取
            m_cur = m_cur->next;
            // 新块偏移从 0 开始
            npos = 0;
            // 新块的剩余可读字节数等于块大小
            ncap = m_cur->size;
        }
    }
}


//从ByteArray指定位置开始读取size个数据，并存入buf
void ByteArray::read(void* buf, size_t size, size_t position) const {
    // 越界检查：确保请求读取的数据范围在已写入数据范围内
    // m_size 是当前有效数据的末尾位置，position 是读取起始位置
    // 如果要读取的 size 超过了剩余可读数据，就抛出异常
    if (size > (m_size - position)) {
        throw std::out_of_range("not enough len");
    }

    // 计算 position 对应的内存块索引，即需要跳过多少完整的内存块（Node）
    size_t node_index = position / m_baseSize;
    // 从链表头节点开始
    Node* cur = m_root;
    // 遍历链表，找到 position 所在的内存块节点
    while (node_index > 0) {
        cur = cur->next;
        node_index--;
    }

    // 计算 position 在当前内存块中的偏移量（块内偏移）
    size_t npos = position % m_baseSize;
    // 计算当前内存块从偏移位置到块末尾剩余可读字节数
    size_t ncap = cur->size - npos;
    // bpos 是已经从源缓冲区（buf）复制的字节数偏移
    size_t bpos = 0;

    // 循环读取，直到读取完所有请求的 size 字节数据
    while (size > 0) {
        // 如果当前块剩余空间足够本次读取
        if (ncap >= size) {
            // 从当前块的 npos 位置读取 size 字节数据到目标缓冲区的对应位置
            memcpy((char*)buf + bpos, cur->ptr + npos, size);
            // 如果刚好读取到当前块末尾，则切换到下一个块（为下一次读取做准备）
            if (cur->size == npos + size) {
                cur = cur->next;
            }
            // 更新 position（读取偏移，用于内部追踪，虽然函数是 const，但修改局部变量 position 无影响）
            position += size;
            // 目标缓冲区偏移前进 size 字节
            bpos += size;
            // 本次读取任务完成
            size = 0;
        } else {
            // 当前块剩余数据不够读取 size 字节，只能先读取 ncap 字节
            memcpy((char*)buf + bpos, cur->ptr + npos, ncap);
            // 更新 position 偏移
            position += ncap;
            // 目标缓冲区偏移前进 ncap 字节
            bpos += ncap;
            // 减少剩余需读取字节数
            size -= ncap;
            // 切换到下一个内存块
            cur = cur->next;
            // 块内偏移重置为 0，表示从新块头开始读取
            npos = 0;
            // 新块剩余可读字节数等于新块大小
            ncap = cur->size;
        }
    }
}


std::string ByteArray::toString() const {
    std::string str;
    str.resize(getReadSize());
    if (str.empty()) return str;
    read(&str[0], str.size(), m_position);
    return str;
}

//str = hello
//return "47 86 a7 c8 87"
std::string ByteArray::toHexString() const {
    std::string str = toString();
    std::stringstream ss;
    for (size_t i = 0; i < str.size(); ++i) {
        if (i > 0 && i % 32 == 0) {//每隔32个换一行 
            ss << std::endl;
        }
        ss << std::setw(2)         //设置宽度为2，不足两位会用前导字符补齐
           << std::setfill('0')    //补齐的字符用0
           << std::hex             //切换为16进制
           << (int)(uint8_t)str[i] //转为无符号整数
           << " "; 
    }
    return ss.str();
}


//写入固定长度的数据
//写入一字节无需字节序转换
void ByteArray::writeFint8(int8_t value) {
    write(&value, sizeof(value));
}
void ByteArray::writeFuint8(uint8_t value) {
    write(&value, sizeof(value));
}
void ByteArray::writeFint16(int16_t value) {
    //如果设置的字节序与系统原生不同，就要字节序变换
    if (m_endian != SYLAR_BYTE_ORDER) {
        value = byteswap(value);
    }
    write(&value, sizeof(value));
}
void ByteArray::writeFuint16(uint16_t value) {
    if (m_endian != SYLAR_BYTE_ORDER) {
        value = byteswap(value);
    }
    write(&value, sizeof(value));
}
void ByteArray::writeFint32(int32_t value) {
    if (m_endian != SYLAR_BYTE_ORDER) {
        value = byteswap(value);
    }
    write(&value, sizeof(value));
}
void ByteArray::writeFuint32(uint32_t value) {
    if (m_endian != SYLAR_BYTE_ORDER) {
        value = byteswap(value);
    }
    write(&value, sizeof(value));
}
void ByteArray::writeFloat(float value) {
    //memcpy用于在内存中按字节拷贝数据，并不关心数据类型，只要这个两个数据区域的字节大小匹配即可
    uint32_t v;
    memcpy(&v, &value, sizeof(value));
    writeFuint32(v);
}
void ByteArray::writeDouble(double value) {
    uint64_t v;
    memcpy(&v, &value, sizeof(value));
    writeFint64(v);
}
//读固定长度的数据
int8_t ByteArray::readFint8() {
    int8_t v;
    read(&v, sizeof(v));
    return v;
}
uint8_t ByteArray::readFuint8() {
    uint8_t v;
    read(&v, sizeof(v));
    return v;
}

#define XX(type)                           \
    type v;                                \
    read(&v, sizeof(v));                   \
    if (m_endian != SYLAR_BYTE_ORDER) {    \
        return byteswap(v);                \
    } else {                               \
        return v;                          \
    }
int16_t ByteArray::readFint16() {
    XX(int16_t);
}
uint16_t ByteArray::readFuint16() {
    XX(uint16_t);
}
int32_t ByteArray::readFint32() {
    XX(int32_t);
}
uint32_t ByteArray::readFuint32() {
    XX(uint32_t);
}
int64_t ByteArray::readFint64() {
    XX(int64_t);
}
uint64_t ByteArray::readFuint64() {
    XX(uint64_t);
}
#undef XX
float ByteArray::readFloat() {
    uint32_t v = readFuint32();
    float value;
    memcpy(&value, &v, sizeof(v));
    return value;
}
double ByteArray::readDouble() {
    uint64_t v = readFuint64();
    double value;
    memcpy(&value, &v, sizeof(v));
    return value;
}


//向ByteArray中写入字符串，连带写入字符串的长度用指定的数据类型
void ByteArray::writeStringF16(const std::string& value) {
    writeFuint16(value.size());
    write(value.c_str(), value.size());
}
void ByteArray::writeStringF32(const std::string& value) {
    writeFuint32(value.size());
    write(value.c_str(), value.size());
}
void ByteArray::writeStringF64(const std::string& value) {
    writeFuint64(value.size());
    write(value.c_str(), value.size());
}
void ByteArray::writeStringVint(const std::string& value) {
    writeUint64(value.size());
    write(value.c_str(), value.size());
}
void ByteArray::writeStringWithoutLength(const std::string& value) {
    write(value.c_str(), value.size());
}
//从ByteArray中读取字符串指定大小的字符串内容
std::string ByteArray::readStringF16() {
    uint16_t len = readFuint16();
    std::string buff;
    buff.resize(len);
    read(&buff[0], len);
    return buff;
}
std::string ByteArray::readStringF32() {
    uint32_t len = readFuint32();
    std::string buff;
    buff.resize(len);
    read(&buff[0], len);
    return buff;
}
std::string ByteArray::readStringF64() {
    uint64_t len = readFuint64();
    std::string buff;
    buff.resize(len);
    read(&buff[0], len);
    return buff;
}
std::string ByteArray::readStringVin() {
    uint64_t len = readUint64();
    std::string buff;
    buff.resize(len);
    read(&buff[0], len);
    return buff;
}


//以下四个函数是ZigZag编码和解码函数，它们用于将有符号整数(int32_t int64_t)转为无符号整数(uint32_t uint64_t)
//以及从无符号整数还原为有符号整数，ZigZag编码在protobuf等序列化中非常有用，用于更高效的表示负数
//int32_t->uint32_t
static uint32_t EncodeZigzag32(const int32_t& v) {
    //负数映射为奇数，正数映射为偶数
    // 0  ---  0
    // -1 ---  1
    // 1  ---  2
    // -2 ---  3
    // 2  ---  4
    if (v < 0) {
        return (uint32_t)(-v)*2 - 1;
    } else {
        return v * 2;
    }
}
//int64_t->uint64_t
static uint64_t EncodeZigzag64(const int64_t& v) {
    if (v < 0) {
        return (uint32_t)(-v)*2 - 1;
    } else {
        return v * 2;
    }
}
//uint32_t->int32_t
static int32_t DecodeZigzag32(const uint32_t& v) {
    //v >> 1相当于除2
    //v & 1取最低位置，如果v为奇数，则最低位为1，得到的结果为1；如果v为偶数，则最低位为0，得到的结果为0
    //-(v&1)，奇数对应-1，偶数对应0,因为编码时将负数编码为了奇数，正数编码为偶数
    //a^0=a  a^a=0 a^(全1)=a各位取反
    //-1的补码表示是全1，所以一个数^-1时会将该数的所有位都取反，5^-1=-6   7^-1=-8
    return (v >> 1) ^ -(v & 1);
}
//uint64_t->int64_t
static int64_t DecodeZigzag64(const uint64_t& v) {
    return (v >> 1) ^ -(v & 1);
}


// 将 int32_t 类型的有符号整数写入 ByteArray，使用 ZigZag + Varint 编码
// ZigZag 编码是为了让负数也能高效地以变长方式存储（避免高位补 1 导致占用多个字节）
// 先使用 ZigZag 编码转成无符号整数，再交由 writeUint32 写入
void ByteArray::writeInt32(int32_t value) {
    writeUint32(EncodeZigzag32(value));
}

// 将 uint32_t 类型的无符号整数写入 ByteArray，使用 Varint 编码（变长编码）
// Varint 通过每 7 位存储一组，最高位（MSB）用作是否还有后续字节的标志位
// 如果最高位为 1，说明后面还有字节；如果最高位为 0，说明当前字节是最后一个
void ByteArray::writeUint32(uint32_t value) {
    uint8_t temp[5];  // 最多 5 字节可以表示一个 uint32_t（7 * 5 = 35 > 32）
    uint8_t i = 0;
    while (value >= 0x80) {  // 0x80 == 10000000（二进制），表示需要继续处理后续字节
        // 将低 7 位取出（value & 0x7F），最高位置 1 表示后续还有数据
        temp[i++] = (value & 0x7F) | 0x80;
        value >>= 7; // 右移 7 位，处理下一组
    }
    // 最后一组低 7 位，不需要设置最高位
    temp[i++] = value;
    // 将编码后的字节写入 ByteArray
    write(temp, i);
}

// int64 版本：先用 ZigZag 编码，再使用变长编码写入
void ByteArray::writeInt64(int64_t value) {
    writeUint64(EncodeZigzag64(value));
}

// 将 uint64_t 类型写入 ByteArray，使用变长 Varint 编码（每 7 位为一组）
// 最多用 10 个字节（7 * 10 = 70 > 64）
void ByteArray::writeUint64(uint64_t value) {
    uint8_t temp[10]; // 最多 10 字节
    uint8_t i = 0;
    while (value >= 0x80) {
        temp[i++] = (value & 0x7F) | 0x80;  // 设置 MSB = 1，表示后续还有
        value >>= 7;
    }
    temp[i++] = value;  // 最后一组 MSB = 0
    write(temp, i);  // 写入字节数组
}

// 从 ByteArray 中读取一个变长编码的 int32_t
// 实际是读取 uint32_t 并解码成 int32_t
int32_t ByteArray::readInt32() {
    return DecodeZigzag32(readUint32());
}

// 从 ByteArray 中读取一个 Varint 编码的 uint32_t
// 每个字节的低 7 位是数据，高位为 1 表示后续还有字节，为 0 表示结束
uint32_t ByteArray::readUint32() {
    uint32_t result = 0;
    for (int i = 0; i < 32; i += 7) {
        uint8_t b = readFuint8();  // 逐字节读取
        if (b < 0x80) {
            // 当前字节是最后一个：将其左移对应的位后合并到结果中
            result |= (uint32_t)b << i;
            break;
        } else {
            // 当前字节不是最后一个：取低 7 位后合并
            result |= ((uint32_t)(b & 0x7F)) << i;
        }
    }
    return result;
}

// 从 ByteArray 中读取 int64_t（变长编码 + ZigZag）
int64_t ByteArray::readInt64() {
    return DecodeZigzag64(readUint64());
}

// 读取变长编码的 uint64_t（每次取 7 位，遇到 MSB = 0 表示结束）
uint64_t ByteArray::readUint64() {
    uint64_t result = 0;
    for (int i = 0; i < 64; i += 7) {
        uint8_t b = readFuint8();
        if (b < 0x80) {
            result |= (uint64_t)b << i;
            break;
        } else {
            result |= ((uint64_t)(b & 0x7F)) << i;
        }
    }
    return result;
}


//将ByteArray中从当前位置(m_position)开始的所有可读数据写入到名为name的文件中
bool writeToFile(const std::string& name) const {
    std::ofstream ofs;
    ofs.open(name, std::ios::trunc | std::ios::binary);
    if (!ofs) {
        return false;
    }
    int64_t read_size = getReadSize();
    int64_t pos = m_position;
    Node* cur = m_cur;
    while (read_size > 0) {
        //当前块中已读取的偏移量
        int diff = pos % m_baseSize;
        int64_t len = (read_size > (int64_t)m_baseSize ? m_baseSize : read_size) - diff;
        ofs.write(cur->ptr + diff, len);
        cur = cur->next;
        pos += len;
        read_size -= len;
    }
    return true;
}
//从一个二进制文件中读取数据，并写入当前ByteArray
bool readFromFile(const std::string& name) const {
    std::ifstream ifs;
    ifs.open(name, std::ios::binary);
    if (!ifs) return false;
    //创建缓冲区，用于每次读取数据
    std::shared_ptr<char> buff(new char[m_baseSize], [](char* ptr) {delete[] ptr});
    while (!ifs.eof()) {  //没读到末尾
        //从文件中最多读取m_baseSize个字节到缓冲区
        ifs.read(buff.get(), m_baseSize);
        //把读取到buff中的数据写入到byteArray中
        write(buff.get(), ifs.gcount());
    }
    return true;
}


//下面的三个函数都是配合可以收发iovec的函数使用如readv、writev
uint64_t ByteArray::getReadBuffers(std::vector<iovec>& buffers, uint64_t len) const {
    len = (len > getReadSize()) ? getReadSize() : len;
    if (len == 0) return;
    uint64_t size = len;
    size_t npos = m_position % m_baseSize;
    size_t ncap = m_cur->size - npos;
    struct iovec iov;
    Node* cur = m_cur;
    while (len > 0) {
        if (ncap > len) {
            iov.iov_base = cur->ptr + npos;
            iov.iov_len = len;
            len = 0;
        } else {
            iov.iov_base = cur->ptr + npos;
            iov.iov_len = ncap;
            len -= ncap;
            ncap = cur->size;
            cur = cur->next;
            npos = 0;
        }
        buffers.push_back(iov);
    }
    return size;
}

uint64_t ByteArray::getReadBuffers(std::vector<iovec>& buffers, uint64_t len, uint64_t position) const {
    len = (len > getReadSize()) ? getReadSize() : len;
    if (len == 0) return;
    uint64_t size = len;
    size_t npos = position % m_baseSize;
    size_t count = position / m_baseSize;
    Node* cur = m_root;
    while (count > 0) {
        cur = cur->next;
        count--;
    }
    size_t ncap = cur->size - npos;
    struct iovec iov;
    while (len > 0) {
        if (ncap > len) {
            iov.iov_base = cur->ptr + npos;
            iov.iov_len = len;
            len = 0;
        } else {
            iov.iov_base = cur->ptr + npos;
            iov.iov_len = ncap;
            len -= ncap;
            ncap = cur->size;
            cur = cur->next;
            npos = 0;
        }
        buffers.push_back(iov);
    }
    return size;
}

uint64_t ByteArray::getWriteBuffers(std::vector<iovec>& buffers, uint64_t len) const {
    if (len == 0) return 0;
    addCapacity(len);  //确保有足够的容量写入len字节的数据
    uint64_t size = len;
    size_t npos = m_position % m_baseSize;
    size_t ncap = m_cur->size - npos;
    struct iovec iov;
    Node* cur = m_cur;
    while (len > 0) {
        if (ncap > len) {
            iov.iov_base = cur->ptr + npos;
            iov.iov_len = len;
            len = 0;
        } else {
            iov.iov_base = cur->ptr + npos;
            iov.iov_len = ncap;
            len -= ncap;
            ncap = cur->size;
            cur = cur->next;
            npos = 0;
        }
        buffers.push_back(iov);
    }
    return size;
}


}
