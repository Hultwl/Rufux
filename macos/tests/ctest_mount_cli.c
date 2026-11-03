#include "mount.h"
#include "platform_macos.h"

#include <limits.h>
#include <stdio.h>
#include <string.h>

int main(int argc, char **argv) {
  rufux_macos_init_bundle_env();
  if (argc != 3) {
    fprintf(stderr, "usage: %s <mount|umount|part1> <arg>\n", argv[0]);
    return 2;
  }

  const char *cmd = argv[1];
  const char *arg = argv[2];
  char err[512] = {0};

  if (strcmp(cmd, "part1") == 0) {
    char out[PATH_MAX] = {0};
    rufux_part1(arg, out, sizeof(out));
    printf("%s\n", out);
    return 0;
  }

  if (strcmp(cmd, "mount") == 0) {
    char mnt[PATH_MAX] = {0};
    if (rufux_mount(arg, 0, mnt, sizeof(mnt), err, sizeof(err)) != 0) {
      fprintf(stderr, "mount failed: %s\n", err[0] ? err : "unknown error");
      return 1;
    }
    printf("%s\n", mnt);
    return 0;
  }

  if (strcmp(cmd, "umount") == 0) {
    if (rufux_unmount(arg, 0, err, sizeof(err)) != 0) {
      fprintf(stderr, "umount failed: %s\n", err[0] ? err : "unknown error");
      return 1;
    }
    return 0;
  }

  fprintf(stderr, "unknown subcommand '%s'\n", cmd);
  return 2;
}
