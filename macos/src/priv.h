#ifndef RUFUX_PRIV_H
#define RUFUX_PRIV_H
// Privilege guard for the CLI: block (/dev/disk*) and raw character (/dev/rdisk*)
// device mutation needs root.
// Returns 0 if allowed (regular file or root), -1 with err otherwise.
int rufux_need_root_for_block(const char *path, char *err, unsigned long cap);
#endif
