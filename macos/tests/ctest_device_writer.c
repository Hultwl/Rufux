#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/param.h>
#include <sys/stat.h>
#include <unistd.h>

#include "badblocks.h"
#include "device.h"
#include "exec.h"
#include "mjson.h"
#include "platform_macos.h"
#include "vdisk.h"
#include "vhd.h"
#include "writer.h"

static char g_attached_disk[128] = {0};
static char g_tmpdir[PATH_MAX] = {0};

static void emergency_cleanup(void) {
  if (g_attached_disk[0]) {
    char d[128];
    snprintf(d, sizeof d, "%s", g_attached_disk);
    g_attached_disk[0] = 0;
    rufux_macos_detach_disk(d);
  }
  if (g_tmpdir[0]) {
    char dir[PATH_MAX];
    snprintf(dir, sizeof dir, "%s", g_tmpdir);
    g_tmpdir[0] = 0;
    const char *av[] = {"/bin/rm", "-rf", dir, NULL};
    rufux_run(av, 0);
  }
}

#define CHECK(expr) do { \
  if (!(expr)) { \
    fprintf(stderr, "CHECK FAILED at %s:%d: %s\n", __FILE__, __LINE__, #expr); \
    emergency_cleanup(); \
    exit(1); \
  } \
} while (0)

static void make_tmpdir(char *out, size_t cap) {
  snprintf(out, cap, "/tmp/rufux-ctest-devw-XXXXXX");
  char *d = mkdtemp(out);
  CHECK(d != NULL);
  snprintf(g_tmpdir, sizeof g_tmpdir, "%s", out);
}

static void prog_counter_cb(unsigned long long done, unsigned long long total, void *user) {
  int *calls = (int *)user;
  (*calls)++;
  CHECK(total == 0 || done <= total);
}

// Extracts whole-disk BSD name from e.g. "/dev/disk3s1s1" -> "disk3".
static void extract_whole_bsd(const char *dev, char *out, size_t cap) {
  out[0] = 0;
  const char *p = dev;
  if (!strncmp(p, "/dev/rdisk", 10)) p += 6;
  else if (!strncmp(p, "/dev/disk", 9)) p += 5;
  else if (!strncmp(p, "rdisk", 5)) p += 1;
  if (strncmp(p, "disk", 4) != 0 || !isdigit((unsigned char)p[4])) return;
  size_t i = 0;
  out[i++] = 'd'; out[i++] = 'i'; out[i++] = 's'; out[i++] = 'k';
  size_t j = 4;
  while (isdigit((unsigned char)p[j]) && i + 1 < cap) {
    out[i++] = p[j++];
  }
  out[i] = 0;
}

static void test_macos_slice_naming(void) {
  char buf[128] = {0};

  rufux_macos_part_slice("/dev/disk4", 1, buf, sizeof buf);
  CHECK(strcmp(buf, "/dev/disk4s1") == 0);

  rufux_macos_part_slice("/dev/disk4", 2, buf, sizeof buf);
  CHECK(strcmp(buf, "/dev/disk4s2") == 0);

  rufux_macos_raw_node("/dev/disk4s1", buf, sizeof buf);
  CHECK(strcmp(buf, "/dev/rdisk4s1") == 0);

  rufux_macos_raw_node("/dev/rdisk4s1", buf, sizeof buf);
  CHECK(strcmp(buf, "/dev/rdisk4s1") == 0);

  rufux_macos_raw_node("/tmp/regular_image.img", buf, sizeof buf);
  CHECK(strcmp(buf, "/tmp/regular_image.img") == 0);

  rufux_macos_block_node("/dev/rdisk12s3", buf, sizeof buf);
  CHECK(strcmp(buf, "/dev/disk12s3") == 0);

  rufux_macos_block_node("/dev/disk12s3", buf, sizeof buf);
  CHECK(strcmp(buf, "/dev/disk12s3") == 0);

  rufux_macos_block_node("/tmp/regular_image.img", buf, sizeof buf);
  CHECK(strcmp(buf, "/tmp/regular_image.img") == 0);

  char hs[32] = {0};
  rufux_human_size(1073741824ULL, hs, sizeof hs);
  CHECK(strcmp(hs, "1.00 GB") == 0);

  // Verify rufux_print_devices and rufux_print_devices_json produce valid output/JSON.
  RufuxDevice sample = {0};
  snprintf(sample.sysname, sizeof sample.sysname, "disk99");
  snprintf(sample.devnode, sizeof sample.devnode, "/dev/disk99");
  snprintf(sample.vendor, sizeof sample.vendor, "SanDisk");
  snprintf(sample.model, sizeof sample.model, "Extreme \"Pro\"");
  snprintf(sample.serial, sizeof sample.serial, "SN\\123");
  snprintf(sample.transport, sizeof sample.transport, "usb");
  sample.size_bytes = 32ULL * 1024ULL * 1024ULL * 1024ULL;
  sample.removable = 1;
  sample.is_usb = 1;
  sample.mounted = 0;

  int pfd[2];
  CHECK(pipe(pfd) == 0);
  fflush(stdout);
  int saved_out = dup(STDOUT_FILENO);
  CHECK(saved_out >= 0);
  CHECK(dup2(pfd[1], STDOUT_FILENO) >= 0);
  close(pfd[1]);
  rufux_print_devices_json(&sample, 1);
  fflush(stdout);
  CHECK(dup2(saved_out, STDOUT_FILENO) >= 0);
  close(saved_out);

  char jbuf[1024] = {0};
  ssize_t jn = rufux_read_full(pfd[0], jbuf, sizeof jbuf - 1);
  close(pfd[0]);
  CHECK(jn > 0);
  char jerr[128] = {0};
  MJ *mj = mj_parse(jbuf, jerr, sizeof jerr);
  CHECK(mj != NULL);
  CHECK(mj_len(mj) == 1);
  const MJ *e0 = mj_at(mj, 0);
  CHECK(strcmp(mj_get_str(e0, "node", ""), "/dev/disk99") == 0);
  CHECK(strcmp(mj_get_str(e0, "model", ""), "Extreme \"Pro\"") == 0);
  CHECK(strcmp(mj_get_str(e0, "serial", ""), "SN\\123") == 0);
  CHECK(mj_bool(mj_get(e0, "removable")) == 1);
  mj_free(mj);
}

static void test_boot_disk_always_refused(const char *tmpdir) {
  struct statfs sfs;
  CHECK(statfs("/", &sfs) == 0);
  CHECK(sfs.f_mntfromname[0] != 0);

  CHECK(rufux_macos_is_boot_disk("/dev/disk0") == 1);
  CHECK(rufux_macos_is_boot_disk("/dev/rdisk0") == 1);
  CHECK(rufux_macos_is_boot_disk("/dev/disk0s1") == 1);
  CHECK(rufux_macos_is_boot_disk("/dev/disk0s2") == 1);
  CHECK(rufux_macos_is_boot_disk("disk0") == 1);
  CHECK(rufux_macos_is_boot_disk("rdisk0") == 1);
  CHECK(rufux_macos_is_boot_disk(sfs.f_mntfromname) == 1);

  char root_whole_bsd[64] = {0};
  extract_whole_bsd(sfs.f_mntfromname, root_whole_bsd, sizeof root_whole_bsd);
  CHECK(root_whole_bsd[0] != 0);

  char root_container_dev[96], root_container_rdev[96];
  snprintf(root_container_dev, sizeof root_container_dev, "/dev/%s", root_whole_bsd);
  snprintf(root_container_rdev, sizeof root_container_rdev, "/dev/r%s", root_whole_bsd);
  CHECK(rufux_macos_is_boot_disk(root_container_dev) == 1);
  CHECK(rufux_macos_is_boot_disk(root_container_rdev) == 1);

  // Symlink pointing at /dev/disk0 and sfs.f_mntfromname must also be caught.
  char sym_disk0[PATH_MAX], sym_root[PATH_MAX], plain_file[PATH_MAX];
  snprintf(sym_disk0, sizeof sym_disk0, "%s/sym_disk0", tmpdir);
  snprintf(sym_root, sizeof sym_root, "%s/sym_root", tmpdir);
  snprintf(plain_file, sizeof plain_file, "%s/not_boot.img", tmpdir);
  CHECK(symlink("/dev/disk0", sym_disk0) == 0);
  CHECK(symlink(sfs.f_mntfromname, sym_root) == 0);
  FILE *pf = fopen(plain_file, "wb");
  CHECK(pf != NULL);
  CHECK(fwrite("ok", 1, 2, pf) == 2);
  fclose(pf);

  CHECK(rufux_macos_is_boot_disk(sym_disk0) == 1);
  CHECK(rufux_macos_is_boot_disk(sym_root) == 1);
  CHECK(rufux_macos_is_boot_disk(plain_file) == 0);

  char err[256] = {0};
  CHECK(rufux_check_target("/dev/disk0", /*allow_fixed=*/1, /*allow_file=*/0, err, sizeof err) == -1);
  CHECK(rufux_check_target("/dev/rdisk0", /*allow_fixed=*/1, /*allow_file=*/0, err, sizeof err) == -1);
  CHECK(rufux_check_target("/dev/disk0s1", /*allow_fixed=*/1, /*allow_file=*/0, err, sizeof err) == -1);
  CHECK(rufux_check_target("/dev/disk0s99", /*allow_fixed=*/1, /*allow_file=*/0, err, sizeof err) == -1);
  CHECK(rufux_check_target(sfs.f_mntfromname, /*allow_fixed=*/1, /*allow_file=*/0, err, sizeof err) == -1);
  CHECK(rufux_check_target(root_container_dev, /*allow_fixed=*/1, /*allow_file=*/0, err, sizeof err) == -1);
  CHECK(rufux_check_target(sym_disk0, /*allow_fixed=*/1, /*allow_file=*/1, err, sizeof err) == -1);
  CHECK(rufux_macos_unmount_disk("/dev/disk0", err, sizeof err) == -1);

  RufuxDevice devs[32];
  int n = rufux_list_devices(devs, 32, /*include_fixed=*/1);
  CHECK(n >= 0);
  for (int i = 0; i < n; i++) {
    CHECK(strcmp(devs[i].sysname, "disk0") != 0);
    CHECK(strncmp(devs[i].sysname, "disk0s", 6) != 0);
    CHECK(strcmp(devs[i].sysname, root_whole_bsd) != 0);
    CHECK(rufux_macos_is_boot_disk(devs[i].devnode) == 0);
    CHECK(rufux_macos_is_boot_disk(devs[i].sysname) == 0);
  }
}

static void write_patterned_vhd(const char *path, const unsigned char *payload, size_t payload_len) {
  unsigned char foot[512] = {0};
  memcpy(foot, "conectix", 8);
  foot[11] = 0x02;
  foot[13] = 0x01;
  memset(foot + 16, 0xFF, 8);
  uint64_t sz = (uint64_t)payload_len;
  for (int b = 0; b < 8; b++) {
    unsigned char byte = (unsigned char)((sz >> ((7 - b) * 8)) & 0xFFu);
    foot[40 + b] = byte;
    foot[48 + b] = byte;
  }
  foot[63] = 2; // Fixed hard disk

  unsigned sum = 0;
  for (int i = 0; i < 512; i++) sum += foot[i];
  unsigned chk = ~sum & 0xFFFFFFFFu;
  foot[64] = (unsigned char)((chk >> 24) & 0xFF);
  foot[65] = (unsigned char)((chk >> 16) & 0xFF);
  foot[66] = (unsigned char)((chk >> 8) & 0xFF);
  foot[67] = (unsigned char)(chk & 0xFF);

  FILE *f = fopen(path, "wb");
  CHECK(f != NULL);
  CHECK(fwrite(payload, 1, payload_len, f) == payload_len);
  CHECK(fwrite(foot, 1, sizeof foot, f) == sizeof foot);
  fclose(f);
}

static void test_writer_and_fixed_vhd_and_badblocks(const char *tmpdir) {
  const size_t src_sz = 2u * 1024u * 1024u; // 2 MiB
  unsigned char *pattern = malloc(src_sz);
  CHECK(pattern != NULL);
  for (size_t i = 0; i < src_sz; i++) {
    pattern[i] = (unsigned char)((i * 131u + (i >> 9) * 17u) & 0xFFu);
  }

  char src_path[PATH_MAX], dst_path[PATH_MAX];
  snprintf(src_path, sizeof src_path, "%s/src_2m.bin", tmpdir);
  snprintf(dst_path, sizeof dst_path, "%s/dst_2m.bin", tmpdir);

  FILE *fs = fopen(src_path, "wb");
  CHECK(fs != NULL);
  CHECK(fwrite(pattern, 1, src_sz, fs) == src_sz);
  fclose(fs);

  CHECK(rufux_macos_dev_bytes(src_path) == (unsigned long long)src_sz);
  CHECK(rufux_macos_sector_size(src_path) >= 512u);

  const char sentinel[] = "SENTINEL_UNTOUCHED_BYTES";
  FILE *fdst = fopen(dst_path, "wb");
  CHECK(fdst != NULL);
  CHECK(fwrite(sentinel, 1, sizeof sentinel, fdst) == sizeof sentinel);
  fclose(fdst);

  char err[512] = {0};
  // Regular file without allow_file must be refused.
  CHECK(rufux_check_target(dst_path, 0, /*allow_file=*/0, err, sizeof err) == -1);
  CHECK(rufux_check_target(dst_path, 0, /*allow_file=*/1, err, sizeof err) == 0);

  // dry_run=1 must read source, fire progress, and leave dst untouched.
  int prog_calls = 0;
  RufuxWriteOpts opts = {0};
  opts.dry_run = 1;
  opts.allow_file = 1;
  CHECK(rufux_write_image(src_path, dst_path, &opts, prog_counter_cb, &prog_calls, err, sizeof err) == 0);
  CHECK(prog_calls > 0);
  {
    char check_buf[64] = {0};
    FILE *fc = fopen(dst_path, "rb");
    CHECK(fc != NULL);
    size_t got = fread(check_buf, 1, sizeof check_buf, fc);
    fclose(fc);
    CHECK(got == sizeof sentinel);
    CHECK(memcmp(check_buf, sentinel, sizeof sentinel) == 0);
  }

  // Real write without yes=1 must be refused and leave dst untouched.
  opts.dry_run = 0;
  opts.yes = 0;
  CHECK(rufux_write_image(src_path, dst_path, &opts, NULL, NULL, err, sizeof err) == -1);
  {
    char check_buf[64] = {0};
    FILE *fc = fopen(dst_path, "rb");
    CHECK(fc != NULL);
    size_t got = fread(check_buf, 1, sizeof check_buf, fc);
    fclose(fc);
    CHECK(got == sizeof sentinel);
    CHECK(memcmp(check_buf, sentinel, sizeof sentinel) == 0);
  }

  // Same-file guard must refuse src == dst.
  opts.yes = 1;
  CHECK(rufux_write_image(src_path, src_path, &opts, NULL, NULL, err, sizeof err) == -1);

  // Real write with verify=1 onto dst_path.
  prog_calls = 0;
  int vprog_calls = 0;
  opts.verify = 1;
  opts.vprog = prog_counter_cb;
  opts.vuser = &vprog_calls;
  CHECK(rufux_write_image(src_path, dst_path, &opts, prog_counter_cb, &prog_calls, err, sizeof err) == 0);
  CHECK(prog_calls > 0);
  CHECK(vprog_calls > 0);
  CHECK(rufux_macos_dev_bytes(dst_path) == (unsigned long long)src_sz);

  unsigned char *readback = malloc(src_sz);
  CHECK(readback != NULL);
  {
    FILE *fc = fopen(dst_path, "rb");
    CHECK(fc != NULL);
    CHECK(fread(readback, 1, src_sz, fc) == src_sz);
    fclose(fc);
    CHECK(memcmp(pattern, readback, src_sz) == 0);
  }

  // Fixed VHD write: 1 MiB payload + 512 B footer -> target receives exact 1 MiB payload.
  const size_t vhd_payload_sz = 1024u * 1024u;
  char vhd_path[PATH_MAX], vhd_dst[PATH_MAX];
  snprintf(vhd_path, sizeof vhd_path, "%s/fixed_1m.vhd", tmpdir);
  snprintf(vhd_dst, sizeof vhd_dst, "%s/vhd_out.bin", tmpdir);
  write_patterned_vhd(vhd_path, pattern, vhd_payload_sz);
  fdst = fopen(vhd_dst, "wb");
  CHECK(fdst != NULL);
  fclose(fdst);

  RufuxWriteOpts vhd_opts = {0};
  vhd_opts.allow_file = 1;
  vhd_opts.yes = 1;
  vhd_opts.verify = 1;
  CHECK(rufux_vhd_adjust(vhd_path, &vhd_opts, NULL, NULL, err, sizeof err) == 0);
  CHECK(vhd_opts.src_len == (unsigned long long)vhd_payload_sz);
  CHECK(rufux_write_image(vhd_path, vhd_dst, &vhd_opts, NULL, NULL, err, sizeof err) == 0);
  CHECK(rufux_macos_dev_bytes(vhd_dst) == (unsigned long long)vhd_payload_sz);
  {
    FILE *fc = fopen(vhd_dst, "rb");
    CHECK(fc != NULL);
    CHECK(fread(readback, 1, vhd_payload_sz, fc) == vhd_payload_sz);
    fclose(fc);
    CHECK(memcmp(pattern, readback, vhd_payload_sz) == 0);
  }

  // 2-pass destructive badblocks write + read-only badblocks scan on dst_path.
  unsigned long long bad = 999;
  int bb_calls = 0;
  CHECK(rufux_badblocks_write(dst_path, /*allow_file=*/1, /*passes=*/2, /*first=*/0,
                              prog_counter_cb, &bb_calls, &bad, err, sizeof err) == 0);
  CHECK(bad == 0);
  CHECK(bb_calls > 0);

  bad = 999;
  bb_calls = 0;
  CHECK(rufux_badblocks(dst_path, /*allow_file=*/1, prog_counter_cb, &bb_calls,
                        &bad, err, sizeof err) == 0);
  CHECK(bad == 0);
  CHECK(bb_calls > 0);

  free(readback);
  free(pattern);
}

static void test_attach_raw_image_and_aligned_rdisk_io(const char *tmpdir) {
  // Only test live /dev/diskK + /dev/rdiskK I/O against a temporary file attached
  // via hdiutil -nomount, ensuring detach always runs on exit/failure.
  const size_t disk_sz = 4u * 1024u * 1024u; // 4 MiB raw disk image
  char raw_disk_img[PATH_MAX], odd_src[PATH_MAX];
  snprintf(raw_disk_img, sizeof raw_disk_img, "%s/virtual_usb.raw", tmpdir);
  snprintf(odd_src, sizeof odd_src, "%s/odd_payload.bin", tmpdir);

  int fd = open(raw_disk_img, O_CREAT | O_RDWR | O_TRUNC, 0600);
  CHECK(fd >= 0);
  unsigned char *prefill = malloc(disk_sz);
  CHECK(prefill != NULL);
  memset(prefill, 0xCC, disk_sz);
  CHECK(rufux_pwrite_all(fd, prefill, disk_sz, 0) == 0);
  free(prefill);
  close(fd);

  // Create a non-sector-multiple source (1 MiB + 333 bytes) to exercise /dev/rdiskK
  // 4096-byte buffer alignment, tail sector zero-padding, and exact-length verify.
  const size_t odd_sz = (1024u * 1024u) + 333u;
  unsigned char *odd_buf = malloc(odd_sz);
  CHECK(odd_buf != NULL);
  for (size_t i = 0; i < odd_sz; i++) odd_buf[i] = (unsigned char)((i * 73u + 19u) & 0xFFu);
  FILE *fo = fopen(odd_src, "wb");
  CHECK(fo != NULL);
  CHECK(fwrite(odd_buf, 1, odd_sz, fo) == odd_sz);
  fclose(fo);

  // Regular file target must keep exact non-sector-multiple byte length (no sector padding).
  char odd_reg_dst[PATH_MAX];
  snprintf(odd_reg_dst, sizeof odd_reg_dst, "%s/odd_reg_out.bin", tmpdir);
  FILE *fr = fopen(odd_reg_dst, "wb");
  CHECK(fr != NULL);
  fclose(fr);
  {
    char rerr[256] = {0};
    RufuxWriteOpts ropts = {0};
    ropts.allow_file = 1;
    ropts.yes = 1;
    ropts.verify = 1;
    CHECK(rufux_write_image(odd_src, odd_reg_dst, &ropts, NULL, NULL, rerr, sizeof rerr) == 0);
    CHECK(rufux_macos_dev_bytes(odd_reg_dst) == (unsigned long long)odd_sz);
  }

  char disk_node[128] = {0};
  CHECK(rufux_macos_attach_raw_image(raw_disk_img, /*readonly=*/0, disk_node, sizeof disk_node) == 0);
  snprintf(g_attached_disk, sizeof g_attached_disk, "%s", disk_node);
  CHECK(strncmp(disk_node, "/dev/disk", 9) == 0);
  CHECK(isdigit((unsigned char)disk_node[9]));

  CHECK(rufux_macos_is_boot_disk(disk_node) == 0);
  CHECK(rufux_macos_dev_bytes(disk_node) == (unsigned long long)disk_sz);
  unsigned sec_sz = rufux_macos_sector_size(disk_node);
  CHECK(sec_sz >= 512u);

  // Virtual DiskImages mounts must not pollute rufux_list_devices.
  RufuxDevice devs[32];
  int n = rufux_list_devices(devs, 32, /*include_fixed=*/1);
  CHECK(n >= 0);
  for (int i = 0; i < n; i++) {
    CHECK(strcmp(devs[i].devnode, disk_node) != 0);
  }

  // Write odd-sized payload to /dev/diskK (auto-unmounted and converted to /dev/rdiskK) with verify=1.
  char err[512] = {0};
  RufuxWriteOpts wopts = {0};
  wopts.allow_fixed = 1;
  wopts.yes = 1;
  wopts.verify = 1;
  CHECK(rufux_write_image(odd_src, disk_node, &wopts, NULL, NULL, err, sizeof err) == 0);

  // Verify raw device readback: exact payload matches odd_buf, tail of final sector is zero-padded (0x00, not 0xCC),
  // and next sector still holds 0xCC sentinel.
  {
    char rnode[128] = {0};
    rufux_macos_raw_node(disk_node, rnode, sizeof rnode);
    size_t pad = (size_t)sec_sz - (odd_sz % sec_sz);
    size_t check_len = odd_sz + pad + sec_sz;
    void *vchk = NULL;
    CHECK(posix_memalign(&vchk, 4096, check_len) == 0 && vchk != NULL);
    int rfd = open(rnode, O_RDONLY | O_CLOEXEC);
    CHECK(rfd >= 0);
    CHECK(rufux_read_full(rfd, vchk, check_len) == (ssize_t)check_len);
    close(rfd);
    unsigned char *chk = (unsigned char *)vchk;
    CHECK(memcmp(chk, odd_buf, odd_sz) == 0);
    for (size_t i = 0; i < pad; i++) CHECK(chk[odd_sz + i] == 0x00u);
    CHECK(chk[odd_sz + pad] == 0xCCu);
    free(vchk);
  }

  // Also test 1-pass badblocks write + read scan directly on /dev/diskK -> /dev/rdiskK.
  unsigned long long bad = 999;
  CHECK(rufux_badblocks_write(disk_node, /*allow_file=*/0, /*passes=*/1, /*first=*/1,
                              NULL, NULL, &bad, err, sizeof err) == 0);
  CHECK(bad == 0);
  bad = 999;
  CHECK(rufux_badblocks(disk_node, /*allow_file=*/0, NULL, NULL, &bad, err, sizeof err) == 0);
  CHECK(bad == 0);

  CHECK(rufux_macos_detach_disk(disk_node) == 0);
  g_attached_disk[0] = 0;
  free(odd_buf);
}

static void test_vdisk_qcow2(const char *tmpdir) {
  rufux_macos_init_bundle_env();
  const char *qimg = "/opt/homebrew/bin/qemu-img";
  if (access(qimg, X_OK) != 0) qimg = rufux_resolve_tool("qemu-img");
  CHECK(qimg != NULL && access(qimg, X_OK) == 0);

  const size_t vsz = 16u * 1024u * 1024u; // 16 MiB
  unsigned char *src_buf = malloc(vsz);
  CHECK(src_buf != NULL);
  for (size_t i = 0; i < vsz; i++) {
    src_buf[i] = (unsigned char)(((i >> 12) * 53u + (i & 0xFFu)) & 0xFFu);
  }

  char raw_in[PATH_MAX], qcow2_path[PATH_MAX], raw_out[PATH_MAX], backed_qcow2[PATH_MAX];
  snprintf(raw_in, sizeof raw_in, "%s/in_16m.raw", tmpdir);
  snprintf(qcow2_path, sizeof qcow2_path, "%s/test_16m.qcow2", tmpdir);
  snprintf(raw_out, sizeof raw_out, "%s/out_16m.raw", tmpdir);
  snprintf(backed_qcow2, sizeof backed_qcow2, "%s/backed_evil.qcow2", tmpdir);

  FILE *fi = fopen(raw_in, "wb");
  CHECK(fi != NULL);
  CHECK(fwrite(src_buf, 1, vsz, fi) == vsz);
  fclose(fi);

  char cap_out[4096] = {0};
  const char *conv_av[] = {qimg, "convert", "-f", "raw", "-O", "qcow2", raw_in, qcow2_path, NULL};
  CHECK(rufux_capture(conv_av, cap_out, sizeof cap_out) == 0);

  // Plain raw file is NOT a vdisk (returns 0 -> normal writer).
  RufuxVdisk vd;
  char err[512] = {0};
  CHECK(rufux_vdisk_detect(raw_in, &vd, err, sizeof err) == 0);

  // Standalone 16 MiB QCOW2 is detected (returns 1).
  CHECK(rufux_vdisk_detect(qcow2_path, &vd, err, sizeof err) == 1);
  CHECK(strcmp(vd.format, "qcow2") == 0);
  CHECK(strcmp(vd.label, "QCOW2") == 0);
  CHECK(vd.virtual_size == (unsigned long long)vsz);

  // Create empty destination file, expand + verify via rufux_vdisk_write.
  FILE *fo = fopen(raw_out, "wb");
  CHECK(fo != NULL);
  fclose(fo);

  RufuxWriteOpts wopts = {0};
  wopts.allow_file = 1;
  wopts.yes = 1;
  wopts.verify = 1;
  int prog_calls = 0;
  CHECK(rufux_vdisk_write(qcow2_path, &vd, raw_out, &wopts, prog_counter_cb, &prog_calls,
                          err, sizeof err) == 0);
  CHECK(rufux_macos_dev_bytes(raw_out) == (unsigned long long)vsz);

  unsigned char *out_buf = malloc(vsz);
  CHECK(out_buf != NULL);
  fo = fopen(raw_out, "rb");
  CHECK(fo != NULL);
  CHECK(fread(out_buf, 1, vsz, fo) == vsz);
  fclose(fo);
  CHECK(memcmp(src_buf, out_buf, vsz) == 0);

  // QCOW2 with -b /etc/hosts backing file MUST be rejected (-1).
  const char *back_av[] = {qimg, "create", "-f", "qcow2", "-F", "raw", "-b", "/etc/hosts",
                           backed_qcow2, NULL};
  CHECK(rufux_capture(back_av, cap_out, sizeof cap_out) == 0);
  err[0] = 0;
  CHECK(rufux_vdisk_detect(backed_qcow2, &vd, err, sizeof err) == -1);
  CHECK(strstr(err, "/etc/hosts") != NULL);

  free(out_buf);
  free(src_buf);
}

int main(void) {
  atexit(emergency_cleanup);
  rufux_macos_init_bundle_env();

  char tmpdir[PATH_MAX];
  make_tmpdir(tmpdir, sizeof tmpdir);

  test_macos_slice_naming();
  test_boot_disk_always_refused(tmpdir);
  test_writer_and_fixed_vhd_and_badblocks(tmpdir);
  test_attach_raw_image_and_aligned_rdisk_io(tmpdir);
  test_vdisk_qcow2(tmpdir);

  emergency_cleanup();
  printf("ctest_device_writer: ALL TESTS PASSED\n");
  return 0;
}
