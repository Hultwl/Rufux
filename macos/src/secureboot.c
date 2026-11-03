#include "secureboot.h"
#include "exec.h"
#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const char *rufux_sb_string(RufuxSbState s) {
  switch (s) {
    case RUFUX_SB_ENABLED: return "enabled";
    case RUFUX_SB_DISABLED: return "disabled";
    default: return "unknown";
  }
}

static RufuxSbState parse_ioreg_sb(const char *text) {
  static const char *const keys[] = {
    "\"apple-secure-boot\"",
    "\"secure-boot\"",
    "\"AppleSecureBootPolicy\"",
    "\"SecureBootModel\"",
    NULL
  };
  for (int k = 0; keys[k]; k++) {
    const char *p = strstr(text, keys[k]);
    if (!p) continue;
    const char *eq = strchr(p, '=');
    if (!eq) continue;
    const char *nl = strchr(eq, '\n');
    size_t len = nl ? (size_t)(nl - eq) : strlen(eq);
    char val[256] = {0};
    if (len >= sizeof val) len = sizeof val - 1;
    memcpy(val, eq, len);
    val[len] = 0;

    if (strstr(val, "Full") || strstr(val, "Medium") || strstr(val, "j")) {
      return RUFUX_SB_ENABLED;
    }
    if (strstr(val, "None") || strstr(val, "Disabled") || strstr(val, "disabled")) {
      return RUFUX_SB_DISABLED;
    }

    // Parse IODeviceTree hex data blob, e.g. <01000000> or <00000000> or <02>
    const char *lt = strchr(val, '<');
    const char *gt = lt ? strchr(lt + 1, '>') : NULL;
    if (lt && gt && gt > lt + 1) {
      int any_hex = 0, non_zero = 0;
      for (const char *c = lt + 1; c < gt; c++) {
        if (isxdigit((unsigned char)*c)) {
          any_hex = 1;
          if (*c != '0') non_zero = 1;
        }
      }
      if (any_hex) return non_zero ? RUFUX_SB_ENABLED : RUFUX_SB_DISABLED;
    }
  }
  return RUFUX_SB_UNKNOWN;
}

RufuxSbState rufux_sb_state(void) {
  if (!rufux_have("ioreg")) return RUFUX_SB_UNKNOWN;

  // Fast query on IODeviceTree:/chosen first (Apple Silicon exposes "secure-boot" here).
  {
    char chosen[32768] = {0};
    const char *av_chosen[] = {"ioreg", "-w", "0", "-r", "-d", "1", "-p", "IODeviceTree", "-n", "chosen", NULL};
    if (rufux_capture(av_chosen, chosen, sizeof chosen) == 0) {
      RufuxSbState st = parse_ioreg_sb(chosen);
      if (st != RUFUX_SB_UNKNOWN) return st;
    }
  }

  // Full IODeviceTree fallback (covers "apple-secure-boot" / "SecureBootModel" in options/NVRAM).
  const unsigned long cap = 512UL * 1024UL;
  char *out = calloc(1, cap);
  if (!out) return RUFUX_SB_UNKNOWN;
  const char *av_tree[] = {"ioreg", "-w", "0", "-l", "-p", "IODeviceTree", NULL};
  RufuxSbState st = RUFUX_SB_UNKNOWN;
  if (rufux_capture(av_tree, out, cap) == 0) {
    st = parse_ioreg_sb(out);
  }
  free(out);
  return st;
}

int rufux_validate_efi(const char *path, unsigned *subsystem_out,
                       char *err, unsigned long cap) {
  FILE *f = fopen(path, "rb");
  if (!f) { snprintf(err, cap, "cannot open '%s'", path); return -1; }
  unsigned char mz[2];
  if (fread(mz, 1, 2, f) != 2 || mz[0] != 'M' || mz[1] != 'Z') {
    fclose(f);
    snprintf(err, cap, "'%s' is not a PE binary (no MZ header)", path);
    return -1;
  }
  uint32_t lfanew = 0;
  unsigned char lf_bytes[4];
  if (fseek(f, 0x3C, SEEK_SET) != 0 || fread(lf_bytes, 1, 4, f) != 4) {
    fclose(f);
    snprintf(err, cap, "cannot read PE offset");
    return -1;
  }
  lfanew = (uint32_t)lf_bytes[0] | ((uint32_t)lf_bytes[1] << 8) |
           ((uint32_t)lf_bytes[2] << 16) | ((uint32_t)lf_bytes[3] << 24);
  if (lfanew > (1u << 20)) { fclose(f); snprintf(err, cap, "bogus PE offset"); return -1; }
  if (fseek(f, (long)lfanew, SEEK_SET) != 0) { fclose(f); snprintf(err, cap, "seek failed"); return -1; }
  unsigned char sig[4];
  if (fread(sig, 1, 4, f) != 4 || memcmp(sig, "PE\0\0", 4)) {
    fclose(f);
    snprintf(err, cap, "'%s' has no PE signature", path);
    return -1;
  }
  // COFF header: 20 bytes, then optional header magic
  if (fseek(f, (long)lfanew + 4 + 20, SEEK_SET) != 0) { fclose(f); snprintf(err, cap, "seek failed"); return -1; }
  unsigned char magic[2];
  if (fread(magic, 1, 2, f) != 2) { fclose(f); snprintf(err, cap, "cannot read optional magic"); return -1; }
  int pe32plus = (magic[0] == 0x0b && magic[1] == 0x02);
  int pe32 = (magic[0] == 0x0b && magic[1] == 0x01);
  if (!pe32 && !pe32plus) {
    fclose(f);
    snprintf(err, cap, "unknown optional magic %02x%02x", magic[0], magic[1]);
    return -1;
  }
  // subsystem at optional+68 (both PE32 and PE32+)
  if (fseek(f, (long)lfanew + 4 + 20 + 68, SEEK_SET) != 0) { fclose(f); snprintf(err, cap, "seek failed"); return -1; }
  unsigned char ss[2];
  if (fread(ss, 1, 2, f) != 2) { fclose(f); snprintf(err, cap, "cannot read subsystem"); return -1; }
  fclose(f);
  unsigned sub = (unsigned)ss[0] | ((unsigned)ss[1] << 8);
  if (subsystem_out) *subsystem_out = sub;
  if (sub == 10 || sub == 11 || sub == 12 || sub == 13) return 0;
  snprintf(err, cap, "'%s' is PE but subsystem %u is not EFI (10/11/12/13)", path, sub);
  return -1;
}
