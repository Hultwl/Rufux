#ifndef RUFUX_MSDL_H
#define RUFUX_MSDL_H
// Windows 10/11 ISO download, following the protocol of Fido (github.com/pbatard/Fido),
// the script Rufus uses. Microsoft offers no documented API for this: the same
// web service that drives the software-download pages is asked for the language
// list and for time-limited links to the official ISO files, and the ISO is then
// fetched from Microsoft's download servers over HTTPS. Requests are made with
// curl. Microsoft may change or block this at any time and rate-limits by IP.
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

// Opens a session for product/edition and fetches the languages Microsoft offers.
// NULL on failure with err set (including the "IP banned" explanation).
RufuxMsdl *rufux_msdl_open(int product, int edition, char *err, size_t cap);
void rufux_msdl_close(RufuxMsdl *h);
int rufux_msdl_language_count(const RufuxMsdl *h);
const char *rufux_msdl_language_name(const RufuxMsdl *h, int i);     // "English"
const char *rufux_msdl_language_display(const RufuxMsdl *h, int i);  // "English (United States)"
// Index of the language that best matches `want` (name or display name, case-insensitive,
// exact first then prefix), or the system locale when want is NULL/empty; -1 when ambiguous
// or unknown (candidates then listed in err).
int rufux_msdl_find_language(const RufuxMsdl *h, const char *want, char *err, size_t cap);
// Download links (one per architecture) for language i. Returns the count, -1 on error.
int rufux_msdl_links(RufuxMsdl *h, int language, RufuxMsdlLink *out, int max, char *err, size_t cap);

// Downloads url into dir (resuming a .part file if one is there) and returns the final
// path in `path`. progress gets done/total in units of 0.01% (total 10000).
typedef void (*RufuxMsdlProgress)(unsigned long long done, unsigned long long total, void *user);
int rufux_msdl_fetch(const char *url, const char *dir, RufuxMsdlProgress cb, void *user,
                     char *path, size_t pathcap, char *err, size_t cap);
// SHA-256 of a file as lower-case hex (65 bytes), 0 on success.
int rufux_msdl_sha256(const char *path, char hex[65]);
#endif
