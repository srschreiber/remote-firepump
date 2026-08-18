// tests/test_framework.h — a deliberately tiny assertion + registration layer.
//
// No external dependencies: the whole suite is one g++ invocation over a
// handful of .cpp files, so it runs anywhere the firmware can be built.

#pragma once

#include <stdio.h>
#include <string.h>

#include <string>
#include <vector>

struct TestCase {
  const char* name;
  void (*fn)();
};

std::vector<TestCase>& testRegistry();

struct TestRegistrar {
  TestRegistrar(const char* name, void (*fn)()) {
    testRegistry().push_back(TestCase{name, fn});
  }
};

#define TEST(name)                                        \
  static void name();                                     \
  static TestRegistrar registrar_##name(#name, name);     \
  static void name()

// Per-run counters, reset by the runner.
extern int g_checksRun;
extern int g_checksFailed;
extern const char* g_currentTest;

void reportFailure(const char* file, int line, const char* expr,
                   const std::string& detail);

#define CHECK(expr)                                                   \
  do {                                                                \
    ++g_checksRun;                                                    \
    if (!(expr)) {                                                    \
      reportFailure(__FILE__, __LINE__, #expr, std::string());        \
    }                                                                 \
  } while (0)

#define CHECK_MSG(expr, msg)                                          \
  do {                                                                \
    ++g_checksRun;                                                    \
    if (!(expr)) {                                                    \
      reportFailure(__FILE__, __LINE__, #expr, std::string(msg));     \
    }                                                                 \
  } while (0)

#define CHECK_EQ(a, b)                                                \
  do {                                                                \
    ++g_checksRun;                                                    \
    const auto va_ = (a);                                             \
    const auto vb_ = (b);                                             \
    if (!(va_ == vb_)) {                                              \
      char buf_[192];                                                 \
      snprintf(buf_, sizeof(buf_), "got %lld, want %lld",             \
               static_cast<long long>(va_),                           \
               static_cast<long long>(vb_));                          \
      reportFailure(__FILE__, __LINE__, #a " == " #b, buf_);          \
    }                                                                 \
  } while (0)

#define CHECK_STREQ(a, b)                                             \
  do {                                                                \
    ++g_checksRun;                                                    \
    const char* sa_ = (a);                                            \
    const char* sb_ = (b);                                            \
    if (sa_ == nullptr || sb_ == nullptr || strcmp(sa_, sb_) != 0) {   \
      char buf_[384];                                                 \
      snprintf(buf_, sizeof(buf_), "got \"%s\", want \"%s\"",         \
               sa_ ? sa_ : "(null)", sb_ ? sb_ : "(null)");           \
      reportFailure(__FILE__, __LINE__, #a " == " #b, buf_);          \
    }                                                                 \
  } while (0)

// Substring assertion, used heavily on generated JSON.
#define CHECK_CONTAINS(haystack, needle)                              \
  do {                                                                \
    ++g_checksRun;                                                    \
    const std::string h_((haystack));                                 \
    const std::string n_((needle));                                   \
    if (h_.find(n_) == std::string::npos) {                           \
      reportFailure(__FILE__, __LINE__, "contains(" #needle ")",      \
                    "in: " + h_);                                     \
    }                                                                 \
  } while (0)

#define CHECK_NOT_CONTAINS(haystack, needle)                          \
  do {                                                                \
    ++g_checksRun;                                                    \
    const std::string h_((haystack));                                 \
    const std::string n_((needle));                                   \
    if (h_.find(n_) != std::string::npos) {                           \
      reportFailure(__FILE__, __LINE__, "!contains(" #needle ")",     \
                    "in: " + h_);                                     \
    }                                                                 \
  } while (0)
