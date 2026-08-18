// tests/test_main.cpp — runner.

#include "test_framework.h"

#include <stdio.h>
#include <string.h>

#include "shim/Arduino.h"

#include "../fire_pump_controller/config.h"

int g_checksRun = 0;
int g_checksFailed = 0;
const char* g_currentTest = "";

static int g_failuresInCurrentTest = 0;

std::vector<TestCase>& testRegistry() {
  static std::vector<TestCase> registry;
  return registry;
}

// A single broken invariant inside a fuzz loop can fire tens of thousands of
// times. Print the first few per test, then keep counting silently.
static constexpr int kMaxReportedPerTest = 5;

void reportFailure(const char* file, int line, const char* expr,
                   const std::string& detail) {
  ++g_checksFailed;
  ++g_failuresInCurrentTest;

  if (g_failuresInCurrentTest > kMaxReportedPerTest) {
    if (g_failuresInCurrentTest == kMaxReportedPerTest + 1) {
      printf("    ... further failures in this test suppressed\n");
    }
    return;
  }

  // Trim to the basename so output stays readable.
  const char* base = strrchr(file, '/');
  const char* base2 = strrchr(file, '\\');
  if (base2 != nullptr && (base == nullptr || base2 > base)) base = base2;
  base = (base != nullptr) ? base + 1 : file;

  printf("    FAIL %s:%d  %s", base, line, expr);
  if (!detail.empty()) {
    printf("   [%s]", detail.c_str());
  }
  printf("\n");
}

int main(int argc, char** argv) {
  const char* filter = (argc > 1) ? argv[1] : nullptr;

  printf("fire-pump-controller host unit tests\n");
  printf("relay polarity under test: active-%s\n",
         RELAY_ACTIVE_LOW ? "LOW" : "HIGH");
  printf("-----------------------------------------------------------\n");

  int passed = 0;
  int failed = 0;

  for (const TestCase& t : testRegistry()) {
    if (filter != nullptr && strstr(t.name, filter) == nullptr) {
      continue;
    }
    g_currentTest = t.name;
    g_failuresInCurrentTest = 0;

    fake::reset();
    t.fn();

    if (g_failuresInCurrentTest == 0) {
      printf("  ok   %s\n", t.name);
      ++passed;
    } else {
      printf("  FAIL %s (%d failed checks)\n", t.name, g_failuresInCurrentTest);
      ++failed;
    }
  }

  printf("-----------------------------------------------------------\n");
  printf("tests: %d passed, %d failed | assertions: %d run, %d failed\n",
         passed, failed, g_checksRun, g_checksFailed);

  return (failed == 0 && g_checksFailed == 0) ? 0 : 1;
}
