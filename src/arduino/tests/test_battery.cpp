// tests/test_battery.cpp — battery voltage monitor.
//
// The measurement is a divider and a multiply. What needs proving is that the
// three published numbers mean what they claim, because each is a different
// question and confusing them gives confident wrong answers:
//
//   * a resting voltage published while charging or just after a crank is
//     simply wrong, and looks entirely plausible;
//   * a crank minimum that has been smoothed is not a minimum;
//   * none of it may gate the engine.

#include "test_support.h"

#include "../fire_pump_controller/battery.h"

namespace {

int countsForVolts(float battVolts) {
  const float atPin = battVolts / BATTERY_DIVIDER_RATIO;
  return static_cast<int>(
      (atPin * 1000.0f / BATTERY_ADC_REF_MV) * BATTERY_ADC_MAX_COUNTS + 0.5f);
}

void setBatteryVolts(float v) {
  fake::analogCounts[PIN_BATTERY_VOLTS] = countsForVolts(v);
}

// Runs the monitor for `ms`, holding the given state.
void run(BatteryMonitor& b, uint32_t ms, PumpState state, bool relaysActive,
         uint32_t step = 100) {
  for (uint32_t done = 0; done < ms; done += step) {
    fake::nowMs += step;
    b.tick(fake::nowMs, state, relaysActive);
  }
}

BatteryMonitor& fresh() {
  fake::reset();
  fake::nowMs = 1000;
  static BatteryMonitor b;
  b = BatteryMonitor();
  b.begin(fake::nowMs);
  return b;
}

}  // namespace

TEST(battery_converts_divider_reading_to_battery_volts) {
  if (!BATTERY_MONITOR_ENABLED) return;

  for (float v : {11.5f, 12.2f, 12.7f, 14.4f}) {
    BatteryMonitor& b = fresh();
    setBatteryVolts(v);
    run(b, 3000, PumpState::IDLE, false);

    const float err = b.volts() - v;
    CHECK_MSG(err > -0.15f && err < 0.15f,
              "voltage conversion is off by more than 0.15 V");
  }
}

TEST(resting_voltage_is_not_published_until_things_settle) {
  if (!BATTERY_MONITOR_ENABLED) return;

  BatteryMonitor& b = fresh();
  setBatteryVolts(12.6f);

  // Quiet, but not for long enough yet.
  run(b, BATTERY_REST_SETTLE_MS / 2, PumpState::IDLE, false, 1000);
  CHECK_MSG(!b.restingValid(),
            "a resting voltage was published before the settle period");

  run(b, BATTERY_REST_SETTLE_MS, PumpState::IDLE, false, 1000);
  CHECK_MSG(b.restingValid(), "resting voltage never became valid");
  const float err = b.restingVolts() - 12.6f;
  CHECK_MSG(err > -0.15f && err < 0.15f, "resting voltage is wrong");
}

TEST(activity_invalidates_the_resting_reading) {
  if (!BATTERY_MONITOR_ENABLED) return;

  // The dangerous case: settled, then something happens. Continuing to
  // publish the old resting figure would be reporting a number that predates
  // the event the operator is asking about.
  BatteryMonitor& b = fresh();
  setBatteryVolts(12.6f);
  run(b, BATTERY_REST_SETTLE_MS + 2000, PumpState::IDLE, false, 1000);
  CHECK(b.restingValid());

  run(b, 1000, PumpState::CRANKING, true, 100);
  CHECK_MSG(!b.restingValid(),
            "a resting voltage survived a crank");

  // And it must not come back the instant things go quiet again.
  run(b, 5000, PumpState::IDLE, false, 1000);
  CHECK_MSG(!b.restingValid(),
            "resting voltage returned without waiting for the settle period");
}

TEST(a_charging_battery_does_not_report_a_resting_voltage) {
  if (!BATTERY_MONITOR_ENABLED) return;

  // RUNNING_ASSUMED means the engine is turning and the charging coil may be
  // pushing 14.4 V. Publishing that as "resting" would read as a
  // fully-charged battery when it says nothing about charge at all.
  BatteryMonitor& b = fresh();
  setBatteryVolts(14.4f);
  run(b, BATTERY_REST_SETTLE_MS + 2000, PumpState::RUNNING_ASSUMED, true, 1000);

  CHECK_MSG(!b.restingValid(),
            "a charging voltage was published as a resting voltage");
  CHECK_MSG(!b.restingLow(), "restingLow was asserted on an invalid reading");
}

TEST(crank_minimum_captures_the_sag_not_the_average) {
  if (!BATTERY_MONITOR_ENABLED) return;

  BatteryMonitor& b = fresh();
  setBatteryVolts(12.6f);
  run(b, 3000, PumpState::IDLE, false);

  // Crank: a brief, deep dip inside an otherwise healthy reading. Smoothing
  // this would hide exactly the battery weakness worth catching.
  fake::nowMs += 100;
  b.tick(fake::nowMs, PumpState::CRANKING, true);
  setBatteryVolts(9.2f);
  fake::nowMs += 40;
  b.tick(fake::nowMs, PumpState::CRANKING, true);
  setBatteryVolts(12.4f);
  run(b, 500, PumpState::CRANKING, true, 20);

  // Leaving CRANKING latches the minimum.
  fake::nowMs += 100;
  b.tick(fake::nowMs, PumpState::UNCHOKING, true);

  const float m = b.lastCrankMinVolts();
  CHECK_MSG(m < 9.6f, "the crank sag was smoothed away");
  CHECK_MSG(m > 8.8f, "the crank minimum is implausibly low");
}

TEST(low_battery_is_only_reported_from_a_settled_reading) {
  if (!BATTERY_MONITOR_ENABLED) return;

  BatteryMonitor& b = fresh();
  setBatteryVolts(11.8f);   // genuinely low

  run(b, 5000, PumpState::IDLE, false, 1000);
  CHECK_MSG(!b.restingLow(),
            "a low-battery warning fired before the reading settled");

  run(b, BATTERY_REST_SETTLE_MS, PumpState::IDLE, false, 1000);
  CHECK_MSG(b.restingLow(), "a genuinely low battery was not reported");
}

TEST(the_battery_monitor_gates_nothing) {
  if (!BATTERY_MONITOR_ENABLED) return;

  // A flat battery must not refuse a start. If there is a fire you crank
  // anyway and find out.
  for (float v : {0.0f, 9.0f, 11.0f, 12.8f}) {
    PumpController p;
    bootAt(p, 1000);
    CHECK(driveToState(p, PumpState::IDLE));

    setBatteryVolts(v);
    battery().begin(fake::nowMs);
    run(battery(), 5000, PumpState::IDLE, false, 1000);

    const CommandResult r =
        p.handleCommand(CommandType::START, nullptr, fake::nowMs);
    CHECK_MSG(r.accepted,
              "the battery monitor blocked a start; it must gate nothing");
  }
}
