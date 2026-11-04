#include "partition.h"
#include "device.h"
#include "exec.h"
#include "platform_macos.h"
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

extern int rufux_boot_load_mbr440(const char *kind, unsigned char out[440],
                                  char *src_desc, size_t desc_cap);

// Mixed-endian EFI GUID constants (fields 1-3 little-endian, fields 4-5 big-endian):
// Microsoft Basic Data: EBD0A0A2-B9E5-4433-87C0-68B6B72699C7
static const unsigned char GUID_MS_BASIC_DATA[16] = {
  0xA2, 0xA0, 0xD0, 0xEB, 0xE5, 0xB9, 0x33, 0x44,
  0x87, 0xC0, 0x68, 0xB6, 0xB7, 0x26, 0x99, 0xC7
};
// Linux Filesystem Data: 0FC63DAF-8483-4772-8E79-3D69D8477DE4
static const unsigned char GUID_LINUX_FS[16] = {
  0xAF, 0x3D, 0xC6, 0x0F, 0x83, 0x84, 0x72, 0x47,
  0x8E, 0x79, 0x3D, 0x69, 0xD8, 0x47, 0x7D, 0xE4
};
// EFI System Partition: C12A7328-F81F-11D2-BA4B-00A0C93EC93B
static const unsigned char GUID_ESP[16] = {
  0x28, 0x73, 0x2A, 0xC1, 0x1F, 0xF8, 0xD2, 0x11,
  0xBA, 0x4B, 0x00, 0xA0, 0xC9, 0x3E, 0xC9, 0x3B
};
// GRUB 2 BIOS Boot Partition: 21686148-6449-6E6F-744E-656564454649 ("Hah!IdontNeedEFI")
static const unsigned char GUID_BIOS_BOOT[16] = {
  0x48, 0x61, 0x68, 0x21, 0x49, 0x64, 0x6F, 0x6E,
  0x74, 0x4E, 0x65, 0x65, 0x64, 0x45, 0x46, 0x49
};

static void put_le32(unsigned char *p, uint32_t v) {
  p[0] = (unsigned char)(v & 0xFFu);
  p[1] = (unsigned char)((v >> 8) & 0xFFu);
  p[2] = (unsigned char)((v >> 16) & 0xFFu);
  p[3] = (unsigned char)((v >> 24) & 0xFFu);
}

static void put_le64(unsigned char *p, uint64_t v) {
  for (int i = 0; i < 8; i++) {
    p[i] = (unsigned char)((v >> (i * 8)) & 0xFFu);
  }
}

static uint32_t get_le32(const unsigned char *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
         ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t get_le64(const unsigned char *p) {
  uint64_t v = 0;
  for (int i = 7; i >= 0; i--) v = (v << 8) | p[i];
  return v;
}

// Standard reflected IEEE 802.3 CRC32 (polynomial 0xEDB88320).
static uint32_t crc32_ieee(const void *data, size_t len) {
  const unsigned char *p = (const unsigned char *)data;
  uint32_t crc = 0xFFFFFFFFu;
  for (size_t i = 0; i < len; i++) {
    crc ^= p[i];
    for (int b = 0; b < 8; b++) {
      crc = (crc >> 1) ^ ((crc & 1u) ? 0xEDB88320u : 0u);
    }
  }
  return crc ^ 0xFFFFFFFFu;
}

// Generates an RFC-4122 v4 UUID stored in mixed-endian EFI GUID layout.
static void gen_random_guid_v4(unsigned char guid[16]) {
  arc4random_buf(guid, 16);
  // Field 3 (time_hi_and_version, bytes 6..7) is little-endian in EFI GUIDs,
  // so the 4-bit version nibble sits in the upper nibble of byte 7.
  guid[7] = (unsigned char)((guid[7] & 0x0Fu) | 0x40u);
  // Field 4 (clock_seq_hi_and_reserved, byte 8) is big-endian.
  guid[8] = (unsigned char)((guid[8] & 0x3Fu) | 0x80u);
}

static void encode_utf16le_name(const char *ascii, unsigned char out72[72]) {
  memset(out72, 0, 72);
  if (!ascii) return;
  for (size_t i = 0; ascii[i] && i < 35; i++) {
    out72[i * 2] = (unsigned char)ascii[i];
    out72[i * 2 + 1] = 0;
  }
}

static void encode_chs(unsigned long long lba, unsigned char chs[3]) {
  const unsigned long long max_chs_lba = 1024ULL * 255ULL * 63ULL - 1ULL;
  if (lba > max_chs_lba) {
    chs[0] = 0xFEu;
    chs[1] = 0xFFu;
    chs[2] = 0xFFu;
    return;
  }
  unsigned sec = (unsigned)(lba % 63ULL) + 1u;
  unsigned long long tmp = lba / 63ULL;
  unsigned head = (unsigned)(tmp % 255ULL);
  unsigned cyl = (unsigned)(tmp / 255ULL);
  chs[0] = (unsigned char)head;
  chs[1] = (unsigned char)((sec & 0x3Fu) | ((cyl >> 2) & 0xC0u));
  chs[2] = (unsigned char)(cyl & 0xFFu);
}

static int pread_all(int fd, void *buf, size_t len, unsigned long long off) {
  unsigned char *p = (unsigned char *)buf;
  size_t got = 0;
  while (got < len) {
    ssize_t r = pread(fd, p + got, len - got, (off_t)(off + got));
    if (r < 0 && errno == EINTR) continue;
    if (r <= 0) return -1;
    got += (size_t)r;
  }
  return 0;
}

static int zero_range(int fd, const unsigned char *zbuf, size_t zcap,
                      unsigned long long off, unsigned long long len) {
  while (len > 0) {
    size_t chunk = (len > (unsigned long long)zcap) ? zcap : (size_t)len;
    if (rufux_pwrite_all(fd, zbuf, chunk, (off_t)off) != 0) return -1;
    off += chunk;
    len -= chunk;
  }
  return 0;
}

typedef struct {
  unsigned long long first_lba;
  unsigned long long last_lba;
  unsigned char mbr_status;
  unsigned char mbr_type;
  unsigned char gpt_type[16];
  uint64_t gpt_attrs;
  const char *gpt_name;
} PartSpec;

// Equivalent of Linux `udevadm settle`: after closing /dev/diskK, IOKit probes
// the new MBR/GPT table asynchronously. Wait briefly so /dev/diskKs1..sN exist
// with their exact new capacities (and old extra slices are gone) before returning.
static void settle_macos_slices(const char *bnode, const PartSpec *parts,
                                int nparts, unsigned sec_sz) {
  if (!bnode || strncmp(bnode, "/dev/disk", 9) != 0 || !isdigit((unsigned char)bnode[9]))
    return;
  for (size_t k = 9; bnode[k]; k++) {
    if (!isdigit((unsigned char)bnode[k])) return; // not a whole-disk /dev/disk<N> node
  }

  for (int attempt = 0; attempt < 250; attempt++) { // up to 5s (250 * 20ms)
    int ready = 1;
    for (int i = 0; i < nparts; i++) {
      char snode[PATH_MAX];
      rufux_macos_part_slice(bnode, i + 1, snode, sizeof snode);
      unsigned long long want =
          (parts[i].last_lba - parts[i].first_lba + 1ULL) * (unsigned long long)sec_sz;
      if (access(snode, F_OK) != 0 || rufux_macos_dev_bytes(snode) != want) {
        ready = 0;
        break;
      }
    }
    if (ready) {
      for (int extra = nparts + 1; extra <= 4; extra++) {
        char snode[PATH_MAX];
        rufux_macos_part_slice(bnode, extra, snode, sizeof snode);
        if (access(snode, F_OK) == 0) {
          ready = 0;
          break;
        }
      }
    }
    if (ready) break;
    usleep(20000);
  }
}

void rufux_partition_plan(const char *dst, const RufuxPartOpts *o,
                          char *out, unsigned long cap) {
  if (!out || cap == 0) return;
  const char *raw_scheme = (o && o->scheme) ? o->scheme : "gpt";
  const char *scheme = (strcmp(raw_scheme, "mbr") == 0) ? "dos" : raw_scheme;
  const char *layout = (o && o->layout) ? o->layout : "single";
  snprintf(out, cap, "partition %s as %s/%s%s",
           dst ? dst : "", scheme, layout,
           (o && o->dry_run) ? " [dry-run]" : "");
}

int rufux_partition(const char *dst, const RufuxPartOpts *o,
                    char *err, unsigned long cap) {
  if (err && cap > 0) err[0] = 0;
  if (!dst || !dst[0] || !o || !o->scheme || !o->layout) {
    if (err && cap > 0) snprintf(err, cap, "invalid partition options");
    return -1;
  }
  if (strcmp(o->scheme, "gpt") != 0 &&
      strcmp(o->scheme, "dos") != 0 &&
      strcmp(o->scheme, "mbr") != 0) {
    if (err && cap > 0) snprintf(err, cap, "scheme must be gpt|dos (mbr alias dos)");
    return -1;
  }
  const char *scheme = (strcmp(o->scheme, "mbr") == 0) ? "dos" : o->scheme;
  if (strcmp(o->layout, "single") != 0 &&
      strcmp(o->layout, "esp+main") != 0 &&
      strcmp(o->layout, "main+uefintfs") != 0) {
    if (err && cap > 0) snprintf(err, cap, "layout must be single|esp+main|main+uefintfs");
    return -1;
  }

  int is_gpt = (strcmp(scheme, "gpt") == 0);
  int bios_part = (o->bios_boot != 0) && is_gpt;

  if (!o->dry_run && !o->yes) {
    if (err && cap > 0) snprintf(err, cap, "refusing real partition without --yes");
    return -1;
  }
  if (rufux_check_target(dst, o->allow_fixed, o->allow_file, err, cap) != 0)
    return -1;

  unsigned char mbr_type = 0x0Cu; // FAT32 LBA
  if (o->fs_main) {
    if (strcmp(o->fs_main, "fat16") == 0) {
      mbr_type = 0x0Eu;
    } else if (strcmp(o->fs_main, "ntfs") == 0 ||
               strcmp(o->fs_main, "exfat") == 0 ||
               strcmp(o->fs_main, "udf") == 0) {
      mbr_type = 0x07u;
    } else if (strcmp(o->fs_main, "ext4") == 0 ||
               strcmp(o->fs_main, "ext3") == 0 ||
               strcmp(o->fs_main, "ext2") == 0) {
      mbr_type = 0x83u;
    }
  }

  unsigned long long total_bytes = rufux_macos_dev_bytes(dst);
  unsigned long long mib = total_bytes >> 20;
  unsigned sec_sz = rufux_macos_sector_size(dst);
  if (sec_sz < 512u || sec_sz > 4096u || (sec_sz & (sec_sz - 1u)) != 0u ||
      (1048576ULL % sec_sz) != 0ULL) {
    if (err && cap > 0) snprintf(err, cap, "unsupported logical sector size %u on '%s'", sec_sz, dst);
    return -1;
  }

  if (strcmp(o->layout, "main+uefintfs") == 0 && mib < 64ULL) {
    if (err && cap > 0) {
      snprintf(err, cap,
               "target '%s' reports %llu MiB; the Windows layout needs at least 64 MiB "
               "(an empty card-reader slot reports 0)",
               dst, mib);
    }
    return -1;
  }
  if (strcmp(o->layout, "single") == 0 && bios_part && mib < 64ULL) {
    if (err && cap > 0) {
      snprintf(err, cap,
               "target '%s' reports %llu MiB; a GPT layout with a BIOS boot "
               "partition needs at least 64 MiB (an empty card-reader slot reports 0)",
               dst, mib);
    }
    return -1;
  }

  if (o->dry_run) {
    fprintf(stderr, "+ partition %s (%s/%s%s) [dry-run]\n",
            dst, scheme, o->layout, bios_part ? "+bios_boot" : "");
    return 0;
  }

  if (strcmp(o->layout, "esp+main") == 0) {
    unsigned long long min_esp_mib = 515ULL + (bios_part ? 2ULL : 0ULL);
    if (mib < min_esp_mib) {
      if (err && cap > 0) {
        snprintf(err, cap, "target '%s' reports %llu MiB; esp+main layout needs at least %llu MiB",
                 dst, mib, min_esp_mib);
      }
      return -1;
    }
  }

  const unsigned long long spm = 1048576ULL / sec_sz; // sectors per MiB
  const unsigned long long total_lbas = total_bytes / sec_sz;
  const unsigned entry_sectors = 16384u / sec_sz;
  if (total_lbas < 2ULL * spm + 2ULL * (entry_sectors + 2ULL)) {
    if (err && cap > 0) {
      snprintf(err, cap, "target '%s' (%llu bytes) is too small to partition", dst, total_bytes);
    }
    return -1;
  }

  const unsigned long long last_lba = total_lbas - 1ULL;
  const unsigned long long first_usable_lba = 2ULL + entry_sectors;
  const unsigned long long last_usable_lba = last_lba - entry_sectors - 1ULL;

  PartSpec parts[3];
  memset(parts, 0, sizeof parts);
  int nparts = 0;

  if (strcmp(o->layout, "main+uefintfs") == 0) {
    unsigned long long main_mib = mib - 3ULL - (bios_part ? 1ULL : 0ULL);
    parts[0].first_lba = spm;
    parts[0].last_lba = spm + main_mib * spm - 1ULL;
    parts[0].mbr_status = 0x80u;
    parts[0].mbr_type = mbr_type;
    memcpy(parts[0].gpt_type, GUID_MS_BASIC_DATA, 16);
    parts[0].gpt_attrs = 0ULL;
    parts[0].gpt_name = "Windows";

    parts[1].first_lba = parts[0].last_lba + 1ULL;
    parts[1].last_lba = parts[1].first_lba + spm - 1ULL;
    parts[1].mbr_status = 0x00u;
    parts[1].mbr_type = 0xEFu;
    memcpy(parts[1].gpt_type, GUID_MS_BASIC_DATA, 16);
    parts[1].gpt_attrs = 0x8000000000000000ULL;
    parts[1].gpt_name = "UEFI:NTFS";
    nparts = 2;

    if (bios_part) {
      parts[2].first_lba = parts[1].last_lba + 1ULL;
      parts[2].last_lba = parts[2].first_lba + spm - 1ULL;
      memcpy(parts[2].gpt_type, GUID_BIOS_BOOT, 16);
      parts[2].gpt_attrs = 0ULL;
      parts[2].gpt_name = "BIOS boot";
      nparts = 3;
    }
  } else if (strcmp(o->layout, "single") == 0) {
    const unsigned char *gtype = (mbr_type == 0x83u) ? GUID_LINUX_FS : GUID_MS_BASIC_DATA;
    if (!bios_part) {
      unsigned long long end_lba = is_gpt ? last_usable_lba : last_lba;
      if (!is_gpt && (end_lba - spm + 1ULL) > 0xFFFFFFFFULL)
        end_lba = spm + 0xFFFFFFFFULL - 1ULL;
      parts[0].first_lba = spm;
      parts[0].last_lba = end_lba;
      parts[0].mbr_status = 0x80u;
      parts[0].mbr_type = mbr_type;
      memcpy(parts[0].gpt_type, gtype, 16);
      parts[0].gpt_attrs = 0ULL;
      parts[0].gpt_name = "";
      nparts = 1;
    } else {
      unsigned long long main_mib = mib - 3ULL;
      parts[0].first_lba = spm;
      parts[0].last_lba = spm + main_mib * spm - 1ULL;
      memcpy(parts[0].gpt_type, gtype, 16);
      parts[0].gpt_attrs = 0ULL;
      parts[0].gpt_name = "";

      parts[1].first_lba = parts[0].last_lba + 1ULL;
      parts[1].last_lba = parts[1].first_lba + spm - 1ULL;
      memcpy(parts[1].gpt_type, GUID_BIOS_BOOT, 16);
      parts[1].gpt_attrs = 0ULL;
      parts[1].gpt_name = "BIOS boot";
      nparts = 2;
    }
  } else { // "esp+main"
    parts[0].first_lba = spm;
    parts[0].last_lba = spm + 512ULL * spm - 1ULL;
    parts[0].mbr_status = 0x00u;
    parts[0].mbr_type = 0xEFu;
    memcpy(parts[0].gpt_type, GUID_ESP, 16);
    parts[0].gpt_attrs = 0ULL;
    parts[0].gpt_name = "";

    unsigned long long p2_first = parts[0].last_lba + 1ULL;
    if (!bios_part) {
      unsigned long long end_lba = is_gpt ? last_usable_lba : last_lba;
      if (!is_gpt && (end_lba - p2_first + 1ULL) > 0xFFFFFFFFULL)
        end_lba = p2_first + 0xFFFFFFFFULL - 1ULL;
      parts[1].first_lba = p2_first;
      parts[1].last_lba = end_lba;
      parts[1].mbr_status = 0x80u;
      parts[1].mbr_type = mbr_type;
      memcpy(parts[1].gpt_type, GUID_MS_BASIC_DATA, 16);
      parts[1].gpt_attrs = 0ULL;
      parts[1].gpt_name = "";
      nparts = 2;
    } else {
      unsigned long long main_mib = mib - 515ULL;
      parts[1].first_lba = p2_first;
      parts[1].last_lba = p2_first + main_mib * spm - 1ULL;
      memcpy(parts[1].gpt_type, GUID_MS_BASIC_DATA, 16);
      parts[1].gpt_attrs = 0ULL;
      parts[1].gpt_name = "";

      parts[2].first_lba = parts[1].last_lba + 1ULL;
      parts[2].last_lba = parts[2].first_lba + spm - 1ULL;
      memcpy(parts[2].gpt_type, GUID_BIOS_BOOT, 16);
      parts[2].gpt_attrs = 0ULL;
      parts[2].gpt_name = "BIOS boot";
      nparts = 3;
    }
  }

  for (int i = 0; i < nparts; i++) {
    if (parts[i].first_lba > parts[i].last_lba) {
      if (err && cap > 0) snprintf(err, cap, "computed invalid partition %d range on '%s'", i + 1, dst);
      return -1;
    }
    if (is_gpt) {
      if (parts[i].first_lba < first_usable_lba || parts[i].last_lba > last_usable_lba) {
        if (err && cap > 0) snprintf(err, cap, "partition %d exceeds usable GPT bounds on '%s'", i + 1, dst);
        return -1;
      }
    } else {
      unsigned long long cnt = parts[i].last_lba - parts[i].first_lba + 1ULL;
      if (parts[i].first_lba > 0xFFFFFFFFULL || cnt > 0xFFFFFFFFULL) {
        if (err && cap > 0)
          snprintf(err, cap, "target '%s' exceeds 2 TiB MBR LBA limit; use --scheme gpt", dst);
        return -1;
      }
    }
  }

  struct stat st;
  if (stat(dst, &st) != 0) {
    if (err && cap > 0) snprintf(err, cap, "cannot stat '%s': %s", dst, strerror(errno));
    return -1;
  }
  if (S_ISBLK(st.st_mode) || S_ISCHR(st.st_mode)) {
    if (rufux_macos_unmount_disk(dst, err, cap) != 0) return -1;
  }

  char bnode[PATH_MAX];
  rufux_macos_block_node(dst, bnode, sizeof bnode);
  int fd = open(bnode, O_RDWR | O_CLOEXEC);
  if (fd < 0) {
    if (err && cap > 0) snprintf(err, cap, "cannot open '%s': %s", bnode, strerror(errno));
    return -1;
  }

  const size_t zcap = 1048576u; // 1 MiB
  unsigned char *zbuf = calloc(1, zcap);
  if (!zbuf) {
    close(fd);
    if (err && cap > 0) snprintf(err, cap, "out of memory");
    return -1;
  }

  int rc = -1;
  // 1. Wipe old MBR & primary/backup GPT signatures at LBA 0..33 and last_lba-33..last_lba.
  unsigned long long wipe_lbas = 34ULL;
  if (wipe_lbas > total_lbas) wipe_lbas = total_lbas;
  if (zero_range(fd, zbuf, zcap, 0ULL, wipe_lbas * sec_sz) != 0) {
    if (err && cap > 0) snprintf(err, cap, "failed wiping primary table on '%s': %s", bnode, strerror(errno));
    goto out;
  }
  if (total_lbas > 34ULL) {
    unsigned long long tail_start = last_lba - 33ULL;
    if (zero_range(fd, zbuf, zcap, tail_start * sec_sz, 34ULL * sec_sz) != 0) {
      if (err && cap > 0) snprintf(err, cap, "failed wiping backup table on '%s': %s", bnode, strerror(errno));
      goto out;
    }
  }

  // 2. Wipe first and last 1 MiB of each created partition extent (preventing stale FAT/NTFS superblocks).
  for (int i = 0; i < nparts; i++) {
    unsigned long long poff = parts[i].first_lba * sec_sz;
    unsigned long long plen = (parts[i].last_lba - parts[i].first_lba + 1ULL) * sec_sz;
    if (plen <= 2ULL * zcap) {
      if (zero_range(fd, zbuf, zcap, poff, plen) != 0) {
        if (err && cap > 0) snprintf(err, cap, "failed wiping partition %d on '%s'", i + 1, bnode);
        goto out;
      }
    } else {
      if (zero_range(fd, zbuf, zcap, poff, zcap) != 0 ||
          zero_range(fd, zbuf, zcap, poff + plen - zcap, zcap) != 0) {
        if (err && cap > 0) snprintf(err, cap, "failed wiping partition %d edges on '%s'", i + 1, bnode);
        goto out;
      }
    }
  }

  // 3. Build LBA 0 (Protective MBR for GPT, or DOS MBR with Syslinux bootstrap).
  unsigned char sec0[4096];
  memset(sec0, 0, sizeof sec0);
  rufux_boot_load_mbr440(is_gpt ? "gpt" : "bios", sec0, NULL, 0);

  uint32_t disk_sig = arc4random();
  if (disk_sig == 0) disk_sig = 0x52554658u;
  put_le32(sec0 + 440, disk_sig);

  if (is_gpt) {
    unsigned char *pe = sec0 + 446;
    unsigned long long prot_cnt = (total_lbas - 1ULL > 0xFFFFFFFFULL)
                                      ? 0xFFFFFFFFULL
                                      : (total_lbas - 1ULL);
    pe[0] = 0x00u;
    encode_chs(1ULL, pe + 1);
    pe[4] = 0xEEu;
    encode_chs(prot_cnt, pe + 5);
    put_le32(pe + 8, 1u);
    put_le32(pe + 12, (uint32_t)prot_cnt);
  } else {
    for (int i = 0; i < nparts; i++) {
      unsigned char *pe = sec0 + 446 + i * 16;
      unsigned long long cnt = parts[i].last_lba - parts[i].first_lba + 1ULL;
      pe[0] = parts[i].mbr_status;
      encode_chs(parts[i].first_lba, pe + 1);
      pe[4] = parts[i].mbr_type;
      encode_chs(parts[i].last_lba, pe + 5);
      put_le32(pe + 8, (uint32_t)parts[i].first_lba);
      put_le32(pe + 12, (uint32_t)cnt);
    }
  }
  sec0[510] = 0x55u;
  sec0[511] = 0xAAu;

  if (rufux_pwrite_all(fd, sec0, sec_sz, 0) != 0) {
    if (err && cap > 0) snprintf(err, cap, "failed writing MBR to '%s': %s", bnode, strerror(errno));
    goto out;
  }

  // 4. Write Primary and Backup GPT headers & 128-entry partition tables.
  if (is_gpt) {
    unsigned char entries[128 * 128];
    memset(entries, 0, sizeof entries);
    for (int i = 0; i < nparts; i++) {
      unsigned char *ge = entries + i * 128;
      memcpy(ge + 0, parts[i].gpt_type, 16);
      gen_random_guid_v4(ge + 16);
      put_le64(ge + 32, parts[i].first_lba);
      put_le64(ge + 40, parts[i].last_lba);
      put_le64(ge + 48, parts[i].gpt_attrs);
      encode_utf16le_name(parts[i].gpt_name, ge + 56);
    }
    uint32_t entries_crc = crc32_ieee(entries, sizeof entries);

    unsigned char disk_guid[16];
    gen_random_guid_v4(disk_guid);

    unsigned char phdr[4096], bhdr[4096];
    memset(phdr, 0, sizeof phdr);
    memset(bhdr, 0, sizeof bhdr);

    memcpy(phdr + 0, "EFI PART", 8);
    put_le32(phdr + 8, 0x00010000u);
    put_le32(phdr + 12, 92u);
    put_le32(phdr + 16, 0u);
    put_le32(phdr + 20, 0u);
    put_le64(phdr + 24, 1ULL);
    put_le64(phdr + 32, last_lba);
    put_le64(phdr + 40, first_usable_lba);
    put_le64(phdr + 48, last_usable_lba);
    memcpy(phdr + 56, disk_guid, 16);
    put_le64(phdr + 72, 2ULL);
    put_le32(phdr + 80, 128u);
    put_le32(phdr + 84, 128u);
    put_le32(phdr + 88, entries_crc);
    put_le32(phdr + 16, crc32_ieee(phdr, 92));

    memcpy(bhdr, phdr, 92);
    put_le32(bhdr + 16, 0u);
    put_le64(bhdr + 24, last_lba);
    put_le64(bhdr + 32, 1ULL);
    put_le64(bhdr + 72, last_lba - entry_sectors);
    put_le32(bhdr + 16, crc32_ieee(bhdr, 92));

    if (rufux_pwrite_all(fd, phdr, sec_sz, (off_t)(1ULL * sec_sz)) != 0 ||
        rufux_pwrite_all(fd, entries, sizeof entries, (off_t)(2ULL * sec_sz)) != 0 ||
        rufux_pwrite_all(fd, entries, sizeof entries, (off_t)((last_lba - entry_sectors) * sec_sz)) != 0 ||
        rufux_pwrite_all(fd, bhdr, sec_sz, (off_t)(last_lba * sec_sz)) != 0) {
      if (err && cap > 0) snprintf(err, cap, "failed writing GPT tables to '%s': %s", bnode, strerror(errno));
      goto out;
    }
  }

  if (fsync(fd) != 0) {
    if (err && cap > 0) snprintf(err, cap, "fsync failed on '%s': %s", bnode, strerror(errno));
    goto out;
  }
  rc = 0;

out:
  free(zbuf);
  if (close(fd) != 0 && rc == 0) {
    if (err && cap > 0) snprintf(err, cap, "close failed on '%s': %s", bnode, strerror(errno));
    rc = -1;
  }
  if (rc == 0 && (S_ISBLK(st.st_mode) || S_ISCHR(st.st_mode))) {
    settle_macos_slices(bnode, parts, nparts, sec_sz);
  }
  return rc;
}

static int parse_gpt_slice_at_hdr(int fd, unsigned sec_sz, unsigned long long hdr_lba,
                                  int slice, unsigned long long *start_lba,
                                  unsigned long long *count_lba) {
  unsigned char hdr[4096];
  if (pread_all(fd, hdr, sec_sz, hdr_lba * sec_sz) != 0) return -1;
  if (memcmp(hdr, "EFI PART", 8) != 0) return -1;

  uint32_t hdr_sz = get_le32(hdr + 12);
  if (hdr_sz < 92u || hdr_sz > sec_sz) return -1;

  uint32_t stored_hcrc = get_le32(hdr + 16);
  unsigned char hcopy[4096];
  memcpy(hcopy, hdr, hdr_sz);
  memset(hcopy + 16, 0, 4);
  if (crc32_ieee(hcopy, hdr_sz) != stored_hcrc) return -1;

  uint64_t ent_lba = get_le64(hdr + 72);
  uint32_t num_ents = get_le32(hdr + 80);
  uint32_t ent_sz = get_le32(hdr + 84);
  uint32_t stored_ecrc = get_le32(hdr + 88);

  if (ent_lba < 2ULL || num_ents == 0u || num_ents > 4096u ||
      ent_sz < 128u || ent_sz > 4096u || (uint32_t)slice > num_ents) {
    return -1;
  }

  size_t raw_bytes = (size_t)num_ents * (size_t)ent_sz;
  if (raw_bytes > (1u << 20)) return -1;
  size_t aligned_bytes = ((raw_bytes + sec_sz - 1u) / sec_sz) * sec_sz;

  unsigned char *ents = malloc(aligned_bytes);
  if (!ents) return -1;
  if (pread_all(fd, ents, aligned_bytes, ent_lba * sec_sz) != 0) {
    free(ents);
    return -1;
  }
  if (crc32_ieee(ents, raw_bytes) != stored_ecrc) {
    free(ents);
    return -1;
  }

  const unsigned char *e = ents + (size_t)(slice - 1) * (size_t)ent_sz;
  int type_nonzero = 0;
  for (int i = 0; i < 16; i++) {
    if (e[i] != 0) { type_nonzero = 1; break; }
  }
  uint64_t first = get_le64(e + 32);
  uint64_t last = get_le64(e + 40);
  free(ents);

  if (!type_nonzero || first == 0ULL || last < first) return -1;
  if (start_lba) *start_lba = (unsigned long long)first;
  if (count_lba) *count_lba = (unsigned long long)(last - first + 1ULL);
  return 0;
}

int rufux_part_offset_sectors(const char *disk, int slice,
                              unsigned long long *start_lba, unsigned long long *count_lba) {
  if (!disk || !disk[0] || slice < 1) return -1;

  char bnode[PATH_MAX];
  rufux_macos_block_node(disk, bnode, sizeof bnode);

  unsigned sec_sz = rufux_macos_sector_size(bnode);
  if (sec_sz < 512u || sec_sz > 4096u) return -1;

  int fd = open(bnode, O_RDONLY | O_CLOEXEC);
  if (fd < 0) return -1;

  unsigned char mbr[4096];
  if (pread_all(fd, mbr, sec_sz, 0ULL) != 0 ||
      mbr[510] != 0x55u || mbr[511] != 0xAAu) {
    close(fd);
    return -1;
  }

  int has_protective = 0;
  for (int i = 0; i < 4; i++) {
    if (mbr[446 + i * 16 + 4] == 0xEEu) {
      has_protective = 1;
      break;
    }
  }

  int rc = -1;
  if (has_protective) {
    rc = parse_gpt_slice_at_hdr(fd, sec_sz, 1ULL, slice, start_lba, count_lba);
    if (rc != 0) {
      unsigned long long total_bytes = rufux_macos_dev_bytes(bnode);
      unsigned long long total_lbas = total_bytes / sec_sz;
      if (total_lbas > 4ULL) {
        rc = parse_gpt_slice_at_hdr(fd, sec_sz, total_lbas - 1ULL, slice, start_lba, count_lba);
      }
    }
  } else {
    if (slice <= 4) {
      const unsigned char *pe = mbr + 446 + (slice - 1) * 16;
      unsigned char ptype = pe[4];
      uint32_t st = get_le32(pe + 8);
      uint32_t cnt = get_le32(pe + 12);
      if (ptype != 0u && st > 0u && cnt > 0u) {
        if (start_lba) *start_lba = (unsigned long long)st;
        if (count_lba) *count_lba = (unsigned long long)cnt;
        rc = 0;
      }
    }
  }

  close(fd);
  return rc;
}
