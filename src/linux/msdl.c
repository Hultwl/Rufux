#define _GNU_SOURCE
#include "msdl.h"
#include "exec.h"
#include "mjson.h"
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <openssl/evp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <sys/prctl.h>
#include <signal.h>
#include <unistd.h>

// Constants of the download service, as used by Fido.
#define ORG_ID "y6jn8c31"
#define PROFILE_ID "606624d44113"
#define INSTANCE_ID "560dc9f3-1aa5-4a2f-b63c-9e18f8d0e175"
#define REFERER "https://www.microsoft.com/software-download/windows11"
#define UA "Mozilla/5.0 (X11; Linux x86_64; rv:130.0) Gecko/20100101 Firefox/130.0"
#define QUERY_LOCALE "en-US"
#define MAXLANG 128
#define REPLY_CAP (2u << 20)

typedef struct { const char *name; int ids[2]; int nids; } Edition;
typedef struct { const char *name; const char *release; Edition ed[3]; int ned; } Product;
// Edition ids come from Fido's table (one id per architecture where Microsoft
// keeps separate x64 and ARM64 products). They change with each Windows release.
static const Product products[] = {
  {"Windows 11", "25H2 v2 (Build 26200.8037 - 2026.03)",
   {{"Windows 11 Home/Pro/Edu", {3321, 3324}, 2},
    {"Windows 11 Home China", {3322, 3325}, 2},
    {"Windows 11 Pro China", {3323, 3326}, 2}}, 3},
  {"Windows 10", "22H2 v1 (Build 19045.2965 - 2023.05)",
   {{"Windows 10 Home/Pro/Edu", {2618}, 1},
    {"Windows 10 Home China", {2378}, 1}}, 2},
};

static RufuxMsdlProduct pub[2];
const RufuxMsdlProduct *rufux_msdl_products(int *count) {
  for (unsigned i = 0; i < 2; i++) {
    pub[i].name = products[i].name;
    pub[i].release = products[i].release;
    pub[i].edition_count = products[i].ned;
    for (int e = 0; e < products[i].ned; e++) pub[i].editions[e] = products[i].ed[e].name;
  }
  if (count) *count = 2;
  return pub;
}

typedef struct { int sess; char sku[40]; } SkuRef;
typedef struct { char name[64]; char display[96]; SkuRef d[2]; int nd; } Lang;

struct RufuxMsdl {
  char sess[2][40];
  int nsess;
  Lang langs[MAXLANG];
  int nlangs;
  char jar[PATH_MAX];
};

// ---- HTTP ----------------------------------------------------------------
// RUFUX_MSDL_MOCK=http://127.0.0.1:PORT points every Microsoft host at a local
// test server (loopback only, so it cannot be used to redirect real traffic).
static const char *mock_base(void) {
  const char *m = getenv("RUFUX_MSDL_MOCK");
  if (m && (!strncmp(m, "http://127.0.0.1:", 17) || !strncmp(m, "http://localhost:", 17))) return m;
  return NULL;
}

static void build_url(char *out, size_t cap, const char *host, const char *path_query) {
  const char *m = mock_base();
  if (m) {
    const char *tag = !strcmp(host, "vlscppe.microsoft.com") ? "vlscppe"
                    : !strcmp(host, "ov-df.microsoft.com") ? "ov-df" : "www";
    snprintf(out, cap, "%s/%s%s", m, tag, path_query);
  } else {
    snprintf(out, cap, "https://%s%s", host, path_query);
  }
}

static int http_get(const RufuxMsdl *h, const char *url, const char *referer, char *out, size_t cap,
                    char *err, size_t ecap) {
  const char *av[20];
  int n = 0;
  av[n++] = "curl"; av[n++] = "-sS"; av[n++] = "-w"; av[n++] = "\n%{http_code}";
  av[n++] = "--max-time"; av[n++] = "30";
  av[n++] = "--connect-timeout"; av[n++] = "15"; av[n++] = "-A"; av[n++] = UA;
  av[n++] = "-b"; av[n++] = h->jar; av[n++] = "-c"; av[n++] = h->jar;
  if (referer) { av[n++] = "-e"; av[n++] = referer; }
  av[n++] = url; av[n] = NULL;
  if (!rufux_have("curl")) { snprintf(err, ecap, "curl is not installed"); return -1; }
  if (rufux_capture(av, out, cap) != 0) {
    char *nl = strchr(out, '\n');
    if (nl) *nl = 0;
    snprintf(err, ecap, "network error: %.300s", out[0] ? out : "curl failed");
    return -1;
  }
  // The status code was appended on its own last line; strip it and insist on a 2xx.
  char *last = strrchr(out, '\n');
  int code = last ? atoi(last + 1) : 0;
  if (last) *last = 0;
  if (code < 200 || code > 299) {
    const char *host = strstr(url, "://");
    char hn[128] = "the server";
    if (host) { host += 3; size_t l = strcspn(host, "/:?"); snprintf(hn, sizeof hn, "%.*s", (int)(l < 100 ? l : 100), host); }
    snprintf(err, ecap, "%s answered HTTP %d%s", hn, code,
             code == 403 || code == 429 ? " (Microsoft rate-limits and blocks by address; try again later)" : "");
    return -1;
  }
  return 0;
}

static void new_guid(char *g, size_t cap) {
  unsigned char b[16];
  FILE *f = fopen("/dev/urandom", "rb");
  if (!f || fread(b, 1, 16, f) != 16) { for (int i = 0; i < 16; i++) b[i] = (unsigned char)(rand() & 0xff); }
  if (f) fclose(f);
  b[6] = (b[6] & 0x0f) | 0x40;
  b[8] = (b[8] & 0x3f) | 0x80;
  snprintf(g, cap, "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
           b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7], b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
}

// "?w=ABC123" / "&w=ABC123" and rticks="+123" / rticks="123" inside mdt.js
static int parse_mdt(const char *js, char *w, size_t wcap, char *rt, size_t rcap) {
  w[0] = rt[0] = 0;
  for (const char *p = js; (p = strstr(p, "w=")) != NULL; p += 2) {
    if (p == js || (p[-1] != '?' && p[-1] != '&')) continue;
    size_t n = strspn(p + 2, "0123456789ABCDEF");
    if (n && n < wcap) { memcpy(w, p + 2, n); w[n] = 0; break; }
  }
  const char *r = strstr(js, "rticks=\"");
  if (r) {
    r += 8;
    if (*r == '+') r++;
    size_t n = strspn(r, "0123456789");
    if (n && n < rcap) { memcpy(rt, r, n); rt[n] = 0; }
  }
  return w[0] && rt[0] ? 0 : -1;
}

static const char *ban_message =
  "Your IP address has been banned by Microsoft for issuing too many ISO download requests or for "
  "belonging to a region where sanctions currently apply. Try again later (message code 715-123130)";

static int api_errors(const MJ *j, char *err, size_t cap) {
  const MJ *e = mj_get(j, "Errors");
  if (mj_len(e) == 0) return 0;
  const MJ *first = mj_at(e, 0);
  if ((int)mj_get_num(first, "Type", 0) == 9) snprintf(err, cap, "%s", ban_message);
  else snprintf(err, cap, "Microsoft's service returned an error: %.300s", mj_get_str(first, "Value", "unknown"));
  return 1;
}

// ---- session ---------------------------------------------------------------
static int whitelist_session(RufuxMsdl *h, const char *guid, char *reply, char *err, size_t cap) {
  char url[1024], w[64], rt[64];
  build_url(url, sizeof url, "vlscppe.microsoft.com", "/tags?org_id=" ORG_ID "&session_id=");
  size_t l = strlen(url);
  snprintf(url + l, sizeof url - l, "%s", guid);
  if (http_get(h, url, NULL, reply, REPLY_CAP, err, cap) != 0) return -1;
  // ov-df request/reply
  char path[512];
  snprintf(path, sizeof path, "/mdt.js?instanceId=" INSTANCE_ID "&PageId=si&session_id=%s", guid);
  build_url(url, sizeof url, "ov-df.microsoft.com", path);
  if (http_get(h, url, NULL, reply, REPLY_CAP, err, cap) != 0) return -1;
  if (parse_mdt(reply, w, sizeof w, rt, sizeof rt) != 0) {
    snprintf(err, cap, "could not read the session token from Microsoft's service (it may have changed)");
    return -1;
  }
  snprintf(path, sizeof path, "/?session_id=%s&CustomerId=" INSTANCE_ID "&PageId=si&w=%s&mdt=%lld&rticks=%s",
           guid, w, (long long)time(NULL) * 1000, rt);
  build_url(url, sizeof url, "ov-df.microsoft.com", path);
  return http_get(h, url, NULL, reply, REPLY_CAP, err, cap);
}

static int add_sku(RufuxMsdl *h, int sess, const char *lang, const char *display, const char *sku) {
  for (int i = 0; i < h->nlangs; i++) {
    if (strcmp(h->langs[i].name, lang)) continue;
    if (h->langs[i].nd < 2) {
      h->langs[i].d[h->langs[i].nd].sess = sess;
      snprintf(h->langs[i].d[h->langs[i].nd].sku, sizeof h->langs[i].d[0].sku, "%s", sku);
      h->langs[i].nd++;
    }
    return 0;
  }
  if (h->nlangs >= MAXLANG) return 0;
  Lang *l = &h->langs[h->nlangs++];
  snprintf(l->name, sizeof l->name, "%s", lang);
  snprintf(l->display, sizeof l->display, "%s", display);
  l->d[0].sess = sess;
  snprintf(l->d[0].sku, sizeof l->d[0].sku, "%s", sku);
  l->nd = 1;
  return 0;
}

RufuxMsdl *rufux_msdl_open(int product, int edition, char *err, size_t cap) {
  if (product < 0 || product >= 2 || edition < 0 || edition >= products[product].ned) {
    snprintf(err, cap, "unknown Windows version or edition");
    return NULL;
  }
  RufuxMsdl *h = calloc(1, sizeof *h);
  char *reply = malloc(REPLY_CAP);
  if (!h || !reply) { free(h); free(reply); snprintf(err, cap, "out of memory"); return NULL; }
  snprintf(h->jar, sizeof h->jar, "/tmp/rufux-msdl-XXXXXX");
  int fd = mkstemp(h->jar);
  if (fd < 0) { snprintf(err, cap, "cannot create a cookie file: %s", strerror(errno)); free(h); free(reply); return NULL; }
  close(fd);
  const Edition *ed = &products[product].ed[edition];
  for (int s = 0; s < ed->nids; s++) {
    new_guid(h->sess[s], sizeof h->sess[s]);
    h->nsess++;
    if (whitelist_session(h, h->sess[s], reply, err, cap) != 0) goto fail;
    char path[512], url[1024];
    snprintf(path, sizeof path,
             "/software-download-connector/api/getskuinformationbyproductedition?profile=" PROFILE_ID
             "&productEditionId=%d&SKU=undefined&friendlyFileName=undefined&Locale=" QUERY_LOCALE "&sessionID=%s",
             ed->ids[s], h->sess[s]);
    build_url(url, sizeof url, "www.microsoft.com", path);
    int ok = 0;
    for (int attempt = 0; attempt < 3 && !ok; attempt++) {
      if (attempt) sleep(2);
      if (http_get(h, url, NULL, reply, REPLY_CAP, err, cap) != 0) goto fail;
      char jerr[128];
      MJ *j = mj_parse(reply, jerr, sizeof jerr);
      if (!j) { snprintf(err, cap, "Microsoft's service sent an unreadable reply (%s)", jerr); continue; }
      if (api_errors(j, err, cap)) { mj_free(j); if (strstr(err, "715-123130")) goto fail; continue; }
      const MJ *skus = mj_get(j, "Skus");
      for (size_t i = 0; i < mj_len(skus); i++) {
        const MJ *k = mj_at(skus, i);
        const char *id = mj_get_str(k, "Id", ""), *lang = mj_get_str(k, "Language", "");
        if (id[0] && lang[0]) add_sku(h, s, lang, mj_get_str(k, "LocalizedLanguage", lang), id);
      }
      mj_free(j);
      if (h->nlangs) ok = 1;
      else snprintf(err, cap, "Microsoft's service returned no languages");
    }
    if (!ok) goto fail;
  }
  free(reply);
  return h;
fail:
  free(reply);
  rufux_msdl_close(h);
  return NULL;
}

void rufux_msdl_close(RufuxMsdl *h) {
  if (!h) return;
  if (h->jar[0]) unlink(h->jar);
  free(h);
}

int rufux_msdl_language_count(const RufuxMsdl *h) { return h ? h->nlangs : 0; }
const char *rufux_msdl_language_name(const RufuxMsdl *h, int i) { return (h && i >= 0 && i < h->nlangs) ? h->langs[i].name : ""; }
const char *rufux_msdl_language_display(const RufuxMsdl *h, int i) { return (h && i >= 0 && i < h->nlangs) ? h->langs[i].display : ""; }

// Which languages does the system locale (LANG=fr_FR.UTF-8 ...) point at?
static const char *locale_language_hint(void) {
  static const struct { const char *code, *name; } m[] = {
    {"en", "English"}, {"fr", "French"}, {"de", "German"}, {"es", "Spanish"}, {"it", "Italian"},
    {"pt", "Portuguese"}, {"nl", "Dutch"}, {"pl", "Polish"}, {"ru", "Russian"}, {"ja", "Japanese"},
    {"ko", "Korean"}, {"zh", "Chinese"}, {"sv", "Swedish"}, {"da", "Danish"}, {"nb", "Norwegian"},
    {"fi", "Finnish"}, {"cs", "Czech"}, {"tr", "Turkish"}, {"ar", "Arabic"}, {"he", "Hebrew"},
    {"uk", "Ukrainian"}, {"hu", "Hungarian"}, {"el", "Greek"}, {"ro", "Romanian"}, {"bg", "Bulgarian"}};
  const char *l = getenv("LC_ALL");
  if (!l || !*l) l = getenv("LANG");
  if (!l || strlen(l) < 2) return "English";
  for (unsigned i = 0; i < sizeof m / sizeof m[0]; i++)
    if (!strncasecmp(l, m[i].code, 2)) return m[i].name;
  return "English";
}

int rufux_msdl_find_language(const RufuxMsdl *h, const char *want, char *err, size_t cap) {
  int hint = !want || !*want;
  const char *w = hint ? locale_language_hint() : want;
  int hit = -1, hits = 0;
  // exact on name, exact on display, then prefix on either
  for (int pass = 0; pass < 3 && hits != 1; pass++) {
    hit = -1; hits = 0;
    for (int i = 0; i < h->nlangs; i++) {
      const char *a = h->langs[i].name, *b = h->langs[i].display;
      int m = pass == 0 ? !strcasecmp(a, w)
            : pass == 1 ? !strcasecmp(b, w)
            : (!strncasecmp(a, w, strlen(w)) || !strncasecmp(b, w, strlen(w)));
      if (m) { hit = i; hits++; }
    }
    if (hits > 1 && pass == 2 && hint) {
      // A locale hint like "English" matches several variants: take the first, as Fido's default does.
      for (int i = 0; i < h->nlangs; i++)
        if (!strncasecmp(h->langs[i].name, w, strlen(w))) return i;
    }
    if (hits > 1 && pass < 2) break;
  }
  if (hits == 1) return hit;
  char list[512] = "";
  for (int i = 0; i < h->nlangs && strlen(list) < 400; i++) {
    if (hits > 1) {
      if (strncasecmp(h->langs[i].name, w, strlen(w)) && strncasecmp(h->langs[i].display, w, strlen(w))) continue;
    }
    size_t l = strlen(list);
    snprintf(list + l, sizeof list - l, "%s'%s'", l ? ", " : "", h->langs[i].display);
  }
  snprintf(err, cap, hits > 1 ? "'%s' matches several languages: %s" : "no language matches '%s'. Available: %s", w, list);
  return -1;
}

static const char *arch_of(int type) { return type == 0 ? "x86" : type == 1 ? "x64" : type == 2 ? "ARM64" : "Unknown"; }

int rufux_msdl_links(RufuxMsdl *h, int language, RufuxMsdlLink *out, int max, char *err, size_t cap) {
  if (!h || language < 0 || language >= h->nlangs) { snprintf(err, cap, "no such language"); return -1; }
  char *reply = malloc(REPLY_CAP);
  if (!reply) { snprintf(err, cap, "out of memory"); return -1; }
  int n = 0;
  const Lang *l = &h->langs[language];
  for (int k = 0; k < l->nd; k++) {
    char path[640], url[1024];
    snprintf(path, sizeof path,
             "/software-download-connector/api/GetProductDownloadLinksBySku?profile=" PROFILE_ID
             "&productEditionId=undefined&SKU=%s&friendlyFileName=undefined&Locale=" QUERY_LOCALE "&sessionID=%s",
             l->d[k].sku, h->sess[l->d[k].sess]);
    build_url(url, sizeof url, "www.microsoft.com", path);
    if (http_get(h, url, REFERER, reply, REPLY_CAP, err, cap) != 0) { free(reply); return -1; }
    char jerr[128];
    MJ *j = mj_parse(reply, jerr, sizeof jerr);
    if (!j) { snprintf(err, cap, "Microsoft's service sent an unreadable reply (%s)", jerr); free(reply); return -1; }
    if (api_errors(j, err, cap)) { mj_free(j); free(reply); return -1; }
    const MJ *opts = mj_get(j, "ProductDownloadOptions");
    for (size_t i = 0; i < mj_len(opts) && n < max; i++) {
      const MJ *o = mj_at(opts, i);
      const char *u = mj_get_str(o, "Uri", "");
      // Only ever download over HTTPS (the loopback test server excepted).
      if (strncmp(u, "https://", 8) && !(mock_base() && !strncmp(u, "http://127.0.0.1", 16))) continue;
      snprintf(out[n].arch, sizeof out[n].arch, "%s", arch_of((int)mj_get_num(o, "DownloadType", -1)));
      snprintf(out[n].url, sizeof out[n].url, "%s", u);
      n++;
    }
    mj_free(j);
  }
  free(reply);
  if (!n) { snprintf(err, cap, "Microsoft's service returned no ISO download links"); return -1; }
  return n;
}

// ---- download -----------------------------------------------------------------
static void iso_name(const char *url, char *out, size_t cap) {
  const char *s = strstr(url, "://");
  s = s ? s + 3 : url;
  const char *slash = strrchr(s, '/');
  const char *b = slash ? slash + 1 : s;
  size_t n = strcspn(b, "?#");
  size_t o = 0;
  for (size_t i = 0; i < n && o + 1 < cap; i++)
    out[o++] = (isalnum((unsigned char)b[i]) || b[i] == '.' || b[i] == '_' || b[i] == '-') ? b[i] : '_';
  out[o] = 0;
  if (o < 5 || strcasecmp(out + o - 4, ".iso") || !strcmp(out, ".iso")) snprintf(out, cap, "Windows.iso");
}

int rufux_msdl_fetch(const char *url, const char *dir, RufuxMsdlProgress cb, void *user,
                     char *path, size_t pathcap, char *err, size_t cap) {
  if (strncmp(url, "https://", 8) && !(mock_base() && !strncmp(url, "http://127.0.0.1", 16))) {
    snprintf(err, cap, "refusing to download over an unencrypted connection");
    return -1;
  }
  if (!rufux_have("curl")) { snprintf(err, cap, "curl is not installed"); return -1; }
  struct stat st;
  if (stat(dir, &st) != 0 || !S_ISDIR(st.st_mode)) { snprintf(err, cap, "'%s' is not a directory", dir); return -1; }
  char name[256];
  iso_name(url, name, sizeof name);
  char part[PATH_MAX];
  snprintf(path, pathcap, "%.3000s/%s", dir, name);
  snprintf(part, sizeof part, "%s.part", path);
  if (access(path, F_OK) == 0) { snprintf(err, cap, "'%s' already exists; move it away or pick another folder", path); return -1; }
  const char *av[] = {"curl", "-L", "--fail", "--retry", "3", "--retry-delay", "2", "--connect-timeout", "30",
                      "-A", UA, "-C", "-", "-#", "-o", part, url, NULL};
  int fd[2];
  if (pipe(fd) != 0) { snprintf(err, cap, "pipe failed"); return -1; }
  pid_t pid = fork();
  if (pid < 0) { close(fd[0]); close(fd[1]); snprintf(err, cap, "fork failed"); return -1; }
  if (pid == 0) {
    prctl(PR_SET_PDEATHSIG, SIGTERM);
    dup2(fd[1], STDERR_FILENO);  // -# draws its bar on stderr
    int nul = open("/dev/null", O_WRONLY);
    if (nul >= 0) dup2(nul, STDOUT_FILENO);
    close(fd[0]); close(fd[1]);
    rufux_execvp(av[0], (const char *const *)av);
    _exit(127);
  }
  close(fd[1]);
  char line[512], tail[300] = "";
  size_t ln = 0;
  char c;
  while (read(fd[0], &c, 1) == 1) {
    if (c == '\r' || c == '\n') {
      line[ln] = 0;
      char *pc = strrchr(line, '%');
      if (pc) {
        char *b = pc;
        while (b > line && (isdigit((unsigned char)b[-1]) || b[-1] == '.')) b--;
        if (b < pc && cb) cb((unsigned long long)(atof(b) * 100.0), 10000ULL, user);
      } else if (line[0]) snprintf(tail, sizeof tail, "%.290s", line);
      ln = 0;
    } else if (ln < sizeof line - 1) line[ln++] = c;
  }
  close(fd[0]);
  int status = 0;
  while (waitpid(pid, &status, 0) < 0) {}
  if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
    snprintf(err, cap, "download failed (curl exit %d)%s%s. The partial file was kept; run the same command to resume",
             WIFEXITED(status) ? WEXITSTATUS(status) : -1, tail[0] ? ": " : "", tail);
    return -1;
  }
  if (rename(part, path) != 0) { snprintf(err, cap, "cannot rename '%s': %s", part, strerror(errno)); return -1; }
  if (cb) cb(10000, 10000, user);
  return 0;
}

int rufux_msdl_sha256(const char *path, char hex[65]) {
  FILE *f = fopen(path, "rb");
  if (!f) return -1;
  EVP_MD_CTX *c = EVP_MD_CTX_new();
  EVP_DigestInit_ex(c, EVP_sha256(), NULL);
  unsigned char buf[65536], md[32];
  size_t n;
  while ((n = fread(buf, 1, sizeof buf, f)) > 0) EVP_DigestUpdate(c, buf, n);
  fclose(f);
  unsigned ol = 0;
  EVP_DigestFinal_ex(c, md, &ol);
  EVP_MD_CTX_free(c);
  for (int i = 0; i < 32; i++) snprintf(hex + i * 2, 3, "%02x", md[i]);
  return 0;
}
