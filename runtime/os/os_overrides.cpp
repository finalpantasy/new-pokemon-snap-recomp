// The nn::os override table (written for this project): mangled symbol -> host function, TotK HOST_OVERRIDE
// convention (arguments in guest x0..x7, result in x0). The loader binds each entry over the lifted function of the
// same symbol in the sdk module; sdk reaches its own exports through its PLT, so internal callers are covered too.
#include <cstring>
#include <mutex>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "os_internal.h"

namespace osk {
extern const OsOverride g_thread_overrides[];
extern const size_t g_thread_override_count;
extern const OsOverride g_sync_overrides[];
extern const size_t g_sync_override_count;
extern const OsOverride g_memory_overrides[];
extern const size_t g_memory_override_count;
}  // namespace osk

namespace {
struct Table {
    std::vector<OsOverride> all;
    std::unordered_map<std::string_view, GuestFn> by_name;
    Table() {
        all.insert(all.end(), osk::g_thread_overrides, osk::g_thread_overrides + osk::g_thread_override_count);
        all.insert(all.end(), osk::g_sync_overrides, osk::g_sync_overrides + osk::g_sync_override_count);
        all.insert(all.end(), osk::g_memory_overrides, osk::g_memory_overrides + osk::g_memory_override_count);
        for (const OsOverride &o : all)
            if (!by_name.emplace(o.sym, o.fn).second) osk::fatal("duplicate nn::os override %s", o.sym);
    }
};
const Table &table() {
    static Table t;
    return t;
}
}  // namespace

extern "C" const OsOverride *os_override_table(size_t *count) {
    if (count) *count = table().all.size();
    return table().all.data();
}

extern "C" GuestFn os_override_find(const char *sym) {
    if (!sym) return nullptr;
    auto it = table().by_name.find(sym);
    return it == table().by_name.end() ? nullptr : it->second;
}
