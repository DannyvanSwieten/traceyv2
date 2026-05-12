#include "cook_cache.hpp"

namespace tracey
{
    namespace sops
    {
        CookCache::Entry *CookCache::find(size_t uid)
        {
            auto it = m_entries.find(uid);
            if (it == m_entries.end()) return nullptr;
            it->second.touched = true;
            return &it->second;
        }

        CookCache::Entry &CookCache::upsert(size_t uid)
        {
            auto &e = m_entries[uid];
            e.touched = true;
            return e;
        }

        void CookCache::markAllUntouched()
        {
            for (auto &[_, e] : m_entries) e.touched = false;
        }

        void CookCache::evictUntouched()
        {
            for (auto it = m_entries.begin(); it != m_entries.end();)
            {
                if (!it->second.touched) it = m_entries.erase(it);
                else ++it;
            }
        }
    }
}
