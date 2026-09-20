#define _GNU_SOURCE
#include "device.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>
#include <ctype.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <linux/fs.h>

static int read_first_line(const char *path, char *out, size_t cap) {
  FILE *f = fopen(path, "r");
  if (!f) return -1;
  if (!fgets(out, (int)cap, f)) { fclose(f); return -1; }
  fclose(f);
  out[strcspn(out, "\r\n")] = 0;
  return 0;
}

static unsigned long long dev_size_bytes(const char *sysname, const char *devnode) {
  int fd = open(devnode, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
  if (fd >= 0) {
    unsigned long long bytes = 0;
    if (ioctl(fd, BLKGETSIZE64, &bytes) == 0 && bytes > 0) { close(fd); return bytes; }
    close(fd);
  }
  // Fallback: sysfs sector count (world-readable, no privileges needed).
  char p[256], buf[64] = {0};
  snprintf(p, sizeof p, "/sys/block/%s/size", sysname);
  if (read_first_line(p, buf, sizeof buf) == 0) {
    unsigned long long sectors = strtoull(buf, NULL, 10);
    if (sectors > 0) return sectors * 512ULL;
  }
  return 0;
}

// 1 if /proc/mounts references /dev/<sys> or /dev/<sys>[0-9p]*
static int dev_is_mounted(const char *sysname) {
  FILE *f = fopen("/proc/mounts", "r");
  if (!f) return 0;
  char line[1024];
  int hit = 0;
  char prefix[96];
  snprintf(prefix, sizeof prefix, "/dev/%s", sysname);
  while (fgets(line, sizeof line, f)) {
    if (strstr(line, prefix) == line) { hit = 1; break; }
  }
  fclose(f);
  return hit;
}

static void detect_transport(const char *sysname, const char *devpath_target,
                             int removable, char *out, size_t cap) {
  (void)devpath_target;
  char p[256], buf[128] = {0};
  // nvme?
  if (!strncmp(sysname, "nvme", 4)) { snprintf(out, cap, "nvme"); return; }
  // mmc?
  if (!strncmp(sysname, "mmcblk", 6)) { snprintf(out, cap, "mmc"); return; }
  // virtio?
  if (!strncmp(sysname, "vd", 2)) { snprintf(out, cap, "virt"); return; }
  // usb via driver link
  char link[256], target[512];
  snprintf(link, sizeof link, "/sys/block/%s/device", sysname);
  ssize_t l = readlink(link, target, sizeof target - 1);
  if (l > 0) {
    target[l] = 0;
    if (strstr(target, "usb")) { snprintf(out, cap, "usb"); return; }
    if (strstr(target, "ata") || strstr(target, "sata")) {
      snprintf(out, cap, "sata"); return;
    }
  }
  // fallback: removable sd* is usually usb/sd reader
  if (removable) { snprintf(out, cap, "usb"); return; }
  snprintf(p, sizeof p, "/sys/block/%s/queue/rotational", sysname);
  if (read_first_line(p, buf, sizeof buf) == 0) { (void)buf; }
  snprintf(out, cap, "unknown");
}

void rufux_human_size(unsigned long long bytes, char *out, unsigned long cap) {
  const char *u[] = {"B", "KB", "MB", "GB", "TB"};
  double v = (double)bytes;
  int i = 0;
  while (v >= 1024.0 && i < 4) { v /= 1024.0; i++; }
  snprintf(out, cap, "%.2f %s", v, u[i]);
}

int rufux_list_devices(RufuxDevice *out, int max, int include_fixed) {
  DIR *d = opendir("/sys/block");
  if (!d) return -1;
  struct dirent *e;
  int n = 0;
  while ((e = readdir(d)) && n < max) {
    if (e->d_name[0] == '.') continue;
    if (!strncmp(e->d_name, "loop", 4) || !strncmp(e->d_name, "ram", 3) ||
        !strncmp(e->d_name, "dm-", 3) || !strncmp(e->d_name, "zram", 4))
      continue;
    char p[256], buf[128] = {0};
    snprintf(p, sizeof p, "/sys/block/%s/removable", e->d_name);
    if (read_first_line(p, buf, sizeof buf) != 0) continue;
    int removable = (buf[0] == '1');
    if (!removable && !include_fixed) continue;

    RufuxDevice *dev = &out[n];
    memset(dev, 0, sizeof *dev);
    snprintf(dev->sysname, sizeof dev->sysname, "%s", e->d_name);
    snprintf(dev->devnode, sizeof dev->devnode, "/dev/%s", e->d_name);
    snprintf(p, sizeof p, "/sys/block/%s/device/vendor", e->d_name);
    read_first_line(p, dev->vendor, sizeof dev->vendor);
    snprintf(p, sizeof p, "/sys/block/%s/device/model", e->d_name);
    read_first_line(p, dev->model, sizeof dev->model);
    snprintf(p, sizeof p, "/sys/block/%s/device/serial", e->d_name);
    if (read_first_line(p, dev->serial, sizeof dev->serial) != 0) {
      snprintf(p, sizeof p, "/sys/block/%s/serial", e->d_name);
      read_first_line(p, dev->serial, sizeof dev->serial);
    }
    // trim whitespace
    for (size_t i = 0; dev->vendor[i]; i++)
      if (!isprint((unsigned char)dev->vendor[i])) dev->vendor[i] = ' ';
    for (size_t i = 0; dev->model[i]; i++)
      if (!isprint((unsigned char)dev->model[i])) dev->model[i] = ' ';
    dev->removable = removable;
    char link[256], target[512];
    snprintf(link, sizeof link, "/sys/block/%s/device", e->d_name);
    ssize_t l = readlink(link, target, sizeof target - 1);
    if (l > 0) {
      target[l] = 0;
      dev->is_usb = (strstr(target, "usb") != NULL);
      detect_transport(e->d_name, target, removable, dev->transport, sizeof dev->transport);
    } else {
      detect_transport(e->d_name, "", removable, dev->transport, sizeof dev->transport);
    }
    dev->mounted = dev_is_mounted(e->d_name);
    dev->size_bytes = dev_size_bytes(dev->sysname, dev->devnode);
    // Empty card-reader slots and dead sticks show up as sdX with capacity 0.
    // They cannot be written and only confuse the list (Rufus hides them too).
    if (dev->size_bytes == 0) continue;
    n++;
  }
  closedir(d);
  return n;
}

void rufux_print_devices(const RufuxDevice *devs, int n) {
  printf("%-10s %-8s %10s  %-6s %-7s %s\n", "NODE", "SYS", "SIZE", "USB", "MOUNTED", "MODEL");
  for (int i = 0; i < n; i++) {
    char hs[32];
    rufux_human_size(devs[i].size_bytes, hs, sizeof hs);
    printf("%-10s %-8s %10s  %-6s %-7s %s %s [%s]\n", devs[i].devnode,
           devs[i].sysname, hs, devs[i].is_usb ? "yes" : "no",
           devs[i].mounted ? "yes" : "no", devs[i].vendor, devs[i].model,
           devs[i].transport);
  }
}

static void json_escape(FILE *o, const char *s) {
  for (; *s; s++) {
    if (*s == '"' || *s == '\\') fputc('\\', o);
    fputc(*s, o);
  }
}

void rufux_print_devices_json(const RufuxDevice *devs, int n) {
  printf("[\n");
  for (int i = 0; i < n; i++) {
    printf("  {\"node\":\"%s\",\"sys\":\"%s\",\"size_bytes\":%llu,\"removable\":%s,"
           "\"usb\":%s,\"mounted\":%s,\"transport\":\"%s\",\"vendor\":\"",
           devs[i].devnode, devs[i].sysname, devs[i].size_bytes,
           devs[i].removable ? "true" : "false",
           devs[i].is_usb ? "true" : "false",
           devs[i].mounted ? "true" : "false", devs[i].transport);
    json_escape(stdout, devs[i].vendor);
    printf("\",\"model\":\"");
    json_escape(stdout, devs[i].model);
    printf("\",\"serial\":\"");
    json_escape(stdout, devs[i].serial);
    printf("\"}%s\n", i + 1 < n ? "," : "");
  }
  printf("]\n");
}

int rufux_check_target(const char *path, int allow_fixed, int allow_file,
                       char *err, unsigned long cap) {
  struct stat st;
  if (stat(path, &st) != 0) {
    snprintf(err, cap, "target '%s' does not exist", path);
    return -1;
  }
  if (S_ISREG(st.st_mode)) {
    if (!allow_file) {
      snprintf(err, cap, "target '%s' is a regular file (need --allow-file for tests)", path);
      return -1;
    }
    return 0;
  }
  if (!S_ISBLK(st.st_mode)) {
    snprintf(err, cap, "target '%s' is not a block device or file", path);
    return -1;
  }
  // derive sysname: resolve /dev/sda -> sda (strip partitions trailing digits)
  const char *base = strrchr(path, '/');
  base = base ? base + 1 : path;
  char sys[64] = {0};
  // handle nvme0n1p2 / mmcblk0p1 / sda1
  snprintf(sys, sizeof sys, "%s", base);
  // strip partition suffix by matching /sys/block entries
  DIR *d = opendir("/sys/block");
  char disk[64] = {0};
  if (d) {
    struct dirent *e;
    size_t best = 0;
    while ((e = readdir(d))) {
      if (e->d_name[0] == '.') continue;
      size_t L = strlen(e->d_name);
      if (!strncmp(base, e->d_name, L) && L > best) {
        best = L;
        snprintf(disk, sizeof disk, "%s", e->d_name);
      }
    }
    closedir(d);
  }
  if (!disk[0]) {
    snprintf(err, cap, "cannot map '%s' to a disk", path);
    return -1;
  }
  char p[256], buf[64] = {0};
  snprintf(p, sizeof p, "/sys/block/%s/removable", disk);
  if (read_first_line(p, buf, sizeof buf) != 0) {
    snprintf(err, cap, "cannot read removable flag for '%s'", disk);
    return -1;
  }
  int removable = (buf[0] == '1');
  if (!removable && !allow_fixed) {
    snprintf(err, cap, "refusing fixed disk '%s' without --allow-fixed", path);
    return -1;
  }
  if (dev_is_mounted(disk)) {
    snprintf(err, cap, "refusing mounted disk '%s' (unmount first)", disk);
    return -1;
  }
  if (dev_size_bytes(disk, path) == 0) {
    snprintf(err, cap,
             "'%s' reports a capacity of 0 bytes: an empty card-reader slot, or a drive the "
             "kernel cannot read. Replug it or choose another entry.", path);
    return -1;
  }
  return 0;
}
