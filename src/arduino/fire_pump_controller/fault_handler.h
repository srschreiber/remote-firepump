// fault_handler.h — CPU fault recovery.
//
// THE PROBLEM
//
// The Renesas core's default HardFault_Handler prints a backtrace and then
// spins forever:
//
//     Fault_Loop:
//         BL  Fault_Loop      /* while(1) */
//
// Recovery therefore depends entirely on the watchdog, ~5.6 s later. But a
// CPU fault does not change GPIO state: the output registers keep driving
// whatever they were driving. So a hard fault during CRANKING leaves the
// starter energised for the whole watchdog period ON TOP of however long it
// had already been cranking -- up to about 7.6 s, which breaks the "starter
// is never engaged for more than five seconds" guarantee that the rest of
// this firmware is built around.
//
// THE FIX
//
// Reset immediately instead of spinning. A reset drives every pin
// high-impedance, which de-energises every relay: the starter releases, and
// K3 de-energises so its NC contact grounds the ignition. Worst-case relay
// hold drops from ~5.6 s to microseconds.
//
// The handlers deliberately do almost nothing. After a fault the system is in
// an undefined state, so calling into libraries, printing, or touching
// peripherals risks faulting again inside the handler. NVIC_SystemReset() is
// a single register write.
//
// DIAGNOSIS
//
// Resetting that fast loses the backtrace, so a small record is stashed in a
// .noinit RAM section, which survives a soft reset, and logged on the way
// back up. You get both the fast shutdown and an explanation.

#pragma once

#include <Arduino.h>

#include "config.h"

// Why the last reset happened, as far as we can tell.
enum class ResetReason : uint8_t {
  POWER_ON = 0,     // no valid record: cold boot, or the record was lost
  HARD_FAULT,
  MEM_MANAGE,
  BUS_FAULT,
  USAGE_FAULT,      // includes divide-by-zero when the trap is enabled
  WATCHDOG,         // the main loop stopped being serviced
  DELIBERATE,       // a bench-console fault-injection command
};

const char* toString(ResetReason r);

// Reads and then clears the surviving record. Call once, early in setup().
// Returns POWER_ON when there was nothing to read.
ResetReason consumeResetReason();

// Faults recorded since the last power-on. Survives soft resets, so a boot
// loop is visible rather than looking like a series of unrelated restarts.
uint32_t faultCountSincePowerOn();

// Total boots since power-on, for the same reason.
uint32_t bootCount();

// Records this boot. Call once in setup(), after consumeResetReason().
void noteBoot();

// Enables the Cortex-M divide-by-zero trap.
//
// By default DIV_0_TRP is clear, so an integer divide by zero silently
// returns 0 and the program carries on computing with wrong numbers. For a
// controller that decides how long to energise a starter, a detectable fault
// that resets into a safe state is strictly better than quietly wrong
// arithmetic.
void enableDivideByZeroTrap();
