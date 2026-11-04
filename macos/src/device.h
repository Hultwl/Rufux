#ifndef RUFUX_DEVICE_H
#define RUFUX_DEVICE_H
// macOS IOKit device scan and boot-disk safety armor.

typedef struct {
  char sysname[64];   // e.g. disk4
  char devnode[128];  // e.g. /dev/disk4
  char vendor[128];
  char model[128];
  char serial[128];
  char transport[32]; // usb|mmc|sata|nvme|virt|unknown
  unsigned long long size_bytes;
  int removable;
  int is_usb;
  int mounted;        // 1 if any partition of this disk is mounted
} RufuxDevice;

int rufux_list_devices(RufuxDevice *out, int max, int include_fixed);
void rufux_print_devices(const RufuxDevice *devs, int n);
void rufux_print_devices_json(const RufuxDevice *devs, int n);
// Safety: 0 = ok to target. allow_file permits regular files (tests).
// err explains refusal (boot/system disk, fixed disk, missing...).
int rufux_check_target(const char *path, int allow_fixed, int allow_file,
                       char *err, unsigned long cap);
void rufux_human_size(unsigned long long bytes, char *out, unsigned long cap);
#endif
