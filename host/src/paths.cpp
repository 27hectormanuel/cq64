// SPDX-License-Identifier: CC0-1.0
#include "paths.h"
#include "common.h"

#include <mutex>
#include <vector>

namespace paths {
namespace {
std::mutex g_lock;
std::vector<std::pair<std::string, std::string>> g_map;
}  // namespace

void add_mapping(const std::string &from, const std::string &to) {
    std::lock_guard<std::mutex> lk(g_lock);
    g_map.emplace_back(from, to);
    LOGI("path mapping %s -> %s", from.c_str(), to.c_str());
}

std::string translate(const char *p) {
    if (!p) return {};
    std::string s = p;
    std::lock_guard<std::mutex> lk(g_lock);
    for (auto &m : g_map) {
        if (s.compare(0, m.first.size(), m.first) == 0) return m.second + s.substr(m.first.size());
    }
    return s;
}
}  // namespace paths
