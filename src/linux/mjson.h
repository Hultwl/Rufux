#ifndef RUFUX_MJSON_H
#define RUFUX_MJSON_H
// A small JSON reader for tool output and web replies (qemu-img, smartctl,
// Microsoft's download service, distro release lists). Read-only, no
// dependencies; documents are bounded in size and depth so a hostile reply
// cannot exhaust memory or the stack.
#include <stddef.h>
typedef struct MJ MJ;
enum { MJ_NULL, MJ_BOOL, MJ_NUM, MJ_STR, MJ_ARR, MJ_OBJ };
MJ *mj_parse(const char *text, char *err, size_t errcap);  // NULL on error (err explains)
void mj_free(MJ *j);
int mj_type(const MJ *j);                                  // MJ_NULL for a NULL node
const MJ *mj_get(const MJ *obj, const char *key);          // NULL when absent / not an object
size_t mj_len(const MJ *j);                                // members of an array or object
const MJ *mj_at(const MJ *j, size_t i);                    // i-th element / member value
const char *mj_key_at(const MJ *obj, size_t i);            // i-th member name
const char *mj_str(const MJ *j);                           // NULL unless a string
double mj_num(const MJ *j);                                // 0 unless a number
int mj_bool(const MJ *j);                                  // 0 unless true
// Convenience: string / number member of an object with a default.
const char *mj_get_str(const MJ *obj, const char *key, const char *def);
double mj_get_num(const MJ *obj, const char *key, double def);
#endif
