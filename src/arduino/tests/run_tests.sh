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
  "$here/test_extensions.cpp"
  "$here/test_valve_water.cpp"
  "$here/test_event_log.cpp"
  "$here/test_danger_override.cpp"
  "$here/test_tank_level.cpp"
  "$here/test_battery.cpp"
  "$fw/pump_controller.cpp"
  "$fw/http_protocol.cpp"
  "$fw/api_handler.cpp"
  "$fw/event_log.cpp"
  "$fw/tank_level.cpp"
  "$fw/battery.cpp"
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

# The suite is built and run across the full configuration matrix:
#   * both relay polarities, so RELAY_ACTIVE_LOW is proven to be the single
#     point of control rather than assumed to be;
#   * maintenance API both enabled and disabled, so the flag's effect on
#     endpoint reachability is proven in each direction;
#   * water interlock present and absent. This install runs WITHOUT a water
#     sensor, but the interlock tests guard `if (!WATER_INTERLOCK_REQUIRED)
#     return;` -- so building only the sensorless configuration would leave
#     22 tests reporting "ok" while asserting nothing. Both are built so the
#     sensor logic stays covered for anyone who fits one.
for polarity in true false; do
  for maint in 1 0; do
    for water in 0 1; do
    if [ "$polarity" = "true" ]; then p_name="active-low"; else p_name="active-high"; fi
    if [ "$maint" = "1" ]; then m_name="maint-on"; else m_name="maint-off"; fi
    if [ "$water" = "1" ]; then w_name="water-sensor"; else w_name="no-water-sensor"; fi
    name="$p_name-$m_name-$w_name"
    exe="$build/tests_$name"

    echo
    echo "=== building host tests ($name) ==="
    "$CXX" "${common[@]}" "-DRELAY_ACTIVE_LOW_OVERRIDE=$polarity" \
           "-DENABLE_MAINTENANCE_API=$maint" \
           "-DREQUIRE_WATER_INTERLOCK=$water" \
           "-DENABLE_TANK_LEVEL=1" -DENABLE_BATTERY_MONITOR=1 "${sources[@]}" -o "$exe"

    echo "=== running host tests ($name) ==="
    if ! "$exe" "$@"; then
      failed=1
    fi
    done
  done
done

# The tank sensor is DEFAULT OFF (no hardware fitted yet). The matrix above
# forces it on so its 12 tests actually assert instead of returning early --
# the same trap the water-interlock tests fell into. This extra build proves
# the compiled-out path still builds and reports itself as absent.
echo
echo "=== building host tests (tank-compiled-out) ==="
"$CXX" "${common[@]}" -DRELAY_ACTIVE_LOW_OVERRIDE=true \
       -DENABLE_MAINTENANCE_API=1 -DREQUIRE_WATER_INTERLOCK=0 \
       -DENABLE_TANK_LEVEL=0 "${sources[@]}" -o "$build/tests_tank-off"
echo "=== running host tests (tank-compiled-out) ==="
if ! "$build/tests_tank-off" "$@"; then
  failed=1
fi

echo
if [ "$failed" -ne 0 ]; then
  echo "HOST TEST SUITE FAILED"
  exit 1
fi
echo "HOST TEST SUITE PASSED (both polarities x maintenance on/off)"
