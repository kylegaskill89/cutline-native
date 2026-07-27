#include "cutline/core/id.hpp"

#include <atomic>
#include <format>

namespace cutline::core {
namespace {

std::atomic<unsigned long long> g_counter{0};

}  // namespace

std::string new_id(std::string_view prefix) {
  const unsigned long long n = g_counter.fetch_add(1, std::memory_order_relaxed) + 1;
  return std::format("{}_{}", prefix, n);
}

void reset_ids() noexcept { g_counter.store(0, std::memory_order_relaxed); }

}  // namespace cutline::core
