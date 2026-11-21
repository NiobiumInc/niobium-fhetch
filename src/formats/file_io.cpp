// Copyright 2024-present Niobium Microsystems, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//

/**
 * @file file_io.cpp
 * @brief Internal file machinery shared by .nbbr and .nbom (see file_io.h).
 */
#include "file_io.h"

#include "niobium/binding_record.h"

#include <fcntl.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <ios>
#include <iterator>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace niobium::binding::detail {

ByteView ByteView::sub(std::uint64_t offset, std::uint64_t count) const {
    if (offset > size_ || count > size_ - offset) throw std::out_of_range("binding formats: read past the end");
    return {data_ + offset, static_cast<std::size_t>(count)};
}

void fail(RecordErrc code, std::string_view what, const std::string& message) {
    throw RecordError(code, std::string(what) + ": " + message);
}

void check_preamble(ByteView bytes, std::size_t header_size, std::array<char, 4> magic,
                    std::uint16_t version, std::string_view what) {
    if (bytes.size() < header_size)
        fail(RecordErrc::SizeMismatch, what,
             "file is " + std::to_string(bytes.size()) + " bytes, shorter than the header");
    const auto p = load<Preamble>(bytes, 0);
    if (p.magic != magic)
        fail(RecordErrc::BadMagic, what, "magic is not \"" + std::string(magic.data(), magic.size()) + "\"");
    if (p.sentinel == kSentinelByteSwapped)
        fail(RecordErrc::BigEndianWriter, what, "sentinel is byte-swapped: written by a big-endian writer");
    if (p.sentinel != kSentinel) fail(RecordErrc::BadSentinel, what, "sentinel is not 0x0123456789ABCDEF");
    if (p.version != version)
        fail(RecordErrc::UnsupportedVersion, what,
             "version " + std::to_string(p.version) + " is not " + std::to_string(version));
}

void check_size(ByteView bytes, std::uint64_t expected, std::string_view what) {
    if (bytes.size() != expected)
        fail(RecordErrc::SizeMismatch, what,
             "file is " + std::to_string(bytes.size()) + " bytes; its layout needs " + std::to_string(expected));
}

std::string pool_label(ByteView pool, std::uint32_t offset, std::uint16_t len,
                       std::string_view what, const std::string& who) {
    if (std::uint64_t{offset} + len > pool.size())
        fail(RecordErrc::BadLabel, what, who + "'s label lies outside the label pool");
    const auto bytes = pool.sub(offset, len);
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

void put_label(std::vector<std::byte>& out, std::uint64_t pool_start, std::uint32_t& pool_at, std::string_view label) {
    if (pool_start + pool_at + label.size() > out.size()) throw std::out_of_range("binding formats: label past the end");
    std::memcpy(out.data() + pool_start + pool_at, label.data(), label.size());
    pool_at += static_cast<std::uint32_t>(label.size());
}

std::vector<std::byte> read_file(const std::filesystem::path& path, std::string_view what) {
    std::ifstream in(path, std::ios::binary);
    if (!in) fail(RecordErrc::Io, what, "cannot open " + path.string());
    const std::vector<char> chars{std::istreambuf_iterator<char>(in), {}};
    if (in.bad()) fail(RecordErrc::Io, what, "cannot read " + path.string());
    std::vector<std::byte> bytes(chars.size());
    std::memcpy(bytes.data(), chars.data(), chars.size());
    return bytes;
}

namespace {

[[noreturn]] void fail_io(std::string_view what, const std::string& action, const std::filesystem::path& path) {
    const int err = errno;
    fail(RecordErrc::Io, what, action + " " + path.string() + ": " + std::strerror(err));
}

int sync_fd(int fd) {
#ifdef F_FULLFSYNC
    if (::fcntl(fd, F_FULLFSYNC) == 0) return 0;
#endif
    return ::fsync(fd);
}

// Closes a descriptor and removes the temp file unless committed.
class TempFile {
public:
    TempFile(std::filesystem::path path, std::string_view what)
        : path_(std::move(path)),
          what_(what),
          fd_(::open(path_.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0666)) {
        if (fd_ < 0) fail_io(what_, "cannot create", path_);
    }
    TempFile(const TempFile&) = delete;
    TempFile& operator=(const TempFile&) = delete;
    TempFile(TempFile&&) = delete;
    TempFile& operator=(TempFile&&) = delete;
    ~TempFile() {
        if (fd_ >= 0) (void)::close(fd_);
        if (!committed_) {
            std::error_code ec;
            std::filesystem::remove(path_, ec);  // best effort: the write already failed
        }
    }

    void write_all(const std::vector<std::byte>& bytes) {
        std::size_t done = 0;
        while (done < bytes.size()) {
            const auto n = ::write(fd_, bytes.data() + done, bytes.size() - done);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) fail_io(what_, "cannot write", path_);
            done += static_cast<std::size_t>(n);
        }
        if (sync_fd(fd_) != 0) fail_io(what_, "cannot sync", path_);
        const int fd = std::exchange(fd_, -1);
        if (::close(fd) != 0) fail_io(what_, "cannot close", path_);
    }

    void commit(const std::filesystem::path& target) {
        if (::rename(path_.c_str(), target.c_str()) != 0) fail_io(what_, "cannot rename onto", target);
        committed_ = true;
        const auto dir = target.parent_path().empty() ? std::filesystem::path(".") : target.parent_path();
        const int dfd = ::open(dir.c_str(), O_RDONLY | O_CLOEXEC);
        if (dfd < 0) fail_io(what_, "cannot open directory", dir);
        const int rc = sync_fd(dfd);
        (void)::close(dfd);
        if (rc != 0) fail_io(what_, "cannot sync directory", dir);
    }

private:
    std::filesystem::path path_;
    std::string_view what_;
    int fd_ = -1;
    bool committed_ = false;
};

}  // namespace

void write_atomically(const std::filesystem::path& path, const std::vector<std::byte>& bytes, std::string_view what) {
    std::random_device rd;
    TempFile file(path.parent_path() / ("." + path.filename().string() + ".tmp-" + std::to_string(rd())), what);
    file.write_all(bytes);
    file.commit(path);
}

}  // namespace niobium::binding::detail
