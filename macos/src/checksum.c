// File hashes via OpenSSL EVP (audited primitive; no hand-rolled crypto).
#include "checksum.h"
#include <openssl/evp.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

static const EVP_MD *md_for(RufuxHashAlg alg) {
  switch (alg) {
    case RUFUX_MD5: return EVP_md5();
    case RUFUX_SHA1: return EVP_sha1();
    case RUFUX_SHA512: return EVP_sha512();
    default: return EVP_sha256();
  }
}

const char *rufux_alg_name(RufuxHashAlg alg) {
  switch (alg) {
    case RUFUX_MD5: return "MD5";
    case RUFUX_SHA1: return "SHA-1";
    case RUFUX_SHA512: return "SHA-512";
    default: return "SHA-256";
  }
}

void rufux_hex(const unsigned char *in, unsigned len, char *out) {
  static const char *H = "0123456789abcdef";
  for (unsigned i = 0; i < len; i++) {
    out[2 * i] = H[in[i] >> 4];
    out[2 * i + 1] = H[in[i] & 15];
  }
  out[2 * len] = 0;
}

void rufux_hex32(const unsigned char in32[32], char out65[65]) {
  rufux_hex(in32, 32, out65);
}

int rufux_hash_file(const char *path, RufuxHashAlg alg, unsigned char *out, unsigned *outlen,
                    RufuxHashProgress cb, void *user,
                    char *err, unsigned long errcap) {
  struct stat st;
  if (stat(path, &st) != 0) {
    if (err) snprintf(err, errcap, "cannot stat '%s'", path);
    return -1;
  }
  FILE *f = fopen(path, "rb");
  if (!f) {
    if (err) snprintf(err, errcap, "cannot open '%s'", path);
    return -1;
  }
  EVP_MD_CTX *ctx = EVP_MD_CTX_new();
  if (!ctx) {
    if (err) snprintf(err, errcap, "EVP context allocation failed");
    fclose(f);
    return -1;
  }
  int rc = -1;
  unsigned char buf[1 << 16];
  size_t n;
  unsigned long long done = 0, total = (unsigned long long)st.st_size;
  unsigned int len = 0;
  if (EVP_DigestInit_ex(ctx, md_for(alg), NULL) == 1) {
    rc = 0;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) {
      if (EVP_DigestUpdate(ctx, buf, n) != 1) { rc = -1; break; }
      done += n;
      if (cb) cb(done, total, user);
    }
    if (ferror(f)) {
      if (err) snprintf(err, errcap, "read error on '%s'", path);
      rc = -1;
    }
    if (rc == 0 && EVP_DigestFinal_ex(ctx, out, &len) != 1) rc = -1;
    if (rc == 0 && len != (unsigned)alg) rc = -1;
  } else if (err) {
    snprintf(err, errcap, "EVP init failed");
  }
  EVP_MD_CTX_free(ctx);
  fclose(f);
  if (rc == 0) {
    if (outlen) *outlen = len;
    if (cb) cb(total, total, user);
  }
  return rc;
}

int rufux_sha256_file(const char *path, unsigned char out32[32],
                      RufuxHashProgress cb, void *user,
                      char *err, unsigned long errcap) {
  unsigned len = 0;
  return rufux_hash_file(path, RUFUX_SHA256, out32, &len, cb, user, err, errcap);
}
