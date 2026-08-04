#include "cutline/core/id.hpp"

#include <atomic>
#include <charconv>
#include <cstddef>
#include <format>
#include <system_error>

namespace cutline::core {
namespace {

std::atomic<unsigned long long> g_counter{0};

}  // namespace

std::string new_id(std::string_view prefix) {
  const unsigned long long n = g_counter.fetch_add(1, std::memory_order_relaxed) + 1;
  return std::format("{}_{}", prefix, n);
}

void reset_ids() noexcept { g_counter.store(0, std::memory_order_relaxed); }

void note_id(std::string_view id) noexcept {
  const std::size_t underscore = id.rfind('_');
  if (underscore == std::string_view::npos) return;
  const std::string_view digits = id.substr(underscore + 1);
  if (digits.empty()) return;

  unsigned long long n = 0;
  if (std::from_chars(digits.data(), digits.data() + digits.size(), n).ec != std::errc{}) {
    return;
  }

  // Compare-exchange rather than a read and a write: another thread may raise
  // it further between the two, and the counter must never go backwards.
  unsigned long long seen = g_counter.load(std::memory_order_relaxed);
  while (seen < n &&
         !g_counter.compare_exchange_weak(seen, n, std::memory_order_relaxed)) {
  }
}

}  // namespace cutline::core
