#include "iso_probe.h"
#include "exec.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

static int listing_has_windows_install(char *out) {
  char *save = NULL;
  for (char *line = strtok_r(out, "\n", &save); line;
       line = strtok_r(NULL, "\n", &save)) {
    while (*line == ' ' || *line == '\t') line++;
    char *e = line + strlen(line);
    while (e > line && (e[-1] == '\r' || e[-1] == ' ' || e[-1] == '\t')) *--e = 0;
    if (!strcasestr(line, "sources/install.") &&
        !strcasestr(line, "sources\\install."))
      continue;
    const char *t = line + strlen(line);
    while (t > line && t[-1] != '/' && t[-1] != '\\') t--;
    if (!strcasecmp(t, "install.wim") ||
        !strcasecmp(t, "install.esd") ||
        !strcasecmp(t, "install.swm"))
      return 1;
  }
  return 0;
}

int rufux_is_windows_iso(const char *iso, char *err, unsigned long cap) {
  struct stat st;
  if (stat(iso, &st) != 0) {
    if (err && cap) snprintf(err, cap, "source '%s' missing", iso);
    return -1;
  }
  const unsigned long out_cap = 1024UL * 1024UL;
  char *out = calloc(1, out_cap);
  if (!out) {
    if (err && cap) snprintf(err, cap, "out of memory");
    return -1;
  }

  int is_udf = rufux_iso_is_udf(iso) > 0;
  int have_7z = rufux_have("7z");
  int have_tar = rufux_have("bsdtar");

  if (is_udf || (!have_tar && have_7z)) {
    if (!have_7z) {
      free(out);
      if (err && cap) snprintf(err, cap, "need 7z to inspect UDF image");
      return -1;
    }
    const char *av[] = {"7z", "l", "-ba", iso, NULL};
    if (rufux_capture(av, out, out_cap) != 0) {
      free(out);
      return 0;
    }
    int r = listing_has_windows_install(out);
    free(out);
    return r;
  }

  if (!have_tar) {
    free(out);
    if (err && cap) snprintf(err, cap, "need bsdtar or 7z to inspect ISO");
    return -1;
  }
  const char *av[] = {"bsdtar", "-tf", iso, NULL};
  if (rufux_capture(av, out, out_cap) != 0) {
    if (have_7z) {
      const char *av7[] = {"7z", "l", "-ba", iso, NULL};
      if (rufux_capture(av7, out, out_cap) == 0) {
        int r = listing_has_windows_install(out);
        free(out);
        return r;
      }
    }
    free(out);
    return 0;
  }
  int r = listing_has_windows_install(out);
  free(out);
  return r;
}

int rufux_probe_iso(const char *path, char *label_out, unsigned long cap) {
  RufuxIsoInfo info = {0};
  int rc = rufux_probe_iso_detail(path, &info);
  if (rc != 0) return rc;
  snprintf(label_out, cap, "%s", info.label);
  return info.valid_iso ? 0 : -2;
}

int rufux_probe_iso_detail(const char *path, RufuxIsoInfo *info) {
  memset(info, 0, sizeof *info);
  struct stat st;
  if (stat(path, &st) != 0) return -1;
  info->size_bytes = (unsigned long long)st.st_size;
  FILE *f = fopen(path, "rb");
  if (!f) return -1;
  // PVD at sector 16: magic "CD001" at +1, label at +40 (32B)
  unsigned char sec[2048];
  if (fseek(f, 16 * 2048, SEEK_SET) != 0) { fclose(f); return -1; }
  if (fread(sec, 1, sizeof sec, f) != sizeof sec) { fclose(f); return -1; }
  if (sec[0] == 1 && !memcmp(sec + 1, "CD001", 5)) {
    info->valid_iso = 1;
    char label[33] = {0};
    memcpy(label, sec + 40, 32);
    label[32] = 0;
    for (int i = 31; i >= 0 && (label[i] == ' ' || label[i] == 0); i--) label[i] = 0;
    snprintf(info->label, sizeof info->label, "%s", label);
  }
  // Boot record at sector 17: "CD001", type 0, id "EL TORITO SPECIFICATION".
  if (fseek(f, 17 * 2048, SEEK_SET) == 0 &&
      fread(sec, 1, sizeof sec, f) == sizeof sec) {
    if (sec[0] == 0 && !memcmp(sec + 1, "CD001", 5) &&
        !memcmp(sec + 7, "EL TORITO SPECIFICATION", 23)) {
      info->bootable = 1;
      unsigned long cat_lba = (unsigned long)sec[0x47] |
                              ((unsigned long)sec[0x48] << 8) |
                              ((unsigned long)sec[0x49] << 16) |
                              ((unsigned long)sec[0x4A] << 24);
      unsigned long long cat_off = cat_lba * 2048ULL;
      if (cat_lba > 0 && cat_off + 32 <= info->size_bytes &&
          fseek(f, (long)cat_off, SEEK_SET) == 0 &&
          fread(sec, 1, 32, f) == 32) {
        if (sec[0] == 0x01 && sec[1] == 0xEF) info->has_efi = 1;
      }
    }
  }
  fclose(f);
  {
    char werr[128] = {0};
    info->is_windows = rufux_is_windows_iso(path, werr, sizeof werr);
  }
  info->is_udf = rufux_iso_is_udf(path);
  return 0;
}

int rufux_iso_is_udf(const char *path) {
  FILE *f = fopen(path, "rb");
  if (!f) return -1;
  unsigned char sec[2048];
  if (fseek(f, 256L * 2048, SEEK_SET) != 0) { fclose(f); return 0; }
  if (fread(sec, 1, sizeof sec, f) != sizeof sec) { fclose(f); return 0; }
  fclose(f);
  unsigned tag = (unsigned)sec[0] | ((unsigned)sec[1] << 8);
  return tag == 2 ? 1 : 0;
}

void rufux_print_iso_detail(const char *path, const RufuxIsoInfo *info) {
  printf("file: %s\nsize: %llu bytes (%.2f MB)\nlabel: %s\nvalid_iso: %s\nbootable: %s\nefi_hint: %s\nwindows: %s\nudf: %s\n",
         path, info->size_bytes, info->size_bytes / 1048576.0,
         info->label[0] ? info->label : "(none)",
         info->valid_iso ? "yes" : "no",
         info->bootable ? "yes" : "no",
         info->has_efi ? "yes" : "no",
         info->is_windows > 0 ? "yes" : (info->is_windows == 0 ? "no" : "unknown"),
         info->is_udf > 0 ? "yes" : (info->is_udf == 0 ? "no" : "unknown"));
}
