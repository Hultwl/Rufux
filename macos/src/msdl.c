#include "msdl.h"
#include "exec.h"
#include "mjson.h"
#include <ctype.h>
#include <curl/curl.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <openssl/evp.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define ORG_ID "y6jn8c31"
#define PROFILE_ID "606624d44113"
#define INSTANCE_ID "560dc9f3-1aa5-4a2f-b63c-9e18f8d0e175"
#define REFERER "https://www.microsoft.com/software-download/windows11"
#define UA "Mozilla/5.0 (Macintosh; Intel Mac OS X 10.15; rv:130.0) Gecko/20100101 Firefox/130.0"
#define QUERY_LOCALE "en-US"
#define MAXLANG 128
#define REPLY_CAP (2u << 20)
#define CACHE_MAGIC 0x524D5346u
#define CACHE_TTL_SEC 900

typedef struct { const char *name; int ids[2]; int nids; } Edition;
typedef struct { const char *name; const char *release; Edition ed[3]; int ned; } Product;

static const Product products[] = {
  {"Windows 11", "25H2 v2 (Build 26200.8037 - 2026.03)",
   {{"Windows 11 Home/Pro/Edu", {3321, 3324}, 2},
    {"Windows 11 Home China", {3322, 3325}, 2},
    {"Windows 11 Pro China", {3323, 3326}, 2}}, 3},
  {"Windows 10", "22H2 v1 (Build 19045.2965 - 2023.05)",
   {{"Windows 10 Home/Pro/Edu", {2618}, 1},
    {"Windows 10 Home China", {2378}, 1}}, 2},
};

static pthread_once_t g_curl_once = PTHREAD_ONCE_INIT;
static void init_curl_global(void) {
  curl_global_init(CURL_GLOBAL_DEFAULT);
}

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
  int product;
  int edition;
  int from_cache;
  char sess[2][40];
  int nsess;
  Lang langs[MAXLANG];
  int nlangs;
  char jar[2][PATH_MAX];
  int keep_jars;
  CURL *curl[2];
};

typedef struct {
  uint32_t magic;
  int product;
  int edition;
  int nsess;
  int nlangs;
  int64_t created_at;
  char sess[2][40];
  Lang langs[MAXLANG];
} MsdlCacheRecord;

static const char *mock_base(void) {
  const char *m = getenv("RUFUX_MSDL_MOCK");
  if (m && (!strncmp(m, "http://127.0.0.1:", 17) || !strncmp(m, "http://localhost:", 17))) return m;
  return NULL;
}

static int cache_enabled(void) {
  if (mock_base()) return 0;
  const char *nc = getenv("RUFUX_MSDL_NO_CACHE");
  if (nc && nc[0] && strcmp(nc, "0") != 0) return 0;
  return 1;
}

static void cache_paths(int product, int edition, char *meta_out, size_t mcap,
                        char jar0[PATH_MAX], char jar1[PATH_MAX]) {
  unsigned uid = (unsigned)geteuid();
  if (meta_out && mcap)
    snprintf(meta_out, mcap, "/tmp/rufux-msdl-cache-%u-p%d-e%d.bin", uid, product, edition);
  if (jar0)
    snprintf(jar0, PATH_MAX, "/tmp/rufux-msdl-cookie-%u-p%d-e%d-s0.jar", uid, product, edition);
  if (jar1)
    snprintf(jar1, PATH_MAX, "/tmp/rufux-msdl-cookie-%u-p%d-e%d-s1.jar", uid, product, edition);
}

static void invalidate_cache(int product, int edition) {
  char meta[PATH_MAX], j0[PATH_MAX], j1[PATH_MAX];
  cache_paths(product, edition, meta, sizeof meta, j0, j1);
  unlink(meta);
  unlink(j0);
  unlink(j1);
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

typedef struct {
  char *buf;
  size_t len;
  size_t cap;
} MemBuf;

static size_t mem_write_cb(void *ptr, size_t size, size_t nmemb, void *userdata) {
  size_t total = size * nmemb;
  MemBuf *m = (MemBuf *)userdata;
  if (!m || !m->buf || m->cap == 0) return 0;
  size_t avail = (m->len + 1 < m->cap) ? (m->cap - 1 - m->len) : 0;
  if (avail > 0) {
    size_t copy = total < avail ? total : avail;
    memcpy(m->buf + m->len, ptr, copy);
    m->len += copy;
    m->buf[m->len] = 0;
  }
  return total;
}

static CURL *make_session_curl(const char *jar_path) {
  pthread_once(&g_curl_once, init_curl_global);
  CURL *c = curl_easy_init();
  if (!c) return NULL;
  curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(c, CURLOPT_USERAGENT, UA);
  curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 15L);
  curl_easy_setopt(c, CURLOPT_TIMEOUT, 30L);
  curl_easy_setopt(c, CURLOPT_TCP_KEEPALIVE, 1L);
  if (jar_path && jar_path[0]) {
    curl_easy_setopt(c, CURLOPT_COOKIEFILE, jar_path);
    curl_easy_setopt(c, CURLOPT_COOKIEJAR, jar_path);
  }
  return c;
}

static int http_get_curl(CURL *c, const char *url, const char *referer,
                         char *out, size_t cap, char *err, size_t ecap) {
  if (!c || !out || cap == 0) {
    snprintf(err, ecap, "internal HTTP client error");
    return -1;
  }
  out[0] = 0;
  MemBuf mb = { .buf = out, .len = 0, .cap = cap };
  char cerr[CURL_ERROR_SIZE] = {0};
  curl_easy_setopt(c, CURLOPT_URL, url);
  curl_easy_setopt(c, CURLOPT_HTTPGET, 1L);
  curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, mem_write_cb);
  curl_easy_setopt(c, CURLOPT_WRITEDATA, &mb);
  curl_easy_setopt(c, CURLOPT_ERRORBUFFER, cerr);
  curl_easy_setopt(c, CURLOPT_REFERER, referer ? referer : "");

  CURLcode res = curl_easy_perform(c);
  curl_easy_setopt(c, CURLOPT_ERRORBUFFER, NULL);
  if (res != CURLE_OK) {
    snprintf(err, ecap, "network error: %.300s", cerr[0] ? cerr : curl_easy_strerror(res));
    return -1;
  }
  long code = 0;
  curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
  if (code < 200 || code > 299) {
    const char *host = strstr(url, "://");
    char hn[128] = "the server";
    if (host) {
      host += 3;
      size_t l = strcspn(host, "/:?");
      snprintf(hn, sizeof hn, "%.*s", (int)(l < 100 ? l : 100), host);
    }
    snprintf(err, ecap, "%s answered HTTP %ld%s", hn, code,
             code == 403 || code == 429 ? " (Microsoft rate-limits and blocks by address; try again later)" : "");
    return -1;
  }
  return 0;
}

static void new_guid(char *g, size_t cap) {
  unsigned char b[16];
  arc4random_buf(b, sizeof b);
  b[6] = (b[6] & 0x0f) | 0x40;
  b[8] = (b[8] & 0x3f) | 0x80;
  snprintf(g, cap, "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
           b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7], b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
}

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

static int whitelist_session_curl(CURL *c, const char *guid, char *reply, char *err, size_t cap) {
  char url[1024], w[64], rt[64];
  build_url(url, sizeof url, "vlscppe.microsoft.com", "/tags?org_id=" ORG_ID "&session_id=");
  size_t l = strlen(url);
  snprintf(url + l, sizeof url - l, "%s", guid);
  if (http_get_curl(c, url, NULL, reply, REPLY_CAP, err, cap) != 0) return -1;
  char path[512];
  snprintf(path, sizeof path, "/mdt.js?instanceId=" INSTANCE_ID "&PageId=si&session_id=%s", guid);
  build_url(url, sizeof url, "ov-df.microsoft.com", path);
  if (http_get_curl(c, url, NULL, reply, REPLY_CAP, err, cap) != 0) return -1;
  if (parse_mdt(reply, w, sizeof w, rt, sizeof rt) != 0) {
    snprintf(err, cap, "could not read the session token from Microsoft's service (it may have changed)");
    return -1;
  }
  snprintf(path, sizeof path, "/?session_id=%s&CustomerId=" INSTANCE_ID "&PageId=si&w=%s&mdt=%lld&rticks=%s",
           guid, w, (long long)time(NULL) * 1000, rt);
  build_url(url, sizeof url, "ov-df.microsoft.com", path);
  return http_get_curl(c, url, NULL, reply, REPLY_CAP, err, cap);
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

typedef struct {
  char id[40];
  char lang[64];
  char display[96];
} RawSku;

typedef struct {
  int sess_idx;
  int edition_id;
  char guid[40];
  CURL *curl;
  RawSku skus[MAXLANG];
  int nskus;
  int rc;
  char err[512];
} SessWorker;

static void *sess_worker_fn(void *arg) {
  SessWorker *w = (SessWorker *)arg;
  char *reply = malloc(REPLY_CAP);
  if (!reply) {
    snprintf(w->err, sizeof w->err, "out of memory");
    w->rc = -1;
    return NULL;
  }
  if (whitelist_session_curl(w->curl, w->guid, reply, w->err, sizeof w->err) != 0) {
    free(reply);
    w->rc = -1;
    return NULL;
  }
  char path[512], url[1024];
  snprintf(path, sizeof path,
           "/software-download-connector/api/getskuinformationbyproductedition?profile=" PROFILE_ID
           "&productEditionId=%d&SKU=undefined&friendlyFileName=undefined&Locale=" QUERY_LOCALE "&sessionID=%s",
           w->edition_id, w->guid);
  build_url(url, sizeof url, "www.microsoft.com", path);
  int ok = 0;
  for (int attempt = 0; attempt < 3 && !ok; attempt++) {
    if (attempt) sleep(1);
    if (http_get_curl(w->curl, url, NULL, reply, REPLY_CAP, w->err, sizeof w->err) != 0) {
      free(reply);
      w->rc = -1;
      return NULL;
    }
    char jerr[128];
    MJ *j = mj_parse(reply, jerr, sizeof jerr);
    if (!j) {
      snprintf(w->err, sizeof w->err, "Microsoft's service sent an unreadable reply (%s)", jerr);
      continue;
    }
    if (api_errors(j, w->err, sizeof w->err)) {
      mj_free(j);
      if (strstr(w->err, "715-123130")) {
        free(reply);
        w->rc = -1;
        return NULL;
      }
      continue;
    }
    const MJ *skus = mj_get(j, "Skus");
    w->nskus = 0;
    for (size_t i = 0; i < mj_len(skus) && w->nskus < MAXLANG; i++) {
      const MJ *k = mj_at(skus, i);
      const char *id = mj_get_str(k, "Id", "");
      const char *lang = mj_get_str(k, "Language", "");
      const char *disp = mj_get_str(k, "LocalizedLanguage", lang);
      if (id[0] && lang[0]) {
        RawSku *rs = &w->skus[w->nskus++];
        snprintf(rs->id, sizeof rs->id, "%s", id);
        snprintf(rs->lang, sizeof rs->lang, "%s", lang);
        snprintf(rs->display, sizeof rs->display, "%s", disp);
      }
    }
    mj_free(j);
    if (w->nskus > 0) ok = 1;
    else snprintf(w->err, sizeof w->err, "Microsoft's service returned no languages");
  }
  free(reply);
  w->rc = ok ? 0 : -1;
  return NULL;
}

static int load_session_cache(RufuxMsdl *h, int product, int edition) {
  if (!cache_enabled()) return -1;
  char meta[PATH_MAX], j0[PATH_MAX], j1[PATH_MAX];
  cache_paths(product, edition, meta, sizeof meta, j0, j1);
  int fd = open(meta, O_RDONLY);
  if (fd < 0) return -1;
  MsdlCacheRecord rec;
  ssize_t r = read(fd, &rec, sizeof rec);
  close(fd);
  if (r != (ssize_t)sizeof rec || rec.magic != CACHE_MAGIC ||
      rec.product != product || rec.edition != edition ||
      rec.nsess <= 0 || rec.nsess > 2 || rec.nlangs <= 0 || rec.nlangs > MAXLANG) {
    return -1;
  }
  int64_t now = (int64_t)time(NULL);
  if (now < rec.created_at || (now - rec.created_at) > CACHE_TTL_SEC) {
    invalidate_cache(product, edition);
    return -1;
  }
  h->product = product;
  h->edition = edition;
  h->from_cache = 1;
  h->nsess = rec.nsess;
  h->nlangs = rec.nlangs;
  h->keep_jars = 1;
  memcpy(h->sess, rec.sess, sizeof h->sess);
  memcpy(h->langs, rec.langs, sizeof h->langs);
  snprintf(h->jar[0], sizeof h->jar[0], "%s", j0);
  snprintf(h->jar[1], sizeof h->jar[1], "%s", j1);
  for (int s = 0; s < h->nsess; s++) {
    h->curl[s] = make_session_curl(h->jar[s]);
    if (!h->curl[s]) return -1;
  }
  return 0;
}

static void save_session_cache(RufuxMsdl *h) {
  if (!cache_enabled() || !h || h->nlangs <= 0) return;
  for (int s = 0; s < h->nsess; s++) {
    if (h->curl[s]) curl_easy_setopt(h->curl[s], CURLOPT_COOKIELIST, "FLUSH");
  }
  char meta[PATH_MAX], tmp[PATH_MAX];
  cache_paths(h->product, h->edition, meta, sizeof meta, NULL, NULL);
  snprintf(tmp, sizeof tmp, "%s.tmp.%d", meta, (int)getpid());
  int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0600);
  if (fd < 0) return;
  MsdlCacheRecord rec;
  memset(&rec, 0, sizeof rec);
  rec.magic = CACHE_MAGIC;
  rec.product = h->product;
  rec.edition = h->edition;
  rec.nsess = h->nsess;
  rec.nlangs = h->nlangs;
  rec.created_at = (int64_t)time(NULL);
  memcpy(rec.sess, h->sess, sizeof rec.sess);
  memcpy(rec.langs, h->langs, sizeof rec.langs);
  if (write(fd, &rec, sizeof rec) == (ssize_t)sizeof rec) {
    close(fd);
    rename(tmp, meta);
  } else {
    close(fd);
    unlink(tmp);
  }
}

static int open_sessions_live(RufuxMsdl *h, char *err, size_t cap) {
  const Edition *ed = &products[h->product].ed[h->edition];
  for (int s = 0; s < 2; s++) {
    if (h->curl[s]) {
      curl_easy_cleanup(h->curl[s]);
      h->curl[s] = NULL;
    }
  }
  h->nsess = ed->nids;
  h->nlangs = 0;
  h->from_cache = 0;

  if (cache_enabled()) {
    cache_paths(h->product, h->edition, NULL, 0, h->jar[0], h->jar[1]);
    h->keep_jars = 1;
    for (int s = 0; s < ed->nids; s++) {
      unlink(h->jar[s]);
      int fd = open(h->jar[s], O_WRONLY | O_CREAT | O_TRUNC, 0600);
      if (fd >= 0) close(fd);
    }
  } else {
    h->keep_jars = 0;
    for (int s = 0; s < ed->nids; s++) {
      snprintf(h->jar[s], sizeof h->jar[s], "/tmp/rufux-msdl-XXXXXX");
      int fd = mkstemp(h->jar[s]);
      if (fd < 0) {
        snprintf(err, cap, "cannot create a cookie file: %s", strerror(errno));
        return -1;
      }
      close(fd);
    }
  }

  SessWorker workers[2];
  pthread_t tids[2];
  memset(workers, 0, sizeof workers);

  for (int s = 0; s < ed->nids; s++) {
    new_guid(h->sess[s], sizeof h->sess[s]);
    h->curl[s] = make_session_curl(h->jar[s]);
    if (!h->curl[s]) {
      snprintf(err, cap, "cannot initialize HTTP client");
      return -1;
    }
    workers[s].sess_idx = s;
    workers[s].edition_id = ed->ids[s];
    snprintf(workers[s].guid, sizeof workers[s].guid, "%s", h->sess[s]);
    workers[s].curl = h->curl[s];
    workers[s].rc = -1;
  }

  if (ed->nids == 1) {
    sess_worker_fn(&workers[0]);
  } else {
    int spawned[2] = {0, 0};
    for (int s = 0; s < ed->nids; s++) {
      if (pthread_create(&tids[s], NULL, sess_worker_fn, &workers[s]) == 0)
        spawned[s] = 1;
      else
        sess_worker_fn(&workers[s]);
    }
    for (int s = 0; s < ed->nids; s++) {
      if (spawned[s]) pthread_join(tids[s], NULL);
    }
  }

  for (int s = 0; s < ed->nids; s++) {
    if (workers[s].rc != 0) {
      snprintf(err, cap, "%s", workers[s].err[0] ? workers[s].err : "failed to contact Microsoft download service");
      return -1;
    }
    for (int i = 0; i < workers[s].nskus; i++) {
      add_sku(h, s, workers[s].skus[i].lang, workers[s].skus[i].display, workers[s].skus[i].id);
    }
  }

  save_session_cache(h);
  return 0;
}

RufuxMsdl *rufux_msdl_open(int product, int edition, char *err, size_t cap) {
  if (product < 0 || product >= 2 || edition < 0 || edition >= products[product].ned) {
    snprintf(err, cap, "unknown Windows version or edition");
    return NULL;
  }
  RufuxMsdl *h = calloc(1, sizeof *h);
  if (!h) {
    snprintf(err, cap, "out of memory");
    return NULL;
  }
  h->product = product;
  h->edition = edition;

  if (load_session_cache(h, product, edition) == 0) {
    return h;
  }
  if (open_sessions_live(h, err, cap) != 0) {
    rufux_msdl_close(h);
    return NULL;
  }
  return h;
}

void rufux_msdl_close(RufuxMsdl *h) {
  if (!h) return;
  for (int s = 0; s < 2; s++) {
    if (h->curl[s]) {
      if (h->keep_jars) curl_easy_setopt(h->curl[s], CURLOPT_COOKIELIST, "FLUSH");
      curl_easy_cleanup(h->curl[s]);
      h->curl[s] = NULL;
    }
    if (!h->keep_jars && h->jar[s][0]) unlink(h->jar[s]);
  }
  free(h);
}

int rufux_msdl_language_count(const RufuxMsdl *h) { return h ? h->nlangs : 0; }
const char *rufux_msdl_language_name(const RufuxMsdl *h, int i) { return (h && i >= 0 && i < h->nlangs) ? h->langs[i].name : ""; }
const char *rufux_msdl_language_display(const RufuxMsdl *h, int i) { return (h && i >= 0 && i < h->nlangs) ? h->langs[i].display : ""; }

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

static int query_links_once(RufuxMsdl *h, int language, const char *arch_hint,
                            RufuxMsdlLink *out, int max, char *err, size_t cap) {
  char *reply = malloc(REPLY_CAP);
  if (!reply) { snprintf(err, cap, "out of memory"); return -1; }
  int n = 0;
  const Lang *l = &h->langs[language];

  int order[2] = {0, 1};
  if (l->nd == 2 && arch_hint && !strcasecmp(arch_hint, "arm64")) {
    order[0] = 1;
    order[1] = 0;
  }

  for (int idx = 0; idx < l->nd; idx++) {
    int k = order[idx];
    if (k >= l->nd) continue;
    int s = l->d[k].sess;
    char path[640], url[1024];
    snprintf(path, sizeof path,
             "/software-download-connector/api/GetProductDownloadLinksBySku?profile=" PROFILE_ID
             "&productEditionId=undefined&SKU=%s&friendlyFileName=undefined&Locale=" QUERY_LOCALE "&sessionID=%s",
             l->d[k].sku, h->sess[s]);
    build_url(url, sizeof url, "www.microsoft.com", path);
    if (http_get_curl(h->curl[s], url, REFERER, reply, REPLY_CAP, err, cap) != 0) {
      free(reply);
      return -1;
    }
    char jerr[128];
    MJ *j = mj_parse(reply, jerr, sizeof jerr);
    if (!j) {
      snprintf(err, cap, "Microsoft's service sent an unreadable reply (%s)", jerr);
      free(reply);
      return -1;
    }
    if (api_errors(j, err, cap)) {
      mj_free(j);
      free(reply);
      return -1;
    }
    const MJ *opts = mj_get(j, "ProductDownloadOptions");
    for (size_t i = 0; i < mj_len(opts) && n < max; i++) {
      const MJ *o = mj_at(opts, i);
      const char *u = mj_get_str(o, "Uri", "");
      if (strncmp(u, "https://", 8) && !(mock_base() && !strncmp(u, "http://127.0.0.1", 16))) continue;
      snprintf(out[n].arch, sizeof out[n].arch, "%s", arch_of((int)mj_get_num(o, "DownloadType", -1)));
      snprintf(out[n].url, sizeof out[n].url, "%s", u);
      n++;
    }
    mj_free(j);

    if (arch_hint && arch_hint[0]) {
      for (int m = 0; m < n; m++) {
        if (!strcasecmp(out[m].arch, arch_hint)) {
          free(reply);
          return n;
        }
      }
    }
  }
  free(reply);
  if (!n) {
    snprintf(err, cap, "Microsoft's service returned no ISO download links");
    return -1;
  }
  return n;
}

int rufux_msdl_links_for_arch(RufuxMsdl *h, int language, const char *arch_hint,
                              RufuxMsdlLink *out, int max, char *err, size_t cap) {
  if (!h || language < 0 || language >= h->nlangs) {
    snprintf(err, cap, "no such language");
    return -1;
  }
  char wanted_lang[64];
  snprintf(wanted_lang, sizeof wanted_lang, "%s", h->langs[language].name);
  int was_cached = h->from_cache;

  int n = query_links_once(h, language, arch_hint, out, max, err, cap);
  if (n < 0 && was_cached && !strstr(err, "715-123130")) {
    invalidate_cache(h->product, h->edition);
    if (open_sessions_live(h, err, cap) == 0) {
      int new_li = rufux_msdl_find_language(h, wanted_lang, err, cap);
      if (new_li >= 0) {
        n = query_links_once(h, new_li, arch_hint, out, max, err, cap);
      }
    }
  }
  return n;
}

int rufux_msdl_links(RufuxMsdl *h, int language, RufuxMsdlLink *out, int max, char *err, size_t cap) {
  return rufux_msdl_links_for_arch(h, language, NULL, out, max, err, cap);
}

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

static struct {
  unsigned long long done_bytes;
  unsigned long long total_bytes;
  double speed_bps;
  int eta_sec;
} g_dl_stats;

void rufux_msdl_get_progress_stats(unsigned long long *done_bytes, unsigned long long *total_bytes,
                                   double *speed_bps, int *eta_sec) {
  if (done_bytes) *done_bytes = g_dl_stats.done_bytes;
  if (total_bytes) *total_bytes = g_dl_stats.total_bytes;
  if (speed_bps) *speed_bps = g_dl_stats.speed_bps;
  if (eta_sec) *eta_sec = g_dl_stats.eta_sec;
}

static struct {
  char path[PATH_MAX];
  off_t size;
  time_t mtime;
  char hex[65];
} g_sha_cache;

typedef struct {
  int fd;
  CURL *curl;
  EVP_MD_CTX *sha_ctx;
  unsigned long long resume_from;
  unsigned long long written_this_run;
  int checked_status;
  int write_err;
  RufuxMsdlProgress cb;
  void *user;
  unsigned long long last_bp;
  double t_start;
  double t_last_cb;
  unsigned long long bytes_last_cb;
  double ema_speed;
} DlStreamCtx;

static double monotonic_sec(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static int seed_sha256_from_fd(int fd, EVP_MD_CTX *ctx, unsigned long long *seeded_bytes) {
  if (lseek(fd, 0, SEEK_SET) < 0) return -1;
  EVP_DigestInit_ex(ctx, EVP_sha256(), NULL);
  unsigned char buf[65536];
  unsigned long long total = 0;
  for (;;) {
    ssize_t r = read(fd, buf, sizeof buf);
    if (r < 0) {
      if (errno == EINTR) continue;
      return -1;
    }
    if (r == 0) break;
    EVP_DigestUpdate(ctx, buf, (size_t)r);
    total += (unsigned long long)r;
  }
  if (seeded_bytes) *seeded_bytes = total;
  return 0;
}

static size_t dl_write_cb(void *ptr, size_t size, size_t nmemb, void *userdata) {
  size_t n = size * nmemb;
  if (n == 0) return 0;
  DlStreamCtx *st = (DlStreamCtx *)userdata;

  if (!st->checked_status) {
    st->checked_status = 1;
    long http_code = 0;
    curl_easy_getinfo(st->curl, CURLINFO_RESPONSE_CODE, &http_code);
    if (st->resume_from > 0 && http_code == 200) {
      // Server ignored Range and sent full representation from byte 0
      if (ftruncate(st->fd, 0) == 0 && lseek(st->fd, 0, SEEK_SET) == 0) {
        st->resume_from = 0;
        if (st->sha_ctx) EVP_DigestInit_ex(st->sha_ctx, EVP_sha256(), NULL);
      }
    }
  }

  const unsigned char *p = (const unsigned char *)ptr;
  size_t rem = n;
  while (rem > 0) {
    ssize_t w = write(st->fd, p, rem);
    if (w < 0) {
      if (errno == EINTR) continue;
      st->write_err = errno;
      return 0;
    }
    p += w;
    rem -= (size_t)w;
  }
  if (st->sha_ctx) EVP_DigestUpdate(st->sha_ctx, ptr, n);
  st->written_this_run += (unsigned long long)n;
  return n;
}

static int dl_xferinfo_cb(void *clientp, curl_off_t dltotal, curl_off_t dlnow,
                          curl_off_t ultotal, curl_off_t ulnow) {
  (void)ultotal;
  (void)ulnow;
  DlStreamCtx *st = (DlStreamCtx *)clientp;
  if (!st || dltotal <= 0) return 0;

  unsigned long long done = st->resume_from + (unsigned long long)(dlnow > 0 ? dlnow : 0);
  unsigned long long total = st->resume_from + (unsigned long long)dltotal;
  if (done > total) done = total;

  double now = monotonic_sec();
  double dt = now - st->t_last_cb;
  unsigned long long bp = total ? (done * 10000ULL / total) : 0;
  if (bp > 10000ULL) bp = 10000ULL;

  if (dt >= 0.12 || bp == 10000ULL || (st->last_bp == (unsigned long long)-1 && done > 0)) {
    if (dt > 0.02 && done >= st->bytes_last_cb) {
      double inst = (double)(done - st->bytes_last_cb) / dt;
      if (st->ema_speed <= 0.0) st->ema_speed = inst;
      else st->ema_speed = 0.35 * inst + 0.65 * st->ema_speed;
    }
    double spd = st->ema_speed;
    if (spd <= 0.0 && (now - st->t_start) > 0.05) {
      spd = (double)st->written_this_run / (now - st->t_start);
    }
    int eta = -1;
    if (spd > 1024.0 && total > done) {
      double rem_s = (double)(total - done) / spd;
      if (rem_s < 864000.0) eta = (int)(rem_s + 0.5);
    }
    g_dl_stats.done_bytes = done;
    g_dl_stats.total_bytes = total;
    g_dl_stats.speed_bps = spd;
    g_dl_stats.eta_sec = eta;

    st->t_last_cb = now;
    st->bytes_last_cb = done;
    if (st->cb && bp != st->last_bp) {
      st->last_bp = bp;
      st->cb(bp, 10000ULL, st->user);
    }
  }
  return 0;
}

int rufux_msdl_fetch(const char *url, const char *dir, RufuxMsdlProgress cb, void *user,
                     char *path, size_t pathcap, char *err, size_t cap) {
  if (strncmp(url, "https://", 8) && !(mock_base() && !strncmp(url, "http://127.0.0.1", 16))) {
    snprintf(err, cap, "refusing to download over an unencrypted connection");
    return -1;
  }
  struct stat st;
  if (stat(dir, &st) != 0 || !S_ISDIR(st.st_mode)) {
    snprintf(err, cap, "'%s' is not a directory", dir);
    return -1;
  }
  char name[256];
  iso_name(url, name, sizeof name);
  char part[PATH_MAX];
  snprintf(path, pathcap, "%.3000s/%s", dir, name);
  snprintf(part, sizeof part, "%s.part", path);

  memset(&g_dl_stats, 0, sizeof g_dl_stats);
  g_dl_stats.eta_sec = -1;

  // If the completed ISO already exists (and no .part file is in progress), reuse it smoothly
  // unless RUFUX_MSDL_STRICT_EXISTS is set.
  if (access(path, F_OK) == 0) {
    const char *strict = getenv("RUFUX_MSDL_STRICT_EXISTS");
    if (strict && strict[0] && strcmp(strict, "0") != 0) {
      snprintf(err, cap, "'%s' already exists; move it away or pick another folder", path);
      return -1;
    }
    if (stat(path, &st) == 0 && st.st_size > 0 && access(part, F_OK) != 0) {
      g_dl_stats.done_bytes = (unsigned long long)st.st_size;
      g_dl_stats.total_bytes = (unsigned long long)st.st_size;
      g_dl_stats.speed_bps = 0.0;
      g_dl_stats.eta_sec = 0;
      if (cb) cb(10000ULL, 10000ULL, user);
      return 0;
    }
  }

  pthread_once(&g_curl_once, init_curl_global);
  EVP_MD_CTX *sha_ctx = EVP_MD_CTX_new();
  int ok = 0;
  char last_err[384] = {0};

  for (int attempt = 0; attempt < 4 && !ok; attempt++) {
    if (attempt > 0) sleep(2);
    int fd = open(part, O_RDWR | O_CREAT, 0644);
    if (fd < 0) {
      snprintf(err, cap, "cannot open '%s': %s", part, strerror(errno));
      if (sha_ctx) EVP_MD_CTX_free(sha_ctx);
      return -1;
    }
    unsigned long long resume_from = 0;
    if (sha_ctx) {
      if (seed_sha256_from_fd(fd, sha_ctx, &resume_from) != 0) {
        EVP_MD_CTX_free(sha_ctx);
        sha_ctx = NULL;
      }
    }
    off_t end_off = lseek(fd, 0, SEEK_END);
    if (end_off >= 0) resume_from = (unsigned long long)end_off;

    CURL *c = curl_easy_init();
    if (!c) {
      close(fd);
      if (sha_ctx) EVP_MD_CTX_free(sha_ctx);
      snprintf(err, cap, "cannot initialize libcurl");
      return -1;
    }

    DlStreamCtx ctx;
    memset(&ctx, 0, sizeof ctx);
    ctx.fd = fd;
    ctx.curl = c;
    ctx.sha_ctx = sha_ctx;
    ctx.resume_from = resume_from;
    ctx.cb = cb;
    ctx.user = user;
    ctx.last_bp = (unsigned long long)-1;
    ctx.t_start = monotonic_sec();
    ctx.t_last_cb = ctx.t_start;
    ctx.bytes_last_cb = resume_from;

    char cerr[CURL_ERROR_SIZE] = {0};
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(c, CURLOPT_URL, url);
    curl_easy_setopt(c, CURLOPT_USERAGENT, UA);
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 30L);
    curl_easy_setopt(c, CURLOPT_LOW_SPEED_LIMIT, 1024L);
    curl_easy_setopt(c, CURLOPT_LOW_SPEED_TIME, 45L);
    curl_easy_setopt(c, CURLOPT_TCP_KEEPALIVE, 1L);
    curl_easy_setopt(c, CURLOPT_BUFFERSIZE, 131072L);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, dl_write_cb);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &ctx);
    curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, dl_xferinfo_cb);
    curl_easy_setopt(c, CURLOPT_XFERINFODATA, &ctx);
    curl_easy_setopt(c, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(c, CURLOPT_ERRORBUFFER, cerr);
    if (resume_from > 0) {
      curl_easy_setopt(c, CURLOPT_RESUME_FROM_LARGE, (curl_off_t)resume_from);
    }

    CURLcode res = curl_easy_perform(c);
    long http_code = 0;
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &http_code);
    curl_easy_cleanup(c);
    close(fd);

    if (ctx.write_err != 0) {
      snprintf(err, cap, "disk write error on '%s': %s", part, strerror(ctx.write_err));
      if (sha_ctx) EVP_MD_CTX_free(sha_ctx);
      return -1;
    }

    if (res == CURLE_OK && (http_code == 200 || http_code == 206)) {
      ok = 1;
      break;
    }
    if (resume_from > 0 && http_code == 416) {
      // File on disk is already 100% downloaded
      ok = 1;
      break;
    }
    if (http_code >= 400) {
      snprintf(last_err, sizeof last_err, "HTTP %ld", http_code);
    } else {
      snprintf(last_err, sizeof last_err, "%s", cerr[0] ? cerr : curl_easy_strerror(res));
    }
  }

  if (!ok) {
    if (sha_ctx) EVP_MD_CTX_free(sha_ctx);
    snprintf(err, cap, "download failed (%s). The partial file was kept; run the same command to resume",
             last_err[0] ? last_err : "network error");
    return -1;
  }

  if (rename(part, path) != 0) {
    if (sha_ctx) EVP_MD_CTX_free(sha_ctx);
    snprintf(err, cap, "cannot rename '%s': %s", part, strerror(errno));
    return -1;
  }

  if (sha_ctx) {
    unsigned char md[32];
    unsigned ol = 0;
    if (EVP_DigestFinal_ex(sha_ctx, md, &ol) == 1 && stat(path, &st) == 0) {
      snprintf(g_sha_cache.path, sizeof g_sha_cache.path, "%s", path);
      g_sha_cache.size = st.st_size;
      g_sha_cache.mtime = st.st_mtime;
      for (int i = 0; i < 32; i++) snprintf(g_sha_cache.hex + i * 2, 3, "%02x", md[i]);
    }
    EVP_MD_CTX_free(sha_ctx);
  }

  if (stat(path, &st) == 0) {
    g_dl_stats.done_bytes = (unsigned long long)st.st_size;
    g_dl_stats.total_bytes = (unsigned long long)st.st_size;
    g_dl_stats.eta_sec = 0;
  }
  if (cb) cb(10000ULL, 10000ULL, user);
  return 0;
}

int rufux_msdl_sha256(const char *path, char hex[65]) {
  struct stat st;
  if (stat(path, &st) == 0 && g_sha_cache.path[0] &&
      strcmp(g_sha_cache.path, path) == 0 &&
      g_sha_cache.size == st.st_size &&
      g_sha_cache.mtime == st.st_mtime &&
      g_sha_cache.hex[0]) {
    memcpy(hex, g_sha_cache.hex, 65);
    return 0;
  }
  FILE *f = fopen(path, "rb");
  if (!f) return -1;
  EVP_MD_CTX *c = EVP_MD_CTX_new();
  if (!c) { fclose(f); return -1; }
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
