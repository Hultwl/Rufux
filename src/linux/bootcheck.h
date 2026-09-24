#ifndef RUFUX_BOOTCHECK_H
#define RUFUX_BOOTCHECK_H
// Secure Boot revocation check for UEFI bootloaders (.efi PE files), modelled on
// Rufus's IsBootloaderRevoked(): UEFI DBX hash and certificate revocations,
// Linux SBAT generation numbers, and the Windows boot manager's security
// version number. Advisory: a revoked loader still gets written, but the
// person is told it will be refused by up-to-date Secure Boot firmware.
typedef enum {
  RUFUX_BOOT_NOTPE = -1,        // not a PE/EFI executable
  RUFUX_BOOT_UNSIGNED = 0,      // no signature: Secure Boot will refuse it anyway
  RUFUX_BOOT_OK,                // signed, nothing revoked
  RUFUX_BOOT_REVOKED_DBX,       // its hash (or a certificate in its chain) is in the DBX
  RUFUX_BOOT_REVOKED_SBAT,      // SBAT generation below the current minimum
  RUFUX_BOOT_REVOKED_SVN        // Windows boot manager security version below the minimum
} RufuxBootState;

typedef struct {
  RufuxBootState state;
  char arch[12];     // x64 | ia32 | arm | aarch64 | other
  char signer[128];  // issuing CA common name ("" if unsigned)
  char detail[256];  // why it was flagged, or a Secure Boot note
} RufuxBootCheck;

// Checks one file. Returns 0 when a verdict was reached, -1 on read errors.
int rufux_bootcheck_file(const char *path, RufuxBootCheck *out);
// Checks every .efi under root (and inside efi.img-style FAT images), logging one
// line per loader plus warnings. Returns the number of revoked loaders found.
int rufux_bootcheck_tree(const char *root, void (*log)(const char *, void *), void *luser);
const char *rufux_bootcheck_state_name(RufuxBootState s);
#endif
