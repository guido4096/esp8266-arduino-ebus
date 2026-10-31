#include <catch2/catch_test_macros.hpp>
#include <array>
#include <cstdint>

#include "core/syn_timing.hpp"
#include "core/request.hpp"

namespace ebus_timing = ebus::detail::syn;

namespace {
ebus_timing::Edges synEdges(uint64_t start) {
  ebus_timing::Edges edges;
  // Generate falling edges from the actual LSB-first 8N1 waveform.
  std::array<int, 10> bits{0, 0, 1, 0, 1, 0, 1, 0, 1, 1};
  int previous = 1;
  for (size_t bit = 0; bit < bits.size(); ++bit) {
    if (previous == 1 && bits[bit] == 0)
      edges.push(start + (bit * 1000000 + 1200) / 2400);
    previous = bits[bit];
  }
  return edges;
}
}

TEST_CASE("SYN capture recognizes the real 2400 baud LSB-first waveform", "[timing]") {
  auto edges = synEdges(100000);
  REQUIRE(edges.count == 4);
  REQUIRE(edges.time[1] - edges.time[0] == 1250);
  REQUIRE(edges.time[2] - edges.time[1] == 833);
  REQUIRE(edges.time[3] - edges.time[2] == 834);
  const auto stamp = edges.stamp(103959);
  REQUIRE(stamp.valid);
  REQUIRE(stamp.start_us == 100000);
  REQUIRE(stamp.generation == edges.generation);
}

TEST_CASE("SYN capture rejects missing, malformed, unfinished and stale edges", "[timing]") {
  ebus_timing::Edges missing;
  missing.push(100000);
  missing.push(101250);
  missing.push(102083);
  REQUIRE_FALSE(missing.stamp(103959).valid);

  ebus_timing::Edges old_guess;
  for (uint64_t t : {100000, 100833, 101667, 102500}) old_guess.push(t);
  REQUIRE_FALSE(old_guess.stamp(103959).valid);

  auto edges = synEdges(100000);
  REQUIRE_FALSE(edges.stamp(102917).valid);
  REQUIRE_FALSE(edges.stamp(104425).valid);
  REQUIRE_FALSE(edges.stamp(99000).valid);
  edges.time[2] += 200;
  REQUIRE_FALSE(edges.stamp(103959).valid);
  edges.clear();
  REQUIRE_FALSE(edges.stamp(103959).valid);
}

TEST_CASE("Arbitration uses an absolute deadline and refuses late arming", "[timing]") {
  auto edges = synEdges(100000);
  auto syn = edges.stamp(103959);
  uint64_t deadline = 0;
  REQUIRE(ebus_timing::deadline(syn, 103959, 4400, deadline));
  REQUIRE(deadline == 104400);
  REQUIRE(ebus_timing::deadline(syn, 104300, 4400, deadline));
  REQUIRE(deadline == 104400);  // setup time never shifts it
  REQUIRE_FALSE(ebus_timing::deadline(syn, 104386, 4400, deadline));
  REQUIRE_FALSE(ebus_timing::deadline(syn, 104400, 4400, deadline));
  REQUIRE_FALSE(ebus_timing::deadline(syn, 104500, 4400, deadline));
  REQUIRE_FALSE(ebus_timing::deadline({}, 103959, 4400, deadline));
}

TEST_CASE("Late ISR, early ISR, intervening traffic and occupied UART never send", "[timing]") {
  auto edges = synEdges(100000);
  auto syn = edges.stamp(103959);
  constexpr uint64_t deadline = 104400;
  REQUIRE(ebus_timing::canFire(syn, deadline, 104400, edges.generation, true));
  REQUIRE(ebus_timing::canFire(syn, deadline, 104456, edges.generation, true));
  REQUIRE_FALSE(ebus_timing::canFire(syn, deadline, 104399, edges.generation, true));
  REQUIRE_FALSE(ebus_timing::canFire(syn, deadline, 104457, edges.generation, true));
  REQUIRE_FALSE(ebus_timing::canFire(syn, deadline, 120000, edges.generation, true));
  REQUIRE_FALSE(ebus_timing::canFire(syn, deadline, 104400, edges.generation, false));
  edges.push(104310);  // a contender started before us: yield this opportunity
  REQUIRE_FALSE(ebus_timing::canFire(syn, deadline, 104400, edges.generation, true));
}

TEST_CASE("Timing window validates the guarded protocol band", "[timing]") {
  REQUIRE(ebus_timing::windowValid(4400));
  REQUIRE(ebus_timing::windowValid(4332));
  REQUIRE(ebus_timing::windowValid(4424));
  REQUIRE_FALSE(ebus_timing::windowValid(4331));
  REQUIRE_FALSE(ebus_timing::windowValid(4425));
}

TEST_CASE("Capture and deadlines work past the 32-bit microsecond wrap", "[timing]") {
  constexpr uint64_t start = UINT64_C(0xffffffff) - 1000;
  auto edges = synEdges(start);
  auto syn = edges.stamp(start + 3959);
  uint64_t deadline = 0;
  REQUIRE(ebus_timing::deadline(syn, start + 3959, 4400, deadline));
  REQUIRE(deadline == start + 4400);
  REQUIRE(ebus_timing::canFire(syn, deadline, deadline, edges.generation, true));
}

TEST_CASE("Every admitted ISR write remains inside the guarded protocol window", "[timing]") {
  auto edges = synEdges(100000);
  const auto syn = edges.stamp(103959);
  for (uint32_t target = 4250; target <= 4500; ++target) {
    uint64_t deadline = 0;
    if (!ebus_timing::deadline(syn, 103959, target, deadline)) continue;
    for (uint64_t now = 104000; now <= 105000; ++now) {
      if (ebus_timing::canFire(syn, deadline, now, edges.generation, true)) {
        REQUIRE(now >= 104332);
        REQUIRE(now <= 104456);
      }
    }
  }
}

TEST_CASE("Cancelled requests cannot reuse an older arbitration generation", "[timing][request]") {
  ebus::detail::Request request;
  request.setLockCounter(0);
  request.reset();
  REQUIRE(request.requestBus(0x15));
  const auto first = request.busRequestGeneration();
  REQUIRE(request.busRequestPending());
  request.withdrawBusRequest();
  REQUIRE_FALSE(request.busRequestPending());
  REQUIRE(request.requestBus(0x15));  // same address: address checking is insufficient
  REQUIRE(request.busRequestGeneration() != first);
  const auto second = request.busRequestGeneration();
  request.reset();
  REQUIRE_FALSE(request.busRequestPending());
  REQUIRE(request.requestBus(0x15));
  REQUIRE(request.busRequestGeneration() != second);
}
