#!/usr/bin/env bash
# run_tests.sh — build and run the host unit test suite (Linux/macOS/WSL).
#
#   ./src/arduino/tests/run_tests.sh [name-filter]
#
# Set CXX to choose a compiler. Requires only a C++17 toolchain; no Arduino
# hardware, no board package, no network.

set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
fw="$(dirname "$here")/fire_pump_controller"
build="$here/build"

CXX="${CXX:-g++}"
mkdir -p "$build"

sources=(
  "$here/shim/Arduino.cpp"
  "$here/test_main.cpp"
  "$here/test_support.cpp"
  "$here/test_pump_controller.cpp"
  "$here/test_http_protocol.cpp"
  "$here/test_api_handler.cpp"
  "$here/test_invariants.cpp"
  "$fw/pump_controller.cpp"
  "$fw/http_protocol.cpp"
  "$fw/api_handler.cpp"
)

# NOTE: -Wuseless-cast is deliberately NOT enabled. The rollover-safe idiom
# `static_cast<uint32_t>(now - startedAt) >= duration` is required by the
# design and documents intent; GCC flags the cast as redundant.
common=(
  -std=c++17 -O1 -g
  -Wall -Wextra -Wshadow -Wconversion -Wsign-conversion
  -Wold-style-cast -Wdouble-promotion -Wformat=2
  -Werror
  "-I$here/shim"
  -DPUMP_CONTROLLER_TEST_ACCESS
)

failed=0

# The suite is built and run twice, once at each relay polarity, so the
# active-low abstraction is proven to be the single point of control.
for polarity in true false; do
  if [ "$polarity" = "true" ]; then name="active-low"; else name="active-high"; fi
  exe="$build/tests_$name"

  echo
  echo "=== building host tests ($name) ==="
  "$CXX" "${common[@]}" "-DRELAY_ACTIVE_LOW_OVERRIDE=$polarity" "${sources[@]}" -o "$exe"

  echo "=== running host tests ($name) ==="
  if ! "$exe" "$@"; then
    failed=1
  fi
done

echo
if [ "$failed" -ne 0 ]; then
  echo "HOST TEST SUITE FAILED"
  exit 1
fi
echo "HOST TEST SUITE PASSED (both relay polarities)"
