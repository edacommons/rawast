#include <rawast/profile.hpp>

#include <algorithm>

namespace rawast {

namespace {

std::vector<ProfileEntry>
sorted_truncated(const std::vector<ProfileEntry>& src,
                  std::size_t n,
                  bool by_time) {
    std::vector<ProfileEntry> copy = src;
    std::sort(copy.begin(), copy.end(),
        [by_time](const ProfileEntry& a, const ProfileEntry& b) {
            if (by_time) return a.total_ns > b.total_ns;
            return a.entry_count > b.entry_count;
        });
    if (n > 0 && n < copy.size()) copy.resize(n);
    return copy;
}

} // namespace

std::vector<ProfileEntry>
ProfileReport::top_by_time(std::size_t n) const {
    return sorted_truncated(entries, n, /*by_time=*/true);
}

std::vector<ProfileEntry>
ProfileReport::top_by_count(std::size_t n) const {
    return sorted_truncated(entries, n, /*by_time=*/false);
}

} // namespace rawast
