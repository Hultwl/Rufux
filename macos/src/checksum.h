#ifndef RUFUX_CHECKSUM_H
#define RUFUX_CHECKSUM_H
// File hashes via OpenSSL EVP: md5, sha1, sha256, sha512.
// cb(done,total,user) may be NULL.
typedef void (*RufuxHashProgress)(unsigned long long done,
                                  unsigned long long total, void *user);
typedef enum { RUFUX_MD5 = 16, RUFUX_SHA1 = 20, RUFUX_SHA256 = 32, RUFUX_SHA512 = 64 } RufuxHashAlg;
int rufux_hash_file(const char *path, RufuxHashAlg alg, unsigned char *out, unsigned *outlen,
                    RufuxHashProgress cb, void *user,
                    char *err, unsigned long errcap);
// Back-compat SHA-256 wrapper.
int rufux_sha256_file(const char *path, unsigned char out32[32],
                      RufuxHashProgress cb, void *user,
                      char *err, unsigned long errcap);
void rufux_hex(const unsigned char *in, unsigned len, char *out);
void rufux_hex32(const unsigned char in32[32], char out65[65]);
const char *rufux_alg_name(RufuxHashAlg alg);
#endif
