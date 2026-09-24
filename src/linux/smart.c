#define _GNU_SOURCE
#include "smart.h"
#include "exec.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

// smartctl exits with a bit mask, so "non-zero" does not mean "no output":
// the JSON is parsed whatever the status was.
#define OUT_CAP 65536

static int has_verdict(const char *j) {
  return strstr(j, "\"smart_status\"") != NULL;
}

// "passed": false / true inside the smart_status object; -1 when absent.
static int passed_flag(const char *j) {
  const char *s = strstr(j, "\"smart_status\"");
  if (!s) return -1;
  const char *p = strstr(s, "\"passed\"");
  if (!p) return -1;
  p = strchr(p, ':');
  if (!p) return -1;
  p++;
  while (*p == ' ') p++;
  if (!strncmp(p, "true", 4)) return 1;
  if (!strncmp(p, "false", 5)) return 0;
  return -1;
}

// Raw value of ATA attribute `id` ("id": N ... "raw": { "value": V), or -1.
static long long ata_raw(const char *j, int id) {
  char key[32];
  snprintf(key, sizeof key, "\"id\": %d,", id);
  const char *p = strstr(j, key);
  if (!p) { snprintf(key, sizeof key, "\"id\":%d,", id); p = strstr(j, key); }
  if (!p) return -1;
  const char *end = strstr(p + 1, "\"id\":");  // stay inside this attribute
  const char *raw = strstr(p, "\"raw\"");
  if (!raw || (end && raw > end)) return -1;
  const char *v = strstr(raw, "\"value\"");
  if (!v || (end && v > end)) return -1;
  v = strchr(v, ':');
  return v ? strtoll(v + 1, NULL, 10) : -1;
}

RufuxSmartState rufux_smart_check(const char *dev, char *msg, unsigned long cap) {
  msg[0] = 0;
  struct stat st;
  if (stat(dev, &st) != 0 || !S_ISBLK(st.st_mode)) {
    snprintf(msg, cap, "SMART: skipped (not a block device)");
    return RUFUX_SMART_NA;
  }
  if (!rufux_have("smartctl")) {
    snprintf(msg, cap, "SMART: skipped (smartctl is not installed)");
    return RUFUX_SMART_NA;
  }
  char *out = malloc(OUT_CAP);
  if (!out) { snprintf(msg, cap, "SMART: skipped (out of memory)"); return RUFUX_SMART_NA; }
  // Plain autodetect first; USB-to-SATA bridges usually need -d sat.
  const char *a1[] = {"smartctl", "-H", "-A", "-j", dev, NULL};
  const char *a2[] = {"smartctl", "-H", "-A", "-j", "-d", "sat", dev, NULL};
  rufux_capture(a1, out, OUT_CAP);
  if (!has_verdict(out)) rufux_capture(a2, out, OUT_CAP);
  if (!has_verdict(out)) {
    snprintf(msg, cap, "SMART: the drive does not report health data (normal for USB sticks and SD cards)");
    free(out);
    return RUFUX_SMART_NA;
  }
  int pf = passed_flag(out);
  RufuxSmartState r;
  if (pf == 0) {
    snprintf(msg, cap, "SMART: the drive reports that it is FAILING; its data is not safe on it");
    r = RUFUX_SMART_FAILED;
  } else if (pf == 1) {
    long long realloc_ = ata_raw(out, 5), pending = ata_raw(out, 197), uncorr = ata_raw(out, 198);
    if (realloc_ > 0 || pending > 0 || uncorr > 0) {
      snprintf(msg, cap,
               "SMART: overall status passed, but the drive has bad sectors "
               "(reallocated %lld, pending %lld, uncorrectable %lld)",
               realloc_ < 0 ? 0 : realloc_, pending < 0 ? 0 : pending, uncorr < 0 ? 0 : uncorr);
      r = RUFUX_SMART_WARN;
    } else {
      snprintf(msg, cap, "SMART: overall health self-assessment passed");
      r = RUFUX_SMART_PASSED;
    }
  } else {
    snprintf(msg, cap, "SMART: the drive returned no usable verdict");
    r = RUFUX_SMART_NA;
  }
  free(out);
  return r;
}
