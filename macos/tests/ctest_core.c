#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "bootcheck.h"
#include "checksum.h"
#include "exec.h"
#include "i18n.h"
#include "iso_probe.h"
#include "mjson.h"
#include "platform_macos.h"
#include "priv.h"
#include "secureboot.h"
#include "smart.h"
#include "update.h"
#include "vhd.h"

#define CHECK(expr) do { \
  if (!(expr)) { \
    fprintf(stderr, "CHECK FAILED at %s:%d: %s\n", __FILE__, __LINE__, #expr); \
    exit(1); \
  } \
} while (0)

static void make_tmpdir(char *out, size_t cap) {
  snprintf(out, cap, "/tmp/rufux-ctest-core-XXXXXX");
  char *d = mkdtemp(out);
  CHECK(d != NULL);
}

static void rm_rf(const char *dir) {
  const char *av[] = {"/bin/rm", "-rf", dir, NULL};
  rufux_run(av, 0);
}

static void test_exe_dir_and_io_helpers(const char *tmpdir) {
  rufux_macos_init_bundle_env();
  const char *ed = rufux_exe_dir();
  CHECK(ed != NULL);
  CHECK(ed[0] == '/');
  struct stat st;
  CHECK(stat(ed, &st) == 0);
  CHECK(S_ISDIR(st.st_mode));

  char io_path[512];
  snprintf(io_path, sizeof io_path, "%s/io.bin", tmpdir);
  int fd = open(io_path, O_CREAT | O_RDWR | O_TRUNC, 0600);
  CHECK(fd >= 0);

  unsigned char wbuf[1024];
  for (size_t i = 0; i < sizeof wbuf; i++) wbuf[i] = (unsigned char)(i & 0xFF);
  CHECK(rufux_pwrite_all(fd, wbuf, sizeof wbuf, 512) == 0);
  CHECK(lseek(fd, 512, SEEK_SET) == 512);

  unsigned char rbuf[1024] = {0};
  ssize_t got = rufux_read_full(fd, rbuf, sizeof rbuf);
  CHECK(got == (ssize_t)sizeof rbuf);
  CHECK(memcmp(wbuf, rbuf, sizeof wbuf) == 0);
  close(fd);
}

static int ends_with(const char *s, const char *suffix) {
  size_t ls = strlen(s), le = strlen(suffix);
  return ls >= le && strcmp(s + ls - le, suffix) == 0;
}

static void test_tool_resolution_and_exec(void) {
  rufux_macos_init_bundle_env();

  CHECK(rufux_have("7z") == 1);
  CHECK(rufux_have("mkfs.ntfs") == 1);
  CHECK(rufux_have("mkfs.ext4") == 1);
  CHECK(rufux_have("mkfs.ext3") == 1);
  CHECK(rufux_have("mkfs.ext2") == 1);
  CHECK(rufux_have("grub-mkimage") == 1);
  CHECK(rufux_have("nonexistent-rufux-tool-xyz-999") == 0);

  const char *p7z = rufux_resolve_tool("7z");
  CHECK(p7z != NULL && access(p7z, X_OK) == 0);
  CHECK(ends_with(p7z, "/7zz") || ends_with(p7z, "/7z"));

  const char *pntfs = rufux_resolve_tool("mkfs.ntfs");
  CHECK(pntfs != NULL && access(pntfs, X_OK) == 0);
  CHECK(ends_with(pntfs, "/mkntfs") || ends_with(pntfs, "/mkfs.ntfs"));

  const char *pext4 = rufux_resolve_tool("mkfs.ext4");
  CHECK(pext4 != NULL && access(pext4, X_OK) == 0);
  CHECK(ends_with(pext4, "/mke2fs") || ends_with(pext4, "/mkfs.ext4"));

  const char *pgrub = rufux_resolve_tool("grub-mkimage");
  CHECK(pgrub != NULL && access(pgrub, X_OK) == 0);
  CHECK(ends_with(pgrub, "/i686-elf-grub-mkimage") || ends_with(pgrub, "/grub-mkimage"));

  // Verify rufux_capture executes aliased tools cleanly via resolved binary path.
  char out[8192] = {0};
  const char *av_7z[] = {"7z", NULL};
  CHECK(rufux_capture(av_7z, out, sizeof out) == 0);
  CHECK(strstr(out, "7-Zip") != NULL);

  const char *av_ntfs[] = {"mkfs.ntfs", "--version", NULL};
  CHECK(rufux_capture(av_ntfs, out, sizeof out) == 0);
  CHECK(strstr(out, "mkntfs") != NULL);

  const char *av_dry[] = {"mkfs.ntfs", "--this-flag-is-not-run", NULL};
  CHECK(rufux_run(av_dry, 1) == 0);
}

static void test_mkfs_ext4_personality_via_mke2fs_only(const char *tmpdir) {
  rufux_macos_init_bundle_env();
  const char *real_mke2fs = rufux_resolve_tool("mke2fs");
  CHECK(real_mke2fs != NULL);

  char bin_dir[512], stripped_mke2fs[540], img_path[540];
  snprintf(bin_dir, sizeof bin_dir, "%s/only_mke2fs_bin", tmpdir);
  snprintf(stripped_mke2fs, sizeof stripped_mke2fs, "%s/mke2fs", bin_dir);
  snprintf(img_path, sizeof img_path, "%s/ext4_personality.img", tmpdir);
  CHECK(mkdir(bin_dir, 0755) == 0);

  const char *cp_av[] = {"/bin/cp", real_mke2fs, stripped_mke2fs, NULL};
  CHECK(rufux_run(cp_av, 0) == 0);
  CHECK(chmod(stripped_mke2fs, 0755) == 0);

  int fd = open(img_path, O_CREAT | O_RDWR | O_TRUNC, 0600);
  CHECK(fd >= 0);
  CHECK(ftruncate(fd, 16 * 1024 * 1024) == 0);
  close(fd);

  const char *old_path_env = getenv("PATH");
  char *saved_path = strdup(old_path_env ? old_path_env : "");
  CHECK(saved_path != NULL);

  char restricted_path[1024];
  snprintf(restricted_path, sizeof restricted_path, "%s:/usr/bin:/bin", bin_dir);
  CHECK(setenv("PATH", restricted_path, 1) == 0);

  const char *resolved = rufux_resolve_tool("mkfs.ext4");
  CHECK(resolved != NULL && strcmp(resolved, stripped_mke2fs) == 0);

  // Omit -t on purpose (matching persist.c): must still format genuine ext4, not ext2.
  const char *mk_av[] = {"mkfs.ext4", "-F", img_path, NULL};
  char out[8192] = {0};
  int rc = rufux_capture(mk_av, out, sizeof out);

  CHECK(setenv("PATH", saved_path, 1) == 0);
  free(saved_path);
  CHECK(rc == 0);

  fd = open(img_path, O_RDONLY);
  CHECK(fd >= 0);
  unsigned char sb[1024] = {0};
  CHECK(lseek(fd, 1024, SEEK_SET) == 1024);
  CHECK(rufux_read_full(fd, sb, sizeof sb) == (ssize_t)sizeof sb);
  close(fd);

  uint16_t magic = (uint16_t)sb[0x38] | ((uint16_t)sb[0x39] << 8);
  uint32_t compat = (uint32_t)sb[0x5C] | ((uint32_t)sb[0x5D] << 8) |
                    ((uint32_t)sb[0x5E] << 16) | ((uint32_t)sb[0x5F] << 24);
  uint32_t incompat = (uint32_t)sb[0x60] | ((uint32_t)sb[0x61] << 8) |
                      ((uint32_t)sb[0x62] << 16) | ((uint32_t)sb[0x63] << 24);
  CHECK(magic == 0xEF53u);
  // EXT3_FEATURE_COMPAT_HAS_JOURNAL (0x0004) and EXT4_FEATURE_INCOMPAT_EXTENTS (0x0040)
  CHECK((compat & 0x0004u) != 0);
  CHECK((incompat & 0x0040u) != 0);
}

static void hash_prog_cb(unsigned long long done, unsigned long long total, void *user) {
  int *calls = (int *)user;
  (*calls)++;
  CHECK(done <= total);
}

static void test_checksum(const char *tmpdir) {
  char p[512];
  snprintf(p, sizeof p, "%s/abc.txt", tmpdir);
  FILE *f = fopen(p, "wb");
  CHECK(f != NULL);
  CHECK(fwrite("abc", 1, 3, f) == 3);
  fclose(f);

  unsigned char md[64] = {0};
  unsigned len = 0;
  int calls = 0;
  char err[128] = {0};
  CHECK(rufux_hash_file(p, RUFUX_SHA256, md, &len, hash_prog_cb, &calls, err, sizeof err) == 0);
  CHECK(len == 32);
  CHECK(calls > 0);

  char hex[130] = {0};
  rufux_hex32(md, hex);
  CHECK(strcmp(hex, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad") == 0);

  CHECK(rufux_hash_file(p, RUFUX_MD5, md, &len, NULL, NULL, err, sizeof err) == 0);
  CHECK(len == 16);
  rufux_hex(md, len, hex);
  CHECK(strcmp(hex, "900150983cd24fb0d6963f7d28e17f72") == 0);

  CHECK(strcmp(rufux_alg_name(RUFUX_SHA256), "SHA-256") == 0);
}

static void write_synthetic_vhd(const char *path, int corrupt_checksum) {
  const size_t payload = 1048576; // 1 MiB
  unsigned char *zeros = calloc(1, payload);
  CHECK(zeros != NULL);

  unsigned char foot[512] = {0};
  memcpy(foot, "conectix", 8);
  foot[11] = 0x02;
  foot[13] = 0x01;
  memset(foot + 16, 0xFF, 8);
  foot[45] = 0x10;
  foot[53] = 0x10;
  foot[63] = 2;

  unsigned sum = 0;
  for (int i = 0; i < 512; i++) sum += foot[i];
  unsigned chk = ~sum & 0xFFFFFFFFu;
  if (corrupt_checksum) chk ^= 0x12345678u;
  foot[64] = (unsigned char)((chk >> 24) & 0xFF);
  foot[65] = (unsigned char)((chk >> 16) & 0xFF);
  foot[66] = (unsigned char)((chk >> 8) & 0xFF);
  foot[67] = (unsigned char)(chk & 0xFF);

  FILE *f = fopen(path, "wb");
  CHECK(f != NULL);
  CHECK(fwrite(zeros, 1, payload, f) == payload);
  CHECK(fwrite(foot, 1, sizeof foot, f) == sizeof foot);
  fclose(f);
  free(zeros);
}

static void test_vhd_probe(const char *tmpdir) {
  char good_vhd[512], bad_vhd[512];
  snprintf(good_vhd, sizeof good_vhd, "%s/fixed.vhd", tmpdir);
  snprintf(bad_vhd, sizeof bad_vhd, "%s/corrupt.vhd", tmpdir);

  write_synthetic_vhd(good_vhd, 0);
  RufuxVhdInfo info;
  CHECK(rufux_vhd_probe(good_vhd, &info) == 0);
  CHECK(info.is_vhd == 1);
  CHECK(info.is_fixed == 1);
  CHECK(info.payload_bytes == 1048576ULL);
  CHECK(strcmp(info.kind, "fixed") == 0);

  RufuxWriteOpts wopts = {0};
  char err[256] = {0};
  CHECK(rufux_vhd_adjust(good_vhd, &wopts, NULL, NULL, err, sizeof err) == 0);
  CHECK(wopts.src_len == 1048576ULL);

  write_synthetic_vhd(bad_vhd, 1);
  CHECK(rufux_vhd_probe(bad_vhd, &info) == 0);
  CHECK(info.is_vhd == 1);
  CHECK(info.is_fixed == 0);
  CHECK(strcmp(info.kind, "corrupt") == 0);
  CHECK(rufux_vhd_adjust(bad_vhd, &wopts, NULL, NULL, err, sizeof err) == -1);
}

static void write_synthetic_pe(const char *path, uint16_t subsystem) {
  unsigned char img[1024] = {0};
  img[0] = 'M';
  img[1] = 'Z';
  uint32_t pe_off = 0x80;
  memcpy(img + 0x3C, &pe_off, 4);
  memcpy(img + pe_off, "PE\0\0", 4);
  img[pe_off + 4] = 0x64;
  img[pe_off + 5] = 0x86;
  img[pe_off + 6] = 1;
  img[pe_off + 20] = 0xF0;
  size_t opt = pe_off + 24;
  img[opt + 0] = 0x0B;
  img[opt + 1] = 0x02;
  img[opt + 60] = 0x00;
  img[opt + 61] = 0x02;
  img[opt + 68] = (unsigned char)(subsystem & 0xFF);
  img[opt + 69] = (unsigned char)((subsystem >> 8) & 0xFF);
  size_t sh = opt + 240;
  memcpy(img + sh, ".text", 5);
  img[sh + 8] = 0x01;
  img[sh + 12] = 0x00; img[sh + 13] = 0x10;
  img[sh + 16] = 0x00; img[sh + 17] = 0x02;
  img[sh + 20] = 0x00; img[sh + 21] = 0x02;
  img[512] = 0xC3;

  FILE *f = fopen(path, "wb");
  CHECK(f != NULL);
  CHECK(fwrite(img, 1, sizeof img, f) == sizeof img);
  fclose(f);
}

static void test_validate_efi_and_bootcheck(const char *tmpdir) {
  char efi_ok[512], efi_bad[512];
  snprintf(efi_ok, sizeof efi_ok, "%s/synth_efi.efi", tmpdir);
  snprintf(efi_bad, sizeof efi_bad, "%s/synth_gui.exe", tmpdir);

  write_synthetic_pe(efi_ok, 10);
  write_synthetic_pe(efi_bad, 2);

  unsigned sub = 0;
  char err[256] = {0};
  CHECK(rufux_validate_efi(efi_ok, &sub, err, sizeof err) == 0);
  CHECK(sub == 10);

  sub = 0;
  CHECK(rufux_validate_efi(efi_bad, &sub, err, sizeof err) == -1);
  CHECK(sub == 2);

  RufuxBootCheck bc;
  CHECK(rufux_bootcheck_file(efi_ok, &bc) == 0);
  CHECK(bc.state == RUFUX_BOOT_UNSIGNED);
  CHECK(strcmp(bc.arch, "x64") == 0);

  // Extract real bootx64.efi from ../res/uefi/uefi-ntfs.img and validate it.
  char uefi_img[1024];
  snprintf(uefi_img, sizeof uefi_img, "%s/../../res/uefi/uefi-ntfs.img", rufux_exe_dir());
  CHECK(access(uefi_img, R_OK) == 0);

  char out_opt[560];
  snprintf(out_opt, sizeof out_opt, "-o%s/uefi_ext", tmpdir);
  const char *av[] = {"7z", "e", "-y", out_opt, uefi_img, "EFI/Boot/bootx64.efi", NULL};
  char cap[4096] = {0};
  CHECK(rufux_capture(av, cap, sizeof cap) == 0);

  char real_efi[560];
  snprintf(real_efi, sizeof real_efi, "%s/uefi_ext/bootx64.efi", tmpdir);
  CHECK(access(real_efi, R_OK) == 0);
  sub = 0;
  CHECK(rufux_validate_efi(real_efi, &sub, err, sizeof err) == 0);
  CHECK(sub == 10);

  CHECK(rufux_bootcheck_file(real_efi, &bc) == 0);
  CHECK(bc.state == RUFUX_BOOT_OK || bc.state == RUFUX_BOOT_UNSIGNED);
  CHECK(strcmp(bc.arch, "x64") == 0);
}

static void test_secureboot_state(void) {
  RufuxSbState s = rufux_sb_state();
  CHECK(s == RUFUX_SB_ENABLED || s == RUFUX_SB_DISABLED || s == RUFUX_SB_UNKNOWN);
  const char *name = rufux_sb_string(s);
  CHECK(strcmp(name, "enabled") == 0 || strcmp(name, "disabled") == 0 ||
        strcmp(name, "unknown") == 0);

  char sb_src[1024];
  snprintf(sb_src, sizeof sb_src, "%s/../src/secureboot.c", rufux_exe_dir());
  FILE *f = fopen(sb_src, "rb");
  CHECK(f != NULL);
  char buf[16384] = {0};
  size_t n = fread(buf, 1, sizeof buf - 1, f);
  fclose(f);
  CHECK(n > 0);
  CHECK(strstr(buf, "/sys/firmware/efi") == NULL);
}

static void test_mjson_iso_priv_smart_i18n(const char *tmpdir) {
  rufux_i18n_init();
  CHECK(strcmp(_("rufux"), "rufux") == 0);

  char jerr[128] = {0};
  MJ *j = mj_parse("{\"ok\":true,\"num\":2.5,\"s\":\"hi\",\"arr\":[1,2]}", jerr, sizeof jerr);
  CHECK(j != NULL);
  CHECK(mj_bool(mj_get(j, "ok")) == 1);
  CHECK(mj_get_num(j, "num", 0.0) == 2.5);
  CHECK(strcmp(mj_get_str(j, "s", ""), "hi") == 0);
  CHECK(mj_len(mj_get(j, "arr")) == 2);
  mj_free(j);

  char iso_path[512];
  snprintf(iso_path, sizeof iso_path, "%s/test.iso", tmpdir);
  unsigned char *iso = calloc(32, 2048);
  CHECK(iso != NULL);
  unsigned char *pvd = iso + 16 * 2048;
  pvd[0] = 1;
  memcpy(pvd + 1, "CD001", 5);
  memset(pvd + 40, ' ', 32);
  memcpy(pvd + 40, "MACOS_ISO", 9);

  unsigned char *br = iso + 17 * 2048;
  br[0] = 0;
  memcpy(br + 1, "CD001", 5);
  memcpy(br + 7, "EL TORITO SPECIFICATION", 23);
  br[0x47] = 18;

  unsigned char *cat = iso + 18 * 2048;
  cat[0] = 0x01;
  cat[1] = 0xEF;

  FILE *f = fopen(iso_path, "wb");
  CHECK(f != NULL);
  CHECK(fwrite(iso, 1, 32 * 2048, f) == 32 * 2048);
  fclose(f);
  free(iso);

  RufuxIsoInfo ii;
  CHECK(rufux_probe_iso_detail(iso_path, &ii) == 0);
  CHECK(ii.valid_iso == 1);
  CHECK(strcmp(ii.label, "MACOS_ISO") == 0);
  CHECK(ii.bootable == 1);
  CHECK(ii.has_efi == 1);
  CHECK(ii.is_udf == 0);
  CHECK(ii.is_windows == 0);

  // Test rufux_is_windows_iso positive detection via bsdtar archive containing sources/install.wim
  char win_stage[512], win_src_dir[540], win_wim[560], win_arc[512];
  snprintf(win_stage, sizeof win_stage, "%s/winstage", tmpdir);
  snprintf(win_src_dir, sizeof win_src_dir, "%s/winstage/sources", tmpdir);
  snprintf(win_wim, sizeof win_wim, "%s/winstage/sources/install.wim", tmpdir);
  snprintf(win_arc, sizeof win_arc, "%s/win_test.iso", tmpdir);
  CHECK(mkdir(win_stage, 0755) == 0);
  CHECK(mkdir(win_src_dir, 0755) == 0);
  FILE *wf = fopen(win_wim, "wb");
  CHECK(wf != NULL);
  CHECK(fwrite("MSWIM\0\0\0", 1, 8, wf) == 8);
  fclose(wf);
  const char *av_tar[] = {"bsdtar", "-cf", win_arc, "-C", win_stage, "sources/install.wim", NULL};
  char tar_out[1024] = {0};
  CHECK(rufux_capture(av_tar, tar_out, sizeof tar_out) == 0);
  char werr[128] = {0};
  CHECK(rufux_is_windows_iso(win_arc, werr, sizeof werr) == 1);

  char msg[256] = {0};
  CHECK(rufux_smart_check(iso_path, msg, sizeof msg) == RUFUX_SMART_NA);

  char perr[256] = {0};
  CHECK(rufux_need_root_for_block(iso_path, perr, sizeof perr) == 0);
  if (geteuid() == 0 && access("/dev/rdisk0", F_OK) == 0) {
    CHECK(rufux_need_root_for_block("/dev/rdisk0", perr, sizeof perr) == 0);
    pid_t pid = fork();
    CHECK(pid >= 0);
    if (pid == 0) {
      if (seteuid(501) == 0) {
        char cerr[256] = {0};
        if (rufux_need_root_for_block(iso_path, cerr, sizeof cerr) != 0) _exit(2);
        if (access("/dev/disk0", F_OK) == 0 &&
            rufux_need_root_for_block("/dev/disk0", cerr, sizeof cerr) != -1) _exit(3);
        if (rufux_need_root_for_block("/dev/rdisk0", cerr, sizeof cerr) != -1) _exit(4);
      }
      _exit(0);
    }
    int st = 0;
    CHECK(waitpid(pid, &st, 0) == pid);
    CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0);
  }
}

static void test_app_bundle_env(const char *tmpdir, const char *self_exe) {
  char app_macos[512], app_res_bin[512], app_res_share[512], app_res_grub[512];
  char plist[540], dummy_tool[540], bundled_exe[540];
  snprintf(app_macos, sizeof app_macos, "%s/Rufux.app/Contents/MacOS", tmpdir);
  snprintf(app_res_bin, sizeof app_res_bin, "%s/Rufux.app/Contents/Resources/bin", tmpdir);
  snprintf(app_res_share, sizeof app_res_share, "%s/Rufux.app/Contents/Resources/share/rufux", tmpdir);
  snprintf(app_res_grub, sizeof app_res_grub, "%s/Rufux.app/Contents/Resources/lib/grub/i386-pc", tmpdir);
  snprintf(plist, sizeof plist, "%s/Rufux.app/Contents/Info.plist", tmpdir);
  snprintf(dummy_tool, sizeof dummy_tool, "%s/rufux-bundled-dummy", app_res_bin);
  snprintf(bundled_exe, sizeof bundled_exe, "%s/ctest_core", app_macos);

  const char *mk[] = {"/bin/mkdir", "-p", app_macos, app_res_bin, app_res_share, app_res_grub, NULL};
  CHECK(rufux_run(mk, 0) == 0);

  FILE *pf = fopen(plist, "wb");
  CHECK(pf != NULL);
  fputs("<plist version=\"1.0\"><dict/></plist>\n", pf);
  fclose(pf);

  FILE *df = fopen(dummy_tool, "wb");
  CHECK(df != NULL);
  fputs("#!/bin/sh\nexit 0\n", df);
  fclose(df);
  CHECK(chmod(dummy_tool, 0755) == 0);

  const char *cp[] = {"/bin/cp", self_exe, bundled_exe, NULL};
  CHECK(rufux_run(cp, 0) == 0);

  const char *run_bundled[] = {
    "/usr/bin/env", "-i", "PATH=/usr/bin:/bin:/usr/sbin:/sbin",
    bundled_exe, "--verify-bundle-env", NULL
  };
  char out[4096] = {0};
  CHECK(rufux_capture(run_bundled, out, sizeof out) == 0);
  CHECK(strstr(out, "BUNDLE_ENV_OK") != NULL);
}

int main(int argc, char **argv) {
  if (argc == 2 && strcmp(argv[1], "--verify-bundle-env") == 0) {
    rufux_macos_init_bundle_env();
    CHECK(rufux_have("rufux-bundled-dummy") == 1);
    const char *res = getenv("RUFUX_RES");
    const char *grub = getenv("RUFUX_GRUB_DIR");
    const char *path = getenv("PATH");
    CHECK(res != NULL && strstr(res, "/Contents/Resources/share/rufux") != NULL);
    CHECK(grub != NULL && strstr(grub, "/Contents/Resources/lib/grub/i386-pc") != NULL);
    CHECK(path != NULL && strstr(path, "/Contents/Resources/bin") != NULL);
    CHECK(strstr(path, "/opt/homebrew") == NULL);
    printf("BUNDLE_ENV_OK\n");
    return 0;
  }

  char tmpdir[256];
  make_tmpdir(tmpdir, sizeof tmpdir);

  char self_exe[1024];
  snprintf(self_exe, sizeof self_exe, "%s/ctest_core", rufux_exe_dir());

  test_exe_dir_and_io_helpers(tmpdir);
  test_tool_resolution_and_exec();
  test_mkfs_ext4_personality_via_mke2fs_only(tmpdir);
  test_app_bundle_env(tmpdir, self_exe);
  test_checksum(tmpdir);
  test_vhd_probe(tmpdir);
  test_validate_efi_and_bootcheck(tmpdir);
  test_secureboot_state();
  test_mjson_iso_priv_smart_i18n(tmpdir);

  rm_rf(tmpdir);
  printf("ctest_core: ALL TESTS PASSED\n");
  return 0;
}
