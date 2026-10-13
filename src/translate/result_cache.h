#pragma once
#include "translate/engine.h"
#include <list>
#include <mutex>

namespace lumashot::translate {
// Exact, whole-job matches preserve paragraph context. Memory only, bounded,
// and used exclusively for the bundled, version-pinned offline models.
class ResultCache {
public:
    static constexpr size_t MaxBytes = 256 * 1024;
    static constexpr size_t MaxEntries = 32;
    std::optional<Outcome> Find(std::string_view model, const Job& job) {
        std::lock_guard guard(lock_);
        for (auto it = entries_.begin(); it != entries_.end(); ++it) {
            if (it->model == model && it->job.source == job.source && it->job.target == job.target && it->job.texts == job.texts) {
                entries_.splice(entries_.begin(), entries_, it);
                return entries_.front().outcome;
            }
        }
        return std::nullopt;
    }
    void Store(std::string_view model, const Job& job, const Outcome& outcome) {
        if (!outcome.ok || outcome.texts.size() != job.texts.size()) return;
        if (job.texts.size() > MaxBytes / (2 * sizeof(std::wstring)) || outcome.error.size() > MaxBytes / sizeof(wchar_t)) return;
        // Reject oversized input before copying it. Capacity is checked again
        // below to include allocator slack and the vectors/string objects.
        size_t chars = model.size() + outcome.detected.size();
        if (chars > MaxBytes) return;
        for (const auto* texts : {&job.texts, &outcome.texts}) for (const auto& text : *texts) {
            if (text.size() > (MaxBytes - chars) / sizeof(wchar_t)) return;
            chars += text.size() * sizeof(wchar_t);
        }
        Entry entry{std::string(model), job, outcome, 0};
        entry.bytes = sizeof(Entry) + 2 * sizeof(void*) + entry.model.capacity() + 1 + entry.outcome.detected.capacity() + 1
            + (entry.outcome.error.capacity() + 1) * sizeof(wchar_t);
        for (const auto* texts : {&entry.job.texts, &entry.outcome.texts}) {
            entry.bytes += texts->capacity() * sizeof(std::wstring);
            for (const auto& text : *texts) entry.bytes += (text.capacity() + 1) * sizeof(wchar_t);
        }
        if (entry.bytes > MaxBytes) return;
        std::lock_guard guard(lock_);
        for (auto it = entries_.begin(); it != entries_.end();) {
            if (it->model == model && it->job.source == job.source && it->job.target == job.target && it->job.texts == job.texts) {
                bytes_ -= it->bytes; it = entries_.erase(it);
            } else ++it;
        }
        while (!entries_.empty() && (bytes_ + entry.bytes > MaxBytes || entries_.size() >= MaxEntries)) {
            bytes_ -= entries_.back().bytes; entries_.pop_back();
        }
        const auto bytes = entry.bytes;
        entries_.push_front(std::move(entry));
        bytes_ += bytes;
    }
private:
    struct Entry { std::string model; Job job; Outcome outcome; size_t bytes; };
    std::mutex lock_;
    std::list<Entry> entries_;
    size_t bytes_{};
};
}
