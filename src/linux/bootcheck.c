#define _GNU_SOURCE
#include "bootcheck.h"
#include "exec.h"
#include <dirent.h>
#include <limits.h>
#include <openssl/evp.h>
#include <openssl/pkcs7.h>
#include <openssl/sha.h>
#include <openssl/x509.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

// Current revocation levels, taken from the Rufus tree this port is based on
// (db.h: db_sbat_level_txt). "sbat,1,<date>" is the policy header; upper-case
// names are Microsoft security version numbers, the rest are SBAT components.
static const char sbat_level[] =
  "sbat,1,2025051000\n"
  "shim,4\n"
  "grub,5\n"
  "grub.proxmox,2\n"
  "BOOTMGRSECURITYVERSIONNUMBER,0x70000";

#define MAX_EFI (64u << 20)

typedef struct {
  const uint8_t *b;
  size_t len;
  int plus;            // PE32+
  uint32_t pe;         // offset of "PE\0\0"
  uint16_t machine, nsec;
  size_t opt, dd, secs;
  uint32_t size_of_headers;
} Pe;

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t rd32(const uint8_t *p) { return p[0] | p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

static int pe_open(Pe *pe, const uint8_t *b, size_t len) {
  memset(pe, 0, sizeof *pe);
  if (len < 0x100 || b[0] != 'M' || b[1] != 'Z') return -1;
  pe->b = b; pe->len = len;
  pe->pe = rd32(b + 0x3c);
  if ((size_t)pe->pe + 24 + 96 > len || memcmp(b + pe->pe, "PE\0\0", 4)) return -1;
  pe->machine = rd16(b + pe->pe + 4);
  pe->nsec = rd16(b + pe->pe + 6);
  uint16_t osz = rd16(b + pe->pe + 20);
  pe->opt = pe->pe + 24;
  uint16_t magic = rd16(b + pe->opt);
  if (magic != 0x10b && magic != 0x20b) return -1;
  pe->plus = magic == 0x20b;
  pe->dd = pe->opt + (pe->plus ? 112 : 96);
  pe->size_of_headers = rd32(b + pe->opt + 60);
  pe->secs = pe->opt + osz;
  if (pe->dd + 8 * 16 > len || pe->secs + (size_t)pe->nsec * 40 > len) return -1;
  return 0;
}

static const char *pe_arch(const Pe *pe) {
  switch (pe->machine) {
    case 0x8664: return "x64";
    case 0x14c: return "ia32";
    case 0x1c0: case 0x1c4: return "arm";
    case 0xaa64: return "aarch64";
    default: return "other";
  }
}

static const uint8_t *sec_hdr(const Pe *pe, unsigned i) { return pe->b + pe->secs + (size_t)i * 40; }

// Authenticode SHA-256, per the PE/COFF signing specification: everything
// except the checksum, the certificate-table directory entry and the
// certificate table itself, with sections hashed in file order.
static int pe_sha256(const Pe *pe, uint8_t out[32]) {
  size_t chk = pe->opt + 64, sec_dir = pe->dd + 4 * 8;
  uint32_t cert_off = rd32(pe->b + sec_dir), cert_size = rd32(pe->b + sec_dir + 4);
  if (pe->size_of_headers > pe->len || sec_dir + 8 > pe->size_of_headers) return -1;
  EVP_MD_CTX *c = EVP_MD_CTX_new();
  if (!c) return -1;
  EVP_DigestInit_ex(c, EVP_sha256(), NULL);
  EVP_DigestUpdate(c, pe->b, chk);
  EVP_DigestUpdate(c, pe->b + chk + 4, sec_dir - (chk + 4));
  EVP_DigestUpdate(c, pe->b + sec_dir + 8, pe->size_of_headers - (sec_dir + 8));
  size_t total = pe->size_of_headers;
  // sort section indices by raw pointer (insertion sort; nsec is small)
  unsigned idx[96];
  unsigned n = pe->nsec > 96 ? 96 : pe->nsec;
  for (unsigned i = 0; i < n; i++) {
    unsigned j = i;
    while (j > 0 && rd32(sec_hdr(pe, idx[j - 1]) + 20) > rd32(sec_hdr(pe, i) + 20)) { idx[j] = idx[j - 1]; j--; }
    idx[j] = i;
  }
  for (unsigned k = 0; k < n; k++) {
    const uint8_t *h = sec_hdr(pe, idx[k]);
    uint32_t rs = rd32(h + 16), rp = rd32(h + 20);
    if (!rs) continue;
    if ((uint64_t)rp + rs > pe->len) { EVP_MD_CTX_free(c); return -1; }
    EVP_DigestUpdate(c, pe->b + rp, rs);
    total += rs;
  }
  if (pe->len > total) {
    size_t end = pe->len;
    if (cert_size && cert_size <= end) end -= cert_size;
    (void)cert_off;
    if (end > total) EVP_DigestUpdate(c, pe->b + total, end - total);
  }
  unsigned ol = 0;
  EVP_DigestFinal_ex(c, out, &ol);
  EVP_MD_CTX_free(c);
  return 0;
}

// Raw contents of a section, NUL-terminated copy. Caller frees.
static char *pe_section_text(const Pe *pe, const char *name) {
  for (unsigned i = 0; i < pe->nsec; i++) {
    const uint8_t *h = sec_hdr(pe, i);
    if (strncmp((const char *)h, name, 8)) continue;
    uint32_t vs = rd32(h + 8), rs = rd32(h + 16), rp = rd32(h + 20);
    uint32_t n = vs && vs < rs ? vs : rs;
    if ((uint64_t)rp + n > pe->len) return NULL;
    char *s = malloc((size_t)n + 1);
    if (!s) return NULL;
    memcpy(s, pe->b + rp, n);
    s[n] = 0;
    return s;
  }
  return NULL;
}

static int rva_to_off(const Pe *pe, uint32_t rva, size_t *off) {
  for (unsigned i = 0; i < pe->nsec; i++) {
    const uint8_t *h = sec_hdr(pe, i);
    uint32_t va = rd32(h + 12), vs = rd32(h + 8), rs = rd32(h + 16), rp = rd32(h + 20);
    uint32_t span = vs > rs ? vs : rs;
    if (rva >= va && rva - va < span) { *off = (size_t)rp + (rva - va); return *off < pe->len ? 0 : -1; }
  }
  return -1;
}

// Resource tree search for a named entry (UTF-16 name compare); returns the RVA
// and size of the first data leaf below it.
static int rsrc_find(const Pe *pe, size_t root, size_t dir, const char *name, int depth,
                     int inside, uint32_t *rva, uint32_t *size) {
  if (depth > 4 || dir + 16 > pe->len) return 0;
  unsigned named = rd16(pe->b + dir + 12), ids = rd16(pe->b + dir + 14);
  if (named + ids > 512) return 0;
  for (unsigned i = 0; i < named + ids; i++) {
    size_t e = dir + 16 + (size_t)i * 8;
    if (e + 8 > pe->len) return 0;
    uint32_t nm = rd32(pe->b + e), of = rd32(pe->b + e + 4);
    int match = inside;
    if (!inside && (nm & 0x80000000u)) {
      size_t so = root + (nm & 0x7fffffffu);
      if (so + 2 <= pe->len) {
        unsigned l = rd16(pe->b + so);
        if (l == strlen(name) && so + 2 + (size_t)l * 2 <= pe->len) {
          match = 1;
          for (unsigned k = 0; k < l; k++)
            if (rd16(pe->b + so + 2 + (size_t)k * 2) != (unsigned char)name[k]) { match = 0; break; }
        }
      }
    }
    if (of & 0x80000000u) {
      if (rsrc_find(pe, root, root + (of & 0x7fffffffu), name, depth + 1, match, rva, size)) return 1;
    } else if (match) {
      size_t de = root + of;
      if (de + 16 > pe->len) return 0;
      *rva = rd32(pe->b + de);
      *size = rd32(pe->b + de + 4);
      return 1;
    }
  }
  return 0;
}

// ---- policy -------------------------------------------------------------
typedef struct { char product[64]; uint32_t version; } Level;

static int load_levels(Level *lv, int max) {
  char *txt = strdup(sbat_level), *save = NULL;
  int n = 0;
  for (char *ln = strtok_r(txt, "\n", &save); ln && n < max; ln = strtok_r(NULL, "\n", &save)) {
    char *c = strchr(ln, ',');
    if (!c) continue;
    *c++ = 0;
    snprintf(lv[n].product, sizeof lv[n].product, "%s", ln);
    lv[n].version = (uint32_t)strtoul(c, NULL, 0);
    n++;
  }
  free(txt);
  return n;
}

static int is_upper(const char *s) {
  for (; *s; s++) if (*s < 'A' || *s > 'Z') return 0;
  return 1;
}

static int revoked_by_sbat(const Pe *pe, char *why, size_t cap) {
  char *sb = pe_section_text(pe, ".sbat");
  if (!sb) return 0;
  Level lv[16];
  int nl = load_levels(lv, 16), hit = 0;
  char *save = NULL;
  for (char *ln = strtok_r(sb, "\n", &save); ln && !hit; ln = strtok_r(NULL, "\n", &save)) {
    char *c = strchr(ln, ',');
    if (!c) continue;
    *c++ = 0;
    uint32_t ver = (uint32_t)strtoul(c, NULL, 10);
    for (int i = 0; i < nl; i++)
      if (!is_upper(lv[i].product) && !strcmp(ln, lv[i].product) && ver < lv[i].version) {
        snprintf(why, cap, "SBAT: '%s' generation %u is below the current minimum %u", ln, ver, lv[i].version);
        hit = 1;
        break;
      }
  }
  free(sb);
  return hit;
}

static int revoked_by_svn(const Pe *pe, char *why, size_t cap) {
  Level lv[16];
  int nl = load_levels(lv, 16);
  uint32_t rr = rd32(pe->b + pe->dd + 2 * 8), rs_ = rd32(pe->b + pe->dd + 2 * 8 + 4);
  size_t root;
  if (!rr || !rs_ || rva_to_off(pe, rr, &root) != 0) return 0;
  for (int i = 0; i < nl; i++) {
    if (!is_upper(lv[i].product)) continue;
    uint32_t rva, size;
    if (!rsrc_find(pe, root, root, lv[i].product, 0, 0, &rva, &size) || size != 4) continue;
    size_t o;
    if (rva_to_off(pe, rva, &o) != 0 || o + 4 > pe->len) continue;
    uint32_t v = rd32(pe->b + o);
    if (v < lv[i].version) {
      snprintf(why, cap, "boot manager security version %u.%u is below the required %u.%u",
               v >> 16, v & 0xffff, lv[i].version >> 16, lv[i].version & 0xffff);
      return 1;
    }
  }
  return 0;
}

// ---- DBX ----------------------------------------------------------------
static const uint8_t GUID_SHA256[16] = {0x26,0x16,0xc4,0xc1,0x4c,0x50,0x92,0x40,0xac,0xa9,0x41,0xf9,0x36,0x93,0x43,0x28};
static const uint8_t GUID_X509_SHA256[16] = {0x92,0xa4,0xd2,0x3b,0xc0,0x96,0x79,0x40,0xb4,0x20,0xfc,0xf9,0x8e,0xf1,0x03,0xed};

static uint8_t *read_all(const char *path, size_t *len, size_t max) {
  FILE *f = fopen(path, "rb");
  if (!f) return NULL;
  fseek(f, 0, SEEK_END);
  long n = ftell(f);
  fseek(f, 0, SEEK_SET);
  if (n <= 0 || (size_t)n > max) { fclose(f); return NULL; }
  uint8_t *b = malloc((size_t)n + 1);
  if (b && fread(b, 1, (size_t)n, f) != (size_t)n) { free(b); b = NULL; }
  fclose(f);
  if (b) { b[n] = 0; *len = (size_t)n; }
  return b;
}

static const char *dbx_dir(void) {
  static char path[1152];
  const char *env = getenv("RUFUX_DBX_DIR");
  const char *ed = rufux_exe_dir();
  char c[5][1152];
  int n = 0;
  if (env && env[0]) snprintf(c[n++], sizeof c[0], "%s", env);
  if (ed[0]) {
    snprintf(c[n++], sizeof c[0], "%s/../share/rufux/dbx", ed);
    snprintf(c[n++], sizeof c[0], "%s/../res/dbx", ed);
  }
  snprintf(c[n++], sizeof c[0], "/usr/share/rufux/dbx");
  snprintf(c[n++], sizeof c[0], "/usr/local/share/rufux/dbx");
  for (int i = 0; i < n; i++) {
    DIR *d = opendir(c[i]);
    if (d) { closedir(d); snprintf(path, sizeof path, "%s", c[i]); return path; }
  }
  return NULL;
}

// 1 when `hash` is a SHA-256 entry, or `tbs` (may be NULL) an X509-SHA256 entry, of the DBX.
static int dbx_has(const char *arch, const uint8_t hash[32], const uint8_t tbs[][32], int ntbs, char *what, size_t cap) {
  const char *dir = dbx_dir();
  if (!dir || !strcmp(arch, "other")) return 0;
  char p[1300];
  snprintf(p, sizeof p, "%s/dbx_%s.efiauth2", dir, arch);
  size_t len;
  uint8_t *d = read_all(p, &len, 8u << 20);
  if (!d) return 0;
  int hit = 0;
  if (len > 16 + 24) {
    // EFI_TIME (16) + WIN_CERTIFICATE_UEFI_GUID whose dwLength covers the whole certificate
    size_t off = 16 + (size_t)rd32(d + 16);
    while (!hit && off + 28 <= len) {
      const uint8_t *g = d + off;
      uint32_t lsz = rd32(g + 16), hsz = rd32(g + 20), ssz = rd32(g + 24);
      if (lsz < 28 || off + lsz > len || ssz < 16 || (size_t)hsz + 28 > lsz) break;
      size_t cnt = (lsz - 28 - hsz) / ssz;
      const uint8_t *e = g + 28 + hsz;
      int is_sha = !memcmp(g, GUID_SHA256, 16), is_x509 = !memcmp(g, GUID_X509_SHA256, 16);
      for (size_t i = 0; i < cnt && !hit; i++, e += ssz) {
        if ((is_sha || is_x509) && ssz >= 48 - (is_sha ? 16 : 0)) {
          const uint8_t *v = e + 16;
          if (is_sha && !memcmp(v, hash, 32)) { snprintf(what, cap, "its hash is in the UEFI DBX (revoked binary)"); hit = 1; }
          for (int t = 0; is_x509 && !hit && t < ntbs; t++)
            if (!memcmp(v, tbs[t], 32)) { snprintf(what, cap, "a certificate in its signature chain is revoked by the UEFI DBX"); hit = 1; }
        }
      }
      off += lsz;
    }
  }
  free(d);
  return hit;
}

// ---- signature ------------------------------------------------------------
typedef struct { char issuer[128]; uint8_t tbs[16][32]; int ntbs; int signed_; } Sig;

static void cn_of(X509_NAME *n, char *out, size_t cap) {
  out[0] = 0;
  if (!n || X509_NAME_get_text_by_NID(n, NID_commonName, out, (int)cap) < 0) out[0] = 0;
}

static void read_sig(const Pe *pe, Sig *s) {
  memset(s, 0, sizeof *s);
  size_t sd = pe->dd + 4 * 8;
  uint32_t off = rd32(pe->b + sd), sz = rd32(pe->b + sd + 4);
  if (!off || sz < 16 || (uint64_t)off + sz > pe->len) return;
  const uint8_t *w = pe->b + off;
  uint32_t dl = rd32(w);
  if (dl < 8 || dl > sz || rd16(w + 6) != 0x0002) return;
  const unsigned char *p = w + 8;
  PKCS7 *p7 = d2i_PKCS7(NULL, &p, (long)(dl - 8));
  if (!p7) return;
  if (PKCS7_type_is_signed(p7) && p7->d.sign) {
    s->signed_ = 1;
    STACK_OF(X509) *bundle = p7->d.sign->cert;
    for (int i = 0; bundle && i < sk_X509_num(bundle) && s->ntbs < 16; i++) {
      unsigned char *tbs = NULL;
      int tl = i2d_re_X509_tbs(sk_X509_value(bundle, i), &tbs);
      if (tl > 0) {
        unsigned ol = 0;
        EVP_Digest(tbs, (size_t)tl, s->tbs[s->ntbs++], &ol, EVP_sha256(), NULL);
        OPENSSL_free(tbs);
      }
    }
    STACK_OF(X509) *sg = PKCS7_get0_signers(p7, NULL, 0);
    if (sg && sk_X509_num(sg) > 0) cn_of(X509_get_issuer_name(sk_X509_value(sg, 0)), s->issuer, sizeof s->issuer);
    sk_X509_free(sg);
  }
  PKCS7_free(p7);
}

const char *rufux_bootcheck_state_name(RufuxBootState s) {
  switch (s) {
    case RUFUX_BOOT_NOTPE: return "not-efi";
    case RUFUX_BOOT_UNSIGNED: return "unsigned";
    case RUFUX_BOOT_OK: return "ok";
    case RUFUX_BOOT_REVOKED_DBX: return "revoked-dbx";
    case RUFUX_BOOT_REVOKED_SBAT: return "revoked-sbat";
    case RUFUX_BOOT_REVOKED_SVN: return "revoked-svn";
  }
  return "?";
}

int rufux_bootcheck_file(const char *path, RufuxBootCheck *o) {
  memset(o, 0, sizeof *o);
  size_t len;
  uint8_t *b = read_all(path, &len, MAX_EFI);
  if (!b) return -1;
  Pe pe;
  if (pe_open(&pe, b, len) != 0) { o->state = RUFUX_BOOT_NOTPE; free(b); return 0; }
  snprintf(o->arch, sizeof o->arch, "%s", pe_arch(&pe));
  Sig sig;
  read_sig(&pe, &sig);
  if (!sig.signed_) { o->state = RUFUX_BOOT_UNSIGNED; free(b); return 0; }
  snprintf(o->signer, sizeof o->signer, "%s", sig.issuer);
  o->state = RUFUX_BOOT_OK;
  uint8_t h[32];
  if (pe_sha256(&pe, h) != 0) { free(b); return -1; }
  if (dbx_has(o->arch, h, (const uint8_t (*)[32])sig.tbs, sig.ntbs, o->detail, sizeof o->detail))
    o->state = RUFUX_BOOT_REVOKED_DBX;
  else if (revoked_by_sbat(&pe, o->detail, sizeof o->detail))
    o->state = RUFUX_BOOT_REVOKED_SBAT;
  else if (revoked_by_svn(&pe, o->detail, sizeof o->detail))
    o->state = RUFUX_BOOT_REVOKED_SVN;
  else if (!strcmp(sig.issuer, "Microsoft Windows Production PCA 2011"))
    snprintf(o->detail, sizeof o->detail, "may fail Secure Boot on systems updated to the 'Windows UEFI CA 2023' certificate");
  else if (!strcmp(sig.issuer, "Windows UEFI CA 2023"))
    snprintf(o->detail, sizeof o->detail, "will fail Secure Boot on systems that have not received the 2023 certificates");
  free(b);
  return 0;
}

// ---- tree walk ------------------------------------------------------------
static int has_ext(const char *n, const char *ext) {
  size_t l = strlen(n), e = strlen(ext);
  return l > e && !strcasecmp(n + l - e, ext);
}

static int walk(const char *dir, int depth, int *checked, void (*log)(const char *, void *), void *lu) {
  if (depth > 8 || *checked > 256) return 0;
  DIR *d = opendir(dir);
  if (!d) return 0;
  int revoked = 0;
  struct dirent *de;
  while ((de = readdir(d)) != NULL) {
    if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
    char p[PATH_MAX];
    snprintf(p, sizeof p, "%s/%s", dir, de->d_name);
    struct stat st;
    if (lstat(p, &st) != 0) continue;
    if (S_ISDIR(st.st_mode)) { revoked += walk(p, depth + 1, checked, log, lu); continue; }
    if (!S_ISREG(st.st_mode) || !has_ext(de->d_name, ".efi")) continue;
    RufuxBootCheck c;
    if (rufux_bootcheck_file(p, &c) != 0 || c.state == RUFUX_BOOT_NOTPE) continue;
    (*checked)++;
    const char *rel = p + strlen(p) - strlen(de->d_name);
    char m[700];
    if (c.state >= RUFUX_BOOT_REVOKED_DBX) {
      revoked++;
      snprintf(m, sizeof m, "WARNING: revoked UEFI bootloader '%s' (%s): %s. "
               "Up-to-date Secure Boot firmware will refuse to run it.", rel, c.arch, c.detail);
    } else if (c.state == RUFUX_BOOT_UNSIGNED) {
      snprintf(m, sizeof m, "UEFI bootloader '%s' (%s) is unsigned: it needs Secure Boot turned off.", rel, c.arch);
    } else {
      snprintf(m, sizeof m, "UEFI bootloader '%s' (%s): signed by '%s', not revoked.%s%s", rel, c.arch,
               c.signer, c.detail[0] ? " Note: " : "", c.detail);
    }
    if (log) log(m, lu);
  }
  closedir(d);
  return revoked;
}

int rufux_bootcheck_tree(const char *root, void (*log)(const char *, void *), void *luser) {
  int checked = 0;
  int r = walk(root, 0, &checked, log, luser);
  if (!checked && log) log("UEFI bootloader check: no .efi files found.", luser);
  return r;
}
