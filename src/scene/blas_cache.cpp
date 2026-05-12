#include "blas_cache.hpp"

namespace tracey
{
    BlasCache::Entry *BlasCache::lookup(const std::string &name, uint64_t contentHash)
    {
        auto it = m_entries.find(name);
        if (it == m_entries.end()) return nullptr;
        if (it->second.contentHash != contentHash) return nullptr;
        it->second.touched = true;
        return &it->second;
    }

    BlasCache::Entry *BlasCache::insert(const std::string &name, Entry entry)
    {
        entry.touched = true;
        // operator[] would default-construct then move-assign; using
        // insert_or_assign keeps the move single-step.
        auto [it, _] = m_entries.insert_or_assign(name, std::move(entry));
        return &it->second;
    }

    void BlasCache::markAllUntouched()
    {
        for (auto &[_, e] : m_entries) e.touched = false;
    }

    void BlasCache::evictUntouched()
    {
        for (auto it = m_entries.begin(); it != m_entries.end();)
        {
            if (!it->second.touched)
                it = m_entries.erase(it);
            else
                ++it;
        }
    }
}
