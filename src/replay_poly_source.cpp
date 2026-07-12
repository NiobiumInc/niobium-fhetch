// Copyright 2024-present Niobium Microsystems, Inc.
// Licensed under the Apache License, Version 2.0.

#include "replay_poly_source.h"

#include "local_replay.h"  // read_ids, load_input_bin, load_key_bin, load_bp_bin

#include <fcntl.h>
#include <unistd.h>

#include <nlohmann/json.hpp>

#include <cerrno>
#include <cstring>
#include <fstream>
#include <iostream>

namespace fs = std::filesystem;

namespace niobium {

namespace {

// Sidecar layout (little-endian, fixed records → offset arithmetic):
//   header:  magic "NFPC" | version u32 | ring_dim u64 | count u64
//            | src_size u64 | src_mtime i64
//   index:   count x { addr u64, modulus u64 }
//   data:    count x ring_dim x u64 coefficients
constexpr char kMagic[4] = {'N', 'F', 'P', 'C'};
constexpr uint32_t kVersion = 1;

struct CacheHeader {
    char magic[4];
    uint32_t version;
    uint64_t ring_dim;
    uint64_t count;
    uint64_t src_size;
    int64_t src_mtime;
};

int64_t mtime_of(const fs::path& p) {
    std::error_code ec;
    auto t = fs::last_write_time(p, ec);
    return ec ? 0 : static_cast<int64_t>(t.time_since_epoch().count());
}

uint64_t size_of(const fs::path& p) {
    std::error_code ec;
    auto s = fs::file_size(p, ec);
    return ec ? 0 : s;
}

}  // namespace

DiskPolySource::~DiskPolySource() {
    for (auto& c : caches_)
        if (c.fd >= 0) ::close(c.fd);
}

std::shared_ptr<DiskPolySource> DiskPolySource::open(
    const fs::path& dir, const std::string& prog, uint64_t ring_dim,
    fs::path cache_dir, std::string* error) {
    auto src = std::shared_ptr<DiskPolySource>(new DiskPolySource());
    src->ring_dim_ = ring_dim;
    if (cache_dir.empty()) cache_dir = dir;

    if (!src->index_inputs(dir, prog, error)) return nullptr;
    for (const char* kind : {"mk", "rk", "bp"}) {
        if (!src->prepare_key_cache(dir, prog, kind, cache_dir, error))
            return nullptr;
    }
    std::cout << "[fhetch_sim] lazy source: " << src->index_.size()
              << " polys indexed (" << src->inputs_.size() << " input files, "
              << src->caches_.size() << " key caches)" << std::endl;
    return src;
}

bool DiskPolySource::index_inputs(const fs::path& dir, const std::string& prog,
                                  std::string* error) {
    fs::path idx_path = dir / (prog + ".inputs.json");
    if (!fs::exists(idx_path)) return true;  // no inputs is fine

    nlohmann::json j;
    try {
        std::ifstream in(idx_path);
        in >> j;
    } catch (const std::exception& e) {
        if (error) *error = "failed to parse " + idx_path.string() + ": " + e.what();
        return false;
    }
    if (!j.contains("inputs")) return true;

    for (const auto& entry : j["inputs"]) {
        std::string bin = entry.value("bin_file", "");
        std::string ids = entry.value("ids_file", "");
        if (bin.empty() || ids.empty()) continue;
        fs::path bin_path = fs::path(bin).is_absolute() ? fs::path(bin) : dir / bin;
        fs::path ids_path = fs::path(ids).is_absolute() ? fs::path(ids) : dir / ids;
        if (!fs::exists(bin_path) || !fs::exists(ids_path)) {
            if (error)
                *error = "input " + entry.value("name", bin) +
                         " references a missing .bin/.ids under " + dir.string();
            return false;
        }
        InputFile f{bin_path, fhetch::read_ids(ids_path)};
        uint32_t file_idx = static_cast<uint32_t>(inputs_.size());
        for (uint64_t addr : f.ids)
            index_[addr] = Rec{Rec::Kind::Input, file_idx, 0, 0};
        inputs_.push_back(std::move(f));
    }
    return true;
}

bool DiskPolySource::prepare_key_cache(const fs::path& dir, const std::string& prog,
                                       const std::string& kind,
                                       const fs::path& cache_dir,
                                       std::string* error) {
    fs::path bin = dir / (prog + "." + kind + ".bin");
    fs::path ids = dir / (prog + "." + kind + ".ids");
    if (!fs::exists(bin) || !fs::exists(ids)) return true;  // optional

    fs::path cache = cache_dir / (prog + "." + kind + ".polycache");
    uint64_t src_size = size_of(bin);
    int64_t src_mtime = mtime_of(bin);

    // Validate an existing sidecar.
    bool valid = false;
    CacheHeader hdr{};
    if (fs::exists(cache)) {
        std::ifstream in(cache, std::ios::binary);
        if (in.read(reinterpret_cast<char*>(&hdr), sizeof(hdr))) {
            valid = std::memcmp(hdr.magic, kMagic, 4) == 0 &&
                    hdr.version == kVersion && hdr.ring_dim == ring_dim_ &&
                    hdr.src_size == src_size && hdr.src_mtime == src_mtime;
        }
    }

    if (!valid) {
        // One streaming pass over the cereal archive (constant memory)
        // into a temp file, atomically renamed. Losers of a concurrent
        // race simply overwrite with identical content.
        std::cout << "[fhetch_sim] building key sidecar " << cache
                  << " (one-time streaming conversion)" << std::endl;
        auto ids_v = fhetch::read_ids(ids);
        fs::path tmp = cache;
        tmp += ".tmp." + std::to_string(::getpid());
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out.is_open()) {
            if (error)
                *error = "cannot write key sidecar " + tmp.string() +
                         " (set NIOBIUM_FHETCH_CACHE_DIR to a writable local dir)";
            return false;
        }
        hdr = CacheHeader{{kMagic[0], kMagic[1], kMagic[2], kMagic[3]},
                          kVersion, ring_dim_, 0, src_size, src_mtime};
        out.write(reinterpret_cast<const char*>(&hdr), sizeof(hdr));

        // Index and data are written in one pass: index entries as we go,
        // data appended after — requires two passes OR buffering the small
        // index. Buffer the index (16 bytes/poly) and write data second.
        struct IdxEnt { uint64_t addr, modulus; };
        std::vector<IdxEnt> idx_ents;
        idx_ents.reserve(ids_v.size());
        // Pass 1: index only (values discarded) — cheap fields, but we'd
        // decode twice. Instead do it in ONE pass: buffer index entries,
        // stream coefficients to a second temp region... simplest correct
        // approach: hold index in RAM (small) and stream data directly at
        // its final offset by pre-seeking past the index block.
        uint64_t count = ids_v.size();
        uint64_t index_bytes = count * sizeof(IdxEnt);
        out.seekp(static_cast<std::streamoff>(sizeof(hdr) + index_bytes));

        bool ok;
        auto sink = [&](uint64_t addr, std::vector<uint64_t>&& values,
                        uint64_t modulus) {
            values.resize(ring_dim_);
            idx_ents.push_back({addr, modulus});
            out.write(reinterpret_cast<const char*>(values.data()),
                      static_cast<std::streamsize>(ring_dim_ * sizeof(uint64_t)));
        };
        if (kind == "bp")
            ok = fhetch::load_bp_bin(bin, ids_v, sink);
        else
            ok = fhetch::load_key_bin(bin, ids_v, sink);
        if (!ok || !out.good()) {
            if (error) *error = "key sidecar conversion failed for " + bin.string();
            std::error_code ec;
            fs::remove(tmp, ec);
            return false;
        }
        hdr.count = idx_ents.size();
        out.seekp(0);
        out.write(reinterpret_cast<const char*>(&hdr), sizeof(hdr));
        out.write(reinterpret_cast<const char*>(idx_ents.data()),
                  static_cast<std::streamsize>(idx_ents.size() * sizeof(IdxEnt)));
        out.close();
        std::error_code ec;
        fs::rename(tmp, cache, ec);
        if (ec) {
            if (error) *error = "cannot rename key sidecar into place: " + ec.message();
            fs::remove(tmp, ec);
            return false;
        }
    }

    // Open + index the (now valid) sidecar.
    int fd = ::open(cache.c_str(), O_RDONLY);
    if (fd < 0) {
        if (error) *error = "cannot open key sidecar " + cache.string();
        return false;
    }
    CacheHeader h{};
    if (::pread(fd, &h, sizeof(h), 0) != sizeof(h)) {
        ::close(fd);
        if (error) *error = "cannot read key sidecar header " + cache.string();
        return false;
    }
    uint32_t cache_idx = static_cast<uint32_t>(caches_.size());
    CacheFile cf;
    cf.path = cache;
    cf.fd = fd;
    cf.count = h.count;
    cf.data_off = sizeof(CacheHeader) + h.count * 16;
    std::vector<std::pair<uint64_t, uint64_t>> ents(h.count);
    if (h.count &&
        ::pread(fd, ents.data(), h.count * 16, sizeof(CacheHeader)) !=
            static_cast<ssize_t>(h.count * 16)) {
        ::close(fd);
        if (error) *error = "cannot read key sidecar index " + cache.string();
        return false;
    }
    for (uint32_t r = 0; r < h.count; ++r)
        index_[ents[r].first] =
            Rec{Rec::Kind::KeyCache, cache_idx, r, ents[r].second};
    caches_.push_back(std::move(cf));
    return true;
}

bool DiskPolySource::contains(uint64_t addr) const {
    return index_.count(addr) != 0U;
}

size_t DiskPolySource::load_granularity(uint64_t addr) const {
    auto it = index_.find(addr);
    if (it == index_.end()) return 0;
    return it->second.kind == Rec::Kind::Input ? inputs_[it->second.file].ids.size()
                                               : 1;
}

bool DiskPolySource::load(uint64_t addr, const fhetch_sim::PolySink& sink) {
    auto it = index_.find(addr);
    if (it == index_.end()) return false;
    const Rec& rec = it->second;

    if (rec.kind == Rec::Kind::KeyCache) {
        const auto& cf = caches_[rec.file];
        fhetch_sim::LoadedPoly p;
        p.addr = addr;
        p.modulus = rec.modulus;
        p.values.resize(ring_dim_);
        off_t off = static_cast<off_t>(cf.data_off) +
                    static_cast<off_t>(rec.record) *
                        static_cast<off_t>(ring_dim_ * sizeof(uint64_t));
        if (::pread(cf.fd, p.values.data(), ring_dim_ * sizeof(uint64_t), off) !=
            static_cast<ssize_t>(ring_dim_ * sizeof(uint64_t))) {
            std::cerr << "[fhetch_sim] key sidecar read failed at record "
                      << rec.record << " of " << cf.path << std::endl;
            return false;
        }
        return sink(std::move(p));
    }

    // Input file: stream-decode the whole ciphertext, offering every tower.
    const auto& f = inputs_[rec.file];
    bool delivered = false;
    bool ok = fhetch::load_input_bin(
        f.bin, f.ids,
        [&](uint64_t a, std::vector<uint64_t>&& values, uint64_t modulus) {
            values.resize(ring_dim_);
            fhetch_sim::LoadedPoly p{a, modulus, std::move(values)};
            bool kept = sink(std::move(p));
            if (a == addr && kept) delivered = true;
        });
    return ok && delivered;
}

}  // namespace niobium
