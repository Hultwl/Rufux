#ifndef RUFUX_UPDATE_H
#define RUFUX_UPDATE_H
// Release check against GitHub API (needs curl binary + network).
int rufux_update_check(const char *current_version, char *latest_out,
                       unsigned long cap, char *err, unsigned long errcap);
#endif
