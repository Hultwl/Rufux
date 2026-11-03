#include "update.h"
#include "exec.h"
#include <stdio.h>
#include <string.h>

int rufux_update_check(const char *current_version, char *latest_out,
                       unsigned long cap, char *err, unsigned long errcap) {
  if (!rufux_have("curl")) {
    snprintf(err, errcap, "curl not found, cannot check for updates");
    return -1;
  }
  const char *av[] = {"curl", "-sL", "--max-time", "15",
                      "-H", "User-Agent: rufux",
                      "-H", "Accept: application/vnd.github+json",
                      "https://api.github.com/repos/Hultwl/Rufux/releases/latest",
                      NULL};
  char buf[8192] = {0};
  if (rufux_capture(av, buf, sizeof buf) != 0 || !buf[0]) {
    snprintf(err, errcap, "network unavailable (offline?)");
    return -1;
  }
  const char *t = strstr(buf, "tag_name");
  if (!t) {
    const char *m = strstr(buf, "\"message\"");
    if (m) {
      const char *a = strchr(m, ':');
      const char *b = a ? strchr(a, '"') : NULL;
      const char *c = b ? strchr(b + 1, '"') : NULL;
      if (c) {
        char msg[128] = {0};
        size_t L = (size_t)(c - (b + 1));
        if (L > sizeof(msg) - 1) L = sizeof(msg) - 1;
        memcpy(msg, b + 1, L);
        snprintf(err, errcap, "GitHub: %s", msg);
        return -1;
      }
    }
    snprintf(err, errcap, "unexpected API response");
    return -1;
  }
  const char *q1 = strchr(t, '"');
  const char *q2 = q1 ? strchr(q1 + 1, '"') : NULL;
  const char *q3 = q2 ? strchr(q2 + 1, '"') : NULL;
  if (!q3) { snprintf(err, errcap, "cannot parse tag_name"); return -1; }
  size_t L = (size_t)(q3 - (q2 + 1));
  if (L >= cap) L = cap - 1;
  memcpy(latest_out, q2 + 1, L);
  latest_out[L] = 0;
  if (latest_out[0] == 'v') memmove(latest_out, latest_out + 1, strlen(latest_out));
  (void)current_version;
  return 0;
}
