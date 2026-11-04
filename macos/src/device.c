#include "device.h"
#include "mount.h"
#include "platform_macos.h"
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOBSD.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/storage/IOBlockStorageDevice.h>
#include <IOKit/storage/IOMedia.h>
#include <IOKit/storage/IOStorageDeviceCharacteristics.h>
#include <IOKit/storage/IOStorageProtocolCharacteristics.h>
#include <ctype.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/mount.h>
#include <sys/param.h>
#include <sys/stat.h>
#include <unistd.h>

static int is_valid_bsd_ident(const char *s) {
  if (!s || strncmp(s, "disk", 4) != 0 || !isdigit((unsigned char)s[4])) return 0;
  size_t i = 4;
  for (;;) {
    if (!isdigit((unsigned char)s[i])) return 0;
    while (isdigit((unsigned char)s[i])) i++;
    if (s[i] == '\0') return 1;
    if (s[i] == 's') { i++; continue; }
    return 0;
  }
}

static int parse_bsd_from_path(const char *path, char *bsd_out, size_t cap) {
  if (!path || !path[0] || !bsd_out || cap == 0) return 0;
  bsd_out[0] = 0;

  char resolved[PATH_MAX];
  const char *p = realpath(path, resolved) ? resolved : path;
  if (strncmp(p, "/private/dev/", 13) == 0) p += 8;

  const char *cand = NULL;
  if (strncmp(p, "/dev/rdisk", 10) == 0) cand = p + 6;
  else if (strncmp(p, "/dev/disk", 9) == 0) cand = p + 5;
  else if (strchr(p, '/') == NULL) {
    if (strncmp(p, "rdisk", 5) == 0) cand = p + 1;
    else if (strncmp(p, "disk", 4) == 0) cand = p;
  }
  if (!cand || !is_valid_bsd_ident(cand)) return 0;
  snprintf(bsd_out, cap, "%s", cand);
  return 1;
}

static void whole_disk_bsd(const char *bsd, char *out, size_t cap) {
  if (!out || cap == 0) return;
  out[0] = 0;
  if (!bsd || strncmp(bsd, "disk", 4) != 0 || !isdigit((unsigned char)bsd[4])) return;
  size_t i = 0;
  while (bsd[i] && (i < 4 || isdigit((unsigned char)bsd[i])) && i + 1 < cap) {
    out[i] = bsd[i];
    i++;
  }
  out[i] = 0;
}

static void sanitize_str(char *s) {
  if (!s) return;
  for (size_t i = 0; s[i]; i++) {
    if (!isprint((unsigned char)s[i])) s[i] = ' ';
  }
  size_t len = strlen(s);
  while (len > 0 && isspace((unsigned char)s[len - 1])) s[--len] = 0;
  size_t lead = 0;
  while (s[lead] && isspace((unsigned char)s[lead])) lead++;
  if (lead > 0) memmove(s, s + lead, strlen(s + lead) + 1);
}

static int cf_to_cstr(CFTypeRef ref, char *out, size_t cap) {
  if (!ref || !out || cap == 0) return 0;
  if (CFGetTypeID(ref) != CFStringGetTypeID()) return 0;
  char tmp[256] = {0};
  if (!CFStringGetCString((CFStringRef)ref, tmp, sizeof tmp, kCFStringEncodingUTF8))
    return 0;
  sanitize_str(tmp);
  if (!tmp[0]) return 0;
  snprintf(out, cap, "%s", tmp);
  return 1;
}

static int cf_prop_bool(io_registry_entry_t entry, CFStringRef key) {
  CFTypeRef ref = IORegistryEntryCreateCFProperty(entry, key, kCFAllocatorDefault, 0);
  if (!ref) return 0;
  int val = (CFGetTypeID(ref) == CFBooleanGetTypeID() &&
             CFBooleanGetValue((CFBooleanRef)ref)) ? 1 : 0;
  CFRelease(ref);
  return val;
}

static unsigned long long cf_prop_u64(io_registry_entry_t entry, CFStringRef key) {
  CFTypeRef ref = IORegistryEntryCreateCFProperty(entry, key, kCFAllocatorDefault, 0);
  if (!ref) return 0;
  unsigned long long out = 0;
  if (CFGetTypeID(ref) == CFNumberGetTypeID()) {
    int64_t v = 0;
    if (CFNumberGetValue((CFNumberRef)ref, kCFNumberSInt64Type, &v) && v > 0)
      out = (unsigned long long)v;
  }
  CFRelease(ref);
  return out;
}

static int cf_prop_str(io_registry_entry_t entry, CFStringRef key, char *out, size_t cap) {
  CFTypeRef ref = IORegistryEntryCreateCFProperty(entry, key, kCFAllocatorDefault, 0);
  if (!ref) return 0;
  int ok = cf_to_cstr(ref, out, cap);
  CFRelease(ref);
  return ok;
}

static void cf_dict_get_str(CFDictionaryRef dict, CFStringRef key, char *out, size_t cap) {
  if (!dict || out[0]) return;
  CFTypeRef val = CFDictionaryGetValue(dict, key);
  if (val) cf_to_cstr(val, out, cap);
}

static int bsd_belongs_to_whole(const char *bsd, const char *whole_sys) {
  size_t wlen = strlen(whole_sys);
  return strncmp(bsd, whole_sys, wlen) == 0 &&
         (bsd[wlen] == '\0' || bsd[wlen] == 's');
}

static int iokit_ancestors_touch_whole(const char *bsd, const char *whole_sys) {
  io_service_t start = IOServiceGetMatchingService(
      kIOMainPortDefault, IOBSDNameMatching(kIOMainPortDefault, 0, bsd));
  if (start == MACH_PORT_NULL) return 0;

  io_registry_entry_t queue[32];
  uint64_t seen[32];
  int qh = 0, qt = 0, ns = 0, hit = 0;
  uint64_t sid = 0;
  IORegistryEntryGetRegistryEntryID(start, &sid);
  if (sid) seen[ns++] = sid;
  queue[qt++] = start;

  while (qh < qt && !hit) {
    io_registry_entry_t cur = queue[qh++];
    char cbsd[64] = {0};
    if (cf_prop_str(cur, CFSTR(kIOBSDNameKey), cbsd, sizeof cbsd) &&
        bsd_belongs_to_whole(cbsd, whole_sys)) {
      hit = 1;
      break;
    }
    io_iterator_t piter = MACH_PORT_NULL;
    if (IORegistryEntryGetParentIterator(cur, kIOServicePlane, &piter) == KERN_SUCCESS) {
      io_registry_entry_t parent;
      while ((parent = IOIteratorNext(piter)) != MACH_PORT_NULL) {
        uint64_t pid = 0;
        IORegistryEntryGetRegistryEntryID(parent, &pid);
        int dup = 0;
        if (pid) {
          for (int k = 0; k < ns; k++) if (seen[k] == pid) { dup = 1; break; }
        }
        if (!dup && qt < 32) {
          if (pid && ns < 32) seen[ns++] = pid;
          queue[qt++] = parent;
        } else {
          IOObjectRelease(parent);
        }
      }
      IOObjectRelease(piter);
    }
  }
  for (int k = 0; k < qt; k++) IOObjectRelease(queue[k]);
  return hit;
}

static int dev_is_mounted(const char *whole_sysname) {
  if (rufux_mount_is_whole_disk_active(whole_sysname)) return 1;
  struct statfs *mnts = NULL;
  int n = getmntinfo(&mnts, MNT_NOWAIT);
  if (n <= 0 || !mnts) return 0;
  for (int i = 0; i < n; i++) {
    char mbsd[64] = {0};
    if (!parse_bsd_from_path(mnts[i].f_mntfromname, mbsd, sizeof mbsd)) continue;
    if (bsd_belongs_to_whole(mbsd, whole_sysname)) return 1;
    if (iokit_ancestors_touch_whole(mbsd, whole_sysname)) return 1;
  }
  return 0;
}

static int inspect_whole_iomedia(io_service_t media, RufuxDevice *dev, int *skip_virtual) {
  memset(dev, 0, sizeof *dev);
  *skip_virtual = 0;

  if (!cf_prop_str(media, CFSTR(kIOBSDNameKey), dev->sysname, sizeof dev->sysname))
    return -1;
  if (!is_valid_bsd_ident(dev->sysname)) return -1;
  snprintf(dev->devnode, sizeof dev->devnode, "/dev/%s", dev->sysname);

  int is_removable = cf_prop_bool(media, CFSTR(kIOMediaRemovableKey));
  int is_ejectable = cf_prop_bool(media, CFSTR(kIOMediaEjectableKey));
  dev->size_bytes = cf_prop_u64(media, CFSTR(kIOMediaSizeKey));

  int is_apfs = IOObjectConformsTo(media, "AppleAPFSMedia") ||
                IOObjectConformsTo(media, "CoreStorageLogical");
  int is_disk_image = 0;
  int is_usb = 0;
  int is_sd = 0;
  int is_external = 0;
  int is_virt_if = 0;
  int saw_nvme = 0;
  int saw_virtio = 0;
  char interconnect[64] = {0};
  char location[64] = {0};

  io_registry_entry_t queue[48];
  uint64_t seen[48];
  int qh = 0, qt = 0, ns = 0;

  IOObjectRetain(media);
  uint64_t mid = 0;
  IORegistryEntryGetRegistryEntryID(media, &mid);
  if (mid) seen[ns++] = mid;
  queue[qt++] = media;

  while (qh < qt) {
    io_registry_entry_t cur = queue[qh++];
    io_name_t cls = {0};
    if (IOObjectGetClass(cur, cls) == KERN_SUCCESS) {
      if (strstr(cls, "APFS") || strstr(cls, "CoreStorage")) is_apfs = 1;
      if (strstr(cls, "DiskImage") || strstr(cls, "HDIX")) is_disk_image = 1;
      if (strstr(cls, "USB")) is_usb = 1;
      if (strstr(cls, "SDXC") || strstr(cls, "SDCard")) is_sd = 1;
      if (strstr(cls, "NVMe")) saw_nvme = 1;
      if (strstr(cls, "VirtIO")) saw_virtio = 1;
    }
    if (IOObjectConformsTo(cur, "IOUSBHostDevice") ||
        IOObjectConformsTo(cur, "IOUSBDevice")) {
      is_usb = 1;
    }

    CFTypeRef dchars = IORegistryEntryCreateCFProperty(
        cur, CFSTR(kIOPropertyDeviceCharacteristicsKey), kCFAllocatorDefault, 0);
    if (dchars) {
      if (CFGetTypeID(dchars) == CFDictionaryGetTypeID()) {
        CFDictionaryRef dict = (CFDictionaryRef)dchars;
        cf_dict_get_str(dict, CFSTR(kIOPropertyVendorNameKey),
                        dev->vendor, sizeof dev->vendor);
        cf_dict_get_str(dict, CFSTR(kIOPropertyProductNameKey),
                        dev->model, sizeof dev->model);
        cf_dict_get_str(dict, CFSTR(kIOPropertyProductSerialNumberKey),
                        dev->serial, sizeof dev->serial);
        cf_dict_get_str(dict, CFSTR("Serial Number"),
                        dev->serial, sizeof dev->serial);
      }
      CFRelease(dchars);
    }

    CFTypeRef pchars = IORegistryEntryCreateCFProperty(
        cur, CFSTR(kIOPropertyProtocolCharacteristicsKey), kCFAllocatorDefault, 0);
    if (pchars) {
      if (CFGetTypeID(pchars) == CFDictionaryGetTypeID()) {
        CFDictionaryRef dict = (CFDictionaryRef)pchars;
        cf_dict_get_str(dict, CFSTR(kIOPropertyPhysicalInterconnectTypeKey),
                        interconnect, sizeof interconnect);
        cf_dict_get_str(dict, CFSTR(kIOPropertyPhysicalInterconnectLocationKey),
                        location, sizeof location);
      }
      CFRelease(pchars);
    }

    if (!interconnect[0])
      cf_prop_str(cur, CFSTR(kIOPropertyPhysicalInterconnectTypeKey),
                  interconnect, sizeof interconnect);
    if (!location[0])
      cf_prop_str(cur, CFSTR(kIOPropertyPhysicalInterconnectLocationKey),
                  location, sizeof location);

    if (!dev->vendor[0])
      cf_prop_str(cur, CFSTR("USB Vendor Name"), dev->vendor, sizeof dev->vendor);
    if (!dev->vendor[0])
      cf_prop_str(cur, CFSTR("kUSBVendorString"), dev->vendor, sizeof dev->vendor);
    if (!dev->model[0])
      cf_prop_str(cur, CFSTR("USB Product Name"), dev->model, sizeof dev->model);
    if (!dev->model[0])
      cf_prop_str(cur, CFSTR("kUSBProductString"), dev->model, sizeof dev->model);
    if (!dev->serial[0])
      cf_prop_str(cur, CFSTR("USB Serial Number"), dev->serial, sizeof dev->serial);
    if (!dev->serial[0])
      cf_prop_str(cur, CFSTR("kUSBSerialNumberString"), dev->serial, sizeof dev->serial);

    io_iterator_t piter = MACH_PORT_NULL;
    if (IORegistryEntryGetParentIterator(cur, kIOServicePlane, &piter) == KERN_SUCCESS) {
      io_registry_entry_t parent;
      while ((parent = IOIteratorNext(piter)) != MACH_PORT_NULL) {
        uint64_t pid = 0;
        IORegistryEntryGetRegistryEntryID(parent, &pid);
        int dup = 0;
        if (pid) {
          for (int k = 0; k < ns; k++) if (seen[k] == pid) { dup = 1; break; }
        }
        if (!dup && qt < 48) {
          if (pid && ns < 48) seen[ns++] = pid;
          queue[qt++] = parent;
        } else {
          IOObjectRelease(parent);
        }
      }
      IOObjectRelease(piter);
    }
  }
  for (int k = 0; k < qt; k++) IOObjectRelease(queue[k]);

  if (strcasecmp(location, "External") == 0) is_external = 1;
  if (strcasecmp(location, "File") == 0) is_disk_image = 1;
  if (strcasecmp(interconnect, "USB") == 0) is_usb = 1;
  if (strcasecmp(interconnect, "Secure Digital") == 0) is_sd = 1;
  if (strcasecmp(interconnect, "Virtual Interface") == 0) is_virt_if = 1;

  if (is_usb) snprintf(dev->transport, sizeof dev->transport, "usb");
  else if (is_sd) snprintf(dev->transport, sizeof dev->transport, "mmc");
  else if (strcasecmp(interconnect, "SATA") == 0 || strcasecmp(interconnect, "ATA") == 0)
    snprintf(dev->transport, sizeof dev->transport, "sata");
  else if (saw_nvme || strcasecmp(interconnect, "PCI-Express") == 0 ||
           strcasecmp(interconnect, "NVM Express") == 0 ||
           strcasecmp(interconnect, "Apple Fabric") == 0)
    snprintf(dev->transport, sizeof dev->transport, "nvme");
  else if (saw_virtio || is_virt_if)
    snprintf(dev->transport, sizeof dev->transport, "virt");
  else
    snprintf(dev->transport, sizeof dev->transport, "unknown");

  if (!dev->model[0]) {
    io_name_t mname = {0};
    if (IORegistryEntryGetName(media, mname) == KERN_SUCCESS && mname[0]) {
      snprintf(dev->model, sizeof dev->model, "%s", mname);
      sanitize_str(dev->model);
      size_t ml = strlen(dev->model);
      if (ml > 6 && strcmp(dev->model + ml - 6, " Media") == 0)
        dev->model[ml - 6] = 0;
    }
  }

  dev->is_usb = is_usb;
  dev->removable = (is_removable || is_ejectable ||
                    (is_external && (is_usb || is_sd))) ? 1 : 0;
  if (dev->size_bytes == 0)
    dev->size_bytes = rufux_macos_dev_bytes(dev->devnode);
  dev->mounted = dev_is_mounted(dev->sysname);

  if (is_apfs || ((is_disk_image || (is_virt_if && !saw_virtio)) && !is_usb))
    *skip_virtual = 1;

  return 0;
}

static int cmp_dev_unit(const void *a, const void *b) {
  const RufuxDevice *da = (const RufuxDevice *)a;
  const RufuxDevice *db = (const RufuxDevice *)b;
  unsigned long ua = strtoul(da->sysname + 4, NULL, 10);
  unsigned long ub = strtoul(db->sysname + 4, NULL, 10);
  if (ua < ub) return -1;
  if (ua > ub) return 1;
  return strcmp(da->sysname, db->sysname);
}

void rufux_human_size(unsigned long long bytes, char *out, unsigned long cap) {
  const char *u[] = {"B", "KB", "MB", "GB", "TB"};
  double v = (double)bytes;
  int i = 0;
  while (v >= 1024.0 && i < 4) { v /= 1024.0; i++; }
  snprintf(out, cap, "%.2f %s", v, u[i]);
}

int rufux_list_devices(RufuxDevice *out, int max, int include_fixed) {
  if (!out || max <= 0) return 0;

  CFMutableDictionaryRef match = IOServiceMatching(kIOMediaClass);
  if (!match) return -1;
  CFDictionarySetValue(match, CFSTR(kIOMediaWholeKey), kCFBooleanTrue);

  io_iterator_t iter = MACH_PORT_NULL;
  if (IOServiceGetMatchingServices(kIOMainPortDefault, match, &iter) != KERN_SUCCESS)
    return -1;

  int n = 0;
  io_service_t media;
  while ((media = IOIteratorNext(iter)) != MACH_PORT_NULL) {
    if (n < max) {
      RufuxDevice cand;
      int skip_virtual = 0;
      if (inspect_whole_iomedia(media, &cand, &skip_virtual) == 0 &&
          !skip_virtual &&
          cand.size_bytes > 0 &&
          !rufux_macos_is_boot_disk(cand.devnode) &&
          (cand.removable || include_fixed)) {
        out[n++] = cand;
      }
    }
    IOObjectRelease(media);
  }
  IOObjectRelease(iter);

  if (n > 1) qsort(out, (size_t)n, sizeof out[0], cmp_dev_unit);
  return n;
}

void rufux_print_devices(const RufuxDevice *devs, int n) {
  printf("%-10s %-8s %10s  %-6s %-7s %s\n",
         "NODE", "SYS", "SIZE", "USB", "MOUNTED", "MODEL");
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
  if (err && cap > 0) err[0] = 0;
  if (!path || !path[0]) {
    if (err && cap > 0) snprintf(err, cap, "empty target path");
    return -1;
  }

  // Boot-disk armor runs unconditionally before any flag check.
  if (rufux_macos_is_boot_disk(path)) {
    if (err && cap > 0) snprintf(err, cap, "refusing boot/system disk '%s'", path);
    return -1;
  }

  struct stat st;
  if (stat(path, &st) != 0) {
    if (err && cap > 0) snprintf(err, cap, "target '%s' does not exist", path);
    return -1;
  }
  if (S_ISREG(st.st_mode)) {
    if (!allow_file) {
      if (err && cap > 0)
        snprintf(err, cap, "target '%s' is a regular file (need --allow-file for tests)", path);
      return -1;
    }
    return 0;
  }
  if (!S_ISBLK(st.st_mode) && !S_ISCHR(st.st_mode)) {
    if (err && cap > 0) snprintf(err, cap, "target '%s' is not a block device or file", path);
    return -1;
  }

  char bsd[64] = {0}, whole[64] = {0};
  if (!parse_bsd_from_path(path, bsd, sizeof bsd)) {
    if (err && cap > 0) snprintf(err, cap, "cannot map '%s' to a disk", path);
    return -1;
  }
  whole_disk_bsd(bsd, whole, sizeof whole);
  if (!whole[0]) {
    if (err && cap > 0) snprintf(err, cap, "cannot map '%s' to a disk", path);
    return -1;
  }
  if (rufux_macos_is_boot_disk(whole)) {
    if (err && cap > 0) snprintf(err, cap, "refusing boot/system disk '%s'", path);
    return -1;
  }

  io_service_t whole_media = IOServiceGetMatchingService(
      kIOMainPortDefault, IOBSDNameMatching(kIOMainPortDefault, 0, whole));
  if (whole_media == MACH_PORT_NULL) {
    if (err && cap > 0) snprintf(err, cap, "cannot map '%s' to a disk", path);
    return -1;
  }

  RufuxDevice info;
  int skip_virtual = 0;
  int ok = inspect_whole_iomedia(whole_media, &info, &skip_virtual);
  IOObjectRelease(whole_media);
  if (ok != 0) {
    if (err && cap > 0) snprintf(err, cap, "cannot inspect disk '%s'", whole);
    return -1;
  }

  if (!info.removable && !allow_fixed) {
    if (err && cap > 0)
      snprintf(err, cap, "refusing fixed disk '%s' without --allow-fixed", path);
    return -1;
  }

  unsigned long long sz = rufux_macos_dev_bytes(path);
  if (sz == 0) sz = info.size_bytes;
  if (sz == 0) {
    if (err && cap > 0) {
      snprintf(err, cap,
               "'%s' reports a capacity of 0 bytes: an empty card-reader slot, or a drive the "
               "kernel cannot read. Replug it or choose another entry.", path);
    }
    return -1;
  }
  return 0;
}
