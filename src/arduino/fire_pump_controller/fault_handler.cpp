// fault_handler.cpp — see fault_handler.h.

#include "fault_handler.h"

namespace {

// Survives a soft reset. .noinit is not cleared by the C runtime startup, so
// the value written just before NVIC_SystemReset() is still there afterwards.
// A magic word guards against reading uninitialised RAM after a cold boot.
constexpr uint32_t FAULT_MAGIC = 0x46504D50;   // "FPMP"

struct FaultRecord {
  uint32_t magic;
  uint32_t reason;
  uint32_t faultCount;
  uint32_t bootCount;
};

__attribute__((section(".noinit"))) FaultRecord g_record;

// Cached at boot, because consumeResetReason() clears the live record.
ResetReason g_bootReason = ResetReason::POWER_ON;
uint32_t    g_faultCount = 0;
uint32_t    g_bootCount  = 0;
bool        g_consumed   = false;

// Everything below runs with the system in an undefined state. No library
// calls, no printing, no peripheral access -- any of those could fault again
// inside the handler and turn a recoverable fault into a lockup.
inline void recordAndReset(ResetReason reason) {
  if (g_record.magic != FAULT_MAGIC) {
    // Cold boot that faulted before noteBoot() ran; start the counters here.
    g_record.magic = FAULT_MAGIC;
    g_record.faultCount = 0;
    g_record.bootCount = 0;
  }
  g_record.reason = static_cast<uint32_t>(reason);
  ++g_record.faultCount;

  // Reset now rather than spinning. The reset drives every pin
  // high-impedance, de-energising every relay: the starter releases and K3's
  // NC contact grounds the ignition. Waiting for the watchdog instead would
  // hold whatever the relays were doing for another ~5.6 seconds.
  NVIC_SystemReset();

  // Unreachable, but never fall through into whatever follows.
  for (;;) {
  }
}

}  // namespace

// These override the weak symbols in the core's vector table, including the
// cm_backtrace HardFault_Handler that would otherwise spin forever.
extern "C" {

void HardFault_Handler(void)  { recordAndReset(ResetReason::HARD_FAULT); }
void MemManage_Handler(void)  { recordAndReset(ResetReason::MEM_MANAGE); }
void BusFault_Handler(void)   { recordAndReset(ResetReason::BUS_FAULT); }
void UsageFault_Handler(void) { recordAndReset(ResetReason::USAGE_FAULT); }

}  // extern "C"

namespace {

// Reads the RA4M1's hardware reset-status flags and clears them.
//
// This is the authoritative source. The .noinit record below is only extra
// detail: measured on hardware, the Renesas startup zeroes SRAM, so the
// record does NOT survive and every boot would otherwise look like a
// power-on. These flags are in the system controller, not RAM, so they
// survive by construction.
ResetReason readHardwareResetReason() {
  ResetReason reason = ResetReason::POWER_ON;

  const uint16_t r1 = R_SYSTEM->RSTSR1;
  const uint8_t  r0 = R_SYSTEM->RSTSR0;

  if ((r1 & (1u << 0)) || (r1 & (1u << 1))) {
    // Independent or normal watchdog. The loop stopped being serviced --
    // a hang rather than a fault.
    reason = ResetReason::WATCHDOG;
  } else if (r1 & (1u << 2)) {
    // Software reset: the only thing that issues one is our fault handler.
    reason = ResetReason::HARD_FAULT;
  } else if (r0 & (1u << 0)) {
    reason = ResetReason::POWER_ON;
  }

  // Flags are sticky and "write 0 to clear", so the next boot is attributed
  // to what actually happens next rather than inheriting this one.
  R_SYSTEM->RSTSR1 = 0;
  R_SYSTEM->RSTSR0 = 0;
  return reason;
}

}  // namespace

ResetReason consumeResetReason() {
  if (g_consumed) {
    return g_bootReason;
  }
  g_consumed = true;

  const ResetReason hw = readHardwareResetReason();

  if (g_record.magic != FAULT_MAGIC) {
    // Cold boot: RAM was not preserved, so this is a genuine power-on.
    g_record.magic = FAULT_MAGIC;
    g_record.reason = static_cast<uint32_t>(ResetReason::POWER_ON);
    g_record.faultCount = 0;
    g_record.bootCount = 0;
    g_bootReason = hw;
  } else {
    // The RAM record survived, so use its more specific fault type; fall
    // back to the hardware flag when it says nothing useful.
    const uint32_t r = g_record.reason;
    const ResetReason detailed =
        (r <= static_cast<uint32_t>(ResetReason::DELIBERATE))
            ? static_cast<ResetReason>(r)
            : ResetReason::POWER_ON;
    g_bootReason = (detailed == ResetReason::POWER_ON) ? hw : detailed;
  }

  g_faultCount = g_record.faultCount;
  g_bootCount = g_record.bootCount;

  // Clear the reason so the NEXT boot is only attributed to a fault if one
  // actually happens. The counters are deliberately left running.
  g_record.reason = static_cast<uint32_t>(ResetReason::POWER_ON);
  return g_bootReason;
}

void noteBoot() {
  if (g_record.magic != FAULT_MAGIC) {
    g_record.magic = FAULT_MAGIC;
    g_record.faultCount = 0;
    g_record.bootCount = 0;
  }
  ++g_record.bootCount;
  g_bootCount = g_record.bootCount;
}

uint32_t faultCountSincePowerOn() { return g_faultCount; }
uint32_t bootCount() { return g_bootCount; }

void enableDivideByZeroTrap() {
  // Two separate things are needed here, and missing the second one silently
  // defeats the first.
  //
  // 1. DIV_0_TRP. Without it, SDIV/UDIV by zero returns 0 and execution
  //    carries on with silently wrong values.
  SCB->CCR |= SCB_CCR_DIV_0_TRP_Msk;

  // 2. Enable the configurable fault exceptions. Without this they are
  //    DISABLED and every UsageFault/BusFault/MemManage ESCALATES to
  //    HardFault -- which the core owns, via an assembly handler that prints
  //    a backtrace and then spins forever waiting for the watchdog.
  //
  //    Measured on hardware: with these disabled, a divide-by-zero took the
  //    full ~5.6 s watchdog period to recover, holding whatever the relays
  //    were driving for that entire time. Enabling them routes the fault to
  //    the handlers above, which reset in microseconds.
  SCB->SHCSR |= SCB_SHCSR_USGFAULTENA_Msk |
                SCB_SHCSR_BUSFAULTENA_Msk |
                SCB_SHCSR_MEMFAULTENA_Msk;

  __DSB();
  __ISB();
}

const char* toString(ResetReason r) {
  switch (r) {
    case ResetReason::POWER_ON:    return "POWER_ON";
    case ResetReason::HARD_FAULT:  return "HARD_FAULT";
    case ResetReason::MEM_MANAGE:  return "MEM_MANAGE";
    case ResetReason::BUS_FAULT:   return "BUS_FAULT";
    case ResetReason::USAGE_FAULT: return "USAGE_FAULT";
    case ResetReason::WATCHDOG:    return "WATCHDOG";
    case ResetReason::DELIBERATE:  return "DELIBERATE";
  }
  return "POWER_ON";
}
