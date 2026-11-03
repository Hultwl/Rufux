#ifndef RUFUX_SMART_H
#define RUFUX_SMART_H
// Drive health check through smartctl (smartmontools), run before a write.
typedef enum {
  RUFUX_SMART_NA = 0,   // no smartctl, not a disk device, or the drive reports nothing
  RUFUX_SMART_PASSED,   // overall health self-assessment passed
  RUFUX_SMART_WARN,     // passed, but reallocated/pending/uncorrectable sectors are non-zero
  RUFUX_SMART_FAILED    // the drive says it is failing
} RufuxSmartState;

RufuxSmartState rufux_smart_check(const char *dev, char *msg, unsigned long cap);
#endif
