// Copyright 2024-present Niobium Microsystems, Inc.
// Licensed under the Apache License, Version 2.0.

#include "residency.h"

#include <algorithm>
#include <iostream>

namespace niobium::fhetch_sim {

ResidencyManager::ResidencyManager(Memory& memory, uint64_t ring_dim,
                                   uint64_t budget_bytes,
                                   const std::filesystem::path& spill_dir,
                                   std::shared_ptr<PolySource> source,
                                   std::unordered_set<uint64_t> live_out)
    : memory_(memory),
      ring_dim_(ring_dim),
      budget_polys_(std::max<uint64_t>(budget_bytes / (ring_dim * sizeof(uint64_t)),
                                       kMinBudgetPolys)),
      spill_(spill_dir, ring_dim * sizeof(uint64_t)),
      source_(std::move(source)) {
    for (uint64_t a : live_out) descs_[a].live_out = true;
}

ResidencyManager::PolyDesc& ResidencyManager::desc(uint64_t addr) {
    return descs_[addr];
}

bool ResidencyManager::is_pinned(uint64_t addr) const {
    for (int j = 0; j < n_pinned_; ++j)
        if (pinned_[j] == addr) return true;
    return false;
}

void ResidencyManager::push_heap(uint64_t addr, uint32_t next_use) {
    descs_[addr].next_use = next_use;
    victims_.push({next_use, addr});
}

void ResidencyManager::begin_instruction(uint32_t i, const InstUses& u) {
    n_pinned_ = 0;
    for (int j = 0; j < u.n_reads; ++j)
        if (u.reads[j] != 0) pinned_[n_pinned_++] = u.reads[j];
    if (u.writes && u.write != 0) pinned_[n_pinned_++] = u.write;

    for (int j = 0; j < u.n_reads; ++j)
        if (u.reads[j] != 0) ensure_resident(u.reads[j], static_cast<int64_t>(i) - 1);

    // Reserve the dest write-only: every kernel overwrites all elements,
    // so evicted prior contents are never faulted back in. If dest
    // aliases a read it is already resident and untouched here.
    if (u.writes && u.write != 0) {
        auto& d = desc(u.write);
        if (!d.resident) {
            memory_.reserve_dest(u.write);
            d.resident = true;
            ++resident_polys_;
        }
    }

    evict_to(budget_polys_);
    if (resident_polys_ > stats_.peak_resident_polys)
        stats_.peak_resident_polys = resident_polys_;
}

void ResidencyManager::end_instruction(uint32_t i, const InstUses& u) {
    // The dest's value just changed: any spill slot or on-disk source
    // copy is stale from here on.
    if (u.writes && u.write != 0) {
        auto& d = desc(u.write);
        d.dirty = true;
        d.source_valid = false;
        if (d.spill_slot != SpillFile::kNoSlot) {
            spill_.release(d.spill_slot);
            d.spill_slot = SpillFile::kNoSlot;
        }
    }

    auto retire = [&](uint64_t a) {
        if (a == 0) return;
        auto& d = desc(a);
        uint32_t next = use_index_.next_use_after(a, i);
        if (next == UseIndex::kNever && !d.live_out) {
            free_poly(a, d);
        } else if (d.resident) {
            push_heap(a, next);
        } else {
            d.next_use = next;
        }
    };
    for (int j = 0; j < u.n_reads; ++j) retire(u.reads[j]);
    // Skip the dest when it aliases a read (already retired above —
    // retiring twice would advance nothing but pushes a duplicate).
    if (u.writes && u.write != 0 &&
        !(u.n_reads > 0 && u.reads[0] == u.write) &&
        !(u.n_reads > 1 && u.reads[1] == u.write))
        retire(u.write);

    n_pinned_ = 0;
}

void ResidencyManager::notify_stored(uint64_t addr) {
    auto& d = desc(addr);
    if (!d.resident) {
        d.resident = true;
        ++resident_polys_;
    }
    // Population happens before instruction 0.
    d.dirty = !d.source_valid;
    uint32_t next = use_index_.next_use_after(addr, -1);
    if (next == UseIndex::kNever && !d.live_out) {
        free_poly(addr, d);
        return;
    }
    push_heap(addr, next);
    evict_to(budget_polys_);
    if (resident_polys_ > stats_.peak_resident_polys)
        stats_.peak_resident_polys = resident_polys_;
}

bool ResidencyManager::fault_for_read(uint64_t addr) {
    auto it = descs_.find(addr);
    if (it != descs_.end() && it->second.resident) return true;
    ensure_resident(addr, /*pos=*/-1);
    it = descs_.find(addr);
    return it != descs_.end() && it->second.resident;
}

void ResidencyManager::ensure_resident(uint64_t addr, int64_t pos) {
    auto& d = desc(addr);
    if (d.resident) return;

    if (d.spill_slot != SpillFile::kNoSlot) {
        std::vector<uint64_t> buf(ring_dim_);
        if (spill_.read(d.spill_slot, buf.data())) {
            memory_.set_owned(addr, std::move(buf), d.modulus);
            d.resident = true;
            ++resident_polys_;
            ++stats_.faults_spill;
            // The slot stays valid: the data is unchanged since the spill,
            // so a clean re-eviction can drop the buffer for free.
        }
        return;
    }

    if (source_ && source_->contains(addr)) {
        // Make room for the whole file up front; siblings that still
        // don't fit (or are dead) are declined by the sink.
        size_t incoming = source_->load_granularity(addr);
        make_room_for(incoming);
        source_->load(addr, [this, addr](LoadedPoly&& p) -> bool {
            auto& pd = desc(p.addr);
            if (pd.resident) return true;
            bool wanted = p.addr == addr;
            if (!wanted) {
                if (resident_polys_ >= budget_polys_) return false;
                uint32_t next = use_index_.next_use_after(p.addr, -1);
                if (next == UseIndex::kNever && !pd.live_out) return false;
                pd.next_use = next;
            }
            pd.modulus = p.modulus;
            pd.source_valid = true;
            pd.dirty = false;
            memory_.set_owned(p.addr, std::move(p.values), p.modulus);
            pd.resident = true;
            ++resident_polys_;
            ++stats_.faults_source;
            push_heap(p.addr, pd.next_use);
            return true;
        });
        // The requested addr's heap entry needs a real next-use.
        auto& dd = desc(addr);
        if (dd.resident) push_heap(addr, use_index_.next_use_after(addr, pos));
        return;
    }
    // Neither spilled nor sourced: leave absent — the kernel's
    // uninitialized-read handling (zeros + warning) applies, as without
    // a budget.
}

void ResidencyManager::make_room_for(size_t incoming_polys) {
    uint64_t target = (incoming_polys >= budget_polys_)
                          ? kMinBudgetPolys
                          : budget_polys_ - incoming_polys;
    evict_to(target);
}

void ResidencyManager::evict_to(uint64_t target_polys) {
    while (resident_polys_ > target_polys && !victims_.empty()) {
        auto [next, addr] = victims_.top();
        victims_.pop();
        auto it = descs_.find(addr);
        if (it == descs_.end()) continue;
        auto& d = it->second;
        // Lazy invalidation: stale priority, gone, or currently pinned
        // entries are skipped (pinned polys get a fresh entry at retire).
        if (!d.resident || d.next_use != next || is_pinned(addr)) continue;
        evict_one(addr, d);
    }
    if (resident_polys_ > target_polys && victims_.empty() &&
        !warned_unenforceable_) {
        warned_unenforceable_ = true;
        std::cerr << "[FHETCH_SIM] WARNING: memory budget unenforceable ("
                  << resident_polys_ << " resident polys, budget "
                  << budget_polys_ << ") — all candidates pinned" << std::endl;
    }
}

void ResidencyManager::evict_one(uint64_t addr, PolyDesc& d) {
    const auto& elem = memory_.get(addr);
    bool preservable_elsewhere =
        d.spill_slot != SpillFile::kNoSlot || (!d.dirty && d.source_valid);
    bool needed = d.next_use != UseIndex::kNever || d.live_out;

    if (needed && !preservable_elsewhere) {
        uint32_t slot = spill_.write(elem.values.data());
        if (slot == SpillFile::kNoSlot) {
            // Spill failed (disk full / unwritable): keep the poly
            // resident rather than lose data; the budget becomes soft.
            return;
        }
        d.spill_slot = slot;
        d.modulus = elem.modulus;
        ++stats_.evictions_spilled;
    } else if (needed) {
        if (d.spill_slot != SpillFile::kNoSlot) d.modulus = elem.modulus;
        ++stats_.evictions_clean;
    }
    if (stats_.peak_spill_bytes < spill_.peak_bytes())
        stats_.peak_spill_bytes = spill_.peak_bytes();

    if (!needed) {
        free_poly(addr, d);
        return;
    }
    memory_.erase(addr);
    d.resident = false;
    --resident_polys_;
}

void ResidencyManager::free_poly(uint64_t addr, PolyDesc& d) {
    if (d.resident) {
        memory_.erase(addr);
        d.resident = false;
        --resident_polys_;
    }
    if (d.spill_slot != SpillFile::kNoSlot) {
        spill_.release(d.spill_slot);
        d.spill_slot = SpillFile::kNoSlot;
    }
}

}  // namespace niobium::fhetch_sim
