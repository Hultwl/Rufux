#ifndef RUFUX_MSDL_H
#define RUFUX_MSDL_H
// Windows 10/11 ISO download, following the protocol of Fido (github.com/pbatard/Fido).
#include <stddef.h>

typedef struct RufuxMsdl RufuxMsdl;

typedef struct {
  const char *name;         // "Windows 11"
  const char *release;      // "25H2 v2 (Build 26200.8037 - 2026.03)"
  int edition_count;
  const char *editions[4];  // "Windows 11 Home/Pro/Edu", ...
} RufuxMsdlProduct;

typedef struct {
  char arch[8];             // x86 | x64 | ARM64
  char url[2048];
} RufuxMsdlLink;

const RufuxMsdlProduct *rufux_msdl_products(int *count);

RufuxMsdl *rufux_msdl_open(int product, int edition, char *err, size_t cap);
void rufux_msdl_close(RufuxMsdl *h);
int rufux_msdl_language_count(const RufuxMsdl *h);
const char *rufux_msdl_language_name(const RufuxMsdl *h, int i);
const char *rufux_msdl_language_display(const RufuxMsdl *h, int i);
int rufux_msdl_find_language(const RufuxMsdl *h, const char *want, char *err, size_t cap);
int rufux_msdl_links(RufuxMsdl *h, int language, RufuxMsdlLink *out, int max, char *err, size_t cap);
int rufux_msdl_links_for_arch(RufuxMsdl *h, int language, const char *arch_hint,
                              RufuxMsdlLink *out, int max, char *err, size_t cap);

typedef void (*RufuxMsdlProgress)(unsigned long long done, unsigned long long total, void *user);
int rufux_msdl_fetch(const char *url, const char *dir, RufuxMsdlProgress cb, void *user,
                     char *path, size_t pathcap, char *err, size_t cap);
void rufux_msdl_get_progress_stats(unsigned long long *done_bytes, unsigned long long *total_bytes,
                                   double *speed_bps, int *eta_sec);
int rufux_msdl_sha256(const char *path, char hex[65]);
#endif
