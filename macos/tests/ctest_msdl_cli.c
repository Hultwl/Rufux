#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "msdl.h"
#include "platform_macos.h"

#define CHECK(expr) do { \
  if (!(expr)) { \
    fprintf(stderr, "CHECK FAILED at %s:%d: %s\n", __FILE__, __LINE__, #expr); \
    exit(1); \
  } \
} while (0)

static void dl_prog_cb(unsigned long long done, unsigned long long total, void *user) {
  int *calls = (int *)user;
  (*calls)++;
  CHECK(total == 10000ULL);
  CHECK(done <= total);
}

int main(int argc, char **argv) {
  rufux_macos_init_bundle_env();
  if (argc < 2) {
    fprintf(stderr, "usage: %s <ok|banned> [download_dir]\n", argv[0]);
    return 2;
  }
  const char *mode = argv[1];

  int nprod = 0;
  const RufuxMsdlProduct *prods = rufux_msdl_products(&nprod);
  CHECK(prods != NULL && nprod == 2);
  CHECK(strcmp(prods[0].name, "Windows 11") == 0);
  CHECK(prods[0].edition_count >= 1);

  char err[512] = {0};
  RufuxMsdl *h = rufux_msdl_open(0, 0, err, sizeof err);
  if (!h) {
    fprintf(stderr, "rufux_msdl_open failed: %s\n", err);
    return 1;
  }

  int nlangs = rufux_msdl_language_count(h);
  CHECK(nlangs == 5);
  CHECK(strcmp(rufux_msdl_language_name(h, 0), "English") == 0);
  CHECK(strcmp(rufux_msdl_language_display(h, 0), "English (United States)") == 0);

  int idx = rufux_msdl_find_language(h, "English (United States)", err, sizeof err);
  CHECK(idx == 0);

  int idx_fr = rufux_msdl_find_language(h, "French Canadian", err, sizeof err);
  CHECK(idx_fr >= 0);

  RufuxMsdlLink links[4];
  memset(links, 0, sizeof links);

  if (strcmp(mode, "banned") == 0) {
    int nl = rufux_msdl_links(h, idx, links, 4, err, sizeof err);
    CHECK(nl == -1);
    CHECK(strstr(err, "715-123130") != NULL);
    rufux_msdl_close(h);
    printf("banned-ok\n");
    return 0;
  }

  if (argc < 3) {
    fprintf(stderr, "missing download_dir for ok mode\n");
    rufux_msdl_close(h);
    return 2;
  }
  const char *dl_dir = argv[2];

  int nl = rufux_msdl_links(h, idx, links, 4, err, sizeof err);
  if (nl != 2) {
    fprintf(stderr, "rufux_msdl_links returned %d: %s\n", nl, err);
    rufux_msdl_close(h);
    return 1;
  }
  CHECK(strcmp(links[0].arch, "x64") == 0);
  CHECK(strcmp(links[1].arch, "ARM64") == 0);
  CHECK(strstr(links[0].url, "/files/Win11_25H2_English_x64.iso") != NULL);
  CHECK(strstr(links[1].url, "/files/Win11_25H2_English_Arm64.iso") != NULL);

  RufuxMsdlLink links_arm[4];
  memset(links_arm, 0, sizeof links_arm);
  int nl_arm = rufux_msdl_links_for_arch(h, idx, "arm64", links_arm, 4, err, sizeof err);
  CHECK(nl_arm == 1);
  CHECK(strcmp(links_arm[0].arch, "ARM64") == 0);

  int prog_calls = 0;
  char out_path[1024] = {0};
  if (rufux_msdl_fetch(links[0].url, dl_dir, dl_prog_cb, &prog_calls,
                       out_path, sizeof out_path, err, sizeof err) != 0) {
    fprintf(stderr, "rufux_msdl_fetch failed: %s\n", err);
    rufux_msdl_close(h);
    return 1;
  }
  CHECK(prog_calls > 0);

  struct stat st;
  CHECK(stat(out_path, &st) == 0);
  CHECK(st.st_size == 3 * 1024 * 1024 + 123);

  unsigned long long done_b = 0, total_b = 0;
  double spd = 0.0;
  int eta = -1;
  rufux_msdl_get_progress_stats(&done_b, &total_b, &spd, &eta);
  CHECK(done_b == (unsigned long long)st.st_size);
  CHECK(total_b == (unsigned long long)st.st_size);

  // Calling rufux_msdl_fetch again when the ISO is already complete should reuse it smoothly
  int reuse_calls = 0;
  char reuse_path[1024] = {0};
  CHECK(rufux_msdl_fetch(links[0].url, dl_dir, dl_prog_cb, &reuse_calls,
                         reuse_path, sizeof reuse_path, err, sizeof err) == 0);
  CHECK(reuse_calls == 1);
  CHECK(strcmp(reuse_path, out_path) == 0);

  char hex[65] = {0};
  CHECK(rufux_msdl_sha256(out_path, hex) == 0);
  printf("%s\n", hex);

  rufux_msdl_close(h);
  return 0;
}
