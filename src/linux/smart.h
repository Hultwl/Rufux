#ifndef RUFUX_SMART_H
#define RUFUX_SMART_H
// Drive health check through smartctl (smartmontools), run before a write.
// Most USB sticks and SD cards report no SMART data at all; that is normal
// and never blocks a write. Only a drive that positively reports a failing
// overall status does.
typedef enum {
  RUFUX_SMART_NA = 0,   // no smartctl, not a block device, or the drive reports nothing
  RUFUX_SMART_PASSED,   // overall health self-assessment passed
  RUFUX_SMART_WARN,     // passed, but reallocated/pending/uncorrectable sectors are non-zero
  RUFUX_SMART_FAILED    // the drive says it is failing
} RufuxSmartState;

// Fills `msg` with a one-line explanation suitable for the log.
RufuxSmartState rufux_smart_check(const char *dev, char *msg, unsigned long cap);
#endif
