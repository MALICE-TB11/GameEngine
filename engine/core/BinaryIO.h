#pragma once
// 二进制读写：烘焙产物（.emodel / .etex）的序列化工具。
//
// 规则很简单：按写入顺序原样读回。只支持「平凡可复制」的 POD、字符串和 POD 数组。
// 读取时每一步都检查越界：烘焙文件被截断或版本不对时返回失败（ok() == false），
// 由上层改用兜底资源，而不是读到垃圾数据后崩溃。

#include <cstdint>
#include <cstring>
#include <string>
#include <type_traits>
#include <vector>

namespace eng {

class BinaryWriter {
public:
    template <class T>
    void write(const T& v) {
        static_assert(std::is_trivially_copyable_v<T>);
        const auto* p = reinterpret_cast<const uint8_t*>(&v);
        m_data.insert(m_data.end(), p, p + sizeof(T));
    }
    void writeString(const std::string& s) {
        write(static_cast<uint32_t>(s.size()));
        m_data.insert(m_data.end(), s.begin(), s.end());
    }
    template <class T>
    void writeArray(const std::vector<T>& v) {
        static_assert(std::is_trivially_copyable_v<T>);
        write(static_cast<uint64_t>(v.size()));
        const auto* p = reinterpret_cast<const uint8_t*>(v.data());
        m_data.insert(m_data.end(), p, p + v.size() * sizeof(T));
    }
    const std::vector<uint8_t>& data() const { return m_data; }
    std::vector<uint8_t>& data() { return m_data; }

private:
    std::vector<uint8_t> m_data;
};

class BinaryReader {
public:
    BinaryReader(const uint8_t* data, size_t size) : m_data(data), m_size(size) {}
    explicit BinaryReader(const std::vector<uint8_t>& v) : BinaryReader(v.data(), v.size()) {}

    template <class T>
    T read() {
        static_assert(std::is_trivially_copyable_v<T>);
        T v{};
        if (!need(sizeof(T))) return v;
        std::memcpy(&v, m_data + m_pos, sizeof(T));
        m_pos += sizeof(T);
        return v;
    }
    std::string readString() {
        const uint32_t n = read<uint32_t>();
        if (!need(n)) return {};
        std::string s(reinterpret_cast<const char*>(m_data + m_pos), n);
        m_pos += n;
        return s;
    }
    template <class T>
    std::vector<T> readArray() {
        static_assert(std::is_trivially_copyable_v<T>);
        const uint64_t n = read<uint64_t>();
        // 先检查剩余字节够不够，防止损坏文件里的超大 n 导致巨量内存分配
        if (!m_ok || n > (m_size - m_pos) / (sizeof(T) ? sizeof(T) : 1) || !need(size_t(n) * sizeof(T))) {
            m_ok = false;
            return {};
        }
        std::vector<T> v(static_cast<size_t>(n));
        std::memcpy(v.data(), m_data + m_pos, size_t(n) * sizeof(T));
        m_pos += size_t(n) * sizeof(T);
        return v;
    }
    bool ok() const { return m_ok; }
    bool atEnd() const { return m_pos == m_size; }

private:
    bool need(size_t n) {
        if (!m_ok || m_pos + n > m_size) {
            m_ok = false;
            return false;
        }
        return true;
    }
    const uint8_t* m_data;
    size_t m_size;
    size_t m_pos = 0;
    bool m_ok = true;
};

} // namespace eng
