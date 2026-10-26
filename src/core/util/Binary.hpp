#pragma once

#include <bit>
#include <cstdint>
#include <cstring>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace fo4mx {

static_assert(std::endian::native == std::endian::little,
              "Bethesda file formats are little-endian; a little-endian host is required.");

// Thrown when a file does not match the structure we expect.
class FormatError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// Bounds-checked reader over a byte span.
class BinaryReader {
public:
    BinaryReader() = default;
    BinaryReader(const uint8_t* data, size_t size) : data_(data), size_(size) {}
    explicit BinaryReader(std::span<const uint8_t> bytes) : data_(bytes.data()), size_(bytes.size()) {}

    template <class T>
    T read()
    {
        static_assert(std::is_trivially_copyable_v<T>);
        require(sizeof(T));
        T value;
        std::memcpy(&value, data_ + pos_, sizeof(T));
        pos_ += sizeof(T);
        return value;
    }

    template <class T>
    T peek() const
    {
        static_assert(std::is_trivially_copyable_v<T>);
        require(sizeof(T));
        T value;
        std::memcpy(&value, data_ + pos_, sizeof(T));
        return value;
    }

    std::span<const uint8_t> readBytes(size_t count)
    {
        require(count);
        std::span<const uint8_t> out(data_ + pos_, count);
        pos_ += count;
        return out;
    }

    // Null-terminated string; consumes the terminator if present.
    std::string readZString()
    {
        size_t end = pos_;
        while (end < size_ && data_[end] != 0)
            ++end;
        std::string s(reinterpret_cast<const char*>(data_ + pos_), end - pos_);
        pos_ = end < size_ ? end + 1 : end;
        return s;
    }

    // Fixed-size field; trailing nulls are trimmed.
    std::string readFixedString(size_t count)
    {
        auto bytes = readBytes(count);
        size_t len = 0;
        while (len < bytes.size() && bytes[len] != 0)
            ++len;
        return std::string(reinterpret_cast<const char*>(bytes.data()), len);
    }

    // uint16 length prefix, no terminator (Papyrus / BA2 name table style).
    std::string readWString()
    {
        uint16_t len = read<uint16_t>();
        auto bytes = readBytes(len);
        return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    }

    // uint8 length prefix (NIF export strings); trailing null trimmed.
    std::string readBString()
    {
        uint8_t len = read<uint8_t>();
        return readFixedString(len);
    }

    void skip(size_t count) { require(count); pos_ += count; }
    void seek(size_t pos)
    {
        if (pos > size_)
            throw FormatError("seek beyond end of buffer");
        pos_ = pos;
    }

    size_t position() const { return pos_; }
    size_t size() const { return size_; }
    size_t remaining() const { return size_ - pos_; }
    bool atEnd() const { return pos_ >= size_; }
    const uint8_t* data() const { return data_; }
    const uint8_t* current() const { return data_ + pos_; }

private:
    void require(size_t count) const
    {
        if (count > size_ - pos_)
            throw FormatError("unexpected end of data (need " + std::to_string(count) + " bytes, have " +
                              std::to_string(size_ - pos_) + ")");
    }

    const uint8_t* data_ = nullptr;
    size_t size_ = 0;
    size_t pos_ = 0;
};

class BinaryWriter {
public:
    template <class T>
    void write(const T& value)
    {
        static_assert(std::is_trivially_copyable_v<T>);
        const auto* p = reinterpret_cast<const uint8_t*>(&value);
        buffer_.insert(buffer_.end(), p, p + sizeof(T));
    }

    void writeBytes(std::span<const uint8_t> bytes) { buffer_.insert(buffer_.end(), bytes.begin(), bytes.end()); }
    void writeBytes(const void* data, size_t size)
    {
        const auto* p = static_cast<const uint8_t*>(data);
        buffer_.insert(buffer_.end(), p, p + size);
    }

    void writeZString(std::string_view s)
    {
        writeBytes(s.data(), s.size());
        buffer_.push_back(0);
    }

    void writeWString(std::string_view s)
    {
        write<uint16_t>(static_cast<uint16_t>(s.size()));
        writeBytes(s.data(), s.size());
    }

    void writeZeros(size_t count) { buffer_.insert(buffer_.end(), count, 0); }

    template <class T>
    void patch(size_t offset, const T& value)
    {
        static_assert(std::is_trivially_copyable_v<T>);
        if (offset + sizeof(T) > buffer_.size())
            throw std::out_of_range("BinaryWriter::patch out of range");
        std::memcpy(buffer_.data() + offset, &value, sizeof(T));
    }

    size_t size() const { return buffer_.size(); }
    const std::vector<uint8_t>& buffer() const { return buffer_; }
    std::vector<uint8_t> take() { return std::move(buffer_); }
    void reserve(size_t n) { buffer_.reserve(n); }

private:
    std::vector<uint8_t> buffer_;
};

template <class T>
inline T loadLE(const uint8_t* p)
{
    T v;
    std::memcpy(&v, p, sizeof(T));
    return v;
}

template <class T>
inline void storeLE(uint8_t* p, T v)
{
    std::memcpy(p, &v, sizeof(T));
}

} // namespace fo4mx
