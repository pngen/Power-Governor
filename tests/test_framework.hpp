// Power Governor - lightweight, deterministic test framework.
//
// Tests are registered with TEST(name) and run through pgtest::run_all. There is no test-timeout
// mechanism of any kind: completeness is enforced by correctness, not by a clock. Random/property
// tests use a fixed, printed seed so any failure is reproducible. This single-header harness has no
// external dependencies.
#pragma once
#include <cstdio>
#include <cstdint>
#include <functional>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

namespace pgtest {

struct TestCase { std::string name; std::function<void()> fn; };

inline std::vector<TestCase>& registry() {
  static std::vector<TestCase> r;
  return r;
}

struct Registrar {
  Registrar(const char* name, std::function<void()> f) { registry().push_back(TestCase{name, std::move(f)}); }
};

// Fixed, printed seed for all property/randomized tests so results are reproducible.
inline std::uint32_t test_seed() { return 0xC0FFEEu; }

inline void fail(const std::string& msg) { throw std::runtime_error(msg); }

#define TEST(name)                                                    \
  static void pgtest_##name();                                        \
  static ::pgtest::Registrar pgtest_reg_##name(#name, &pgtest_##name); \
  static void pgtest_##name()

#define CHECK(cond)                                                                      \
  do {                                                                                   \
    if (!(cond)) ::pgtest::fail(std::string("CHECK failed: ") + #cond + " at " + __FILE__ + \
                                ":" + std::to_string(__LINE__));                         \
  } while (0)

#define CHECK_EQ(a, b)                                                                    \
  do {                                                                                    \
    auto _a_ = (a); auto _b_ = (b);                                                       \
    if (!(_a_ == _b_)) ::pgtest::fail(std::string("CHECK_EQ failed: ") + #a + " == " + #b + \
                                      " at " + __FILE__ + ":" + std::to_string(__LINE__)); \
  } while (0)

#define CHECK_NEAR(a, b, eps)                                                                \
  do {                                                                                       \
    auto _a_ = (a); auto _b_ = (b);                                                          \
    if (!((_a_ >= (_b_) - (eps)) && (_a_ <= (_b_) + (eps))))                                 \
      ::pgtest::fail(std::string("CHECK_NEAR failed: ") + #a + " ~ " + #b + " at " +         \
                     __FILE__ + ":" + std::to_string(__LINE__));                            \
  } while (0)

#define CHECK_THROWS(expr)                                            \
  do {                                                                \
    bool _threw_ = false;                                             \
    try { (void)(expr); } catch (...) { _threw_ = true; }             \
    if (!_threw_) ::pgtest::fail(std::string("expected exception: ") + #expr); \
  } while (0)

// Deterministic per-test RNG seeded from the fixed seed.
inline std::mt19937 make_rng(std::uint64_t salt) {
  return std::mt19937(test_seed() + static_cast<std::uint32_t>(salt));
}

inline int run_all(int argc, char** argv, const char* filter = nullptr) {
  (void)argc; (void)argv;
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  std::printf("Power Governor test runner. fixed seed = 0x%08X\n", test_seed());
  int passed = 0, failed = 0;
  for (const auto& tc : registry()) {
    if (filter && tc.name.find(filter) == std::string::npos) continue;
    std::printf("[run] %s\n", tc.name.c_str());
    try {
      tc.fn();
      ++passed;
      std::printf("[ ok ] %s\n", tc.name.c_str());
    } catch (const std::exception& e) {
      ++failed;
      std::printf("[FAIL] %s : %s\n", tc.name.c_str(), e.what());
    } catch (...) {
      ++failed;
      std::printf("[FAIL] %s : unknown exception\n", tc.name.c_str());
    }
  }
  std::printf("passed=%d failed=%d total=%zu\n", passed, failed, registry().size());
  return failed == 0 ? 0 : 1;
}

}  // namespace pgtest
