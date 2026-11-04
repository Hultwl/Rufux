#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include <ntfs-3g/attrib.h>
#include <ntfs-3g/dir.h>
#include <ntfs-3g/inode.h>
#include <ntfs-3g/layout.h>
#include <ntfs-3g/ntfstime.h>
#include <ntfs-3g/reparse.h>
#include <ntfs-3g/types.h>
#include <ntfs-3g/unistr.h>
#include <ntfs-3g/volume.h>

#include <ext2fs/ext2_fs.h>
#include <ext2fs/ext2fs.h>

/* ONC RPC v2 constants (RFC 5531) */
#define RPC_VERSION_2       2U
#define RPC_CALL            0U
#define RPC_REPLY           1U
#define RPC_MSG_ACCEPTED    0U
#define RPC_MSG_DENIED      1U
#define RPC_SUCCESS         0U
#define RPC_PROG_UNAVAIL    1U
#define RPC_PROG_MISMATCH   2U
#define RPC_PROC_UNAVAIL    3U
#define RPC_GARBAGE_ARGS    4U
#define RPC_MISMATCH        0U
#define AUTH_NONE           0U
#define AUTH_SYS            1U

/* Programs & versions (RFC 1813) */
#define NFS_PROGRAM         100003U
#define NFS_V3              3U
#define MOUNT_PROGRAM       100005U
#define MOUNT_V3            3U

/* MOUNTv3 procedures */
#define MOUNTPROC3_NULL     0U
#define MOUNTPROC3_MNT      1U
#define MOUNTPROC3_DUMP     2U
#define MOUNTPROC3_UMNT     3U
#define MOUNTPROC3_UMNTALL  4U
#define MOUNTPROC3_EXPORT   5U

#define MNT3_OK             0U
#define MNT3ERR_NOENT       2U
#define MNT3ERR_ACCES       13U

/* NFSv3 procedures */
#define NFSPROC3_NULL        0U
#define NFSPROC3_GETATTR     1U
#define NFSPROC3_SETATTR     2U
#define NFSPROC3_LOOKUP      3U
#define NFSPROC3_ACCESS      4U
#define NFSPROC3_READLINK    5U
#define NFSPROC3_READ        6U
#define NFSPROC3_WRITE       7U
#define NFSPROC3_CREATE      8U
#define NFSPROC3_MKDIR       9U
#define NFSPROC3_SYMLINK     10U
#define NFSPROC3_MKNOD       11U
#define NFSPROC3_REMOVE      12U
#define NFSPROC3_RMDIR       13U
#define NFSPROC3_RENAME      14U
#define NFSPROC3_LINK        15U
#define NFSPROC3_READDIR     16U
#define NFSPROC3_READDIRPLUS 17U
#define NFSPROC3_FSSTAT      18U
#define NFSPROC3_FSINFO      19U
#define NFSPROC3_PATHCONF    20U
#define NFSPROC3_COMMIT      21U

/* NFSv3 status codes */
#define NFS3_OK              0U
#define NFS3ERR_PERM         1U
#define NFS3ERR_NOENT        2U
#define NFS3ERR_IO           5U
#define NFS3ERR_NXIO         6U
#define NFS3ERR_ACCES        13U
#define NFS3ERR_EXIST        17U
#define NFS3ERR_XDEV         18U
#define NFS3ERR_NODEV        19U
#define NFS3ERR_NOTDIR       20U
#define NFS3ERR_ISDIR        21U
#define NFS3ERR_INVAL        22U
#define NFS3ERR_FBIG         27U
#define NFS3ERR_NOSPC        28U
#define NFS3ERR_ROFS         30U
#define NFS3ERR_NAMETOOLONG  63U
#define NFS3ERR_NOTEMPTY     66U
#define NFS3ERR_STALE        70U
#define NFS3ERR_BADHANDLE    10001U
#define NFS3ERR_NOT_SYNC     10002U
#define NFS3ERR_BAD_COOKIE   10003U
#define NFS3ERR_NOTSUPP      10004U
#define NFS3ERR_TOOSMALL     10005U
#define NFS3ERR_SERVERFAULT  10006U

/* NFSv3 file types (ftype3) */
#define NF3REG   1U
#define NF3DIR   2U
#define NF3BLK   3U
#define NF3CHR   4U
#define NF3LNK   5U
#define NF3SOCK  6U
#define NF3FIFO  7U

/* NFSv3 ACCESS bits */
#define ACCESS3_READ     0x0001U
#define ACCESS3_LOOKUP   0x0002U
#define ACCESS3_MODIFY   0x0004U
#define ACCESS3_EXTEND   0x0008U
#define ACCESS3_DELETE   0x0010U
#define ACCESS3_EXECUTE  0x0020U

/* NFSv3 WRITE stability */
#define UNSTABLE   0U
#define DATA_SYNC  1U
#define FILE_SYNC  2U

/* NFSv3 CREATE modes */
#define CREATE_UNCHECKED 0U
#define CREATE_GUARDED   1U
#define CREATE_EXCLUSIVE 2U

/* NFSv3 FSINFO properties */
#define FSF3_SYMLINK      0x0002U
#define FSF3_HOMOGENEOUS  0x0008U
#define FSF3_CANSETTIME   0x0010U

/* NFSv3 time_how */
#define DONT_CHANGE        0U
#define SET_TO_SERVER_TIME 1U
#define SET_TO_CLIENT_TIME 2U

#define MOUNTD_FH_LEN      8U
#define MOUNTD_MAX_RPC_MSG (2U * 1024U * 1024U)
#define MOUNTD_MAX_IO_SIZE (1024U * 1024U)

typedef enum {
  FS_KIND_NTFS = 1,
  FS_KIND_EXT  = 2
} FsKind;

typedef struct {
  uint32_t type;
  uint32_t mode;
  uint32_t nlink;
  uint32_t uid;
  uint32_t gid;
  uint64_t size;
  uint64_t used;
  uint32_t rdev1;
  uint32_t rdev2;
  uint64_t fsid;
  uint64_t fileid;
  uint32_t atime_sec;
  uint32_t atime_nsec;
  uint32_t mtime_sec;
  uint32_t mtime_nsec;
  uint32_t ctime_sec;
  uint32_t ctime_nsec;
} NfsAttr;

typedef struct {
  int valid;
  uint64_t size;
  uint32_t mtime_sec;
  uint32_t mtime_nsec;
  uint32_t ctime_sec;
  uint32_t ctime_nsec;
} WccPre;

typedef struct {
  int set_mode;
  uint32_t mode;
  int set_uid;
  uint32_t uid;
  int set_gid;
  uint32_t gid;
  int set_size;
  uint64_t size;
  uint32_t set_atime;
  uint32_t atime_sec;
  uint32_t atime_nsec;
  uint32_t set_mtime;
  uint32_t mtime_sec;
  uint32_t mtime_nsec;
} SAttr3;

typedef struct {
  uint64_t fileid;
  char *name;
} DirEntryItem;

typedef struct {
  DirEntryItem *items;
  size_t count;
  size_t cap;
} DirEntryList;

typedef struct {
  uint64_t tbytes;
  uint64_t fbytes;
  uint64_t abytes;
  uint64_t tfiles;
  uint64_t ffiles;
  uint64_t afiles;
} FsStatInfo;

static FsKind g_fskind;
static ntfs_volume *g_ntfs = NULL;
static ext2_filsys g_ext = NULL;
static uint32_t g_uid = 0;
static uint32_t g_gid = 0;
static uint64_t g_fsid = 0x52554655584E4653ULL;
static uint8_t g_writeverf[8];
static volatile sig_atomic_t g_stop = 0;

/* Open-addressed parent inode cache (child_ino -> parent_ino) */
#define PARENT_CACHE_CAP 16384U
typedef struct {
  uint64_t child;
  uint64_t parent;
  int used;
} ParentCacheSlot;

static ParentCacheSlot g_parent_cache[PARENT_CACHE_CAP];

static void cache_set_parent(uint64_t child, uint64_t parent) {
  if (child == 0 || parent == 0) return;
  uint32_t idx = (uint32_t)((child * 11400714819323198485ULL) >> 50) & (PARENT_CACHE_CAP - 1U);
  for (uint32_t probe = 0; probe < PARENT_CACHE_CAP; probe++) {
    uint32_t s = (idx + probe) & (PARENT_CACHE_CAP - 1U);
    if (!g_parent_cache[s].used || g_parent_cache[s].child == child) {
      g_parent_cache[s].used = 1;
      g_parent_cache[s].child = child;
      g_parent_cache[s].parent = parent;
      return;
    }
  }
  g_parent_cache[idx].used = 1;
  g_parent_cache[idx].child = child;
  g_parent_cache[idx].parent = parent;
}

static int cache_get_parent(uint64_t child, uint64_t *parent_out) {
  if (child == 0) return 0;
  uint32_t idx = (uint32_t)((child * 11400714819323198485ULL) >> 50) & (PARENT_CACHE_CAP - 1U);
  for (uint32_t probe = 0; probe < PARENT_CACHE_CAP; probe++) {
    uint32_t s = (idx + probe) & (PARENT_CACHE_CAP - 1U);
    if (!g_parent_cache[s].used) return 0;
    if (g_parent_cache[s].child == child) {
      *parent_out = g_parent_cache[s].parent;
      return 1;
    }
  }
  return 0;
}

static void dirlist_free(DirEntryList *list) {
  if (!list) return;
  for (size_t i = 0; i < list->count; i++) {
    free(list->items[i].name);
  }
  free(list->items);
  list->items = NULL;
  list->count = 0;
  list->cap = 0;
}

static int dirlist_push(DirEntryList *list, uint64_t fileid, const char *name) {
  if (!list || !name || !name[0]) return -1;
  if (list->count == list->cap) {
    size_t ncap = list->cap ? list->cap * 2U : 32U;
    DirEntryItem *nitems = realloc(list->items, ncap * sizeof(*nitems));
    if (!nitems) return -1;
    list->items = nitems;
    list->cap = ncap;
  }
  char *dup = strdup(name);
  if (!dup) return -1;
  list->items[list->count].fileid = fileid;
  list->items[list->count].name = dup;
  list->count++;
  return 0;
}

/* XDR Reader & Writer */
typedef struct {
  const uint8_t *data;
  size_t len;
  size_t pos;
  int err;
} XdrReader;

typedef struct {
  uint8_t *data;
  size_t len;
  size_t cap;
  int err;
} XdrBuf;

static uint32_t xdr_get_u32(XdrReader *r) {
  if (r->err || r->pos + 4U > r->len) {
    r->err = 1;
    return 0;
  }
  const uint8_t *p = r->data + r->pos;
  r->pos += 4U;
  return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
         ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static uint64_t xdr_get_u64(XdrReader *r) {
  uint64_t hi = xdr_get_u32(r);
  uint64_t lo = xdr_get_u32(r);
  return (hi << 32) | lo;
}

static int xdr_get_fixed(XdrReader *r, void *out, size_t n) {
  size_t pad = (4U - (n & 3U)) & 3U;
  if (r->err || n > r->len - r->pos || pad > r->len - r->pos - n) {
    r->err = 1;
    return -1;
  }
  if (out && n > 0) memcpy(out, r->data + r->pos, n);
  r->pos += n + pad;
  return 0;
}

static int xdr_skip_opaque(XdrReader *r, uint32_t max_len) {
  uint32_t n = xdr_get_u32(r);
  if (r->err || n > max_len) {
    r->err = 1;
    return -1;
  }
  return xdr_get_fixed(r, NULL, n);
}

static int xdr_get_str(XdrReader *r, char *out, size_t cap) {
  uint32_t n = xdr_get_u32(r);
  if (r->err || cap == 0 || (size_t)n >= cap) {
    r->err = 1;
    return -1;
  }
  if (xdr_get_fixed(r, out, n) != 0) return -1;
  out[n] = '\0';
  if (memchr(out, '\0', n) != NULL) {
    r->err = 1;
    return -1;
  }
  return 0;
}

static int xdr_get_opaque_ptr(XdrReader *r, const uint8_t **ptr_out, uint32_t *len_out,
                              uint32_t max_len) {
  uint32_t n = xdr_get_u32(r);
  size_t pad = (4U - ((size_t)n & 3U)) & 3U;
  if (r->err || n > max_len || (size_t)n > r->len - r->pos ||
      pad > r->len - r->pos - (size_t)n) {
    r->err = 1;
    return -1;
  }
  *ptr_out = r->data + r->pos;
  *len_out = n;
  r->pos += (size_t)n + pad;
  return 0;
}

static int xdr_get_fh(XdrReader *r, uint64_t *ino_out, int *bad_fh) {
  *bad_fh = 0;
  const uint8_t *p = NULL;
  uint32_t n = 0;
  if (xdr_get_opaque_ptr(r, &p, &n, 64U) != 0) return -1;
  if (n != MOUNTD_FH_LEN) {
    *bad_fh = 1;
    *ino_out = 0;
    return 0;
  }
  uint64_t v = ((uint64_t)p[0] << 56) | ((uint64_t)p[1] << 48) |
               ((uint64_t)p[2] << 40) | ((uint64_t)p[3] << 32) |
               ((uint64_t)p[4] << 24) | ((uint64_t)p[5] << 16) |
               ((uint64_t)p[6] << 8)  | (uint64_t)p[7];
  if (v == 0) *bad_fh = 1;
  *ino_out = v;
  return 0;
}

static int xdr_get_sattr3(XdrReader *r, SAttr3 *s) {
  memset(s, 0, sizeof(*s));
  s->set_mode = (xdr_get_u32(r) != 0);
  if (s->set_mode) s->mode = xdr_get_u32(r);
  s->set_uid = (xdr_get_u32(r) != 0);
  if (s->set_uid) s->uid = xdr_get_u32(r);
  s->set_gid = (xdr_get_u32(r) != 0);
  if (s->set_gid) s->gid = xdr_get_u32(r);
  s->set_size = (xdr_get_u32(r) != 0);
  if (s->set_size) s->size = xdr_get_u64(r);
  s->set_atime = xdr_get_u32(r);
  if (s->set_atime == SET_TO_CLIENT_TIME) {
    s->atime_sec = xdr_get_u32(r);
    s->atime_nsec = xdr_get_u32(r);
  }
  s->set_mtime = xdr_get_u32(r);
  if (s->set_mtime == SET_TO_CLIENT_TIME) {
    s->mtime_sec = xdr_get_u32(r);
    s->mtime_nsec = xdr_get_u32(r);
  }
  return r->err ? -1 : 0;
}

static int xdr_reserve(XdrBuf *b, size_t add) {
  if (b->err) return -1;
  if (b->len + add <= b->cap) return 0;
  size_t ncap = b->cap ? b->cap * 2U : 512U;
  while (ncap < b->len + add) ncap *= 2U;
  uint8_t *p = realloc(b->data, ncap);
  if (!p) {
    b->err = 1;
    return -1;
  }
  b->data = p;
  b->cap = ncap;
  return 0;
}

static void xdr_put_u32(XdrBuf *b, uint32_t v) {
  if (xdr_reserve(b, 4U) != 0) return;
  b->data[b->len++] = (uint8_t)(v >> 24);
  b->data[b->len++] = (uint8_t)(v >> 16);
  b->data[b->len++] = (uint8_t)(v >> 8);
  b->data[b->len++] = (uint8_t)v;
}

static void xdr_put_u64(XdrBuf *b, uint64_t v) {
  xdr_put_u32(b, (uint32_t)(v >> 32));
  xdr_put_u32(b, (uint32_t)v);
}

static void xdr_put_fixed(XdrBuf *b, const void *src, size_t n) {
  size_t pad = (4U - (n & 3U)) & 3U;
  if (xdr_reserve(b, n + pad) != 0) return;
  if (n > 0) memcpy(b->data + b->len, src, n);
  b->len += n;
  for (size_t i = 0; i < pad; i++) b->data[b->len++] = 0;
}

static void xdr_put_opaque(XdrBuf *b, const void *src, uint32_t n) {
  xdr_put_u32(b, n);
  xdr_put_fixed(b, src, n);
}

static void xdr_put_str(XdrBuf *b, const char *s) {
  uint32_t n = s ? (uint32_t)strlen(s) : 0U;
  xdr_put_opaque(b, s, n);
}

static void xdr_put_fh(XdrBuf *b, uint64_t ino) {
  uint8_t raw[8];
  raw[0] = (uint8_t)(ino >> 56);
  raw[1] = (uint8_t)(ino >> 48);
  raw[2] = (uint8_t)(ino >> 40);
  raw[3] = (uint8_t)(ino >> 32);
  raw[4] = (uint8_t)(ino >> 24);
  raw[5] = (uint8_t)(ino >> 16);
  raw[6] = (uint8_t)(ino >> 8);
  raw[7] = (uint8_t)ino;
  xdr_put_opaque(b, raw, 8U);
}

static void xdr_put_post_op_fh(XdrBuf *b, int present, uint64_t ino) {
  if (!present) {
    xdr_put_u32(b, 0U);
    return;
  }
  xdr_put_u32(b, 1U);
  xdr_put_fh(b, ino);
}

static void xdr_put_fattr3(XdrBuf *b, const NfsAttr *a) {
  xdr_put_u32(b, a->type);
  xdr_put_u32(b, a->mode & 07777U);
  xdr_put_u32(b, a->nlink);
  xdr_put_u32(b, a->uid);
  xdr_put_u32(b, a->gid);
  xdr_put_u64(b, a->size);
  xdr_put_u64(b, a->used);
  xdr_put_u32(b, a->rdev1);
  xdr_put_u32(b, a->rdev2);
  xdr_put_u64(b, a->fsid);
  xdr_put_u64(b, a->fileid);
  xdr_put_u32(b, a->atime_sec);
  xdr_put_u32(b, a->atime_nsec);
  xdr_put_u32(b, a->mtime_sec);
  xdr_put_u32(b, a->mtime_nsec);
  xdr_put_u32(b, a->ctime_sec);
  xdr_put_u32(b, a->ctime_nsec);
}

static void xdr_put_post_op_attr(XdrBuf *b, int present, const NfsAttr *a) {
  if (!present || !a) {
    xdr_put_u32(b, 0U);
    return;
  }
  xdr_put_u32(b, 1U);
  xdr_put_fattr3(b, a);
}

static void xdr_put_wcc_data(XdrBuf *b, const WccPre *pre, int post_ok, const NfsAttr *post) {
  if (pre && pre->valid) {
    xdr_put_u32(b, 1U);
    xdr_put_u64(b, pre->size);
    xdr_put_u32(b, pre->mtime_sec);
    xdr_put_u32(b, pre->mtime_nsec);
    xdr_put_u32(b, pre->ctime_sec);
    xdr_put_u32(b, pre->ctime_nsec);
  } else {
    xdr_put_u32(b, 0U);
  }
  xdr_put_post_op_attr(b, post_ok, post);
}

static void attr_to_wcc_pre(int ok, const NfsAttr *a, WccPre *pre) {
  memset(pre, 0, sizeof(*pre));
  if (ok && a) {
    pre->valid = 1;
    pre->size = a->size;
    pre->mtime_sec = a->mtime_sec;
    pre->mtime_nsec = a->mtime_nsec;
    pre->ctime_sec = a->ctime_sec;
    pre->ctime_nsec = a->ctime_nsec;
  }
}

/* Common helpers & NTFS backend */
static uint32_t errno_to_nfs3(int e) {
  switch (e) {
    case 0:            return NFS3_OK;
    case EPERM:        return NFS3ERR_PERM;
    case ENOENT:       return NFS3ERR_NOENT;
    case EIO:          return NFS3ERR_IO;
    case ENXIO:        return NFS3ERR_NXIO;
    case EACCES:       return NFS3ERR_ACCES;
    case EEXIST:       return NFS3ERR_EXIST;
    case EXDEV:        return NFS3ERR_XDEV;
    case ENODEV:       return NFS3ERR_NODEV;
    case ENOTDIR:      return NFS3ERR_NOTDIR;
    case EISDIR:       return NFS3ERR_ISDIR;
    case EINVAL:       return NFS3ERR_INVAL;
    case EFBIG:        return NFS3ERR_FBIG;
    case ENOSPC:       return NFS3ERR_NOSPC;
    case EROFS:        return NFS3ERR_ROFS;
    case ENAMETOOLONG: return NFS3ERR_NAMETOOLONG;
    case ENOTEMPTY:    return NFS3ERR_NOTEMPTY;
    case ESTALE:       return NFS3ERR_STALE;
    case EOPNOTSUPP:   return NFS3ERR_NOTSUPP;
    default:           return NFS3ERR_IO;
  }
}

static int valid_leaf_name(const char *name) {
  if (!name || !name[0]) return 0;
  if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) return 0;
  if (strchr(name, '/') != NULL) return 0;
  return 1;
}

static uint32_t ntfs_utf8_to_ucs(const char *name, ntfschar **u_out, int *len_out) {
  *u_out = NULL;
  *len_out = 0;
  if (!valid_leaf_name(name)) return NFS3ERR_INVAL;
  ntfschar *u = NULL;
  int len = ntfs_mbstoucs(name, &u);
  if (len <= 0 || !u) {
    free(u);
    return NFS3ERR_INVAL;
  }
  if (len > 255) {
    free(u);
    return NFS3ERR_NAMETOOLONG;
  }
  *u_out = u;
  *len_out = len;
  return NFS3_OK;
}

static int ntfs_is_symlink_inode(ntfs_inode *ni) {
  if (!ni || (ni->mrec->flags & MFT_RECORD_IS_DIRECTORY) != 0) return 0;
  if ((ni->flags & FILE_ATTR_REPARSE_POINT) != 0 && ntfs_possible_symlink(ni)) {
    return 1;
  }
  if ((ni->flags & FILE_ATTR_SYSTEM) != 0 &&
      ntfs_interix_types(ni) == NTFS_DT_LNK) {
    return 1;
  }
  return 0;
}

static uint32_t ntfs_read_symlink_str(ntfs_inode *ni, char *out, size_t cap) {
  if (!ni || !out || cap == 0) return NFS3ERR_INVAL;
  if ((ni->flags & FILE_ATTR_REPARSE_POINT) != 0 && ntfs_possible_symlink(ni)) {
    char *s = ntfs_make_symlink(ni, "/");
    if (!s) return errno_to_nfs3(errno ? errno : EIO);
    size_t n = strlen(s);
    if (n >= cap) {
      free(s);
      return NFS3ERR_NAMETOOLONG;
    }
    memcpy(out, s, n + 1U);
    free(s);
    return NFS3_OK;
  }
  if ((ni->flags & FILE_ATTR_SYSTEM) != 0) {
    s64 sz = 0;
    INTX_FILE *ix = (INTX_FILE *)ntfs_attr_readall(ni, AT_DATA, AT_UNNAMED, 0, &sz);
    if (!ix) return errno_to_nfs3(errno ? errno : EINVAL);
    if ((size_t)sz < sizeof(INTX_FILE_TYPES) || ix->magic != INTX_SYMBOLIC_LINK) {
      free(ix);
      return NFS3ERR_INVAL;
    }
    int ulen = (int)(((size_t)sz - sizeof(INTX_FILE_TYPES)) / sizeof(ntfschar));
    char *utf8 = NULL;
    int mlen = ntfs_ucstombs(ix->target, ulen, &utf8, 0);
    free(ix);
    if (mlen < 0 || !utf8) {
      free(utf8);
      return NFS3ERR_IO;
    }
    if ((size_t)mlen >= cap) {
      free(utf8);
      return NFS3ERR_NAMETOOLONG;
    }
    memcpy(out, utf8, (size_t)mlen + 1U);
    free(utf8);
    return NFS3_OK;
  }
  return NFS3ERR_INVAL;
}

static uint64_t ntfs_resolve_parent_ino(ntfs_inode *ni, uint64_t ino) {
  if (ino == FILE_root) return FILE_root;
  uint64_t p = 0;
  if (cache_get_parent(ino, &p) && p != 0) return p;
  if (ni) {
    ntfs_inode *pni = ntfs_dir_parent_inode(ni);
    if (pni) {
      p = (uint64_t)MREF(pni->mft_no);
      (void)ntfs_inode_close(pni);
      if (p != 0) {
        cache_set_parent(ino, p);
        return p;
      }
    }
  }
  return 0;
}

static void ntfs_fill_attr(ntfs_inode *ni, uint64_t ino, NfsAttr *a) {
  memset(a, 0, sizeof(*a));
  int is_dir = ((ni->mrec->flags & MFT_RECORD_IS_DIRECTORY) != 0);
  int is_lnk = (!is_dir && ntfs_is_symlink_inode(ni));

  a->type = is_dir ? NF3DIR : (is_lnk ? NF3LNK : NF3REG);
  a->mode = is_dir ? 0777U : (is_lnk ? 0777U : 0666U);
  uint16_t lc = le16_to_cpu(ni->mrec->link_count);
  a->nlink = lc ? (uint32_t)lc : 1U;
  a->uid = g_uid;
  a->gid = g_gid;
  a->fsid = g_fsid;
  a->fileid = ino;

  if (is_dir) {
    a->size = 4096ULL;
    a->used = 4096ULL;
  } else if (is_lnk) {
    char sym[PATH_MAX];
    if (ntfs_read_symlink_str(ni, sym, sizeof(sym)) == NFS3_OK) {
      a->size = (uint64_t)strlen(sym);
    } else {
      a->size = (uint64_t)(ni->data_size > 0 ? ni->data_size : 0);
    }
    a->used = (uint64_t)(ni->allocated_size > 0 ? ni->allocated_size : 0);
  } else {
    ntfs_attr *na = ntfs_attr_open(ni, AT_DATA, AT_UNNAMED, 0);
    if (na) {
      a->size = (uint64_t)(na->data_size > 0 ? na->data_size : 0);
      a->used = (uint64_t)(na->allocated_size > 0 ? na->allocated_size : 0);
      ntfs_attr_close(na);
    } else {
      a->size = (uint64_t)(ni->data_size > 0 ? ni->data_size : 0);
      a->used = (uint64_t)(ni->allocated_size > 0 ? ni->allocated_size : 0);
    }
  }

  struct timespec ts_a = ntfs2timespec(ni->last_access_time);
  struct timespec ts_m = ntfs2timespec(ni->last_data_change_time);
  struct timespec ts_c = ntfs2timespec(ni->last_mft_change_time);
  a->atime_sec  = (uint32_t)(ts_a.tv_sec > 0 ? ts_a.tv_sec : 0);
  a->atime_nsec = (uint32_t)(ts_a.tv_nsec > 0 ? ts_a.tv_nsec : 0);
  a->mtime_sec  = (uint32_t)(ts_m.tv_sec > 0 ? ts_m.tv_sec : 0);
  a->mtime_nsec = (uint32_t)(ts_m.tv_nsec > 0 ? ts_m.tv_nsec : 0);
  a->ctime_sec  = (uint32_t)(ts_c.tv_sec > 0 ? ts_c.tv_sec : 0);
  a->ctime_nsec = (uint32_t)(ts_c.tv_nsec > 0 ? ts_c.tv_nsec : 0);
}

static uint32_t ntfs_op_getattr(uint64_t ino, NfsAttr *out) {
  errno = 0;
  ntfs_inode *ni = ntfs_inode_open(g_ntfs, (MFT_REF)ino);
  if (!ni) return (errno == ENOENT || errno == EIO) ? NFS3ERR_STALE : errno_to_nfs3(errno);
  ntfs_fill_attr(ni, ino, out);
  (void)ntfs_inode_close(ni);
  return NFS3_OK;
}

static int ntfs_open_with_parent(uint64_t ino, ntfs_inode **ni_out, ntfs_inode **pni_out) {
  *ni_out = NULL;
  *pni_out = NULL;
  uint64_t pino = 0;
  if (ino != FILE_root && cache_get_parent(ino, &pino) && pino != 0 && pino != ino) {
    *pni_out = ntfs_inode_open(g_ntfs, (MFT_REF)pino);
  }
  errno = 0;
  ntfs_inode *ni = ntfs_inode_open(g_ntfs, (MFT_REF)ino);
  if (!ni) {
    if (*pni_out) {
      (void)ntfs_inode_close(*pni_out);
      *pni_out = NULL;
    }
    return -1;
  }
  if (!*pni_out && ino != FILE_root) {
    pino = ntfs_resolve_parent_ino(ni, ino);
    if (pino != 0 && pino != ino) {
      *pni_out = ntfs_inode_open(g_ntfs, (MFT_REF)pino);
    }
  }
  *ni_out = ni;
  return 0;
}

static int ntfs_close_with_parent(ntfs_inode *ni, ntfs_inode *pni) {
  int rc = 0;
  if (ni && pni) {
    if (ntfs_inode_close_in_dir(ni, pni) != 0) rc = -1;
    if (ntfs_inode_close(pni) != 0) rc = -1;
  } else {
    if (ni && ntfs_inode_close(ni) != 0) rc = -1;
    if (pni && ntfs_inode_close(pni) != 0) rc = -1;
  }
  return rc;
}

static void ntfs_apply_sattr_times(ntfs_inode *ni, const SAttr3 *s) {
  struct timespec now_ts;
  clock_gettime(CLOCK_REALTIME, &now_ts);
  int touched = 0;
  if (s->set_atime == SET_TO_SERVER_TIME) {
    ni->last_access_time = timespec2ntfs(now_ts);
    touched = 1;
  } else if (s->set_atime == SET_TO_CLIENT_TIME) {
    struct timespec ts = {(time_t)s->atime_sec, (long)s->atime_nsec};
    ni->last_access_time = timespec2ntfs(ts);
    touched = 1;
  }
  if (s->set_mtime == SET_TO_SERVER_TIME) {
    ni->last_data_change_time = timespec2ntfs(now_ts);
    touched = 1;
  } else if (s->set_mtime == SET_TO_CLIENT_TIME) {
    struct timespec ts = {(time_t)s->mtime_sec, (long)s->mtime_nsec};
    ni->last_data_change_time = timespec2ntfs(ts);
    touched = 1;
  }
  if (touched) {
    ni->last_mft_change_time = timespec2ntfs(now_ts);
    NInoFileNameSetDirty(ni);
    NInoSetDirty(ni);
  }
}

static uint32_t ntfs_op_setattr(uint64_t ino, const SAttr3 *s, NfsAttr *post_out) {
  ntfs_inode *ni = NULL, *pni = NULL;
  if (ntfs_open_with_parent(ino, &ni, &pni) != 0) return NFS3ERR_STALE;

  if (s->set_size) {
    if ((ni->mrec->flags & MFT_RECORD_IS_DIRECTORY) != 0) {
      (void)ntfs_close_with_parent(ni, pni);
      return NFS3ERR_ISDIR;
    }
    ntfs_attr *na = ntfs_attr_open(ni, AT_DATA, AT_UNNAMED, 0);
    if (!na) {
      int e = errno;
      (void)ntfs_close_with_parent(ni, pni);
      return errno_to_nfs3(e ? e : EIO);
    }
    if (ntfs_attr_truncate(na, (s64)s->size) != 0) {
      int e = errno;
      ntfs_attr_close(na);
      (void)ntfs_close_with_parent(ni, pni);
      return errno_to_nfs3(e ? e : EIO);
    }
    (void)ntfs_attr_pclose(na);
    ntfs_attr_close(na);
    struct timespec now_ts;
    clock_gettime(CLOCK_REALTIME, &now_ts);
    ntfs_time nt = timespec2ntfs(now_ts);
    ni->last_data_change_time = nt;
    ni->last_mft_change_time = nt;
    NInoFileNameSetDirty(ni);
    NInoSetDirty(ni);
  }

  ntfs_apply_sattr_times(ni, s);
  ntfs_fill_attr(ni, ino, post_out);
  if (ntfs_close_with_parent(ni, pni) != 0) return NFS3ERR_IO;
  return NFS3_OK;
}

static uint32_t ntfs_op_lookup(uint64_t dir_ino, const char *name,
                               uint64_t *child_ino_out, NfsAttr *child_attr_out) {
  if (!name || !name[0]) return NFS3ERR_ACCES;
  errno = 0;
  ntfs_inode *dir_ni = ntfs_inode_open(g_ntfs, (MFT_REF)dir_ino);
  if (!dir_ni) return NFS3ERR_STALE;
  if ((dir_ni->mrec->flags & MFT_RECORD_IS_DIRECTORY) == 0) {
    (void)ntfs_inode_close(dir_ni);
    return NFS3ERR_NOTDIR;
  }

  if (strcmp(name, ".") == 0) {
    *child_ino_out = dir_ino;
    ntfs_fill_attr(dir_ni, dir_ino, child_attr_out);
    (void)ntfs_inode_close(dir_ni);
    return NFS3_OK;
  }
  if (strcmp(name, "..") == 0) {
    uint64_t pino = ntfs_resolve_parent_ino(dir_ni, dir_ino);
    if (pino == 0) pino = FILE_root;
    (void)ntfs_inode_close(dir_ni);
    *child_ino_out = pino;
    return ntfs_op_getattr(pino, child_attr_out);
  }

  ntfschar *u = NULL;
  int ulen = 0;
  uint32_t st = ntfs_utf8_to_ucs(name, &u, &ulen);
  if (st != NFS3_OK) {
    (void)ntfs_inode_close(dir_ni);
    return st;
  }

  errno = 0;
  u64 mref = ntfs_inode_lookup_by_name(dir_ni, u, ulen);
  int e = errno;
  free(u);
  if (mref == (u64)-1) {
    (void)ntfs_inode_close(dir_ni);
    return errno_to_nfs3(e ? e : ENOENT);
  }

  uint64_t cino = (uint64_t)MREF(mref);
  if (cino < FILE_first_user && cino != FILE_root) {
    (void)ntfs_inode_close(dir_ni);
    return NFS3ERR_NOENT;
  }

  ntfs_inode *cni = ntfs_inode_open(g_ntfs, (MFT_REF)cino);
  if (!cni) {
    e = errno;
    (void)ntfs_inode_close(dir_ni);
    return errno_to_nfs3(e ? e : EIO);
  }
  cache_set_parent(cino, dir_ino);
  ntfs_fill_attr(cni, cino, child_attr_out);
  (void)ntfs_inode_close_in_dir(cni, dir_ni);
  (void)ntfs_inode_close(dir_ni);
  *child_ino_out = cino;
  return NFS3_OK;
}

static uint32_t ntfs_op_readlink(uint64_t ino, char *out, size_t cap,
                                 NfsAttr *attr_out, int *attr_ok) {
  *attr_ok = 0;
  errno = 0;
  ntfs_inode *ni = ntfs_inode_open(g_ntfs, (MFT_REF)ino);
  if (!ni) return NFS3ERR_STALE;
  ntfs_fill_attr(ni, ino, attr_out);
  *attr_ok = 1;
  if (!ntfs_is_symlink_inode(ni)) {
    (void)ntfs_inode_close(ni);
    return NFS3ERR_INVAL;
  }
  uint32_t st = ntfs_read_symlink_str(ni, out, cap);
  (void)ntfs_inode_close(ni);
  return st;
}

static uint32_t ntfs_op_read(uint64_t ino, uint64_t offset, uint32_t count,
                             uint8_t *buf, uint32_t *got_out, int *eof_out,
                             NfsAttr *attr_out, int *attr_ok) {
  *got_out = 0;
  *eof_out = 0;
  *attr_ok = 0;
  errno = 0;
  ntfs_inode *ni = ntfs_inode_open(g_ntfs, (MFT_REF)ino);
  if (!ni) return NFS3ERR_STALE;
  ntfs_fill_attr(ni, ino, attr_out);
  *attr_ok = 1;

  if ((ni->mrec->flags & MFT_RECORD_IS_DIRECTORY) != 0) {
    (void)ntfs_inode_close(ni);
    return NFS3ERR_ISDIR;
  }
  if (ntfs_is_symlink_inode(ni)) {
    (void)ntfs_inode_close(ni);
    return NFS3ERR_INVAL;
  }

  ntfs_attr *na = ntfs_attr_open(ni, AT_DATA, AT_UNNAMED, 0);
  if (!na) {
    int e = errno;
    (void)ntfs_inode_close(ni);
    return errno_to_nfs3(e ? e : EIO);
  }

  uint64_t fsize = (na->data_size > 0) ? (uint64_t)na->data_size : 0ULL;
  if (offset >= fsize || count == 0) {
    *got_out = 0;
    *eof_out = (offset >= fsize) ? 1 : 0;
    ntfs_attr_close(na);
    (void)ntfs_inode_close(ni);
    return NFS3_OK;
  }

  uint64_t avail = fsize - offset;
  uint32_t to_read = (avail < (uint64_t)count) ? (uint32_t)avail : count;
  uint32_t total = 0;
  while (total < to_read) {
    errno = 0;
    s64 r = ntfs_attr_pread(na, (s64)(offset + total), (s64)(to_read - total),
                            buf + total);
    if (r < 0) {
      int e = errno;
      ntfs_attr_close(na);
      (void)ntfs_inode_close(ni);
      return errno_to_nfs3(e ? e : EIO);
    }
    if (r == 0) break;
    total += (uint32_t)r;
  }

  *got_out = total;
  *eof_out = (offset + total >= fsize) ? 1 : 0;
  ntfs_attr_close(na);
  (void)ntfs_inode_close(ni);
  return NFS3_OK;
}

static uint32_t ntfs_op_write(uint64_t ino, uint64_t offset, const uint8_t *buf,
                              uint32_t count, uint32_t stable,
                              uint32_t *written_out, uint32_t *committed_out,
                              WccPre *pre_out, NfsAttr *post_out, int *post_ok) {
  *written_out = 0;
  *committed_out = FILE_SYNC;
  memset(pre_out, 0, sizeof(*pre_out));
  *post_ok = 0;

  ntfs_inode *ni = NULL, *pni = NULL;
  if (ntfs_open_with_parent(ino, &ni, &pni) != 0) return NFS3ERR_STALE;

  NfsAttr pre_attr;
  ntfs_fill_attr(ni, ino, &pre_attr);
  attr_to_wcc_pre(1, &pre_attr, pre_out);

  if ((ni->mrec->flags & MFT_RECORD_IS_DIRECTORY) != 0) {
    *post_out = pre_attr;
    *post_ok = 1;
    (void)ntfs_close_with_parent(ni, pni);
    return NFS3ERR_ISDIR;
  }
  if (ntfs_is_symlink_inode(ni)) {
    *post_out = pre_attr;
    *post_ok = 1;
    (void)ntfs_close_with_parent(ni, pni);
    return NFS3ERR_INVAL;
  }

  ntfs_attr *na = ntfs_attr_open(ni, AT_DATA, AT_UNNAMED, 0);
  if (!na) {
    int e = errno;
    *post_out = pre_attr;
    *post_ok = 1;
    (void)ntfs_close_with_parent(ni, pni);
    return errno_to_nfs3(e ? e : EIO);
  }

  if ((s64)offset > na->data_size) {
    if (ntfs_attr_truncate(na, (s64)offset) != 0) {
      int e = errno;
      ntfs_attr_close(na);
      (void)ntfs_close_with_parent(ni, pni);
      return errno_to_nfs3(e ? e : EIO);
    }
  }

  uint32_t total = 0;
  while (total < count) {
    errno = 0;
    s64 w = ntfs_attr_pwrite(na, (s64)(offset + total), (s64)(count - total),
                             buf + total);
    if (w <= 0) {
      int e = errno;
      (void)ntfs_attr_pclose(na);
      ntfs_attr_close(na);
      ntfs_fill_attr(ni, ino, post_out);
      *post_ok = 1;
      (void)ntfs_close_with_parent(ni, pni);
      return errno_to_nfs3(e ? e : ENOSPC);
    }
    total += (uint32_t)w;
  }

  (void)ntfs_attr_pclose(na);
  ntfs_attr_close(na);

  struct timespec now_ts;
  clock_gettime(CLOCK_REALTIME, &now_ts);
  ntfs_time nt = timespec2ntfs(now_ts);
  ni->last_data_change_time = nt;
  ni->last_mft_change_time = nt;
  NInoFileNameSetDirty(ni);
  NInoSetDirty(ni);

  ntfs_fill_attr(ni, ino, post_out);
  *post_ok = 1;
  if (ntfs_close_with_parent(ni, pni) != 0) return NFS3ERR_IO;

  if (stable != UNSTABLE && g_ntfs->dev && g_ntfs->dev->d_ops &&
      g_ntfs->dev->d_ops->sync) {
    (void)g_ntfs->dev->d_ops->sync(g_ntfs->dev);
  }
  *written_out = total;
  *committed_out = FILE_SYNC;
  return NFS3_OK;
}

static uint32_t ntfs_op_create(uint64_t dir_ino, const char *name,
                               uint32_t create_mode, const SAttr3 *sattr,
                               uint64_t *new_ino_out, NfsAttr *obj_attr_out,
                               WccPre *dir_pre_out, NfsAttr *dir_post_out,
                               int *dir_post_ok) {
  memset(dir_pre_out, 0, sizeof(*dir_pre_out));
  *dir_post_ok = 0;

  errno = 0;
  ntfs_inode *dir_ni = ntfs_inode_open(g_ntfs, (MFT_REF)dir_ino);
  if (!dir_ni) return NFS3ERR_STALE;
  NfsAttr dpre;
  ntfs_fill_attr(dir_ni, dir_ino, &dpre);
  attr_to_wcc_pre(1, &dpre, dir_pre_out);

  if ((dir_ni->mrec->flags & MFT_RECORD_IS_DIRECTORY) == 0) {
    *dir_post_out = dpre;
    *dir_post_ok = 1;
    (void)ntfs_inode_close(dir_ni);
    return NFS3ERR_NOTDIR;
  }

  ntfschar *u = NULL;
  int ulen = 0;
  uint32_t st = ntfs_utf8_to_ucs(name, &u, &ulen);
  if (st != NFS3_OK) {
    *dir_post_out = dpre;
    *dir_post_ok = 1;
    (void)ntfs_inode_close(dir_ni);
    return st;
  }

  errno = 0;
  u64 existing = ntfs_inode_lookup_by_name(dir_ni, u, ulen);
  if (existing != (u64)-1) {
    free(u);
    uint64_t eino = (uint64_t)MREF(existing);
    if (create_mode != CREATE_UNCHECKED) {
      ntfs_fill_attr(dir_ni, dir_ino, dir_post_out);
      *dir_post_ok = 1;
      (void)ntfs_inode_close(dir_ni);
      return NFS3ERR_EXIST;
    }
    (void)ntfs_inode_close(dir_ni);
    cache_set_parent(eino, dir_ino);
    if (sattr) {
      st = ntfs_op_setattr(eino, sattr, obj_attr_out);
    } else {
      st = ntfs_op_getattr(eino, obj_attr_out);
    }
    *new_ino_out = eino;
    *dir_post_ok = (ntfs_op_getattr(dir_ino, dir_post_out) == NFS3_OK);
    return st;
  }
  if (errno != ENOENT) {
    int e = errno;
    free(u);
    ntfs_fill_attr(dir_ni, dir_ino, dir_post_out);
    *dir_post_ok = 1;
    (void)ntfs_inode_close(dir_ni);
    return errno_to_nfs3(e ? e : EIO);
  }

  errno = 0;
  ntfs_inode *cni = ntfs_create(dir_ni, const_cpu_to_le32(0), u, (u8)ulen, S_IFREG);
  int e = errno;
  free(u);
  if (!cni) {
    ntfs_fill_attr(dir_ni, dir_ino, dir_post_out);
    *dir_post_ok = 1;
    (void)ntfs_inode_close(dir_ni);
    return errno_to_nfs3(e ? e : EIO);
  }

  uint64_t cino = (uint64_t)MREF(cni->mft_no);
  cache_set_parent(cino, dir_ino);
  if (sattr) {
    if (sattr->set_size && sattr->size > 0) {
      ntfs_attr *na = ntfs_attr_open(cni, AT_DATA, AT_UNNAMED, 0);
      if (na) {
        (void)ntfs_attr_truncate(na, (s64)sattr->size);
        (void)ntfs_attr_pclose(na);
        ntfs_attr_close(na);
      }
    }
    ntfs_apply_sattr_times(cni, sattr);
  }
  ntfs_fill_attr(cni, cino, obj_attr_out);
  if (ntfs_inode_close_in_dir(cni, dir_ni) != 0) {
    (void)ntfs_inode_close(dir_ni);
    return NFS3ERR_IO;
  }
  ntfs_fill_attr(dir_ni, dir_ino, dir_post_out);
  *dir_post_ok = 1;
  if (ntfs_inode_close(dir_ni) != 0) return NFS3ERR_IO;
  *new_ino_out = cino;
  return NFS3_OK;
}

static uint32_t ntfs_op_mkdir(uint64_t dir_ino, const char *name,
                              const SAttr3 *sattr, uint64_t *new_ino_out,
                              NfsAttr *obj_attr_out, WccPre *dir_pre_out,
                              NfsAttr *dir_post_out, int *dir_post_ok) {
  memset(dir_pre_out, 0, sizeof(*dir_pre_out));
  *dir_post_ok = 0;

  errno = 0;
  ntfs_inode *dir_ni = ntfs_inode_open(g_ntfs, (MFT_REF)dir_ino);
  if (!dir_ni) return NFS3ERR_STALE;
  NfsAttr dpre;
  ntfs_fill_attr(dir_ni, dir_ino, &dpre);
  attr_to_wcc_pre(1, &dpre, dir_pre_out);

  if ((dir_ni->mrec->flags & MFT_RECORD_IS_DIRECTORY) == 0) {
    *dir_post_out = dpre;
    *dir_post_ok = 1;
    (void)ntfs_inode_close(dir_ni);
    return NFS3ERR_NOTDIR;
  }

  ntfschar *u = NULL;
  int ulen = 0;
  uint32_t st = ntfs_utf8_to_ucs(name, &u, &ulen);
  if (st != NFS3_OK) {
    *dir_post_out = dpre;
    *dir_post_ok = 1;
    (void)ntfs_inode_close(dir_ni);
    return st;
  }

  errno = 0;
  if (ntfs_inode_lookup_by_name(dir_ni, u, ulen) != (u64)-1) {
    free(u);
    *dir_post_out = dpre;
    *dir_post_ok = 1;
    (void)ntfs_inode_close(dir_ni);
    return NFS3ERR_EXIST;
  }

  errno = 0;
  ntfs_inode *cni = ntfs_create(dir_ni, const_cpu_to_le32(0), u, (u8)ulen, S_IFDIR);
  int e = errno;
  free(u);
  if (!cni) {
    ntfs_fill_attr(dir_ni, dir_ino, dir_post_out);
    *dir_post_ok = 1;
    (void)ntfs_inode_close(dir_ni);
    return errno_to_nfs3(e ? e : EIO);
  }

  uint64_t cino = (uint64_t)MREF(cni->mft_no);
  cache_set_parent(cino, dir_ino);
  if (sattr) ntfs_apply_sattr_times(cni, sattr);
  ntfs_fill_attr(cni, cino, obj_attr_out);
  if (ntfs_inode_close_in_dir(cni, dir_ni) != 0) {
    (void)ntfs_inode_close(dir_ni);
    return NFS3ERR_IO;
  }
  ntfs_fill_attr(dir_ni, dir_ino, dir_post_out);
  *dir_post_ok = 1;
  if (ntfs_inode_close(dir_ni) != 0) return NFS3ERR_IO;
  *new_ino_out = cino;
  return NFS3_OK;
}

static uint32_t ntfs_op_symlink(uint64_t dir_ino, const char *name,
                                const SAttr3 *sattr, const char *target,
                                uint64_t *new_ino_out, NfsAttr *obj_attr_out,
                                WccPre *dir_pre_out, NfsAttr *dir_post_out,
                                int *dir_post_ok) {
  memset(dir_pre_out, 0, sizeof(*dir_pre_out));
  *dir_post_ok = 0;
  if (!target || !target[0]) return NFS3ERR_INVAL;

  errno = 0;
  ntfs_inode *dir_ni = ntfs_inode_open(g_ntfs, (MFT_REF)dir_ino);
  if (!dir_ni) return NFS3ERR_STALE;
  NfsAttr dpre;
  ntfs_fill_attr(dir_ni, dir_ino, &dpre);
  attr_to_wcc_pre(1, &dpre, dir_pre_out);

  if ((dir_ni->mrec->flags & MFT_RECORD_IS_DIRECTORY) == 0) {
    *dir_post_out = dpre;
    *dir_post_ok = 1;
    (void)ntfs_inode_close(dir_ni);
    return NFS3ERR_NOTDIR;
  }

  ntfschar *u = NULL;
  int ulen = 0;
  uint32_t st = ntfs_utf8_to_ucs(name, &u, &ulen);
  if (st != NFS3_OK) {
    *dir_post_out = dpre;
    *dir_post_ok = 1;
    (void)ntfs_inode_close(dir_ni);
    return st;
  }

  if (ntfs_inode_lookup_by_name(dir_ni, u, ulen) != (u64)-1) {
    free(u);
    *dir_post_out = dpre;
    *dir_post_ok = 1;
    (void)ntfs_inode_close(dir_ni);
    return NFS3ERR_EXIST;
  }

  ntfschar *utarget = NULL;
  int utlen = ntfs_mbstoucs(target, &utarget);
  if (utlen < 0 || !utarget) {
    free(u);
    free(utarget);
    *dir_post_out = dpre;
    *dir_post_ok = 1;
    (void)ntfs_inode_close(dir_ni);
    return NFS3ERR_INVAL;
  }

  errno = 0;
  ntfs_inode *lni = ntfs_create_symlink(dir_ni, const_cpu_to_le32(0),
                                        u, (u8)ulen, utarget, utlen);
  int e = errno;
  free(u);
  free(utarget);
  if (!lni) {
    ntfs_fill_attr(dir_ni, dir_ino, dir_post_out);
    *dir_post_ok = 1;
    (void)ntfs_inode_close(dir_ni);
    return errno_to_nfs3(e ? e : EIO);
  }

  uint64_t lino = (uint64_t)MREF(lni->mft_no);
  cache_set_parent(lino, dir_ino);
  if (sattr) ntfs_apply_sattr_times(lni, sattr);
  ntfs_fill_attr(lni, lino, obj_attr_out);
  if (ntfs_inode_close_in_dir(lni, dir_ni) != 0) {
    (void)ntfs_inode_close(dir_ni);
    return NFS3ERR_IO;
  }
  ntfs_fill_attr(dir_ni, dir_ino, dir_post_out);
  *dir_post_ok = 1;
  if (ntfs_inode_close(dir_ni) != 0) return NFS3ERR_IO;
  *new_ino_out = lino;
  return NFS3_OK;
}

static uint32_t ntfs_op_unlink_common(uint64_t dir_ino, const char *name,
                                      int expect_dir, WccPre *dir_pre_out,
                                      NfsAttr *dir_post_out, int *dir_post_ok) {
  memset(dir_pre_out, 0, sizeof(*dir_pre_out));
  *dir_post_ok = 0;

  errno = 0;
  ntfs_inode *dir_ni = ntfs_inode_open(g_ntfs, (MFT_REF)dir_ino);
  if (!dir_ni) return NFS3ERR_STALE;
  NfsAttr dpre;
  ntfs_fill_attr(dir_ni, dir_ino, &dpre);
  attr_to_wcc_pre(1, &dpre, dir_pre_out);

  if ((dir_ni->mrec->flags & MFT_RECORD_IS_DIRECTORY) == 0) {
    *dir_post_out = dpre;
    *dir_post_ok = 1;
    (void)ntfs_inode_close(dir_ni);
    return NFS3ERR_NOTDIR;
  }

  ntfschar *u = NULL;
  int ulen = 0;
  uint32_t st = ntfs_utf8_to_ucs(name, &u, &ulen);
  if (st != NFS3_OK) {
    *dir_post_out = dpre;
    *dir_post_ok = 1;
    (void)ntfs_inode_close(dir_ni);
    return st;
  }

  errno = 0;
  u64 mref = ntfs_inode_lookup_by_name(dir_ni, u, ulen);
  if (mref == (u64)-1) {
    int e = errno;
    free(u);
    *dir_post_out = dpre;
    *dir_post_ok = 1;
    (void)ntfs_inode_close(dir_ni);
    return errno_to_nfs3(e ? e : ENOENT);
  }

  uint64_t cino = (uint64_t)MREF(mref);
  if (cino < FILE_first_user) {
    free(u);
    *dir_post_out = dpre;
    *dir_post_ok = 1;
    (void)ntfs_inode_close(dir_ni);
    return NFS3ERR_ACCES;
  }

  errno = 0;
  ntfs_inode *cni = ntfs_inode_open(g_ntfs, (MFT_REF)cino);
  if (!cni) {
    int e = errno;
    free(u);
    *dir_post_out = dpre;
    *dir_post_ok = 1;
    (void)ntfs_inode_close(dir_ni);
    return errno_to_nfs3(e ? e : EIO);
  }

  int is_dir = ((cni->mrec->flags & MFT_RECORD_IS_DIRECTORY) != 0);
  if (expect_dir && !is_dir) {
    free(u);
    (void)ntfs_inode_close(cni);
    *dir_post_out = dpre;
    *dir_post_ok = 1;
    (void)ntfs_inode_close(dir_ni);
    return NFS3ERR_NOTDIR;
  }
  if (!expect_dir && is_dir) {
    free(u);
    (void)ntfs_inode_close(cni);
    *dir_post_out = dpre;
    *dir_post_ok = 1;
    (void)ntfs_inode_close(dir_ni);
    return NFS3ERR_ISDIR;
  }

  /* Note: ntfs_delete ALWAYS closes both cni and dir_ni internally. */
  errno = 0;
  int drc = ntfs_delete(g_ntfs, NULL, cni, dir_ni, u, (u8)ulen);
  int e = errno;
  free(u);
  *dir_post_ok = (ntfs_op_getattr(dir_ino, dir_post_out) == NFS3_OK);
  if (drc != 0) {
    return errno_to_nfs3(e ? e : EIO);
  }
  return NFS3_OK;
}

static uint32_t ntfs_op_remove(uint64_t dir_ino, const char *name,
                               WccPre *dir_pre_out, NfsAttr *dir_post_out,
                               int *dir_post_ok) {
  return ntfs_op_unlink_common(dir_ino, name, 0, dir_pre_out, dir_post_out, dir_post_ok);
}

static uint32_t ntfs_op_rmdir(uint64_t dir_ino, const char *name,
                              WccPre *dir_pre_out, NfsAttr *dir_post_out,
                              int *dir_post_ok) {
  return ntfs_op_unlink_common(dir_ino, name, 1, dir_pre_out, dir_post_out, dir_post_ok);
}

static uint32_t ntfs_op_rename(uint64_t from_dir_ino, const char *from_name,
                               uint64_t to_dir_ino, const char *to_name,
                               WccPre *from_pre_out, NfsAttr *from_post_out, int *from_post_ok,
                               WccPre *to_pre_out, NfsAttr *to_post_out, int *to_post_ok) {
  memset(from_pre_out, 0, sizeof(*from_pre_out));
  memset(to_pre_out, 0, sizeof(*to_pre_out));
  *from_post_ok = 0;
  *to_post_ok = 0;

  NfsAttr fpre, tpre;
  int fok = (ntfs_op_getattr(from_dir_ino, &fpre) == NFS3_OK);
  int tok = (ntfs_op_getattr(to_dir_ino, &tpre) == NFS3_OK);
  attr_to_wcc_pre(fok, &fpre, from_pre_out);
  attr_to_wcc_pre(tok, &tpre, to_pre_out);
  if (!fok || !tok) return NFS3ERR_STALE;
  if (fpre.type != NF3DIR || tpre.type != NF3DIR) {
    *from_post_out = fpre; *from_post_ok = 1;
    *to_post_out = tpre;   *to_post_ok = 1;
    return NFS3ERR_NOTDIR;
  }

  ntfschar *fu = NULL, *tu = NULL;
  int fulen = 0, tulen = 0;
  uint32_t st = ntfs_utf8_to_ucs(from_name, &fu, &fulen);
  if (st == NFS3_OK) st = ntfs_utf8_to_ucs(to_name, &tu, &tulen);
  if (st != NFS3_OK) {
    free(fu); free(tu);
    *from_post_out = fpre; *from_post_ok = 1;
    *to_post_out = tpre;   *to_post_ok = 1;
    return st;
  }

  uint64_t from_ino = 0;
  NfsAttr from_attr;
  st = ntfs_op_lookup(from_dir_ino, from_name, &from_ino, &from_attr);
  if (st != NFS3_OK) {
    free(fu); free(tu);
    *from_post_out = fpre; *from_post_ok = 1;
    *to_post_out = tpre;   *to_post_ok = 1;
    return st;
  }

  uint64_t to_ino = 0;
  NfsAttr to_attr;
  uint32_t tst = ntfs_op_lookup(to_dir_ino, to_name, &to_ino, &to_attr);
  if (tst == NFS3_OK) {
    if (to_ino == from_ino) {
      free(fu); free(tu);
      *from_post_out = fpre; *from_post_ok = 1;
      *to_post_out = tpre;   *to_post_ok = 1;
      return NFS3_OK;
    }
    if (from_attr.type == NF3DIR && to_attr.type != NF3DIR) {
      free(fu); free(tu);
      *from_post_out = fpre; *from_post_ok = 1;
      *to_post_out = tpre;   *to_post_ok = 1;
      return NFS3ERR_NOTDIR;
    }
    if (from_attr.type != NF3DIR && to_attr.type == NF3DIR) {
      free(fu); free(tu);
      *from_post_out = fpre; *from_post_ok = 1;
      *to_post_out = tpre;   *to_post_ok = 1;
      return NFS3ERR_ISDIR;
    }
    WccPre dummy_pre;
    NfsAttr dummy_post;
    int dummy_ok = 0;
    st = ntfs_op_unlink_common(to_dir_ino, to_name, (to_attr.type == NF3DIR),
                               &dummy_pre, &dummy_post, &dummy_ok);
    if (st != NFS3_OK) {
      free(fu); free(tu);
      *from_post_ok = (ntfs_op_getattr(from_dir_ino, from_post_out) == NFS3_OK);
      *to_post_ok = (ntfs_op_getattr(to_dir_ino, to_post_out) == NFS3_OK);
      return st;
    }
  } else if (tst != NFS3ERR_NOENT) {
    free(fu); free(tu);
    *from_post_out = fpre; *from_post_ok = 1;
    *to_post_out = tpre;   *to_post_ok = 1;
    return tst;
  }

  /* Step 1: link into destination directory and close cleanly */
  errno = 0;
  ntfs_inode *to_dir_ni = ntfs_inode_open(g_ntfs, (MFT_REF)to_dir_ino);
  ntfs_inode *ni = to_dir_ni ? ntfs_inode_open(g_ntfs, (MFT_REF)from_ino) : NULL;
  if (!to_dir_ni || !ni) {
    int e = errno;
    if (ni) (void)ntfs_inode_close(ni);
    if (to_dir_ni) (void)ntfs_inode_close(to_dir_ni);
    free(fu); free(tu);
    *from_post_ok = (ntfs_op_getattr(from_dir_ino, from_post_out) == NFS3_OK);
    *to_post_ok = (ntfs_op_getattr(to_dir_ino, to_post_out) == NFS3_OK);
    return errno_to_nfs3(e ? e : EIO);
  }

  errno = 0;
  int lrc = ntfs_link(ni, to_dir_ni, tu, (u8)tulen);
  int le = errno;
  (void)ntfs_inode_close_in_dir(ni, to_dir_ni);
  (void)ntfs_inode_close(to_dir_ni);
  free(tu);
  if (lrc != 0) {
    free(fu);
    *from_post_ok = (ntfs_op_getattr(from_dir_ino, from_post_out) == NFS3_OK);
    *to_post_ok = (ntfs_op_getattr(to_dir_ino, to_post_out) == NFS3_OK);
    return errno_to_nfs3(le ? le : EIO);
  }

  /* Step 2: unlink from source directory (ntfs_delete closes both ni and from_dir_ni) */
  errno = 0;
  ntfs_inode *from_dir_ni = ntfs_inode_open(g_ntfs, (MFT_REF)from_dir_ino);
  ni = from_dir_ni ? ntfs_inode_open(g_ntfs, (MFT_REF)from_ino) : NULL;
  if (!from_dir_ni || !ni) {
    int e = errno;
    if (ni) (void)ntfs_inode_close(ni);
    if (from_dir_ni) (void)ntfs_inode_close(from_dir_ni);
    free(fu);
    *from_post_ok = (ntfs_op_getattr(from_dir_ino, from_post_out) == NFS3_OK);
    *to_post_ok = (ntfs_op_getattr(to_dir_ino, to_post_out) == NFS3_OK);
    return errno_to_nfs3(e ? e : EIO);
  }

  errno = 0;
  int drc = ntfs_delete(g_ntfs, NULL, ni, from_dir_ni, fu, (u8)fulen);
  int de = errno;
  free(fu);
  cache_set_parent(from_ino, to_dir_ino);
  *from_post_ok = (ntfs_op_getattr(from_dir_ino, from_post_out) == NFS3_OK);
  *to_post_ok = (ntfs_op_getattr(to_dir_ino, to_post_out) == NFS3_OK);
  if (drc != 0) return errno_to_nfs3(de ? de : EIO);
  return NFS3_OK;
}

static uint32_t ntfs_op_link(uint64_t ino, uint64_t dir_ino, const char *name,
                             NfsAttr *file_attr_out, int *file_attr_ok,
                             WccPre *dir_pre_out, NfsAttr *dir_post_out,
                             int *dir_post_ok) {
  *file_attr_ok = 0;
  memset(dir_pre_out, 0, sizeof(*dir_pre_out));
  *dir_post_ok = 0;

  NfsAttr dpre;
  if (ntfs_op_getattr(dir_ino, &dpre) != NFS3_OK) return NFS3ERR_STALE;
  attr_to_wcc_pre(1, &dpre, dir_pre_out);
  if (dpre.type != NF3DIR) {
    *dir_post_out = dpre; *dir_post_ok = 1;
    return NFS3ERR_NOTDIR;
  }

  ntfschar *u = NULL;
  int ulen = 0;
  uint32_t st = ntfs_utf8_to_ucs(name, &u, &ulen);
  if (st != NFS3_OK) {
    *dir_post_out = dpre; *dir_post_ok = 1;
    return st;
  }

  ntfs_inode *dir_ni = ntfs_inode_open(g_ntfs, (MFT_REF)dir_ino);
  ntfs_inode *ni = dir_ni ? ntfs_inode_open(g_ntfs, (MFT_REF)ino) : NULL;
  if (!dir_ni || !ni) {
    if (ni) (void)ntfs_inode_close(ni);
    if (dir_ni) (void)ntfs_inode_close(dir_ni);
    free(u);
    return NFS3ERR_STALE;
  }
  if ((ni->mrec->flags & MFT_RECORD_IS_DIRECTORY) != 0) {
    ntfs_fill_attr(ni, ino, file_attr_out); *file_attr_ok = 1;
    (void)ntfs_inode_close(ni);
    ntfs_fill_attr(dir_ni, dir_ino, dir_post_out); *dir_post_ok = 1;
    (void)ntfs_inode_close(dir_ni);
    free(u);
    return NFS3ERR_ISDIR;
  }

  errno = 0;
  int rc = ntfs_link(ni, dir_ni, u, (u8)ulen);
  int e = errno;
  free(u);
  ntfs_fill_attr(ni, ino, file_attr_out);
  *file_attr_ok = 1;
  (void)ntfs_inode_close_in_dir(ni, dir_ni);
  ntfs_fill_attr(dir_ni, dir_ino, dir_post_out);
  *dir_post_ok = 1;
  (void)ntfs_inode_close(dir_ni);
  return (rc == 0) ? NFS3_OK : errno_to_nfs3(e ? e : EIO);
}

typedef struct {
  uint64_t dir_ino;
  DirEntryList *list;
  int oom;
} NtfsReaddirCtx;

static int ntfs_filldir_cb(void *dirent, const ntfschar *name, const int name_len,
                           const int name_type, const s64 pos, const MFT_REF mref,
                           const unsigned dt_type) {
  (void)pos;
  (void)dt_type;
  NtfsReaddirCtx *ctx = (NtfsReaddirCtx *)dirent;
  if (name_type == FILE_NAME_DOS) return 0;
  uint64_t cino = (uint64_t)MREF(mref);
  if (cino < FILE_first_user) return 0;

  char *utf8 = NULL;
  int mlen = ntfs_ucstombs(name, name_len, &utf8, 0);
  if (mlen <= 0 || !utf8) {
    free(utf8);
    return 0;
  }
  if (strcmp(utf8, ".") != 0 && strcmp(utf8, "..") != 0) {
    cache_set_parent(cino, ctx->dir_ino);
    if (dirlist_push(ctx->list, cino, utf8) != 0) {
      ctx->oom = 1;
      free(utf8);
      return -1;
    }
  }
  free(utf8);
  return 0;
}

static uint32_t ntfs_op_readdir(uint64_t dir_ino, DirEntryList *list_out,
                                NfsAttr *dir_attr_out, int *dir_attr_ok) {
  *dir_attr_ok = 0;
  memset(list_out, 0, sizeof(*list_out));

  errno = 0;
  ntfs_inode *dir_ni = ntfs_inode_open(g_ntfs, (MFT_REF)dir_ino);
  if (!dir_ni) return NFS3ERR_STALE;
  ntfs_fill_attr(dir_ni, dir_ino, dir_attr_out);
  *dir_attr_ok = 1;
  if ((dir_ni->mrec->flags & MFT_RECORD_IS_DIRECTORY) == 0) {
    (void)ntfs_inode_close(dir_ni);
    return NFS3ERR_NOTDIR;
  }

  uint64_t pino = ntfs_resolve_parent_ino(dir_ni, dir_ino);
  if (pino == 0) pino = FILE_root;
  if (dirlist_push(list_out, dir_ino, ".") != 0 ||
      dirlist_push(list_out, pino, "..") != 0) {
    (void)ntfs_inode_close(dir_ni);
    dirlist_free(list_out);
    return NFS3ERR_SERVERFAULT;
  }

  NtfsReaddirCtx rctx = {dir_ino, list_out, 0};
  s64 pos = 0;
  errno = 0;
  int rc = ntfs_readdir(dir_ni, &pos, &rctx, ntfs_filldir_cb);
  int e = errno;
  (void)ntfs_inode_close(dir_ni);
  if (rctx.oom) {
    dirlist_free(list_out);
    return NFS3ERR_SERVERFAULT;
  }
  if (rc != 0) {
    dirlist_free(list_out);
    return errno_to_nfs3(e ? e : EIO);
  }
  return NFS3_OK;
}

static uint32_t ntfs_op_fsstat(uint64_t ino, FsStatInfo *st_out,
                               NfsAttr *attr_out, int *attr_ok) {
  *attr_ok = (ntfs_op_getattr(ino, attr_out) == NFS3_OK);
  if (!*attr_ok) return NFS3ERR_STALE;

  (void)ntfs_volume_get_free_space(g_ntfs);
  uint64_t cs = (uint64_t)g_ntfs->cluster_size;
  uint64_t total_c = (g_ntfs->nr_clusters > 0) ? (uint64_t)g_ntfs->nr_clusters : 0ULL;
  uint64_t free_c = (g_ntfs->free_clusters > 0) ? (uint64_t)g_ntfs->free_clusters : 0ULL;
  if (free_c > total_c) free_c = total_c;

  st_out->tbytes = total_c * cs;
  st_out->fbytes = free_c * cs;
  st_out->abytes = free_c * cs;
  uint64_t total_mft = 4096ULL;
  if (g_ntfs->mft_na && g_ntfs->mft_record_size > 0 && g_ntfs->mft_na->data_size > 0) {
    total_mft = (uint64_t)g_ntfs->mft_na->data_size / (uint64_t)g_ntfs->mft_record_size;
  }
  uint64_t free_mft = (g_ntfs->free_mft_records > 0) ? (uint64_t)g_ntfs->free_mft_records : 1024ULL;
  if (total_mft < free_mft) total_mft = free_mft + 64ULL;
  st_out->tfiles = total_mft;
  st_out->ffiles = free_mft;
  st_out->afiles = free_mft;
  return NFS3_OK;
}

static uint32_t ntfs_op_commit(uint64_t ino, WccPre *pre_out,
                               NfsAttr *post_out, int *post_ok) {
  memset(pre_out, 0, sizeof(*pre_out));
  *post_ok = (ntfs_op_getattr(ino, post_out) == NFS3_OK);
  if (!*post_ok) return NFS3ERR_STALE;
  attr_to_wcc_pre(1, post_out, pre_out);
  if (g_ntfs->dev && g_ntfs->dev->d_ops && g_ntfs->dev->d_ops->sync) {
    if (g_ntfs->dev->d_ops->sync(g_ntfs->dev) != 0) return NFS3ERR_IO;
  }
  return NFS3_OK;
}

/* =========================================================================
 * libext2fs backend (ext2 / ext3 / ext4)
 * ========================================================================= */

static uint32_t ext_err_to_nfs3(errcode_t ec) {
  if (ec == 0) return NFS3_OK;
  if (ec == EXT2_ET_FILE_NOT_FOUND) return NFS3ERR_NOENT;
  if (ec == EXT2_ET_FILE_EXISTS) return NFS3ERR_EXIST;
  if (ec == EXT2_ET_DIR_NO_SPACE || ec == EXT2_ET_DIR_NO_SPACE_FOR_CSUM) return NFS3ERR_NOSPC;
  if (ec == EXT2_ET_RO_FILSYS) return NFS3ERR_ROFS;
  if (ec == EXT2_ET_INVALID_ARGUMENT) return NFS3ERR_INVAL;
  if (ec == EXT2_ET_TOOSMALL) return NFS3ERR_TOOSMALL;
  if (ec > 0 && ec < 256) return errno_to_nfs3((int)ec);
  return NFS3ERR_IO;
}

static uint32_t ext_now_sec(void) {
  struct timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);
  return (ts.tv_sec > 0) ? (uint32_t)ts.tv_sec : 0U;
}

static uint32_t ext_read_valid_inode(uint64_t ino, struct ext2_inode *out) {
  if (!g_ext || ino < EXT2_ROOT_INO || ino > (uint64_t)g_ext->super->s_inodes_count) {
    return NFS3ERR_STALE;
  }
  if (g_ext->inode_map &&
      !ext2fs_test_inode_bitmap2(g_ext->inode_map, (ext2_ino_t)ino)) {
    return NFS3ERR_STALE;
  }
  errcode_t ec = ext2fs_read_inode(g_ext, (ext2_ino_t)ino, out);
  if (ec != 0) return ext_err_to_nfs3(ec);
  if (out->i_links_count == 0) return NFS3ERR_STALE;
  return NFS3_OK;
}

static void ext_fill_attr(uint64_t ino, const struct ext2_inode *in, NfsAttr *a) {
  memset(a, 0, sizeof(*a));
  uint16_t m = in->i_mode;
  if (LINUX_S_ISDIR(m))      a->type = NF3DIR;
  else if (LINUX_S_ISLNK(m)) a->type = NF3LNK;
  else if (LINUX_S_ISBLK(m)) a->type = NF3BLK;
  else if (LINUX_S_ISCHR(m)) a->type = NF3CHR;
  else if (LINUX_S_ISFIFO(m)) a->type = NF3FIFO;
  else if (LINUX_S_ISSOCK(m)) a->type = NF3SOCK;
  else                       a->type = NF3REG;

  uint32_t perm = (uint32_t)(m & 07777U);
  if (a->type == NF3DIR) perm |= 0700U;
  else if (a->type == NF3REG) perm |= 0600U;
  a->mode = perm;
  a->nlink = in->i_links_count ? (uint32_t)in->i_links_count : 1U;
  /* Present files as owned by the mounting user so non-root desktop mounts
   * can write cleanly into mke2fs-formatted root directories (i_uid=0). */
  a->uid = g_uid;
  a->gid = g_gid;
  if (a->type == NF3DIR) {
    a->size = (uint64_t)in->i_size;
  } else if (a->type == NF3LNK) {
    a->size = (uint64_t)in->i_size;
  } else {
    a->size = (uint64_t)EXT2_I_SIZE(in);
  }
  struct ext2_inode tmp = *in;
  a->used = (uint64_t)ext2fs_get_stat_i_blocks(g_ext, &tmp) * 512ULL;
  a->fsid = g_fsid;
  a->fileid = ino;
  a->atime_sec = in->i_atime;
  a->mtime_sec = in->i_mtime;
  a->ctime_sec = in->i_ctime;
}

static uint32_t ext_op_getattr(uint64_t ino, NfsAttr *out) {
  struct ext2_inode inode;
  uint32_t st = ext_read_valid_inode(ino, &inode);
  if (st != NFS3_OK) return st;
  ext_fill_attr(ino, &inode, out);
  return NFS3_OK;
}

static int ext_link_with_expand(ext2_ino_t dir_ino, const char *name,
                                ext2_ino_t target_ino, int ft_flag,
                                errcode_t *ec_out) {
  errcode_t ec = ext2fs_link(g_ext, dir_ino, name, target_ino, ft_flag);
  if (ec == EXT2_ET_DIR_NO_SPACE || ec == EXT2_ET_DIR_NO_SPACE_FOR_CSUM) {
    ec = ext2fs_expand_dir(g_ext, dir_ino);
    if (ec == 0) {
      ec = ext2fs_link(g_ext, dir_ino, name, target_ino, ft_flag);
    }
  }
  if (ec_out) *ec_out = ec;
  return (ec == 0) ? 0 : -1;
}

static void ext_touch_dir(ext2_ino_t dir_ino) {
  struct ext2_inode din;
  if (ext2fs_read_inode(g_ext, dir_ino, &din) == 0) {
    uint32_t now = ext_now_sec();
    din.i_mtime = now;
    din.i_ctime = now;
    (void)ext2fs_write_inode(g_ext, dir_ino, &din);
  }
}

static void ext_apply_sattr_meta(struct ext2_inode *in, const SAttr3 *s) {
  uint32_t now = ext_now_sec();
  int touched = 0;
  if (s->set_mode) {
    in->i_mode = (uint16_t)((in->i_mode & LINUX_S_IFMT) | (s->mode & 07777U));
    touched = 1;
  }
  if (s->set_uid) {
    in->i_uid = (uint16_t)(s->uid & 0xFFFFU);
    ext2fs_set_i_uid_high(*in, (uint16_t)(s->uid >> 16));
    touched = 1;
  }
  if (s->set_gid) {
    in->i_gid = (uint16_t)(s->gid & 0xFFFFU);
    in->osd2.linux2.l_i_gid_high = (uint16_t)(s->gid >> 16);
    touched = 1;
  }
  if (s->set_atime == SET_TO_SERVER_TIME) {
    in->i_atime = now;
    touched = 1;
  } else if (s->set_atime == SET_TO_CLIENT_TIME) {
    in->i_atime = s->atime_sec;
    touched = 1;
  }
  if (s->set_mtime == SET_TO_SERVER_TIME) {
    in->i_mtime = now;
    touched = 1;
  } else if (s->set_mtime == SET_TO_CLIENT_TIME) {
    in->i_mtime = s->mtime_sec;
    touched = 1;
  }
  if (touched) {
    in->i_ctime = now;
  }
}

static uint32_t ext_op_setattr(uint64_t ino, const SAttr3 *s, NfsAttr *post_out) {
  struct ext2_inode inode;
  uint32_t st = ext_read_valid_inode(ino, &inode);
  if (st != NFS3_OK) return st;

  if (s->set_size) {
    if (LINUX_S_ISDIR(inode.i_mode)) return NFS3ERR_ISDIR;
    if (!LINUX_S_ISREG(inode.i_mode)) return NFS3ERR_INVAL;
    ext2_file_t ef = NULL;
    errcode_t ec = ext2fs_file_open(g_ext, (ext2_ino_t)ino, EXT2_FILE_WRITE, &ef);
    if (ec != 0) return ext_err_to_nfs3(ec);
    ec = ext2fs_file_set_size2(ef, (ext2_off64_t)s->size);
    errcode_t cec = ext2fs_file_close(ef);
    if (ec != 0) return ext_err_to_nfs3(ec);
    if (cec != 0) return ext_err_to_nfs3(cec);
    if (ext2fs_read_inode(g_ext, (ext2_ino_t)ino, &inode) != 0) return NFS3ERR_IO;
    uint32_t now = ext_now_sec();
    inode.i_mtime = now;
    inode.i_ctime = now;
  }

  ext_apply_sattr_meta(&inode, s);
  errcode_t wec = ext2fs_write_inode(g_ext, (ext2_ino_t)ino, &inode);
  if (wec != 0) return ext_err_to_nfs3(wec);
  ext_fill_attr(ino, &inode, post_out);
  return NFS3_OK;
}

static uint32_t ext_op_lookup(uint64_t dir_ino, const char *name,
                              uint64_t *child_ino_out, NfsAttr *child_attr_out) {
  if (!name || !name[0] || strchr(name, '/') != NULL) return NFS3ERR_INVAL;
  struct ext2_inode din;
  uint32_t st = ext_read_valid_inode(dir_ino, &din);
  if (st != NFS3_OK) return st;
  if (!LINUX_S_ISDIR(din.i_mode)) return NFS3ERR_NOTDIR;

  ext2_ino_t cino = 0;
  errcode_t ec = ext2fs_lookup(g_ext, (ext2_ino_t)dir_ino, name,
                               (int)strlen(name), NULL, &cino);
  if (ec != 0) return ext_err_to_nfs3(ec);

  struct ext2_inode cin;
  st = ext_read_valid_inode((uint64_t)cino, &cin);
  if (st != NFS3_OK) return st;
  if (strcmp(name, ".") != 0 && strcmp(name, "..") != 0) {
    cache_set_parent((uint64_t)cino, dir_ino);
  }
  *child_ino_out = (uint64_t)cino;
  ext_fill_attr((uint64_t)cino, &cin, child_attr_out);
  return NFS3_OK;
}

static uint32_t ext_op_readlink(uint64_t ino, char *out, size_t cap,
                                NfsAttr *attr_out, int *attr_ok) {
  *attr_ok = 0;
  struct ext2_inode inode;
  uint32_t st = ext_read_valid_inode(ino, &inode);
  if (st != NFS3_OK) return st;
  ext_fill_attr(ino, &inode, attr_out);
  *attr_ok = 1;
  if (!LINUX_S_ISLNK(inode.i_mode)) return NFS3ERR_INVAL;

  size_t sz = (size_t)inode.i_size;
  if (sz >= cap) return NFS3ERR_NAMETOOLONG;

  if (ext2fs_is_fast_symlink(&inode)) {
    if (sz > sizeof(inode.i_block)) sz = sizeof(inode.i_block);
    memcpy(out, (const char *)inode.i_block, sz);
    out[sz] = '\0';
    return NFS3_OK;
  }

  ext2_file_t ef = NULL;
  errcode_t ec = ext2fs_file_open(g_ext, (ext2_ino_t)ino, 0, &ef);
  if (ec != 0) return ext_err_to_nfs3(ec);
  unsigned int got = 0;
  ec = ext2fs_file_read(ef, out, (unsigned int)sz, &got);
  (void)ext2fs_file_close(ef);
  if (ec != 0) return ext_err_to_nfs3(ec);
  out[got] = '\0';
  return NFS3_OK;
}

static uint32_t ext_op_read(uint64_t ino, uint64_t offset, uint32_t count,
                            uint8_t *buf, uint32_t *got_out, int *eof_out,
                            NfsAttr *attr_out, int *attr_ok) {
  *got_out = 0;
  *eof_out = 0;
  *attr_ok = 0;

  struct ext2_inode inode;
  uint32_t st = ext_read_valid_inode(ino, &inode);
  if (st != NFS3_OK) return st;
  ext_fill_attr(ino, &inode, attr_out);
  *attr_ok = 1;

  if (LINUX_S_ISDIR(inode.i_mode)) return NFS3ERR_ISDIR;
  if (!LINUX_S_ISREG(inode.i_mode)) return NFS3ERR_INVAL;

  uint64_t fsize = (uint64_t)EXT2_I_SIZE(&inode);
  if (offset >= fsize || count == 0) {
    *got_out = 0;
    *eof_out = (offset >= fsize) ? 1 : 0;
    return NFS3_OK;
  }

  ext2_file_t ef = NULL;
  errcode_t ec = ext2fs_file_open(g_ext, (ext2_ino_t)ino, 0, &ef);
  if (ec != 0) return ext_err_to_nfs3(ec);
  ec = ext2fs_file_llseek(ef, offset, EXT2_SEEK_SET, NULL);
  if (ec != 0) {
    (void)ext2fs_file_close(ef);
    return ext_err_to_nfs3(ec);
  }

  uint64_t avail = fsize - offset;
  uint32_t to_read = (avail < (uint64_t)count) ? (uint32_t)avail : count;
  uint32_t total = 0;
  while (total < to_read) {
    unsigned int got = 0;
    ec = ext2fs_file_read(ef, buf + total, (unsigned int)(to_read - total), &got);
    if (ec != 0) {
      (void)ext2fs_file_close(ef);
      return ext_err_to_nfs3(ec);
    }
    if (got == 0) break;
    total += got;
  }
  (void)ext2fs_file_close(ef);
  *got_out = total;
  *eof_out = (offset + total >= fsize) ? 1 : 0;
  return NFS3_OK;
}

static uint32_t ext_op_write(uint64_t ino, uint64_t offset, const uint8_t *buf,
                             uint32_t count, uint32_t stable,
                             uint32_t *written_out, uint32_t *committed_out,
                             WccPre *pre_out, NfsAttr *post_out, int *post_ok) {
  *written_out = 0;
  *committed_out = FILE_SYNC;
  memset(pre_out, 0, sizeof(*pre_out));
  *post_ok = 0;

  struct ext2_inode inode;
  uint32_t st = ext_read_valid_inode(ino, &inode);
  if (st != NFS3_OK) return st;

  NfsAttr pre_attr;
  ext_fill_attr(ino, &inode, &pre_attr);
  attr_to_wcc_pre(1, &pre_attr, pre_out);

  if (LINUX_S_ISDIR(inode.i_mode)) {
    *post_out = pre_attr; *post_ok = 1;
    return NFS3ERR_ISDIR;
  }
  if (!LINUX_S_ISREG(inode.i_mode)) {
    *post_out = pre_attr; *post_ok = 1;
    return NFS3ERR_INVAL;
  }

  ext2_file_t ef = NULL;
  errcode_t ec = ext2fs_file_open(g_ext, (ext2_ino_t)ino, EXT2_FILE_WRITE, &ef);
  if (ec != 0) {
    *post_out = pre_attr; *post_ok = 1;
    return ext_err_to_nfs3(ec);
  }

  ec = ext2fs_file_llseek(ef, offset, EXT2_SEEK_SET, NULL);
  if (ec != 0) {
    (void)ext2fs_file_close(ef);
    *post_out = pre_attr; *post_ok = 1;
    return ext_err_to_nfs3(ec);
  }

  uint32_t total = 0;
  while (total < count) {
    unsigned int wr = 0;
    ec = ext2fs_file_write(ef, buf + total, (unsigned int)(count - total), &wr);
    if (ec != 0 || wr == 0) {
      (void)ext2fs_file_close(ef);
      if (ext2fs_read_inode(g_ext, (ext2_ino_t)ino, &inode) == 0) {
        ext_fill_attr(ino, &inode, post_out);
        *post_ok = 1;
      }
      return (ec != 0) ? ext_err_to_nfs3(ec) : NFS3ERR_NOSPC;
    }
    total += wr;
  }

  ec = ext2fs_file_flush(ef);
  errcode_t cec = ext2fs_file_close(ef);
  if (ec != 0) return ext_err_to_nfs3(ec);
  if (cec != 0) return ext_err_to_nfs3(cec);

  if (ext2fs_read_inode(g_ext, (ext2_ino_t)ino, &inode) == 0) {
    uint32_t now = ext_now_sec();
    inode.i_mtime = now;
    inode.i_ctime = now;
    (void)ext2fs_write_inode(g_ext, (ext2_ino_t)ino, &inode);
    ext_fill_attr(ino, &inode, post_out);
    *post_ok = 1;
  }

  if (stable != UNSTABLE) {
    (void)ext2fs_write_bitmaps(g_ext);
    (void)ext2fs_flush(g_ext);
  }
  *written_out = total;
  *committed_out = FILE_SYNC;
  return NFS3_OK;
}

static uint32_t ext_op_create(uint64_t dir_ino, const char *name,
                              uint32_t create_mode, const SAttr3 *sattr,
                              uint64_t *new_ino_out, NfsAttr *obj_attr_out,
                              WccPre *dir_pre_out, NfsAttr *dir_post_out,
                              int *dir_post_ok) {
  memset(dir_pre_out, 0, sizeof(*dir_pre_out));
  *dir_post_ok = 0;

  struct ext2_inode din;
  uint32_t st = ext_read_valid_inode(dir_ino, &din);
  if (st != NFS3_OK) return st;
  NfsAttr dpre;
  ext_fill_attr(dir_ino, &din, &dpre);
  attr_to_wcc_pre(1, &dpre, dir_pre_out);

  if (!LINUX_S_ISDIR(din.i_mode)) {
    *dir_post_out = dpre; *dir_post_ok = 1;
    return NFS3ERR_NOTDIR;
  }
  if (!valid_leaf_name(name)) {
    *dir_post_out = dpre; *dir_post_ok = 1;
    return NFS3ERR_INVAL;
  }
  if (strlen(name) > 255U) {
    *dir_post_out = dpre; *dir_post_ok = 1;
    return NFS3ERR_NAMETOOLONG;
  }

  ext2_ino_t existing = 0;
  errcode_t lec = ext2fs_lookup(g_ext, (ext2_ino_t)dir_ino, name,
                                (int)strlen(name), NULL, &existing);
  if (lec == 0) {
    if (create_mode != CREATE_UNCHECKED) {
      *dir_post_out = dpre; *dir_post_ok = 1;
      return NFS3ERR_EXIST;
    }
    cache_set_parent((uint64_t)existing, dir_ino);
    if (sattr) {
      st = ext_op_setattr((uint64_t)existing, sattr, obj_attr_out);
    } else {
      st = ext_op_getattr((uint64_t)existing, obj_attr_out);
    }
    *new_ino_out = (uint64_t)existing;
    *dir_post_ok = (ext_op_getattr(dir_ino, dir_post_out) == NFS3_OK);
    return st;
  }

  uint32_t perm = (sattr && sattr->set_mode) ? (sattr->mode & 07777U) : 0644U;
  ext2_ino_t new_ino = 0;
  errcode_t ec = ext2fs_new_inode(g_ext, (ext2_ino_t)dir_ino,
                                  (int)(LINUX_S_IFREG | perm), 0, &new_ino);
  if (ec != 0) {
    *dir_post_out = dpre; *dir_post_ok = 1;
    return ext_err_to_nfs3(ec);
  }
  ext2fs_inode_alloc_stats2(g_ext, new_ino, +1, 0);

  uint32_t now = ext_now_sec();
  uint32_t uid = (sattr && sattr->set_uid) ? sattr->uid : g_uid;
  uint32_t gid = (sattr && sattr->set_gid) ? sattr->gid : g_gid;
  struct ext2_inode nin;
  memset(&nin, 0, sizeof(nin));
  nin.i_mode = (uint16_t)(LINUX_S_IFREG | perm);
  nin.i_uid = (uint16_t)(uid & 0xFFFFU);
  ext2fs_set_i_uid_high(nin, (uint16_t)(uid >> 16));
  nin.i_gid = (uint16_t)(gid & 0xFFFFU);
  nin.osd2.linux2.l_i_gid_high = (uint16_t)(gid >> 16);
  nin.i_links_count = 1;
  nin.i_atime = now;
  nin.i_ctime = now;
  nin.i_mtime = now;
  if (sattr) ext_apply_sattr_meta(&nin, sattr);

  ec = ext2fs_write_new_inode(g_ext, new_ino, &nin);
  if (ec == 0 && ext2fs_has_feature_extents(g_ext->super)) {
    ext2_extent_handle_t eh = NULL;
    if (ext2fs_extent_open2(g_ext, new_ino, &nin, &eh) == 0) {
      ext2fs_extent_free(eh);
    }
  }
  if (ec != 0 || ext_link_with_expand((ext2_ino_t)dir_ino, name, new_ino,
                                      EXT2_FT_REG_FILE, &ec) != 0) {
    nin.i_links_count = 0;
    nin.i_dtime = now;
    (void)ext2fs_write_inode(g_ext, new_ino, &nin);
    ext2fs_inode_alloc_stats2(g_ext, new_ino, -1, 0);
    *dir_post_ok = (ext_op_getattr(dir_ino, dir_post_out) == NFS3_OK);
    return ext_err_to_nfs3(ec);
  }

  ext_touch_dir((ext2_ino_t)dir_ino);
  cache_set_parent((uint64_t)new_ino, dir_ino);
  if (sattr && sattr->set_size && sattr->size > 0) {
    (void)ext_op_setattr((uint64_t)new_ino, sattr, obj_attr_out);
  } else {
    (void)ext_op_getattr((uint64_t)new_ino, obj_attr_out);
  }
  *dir_post_ok = (ext_op_getattr(dir_ino, dir_post_out) == NFS3_OK);
  *new_ino_out = (uint64_t)new_ino;
  return NFS3_OK;
}

static uint32_t ext_op_mkdir(uint64_t dir_ino, const char *name,
                             const SAttr3 *sattr, uint64_t *new_ino_out,
                             NfsAttr *obj_attr_out, WccPre *dir_pre_out,
                             NfsAttr *dir_post_out, int *dir_post_ok) {
  memset(dir_pre_out, 0, sizeof(*dir_pre_out));
  *dir_post_ok = 0;

  struct ext2_inode din;
  uint32_t st = ext_read_valid_inode(dir_ino, &din);
  if (st != NFS3_OK) return st;
  NfsAttr dpre;
  ext_fill_attr(dir_ino, &din, &dpre);
  attr_to_wcc_pre(1, &dpre, dir_pre_out);

  if (!LINUX_S_ISDIR(din.i_mode)) {
    *dir_post_out = dpre; *dir_post_ok = 1;
    return NFS3ERR_NOTDIR;
  }
  if (!valid_leaf_name(name)) {
    *dir_post_out = dpre; *dir_post_ok = 1;
    return NFS3ERR_INVAL;
  }
  if (strlen(name) > 255U) {
    *dir_post_out = dpre; *dir_post_ok = 1;
    return NFS3ERR_NAMETOOLONG;
  }

  ext2_ino_t existing = 0;
  if (ext2fs_lookup(g_ext, (ext2_ino_t)dir_ino, name,
                    (int)strlen(name), NULL, &existing) == 0) {
    *dir_post_out = dpre; *dir_post_ok = 1;
    return NFS3ERR_EXIST;
  }

  ext2_ino_t new_ino = 0;
  errcode_t ec = ext2fs_mkdir2(g_ext, (ext2_ino_t)dir_ino, 0, 0,
                               EXT2FS_LINK_EXPAND, name, &new_ino);
  if (ec == EXT2_ET_DIR_NO_SPACE || ec == EXT2_ET_DIR_NO_SPACE_FOR_CSUM) {
    if (ext2fs_expand_dir(g_ext, (ext2_ino_t)dir_ino) == 0) {
      ec = ext2fs_mkdir2(g_ext, (ext2_ino_t)dir_ino, 0, 0,
                         EXT2FS_LINK_EXPAND, name, &new_ino);
    }
  }
  if (ec != 0) {
    *dir_post_ok = (ext_op_getattr(dir_ino, dir_post_out) == NFS3_OK);
    return ext_err_to_nfs3(ec);
  }

  struct ext2_inode nin;
  if (ext2fs_read_inode(g_ext, new_ino, &nin) == 0) {
    uint32_t perm = (sattr && sattr->set_mode) ? (sattr->mode & 07777U) : 0755U;
    uint32_t uid = (sattr && sattr->set_uid) ? sattr->uid : g_uid;
    uint32_t gid = (sattr && sattr->set_gid) ? sattr->gid : g_gid;
    uint32_t now = ext_now_sec();
    nin.i_mode = (uint16_t)(LINUX_S_IFDIR | perm);
    nin.i_uid = (uint16_t)(uid & 0xFFFFU);
    ext2fs_set_i_uid_high(nin, (uint16_t)(uid >> 16));
    nin.i_gid = (uint16_t)(gid & 0xFFFFU);
    nin.osd2.linux2.l_i_gid_high = (uint16_t)(gid >> 16);
    nin.i_atime = now;
    nin.i_mtime = now;
    nin.i_ctime = now;
    if (sattr) ext_apply_sattr_meta(&nin, sattr);
    (void)ext2fs_write_inode(g_ext, new_ino, &nin);
    ext_fill_attr((uint64_t)new_ino, &nin, obj_attr_out);
  }
  ext_touch_dir((ext2_ino_t)dir_ino);
  cache_set_parent((uint64_t)new_ino, dir_ino);
  *dir_post_ok = (ext_op_getattr(dir_ino, dir_post_out) == NFS3_OK);
  *new_ino_out = (uint64_t)new_ino;
  return NFS3_OK;
}

static uint32_t ext_op_symlink(uint64_t dir_ino, const char *name,
                               const SAttr3 *sattr, const char *target,
                               uint64_t *new_ino_out, NfsAttr *obj_attr_out,
                               WccPre *dir_pre_out, NfsAttr *dir_post_out,
                               int *dir_post_ok) {
  memset(dir_pre_out, 0, sizeof(*dir_pre_out));
  *dir_post_ok = 0;
  if (!target || !target[0]) return NFS3ERR_INVAL;

  struct ext2_inode din;
  uint32_t st = ext_read_valid_inode(dir_ino, &din);
  if (st != NFS3_OK) return st;
  NfsAttr dpre;
  ext_fill_attr(dir_ino, &din, &dpre);
  attr_to_wcc_pre(1, &dpre, dir_pre_out);

  if (!LINUX_S_ISDIR(din.i_mode)) {
    *dir_post_out = dpre; *dir_post_ok = 1;
    return NFS3ERR_NOTDIR;
  }
  if (!valid_leaf_name(name)) {
    *dir_post_out = dpre; *dir_post_ok = 1;
    return NFS3ERR_INVAL;
  }
  if (strlen(name) > 255U || strlen(target) >= PATH_MAX) {
    *dir_post_out = dpre; *dir_post_ok = 1;
    return NFS3ERR_NAMETOOLONG;
  }

  ext2_ino_t existing = 0;
  if (ext2fs_lookup(g_ext, (ext2_ino_t)dir_ino, name,
                    (int)strlen(name), NULL, &existing) == 0) {
    *dir_post_out = dpre; *dir_post_ok = 1;
    return NFS3ERR_EXIST;
  }

  errcode_t ec = ext2fs_symlink(g_ext, (ext2_ino_t)dir_ino, 0, name, target);
  if (ec == EXT2_ET_DIR_NO_SPACE || ec == EXT2_ET_DIR_NO_SPACE_FOR_CSUM) {
    if (ext2fs_expand_dir(g_ext, (ext2_ino_t)dir_ino) == 0) {
      ec = ext2fs_symlink(g_ext, (ext2_ino_t)dir_ino, 0, name, target);
    }
  }
  if (ec != 0) {
    *dir_post_ok = (ext_op_getattr(dir_ino, dir_post_out) == NFS3_OK);
    return ext_err_to_nfs3(ec);
  }

  ext2_ino_t lino = 0;
  ec = ext2fs_lookup(g_ext, (ext2_ino_t)dir_ino, name,
                     (int)strlen(name), NULL, &lino);
  if (ec != 0) {
    *dir_post_ok = (ext_op_getattr(dir_ino, dir_post_out) == NFS3_OK);
    return ext_err_to_nfs3(ec);
  }

  struct ext2_inode lin;
  if (ext2fs_read_inode(g_ext, lino, &lin) == 0) {
    uint32_t now = ext_now_sec();
    lin.i_uid = (uint16_t)(g_uid & 0xFFFFU);
    ext2fs_set_i_uid_high(lin, (uint16_t)(g_uid >> 16));
    lin.i_gid = (uint16_t)(g_gid & 0xFFFFU);
    lin.osd2.linux2.l_i_gid_high = (uint16_t)(g_gid >> 16);
    lin.i_atime = now;
    lin.i_mtime = now;
    lin.i_ctime = now;
    if (sattr) ext_apply_sattr_meta(&lin, sattr);
    (void)ext2fs_write_inode(g_ext, lino, &lin);
    ext_fill_attr((uint64_t)lino, &lin, obj_attr_out);
  }
  ext_touch_dir((ext2_ino_t)dir_ino);
  cache_set_parent((uint64_t)lino, dir_ino);
  *dir_post_ok = (ext_op_getattr(dir_ino, dir_post_out) == NFS3_OK);
  *new_ino_out = (uint64_t)lino;
  return NFS3_OK;
}

static uint32_t ext_op_remove(uint64_t dir_ino, const char *name,
                              WccPre *dir_pre_out, NfsAttr *dir_post_out,
                              int *dir_post_ok) {
  memset(dir_pre_out, 0, sizeof(*dir_pre_out));
  *dir_post_ok = 0;

  struct ext2_inode din;
  uint32_t st = ext_read_valid_inode(dir_ino, &din);
  if (st != NFS3_OK) return st;
  NfsAttr dpre;
  ext_fill_attr(dir_ino, &din, &dpre);
  attr_to_wcc_pre(1, &dpre, dir_pre_out);

  if (!LINUX_S_ISDIR(din.i_mode)) {
    *dir_post_out = dpre; *dir_post_ok = 1;
    return NFS3ERR_NOTDIR;
  }
  if (!valid_leaf_name(name)) {
    *dir_post_out = dpre; *dir_post_ok = 1;
    return NFS3ERR_INVAL;
  }

  ext2_ino_t cino = 0;
  errcode_t ec = ext2fs_lookup(g_ext, (ext2_ino_t)dir_ino, name,
                               (int)strlen(name), NULL, &cino);
  if (ec != 0) {
    *dir_post_out = dpre; *dir_post_ok = 1;
    return ext_err_to_nfs3(ec);
  }

  struct ext2_inode cin;
  st = ext_read_valid_inode((uint64_t)cino, &cin);
  if (st != NFS3_OK) {
    *dir_post_out = dpre; *dir_post_ok = 1;
    return st;
  }
  if (LINUX_S_ISDIR(cin.i_mode)) {
    *dir_post_out = dpre; *dir_post_ok = 1;
    return NFS3ERR_ISDIR;
  }

  ec = ext2fs_unlink(g_ext, (ext2_ino_t)dir_ino, name, cino, 0);
  if (ec != 0) {
    *dir_post_out = dpre; *dir_post_ok = 1;
    return ext_err_to_nfs3(ec);
  }

  uint32_t now = ext_now_sec();
  if (cin.i_links_count > 1) {
    cin.i_links_count--;
    cin.i_ctime = now;
    (void)ext2fs_write_inode(g_ext, cino, &cin);
  } else {
    if (!LINUX_S_ISLNK(cin.i_mode) || !ext2fs_is_fast_symlink(&cin)) {
      (void)ext2fs_punch(g_ext, cino, &cin, NULL, 0, ~0ULL);
    } else {
      memset(cin.i_block, 0, sizeof(cin.i_block));
    }
    cin.i_links_count = 0;
    cin.i_size = 0;
    cin.i_size_high = 0;
    cin.i_dtime = now;
    cin.i_ctime = now;
    (void)ext2fs_write_inode(g_ext, cino, &cin);
    ext2fs_inode_alloc_stats2(g_ext, cino, -1, 0);
  }

  ext_touch_dir((ext2_ino_t)dir_ino);
  *dir_post_ok = (ext_op_getattr(dir_ino, dir_post_out) == NFS3_OK);
  return NFS3_OK;
}

static int ext_check_empty_cb(ext2_ino_t dir, int entry,
                              struct ext2_dir_entry *dirent, int offset,
                              int blocksize, char *buf, void *priv_data) {
  (void)dir; (void)entry; (void)offset; (void)blocksize; (void)buf;
  if (dirent->inode == 0) return 0;
  unsigned int nlen = (unsigned int)(dirent->name_len & 0xFFU);
  if (nlen == 1U && dirent->name[0] == '.') return 0;
  if (nlen == 2U && dirent->name[0] == '.' && dirent->name[1] == '.') return 0;
  int *non_empty = (int *)priv_data;
  *non_empty = 1;
  return DIRENT_ABORT;
}

static uint32_t ext_op_rmdir(uint64_t dir_ino, const char *name,
                             WccPre *dir_pre_out, NfsAttr *dir_post_out,
                             int *dir_post_ok) {
  memset(dir_pre_out, 0, sizeof(*dir_pre_out));
  *dir_post_ok = 0;

  struct ext2_inode din;
  uint32_t st = ext_read_valid_inode(dir_ino, &din);
  if (st != NFS3_OK) return st;
  NfsAttr dpre;
  ext_fill_attr(dir_ino, &din, &dpre);
  attr_to_wcc_pre(1, &dpre, dir_pre_out);

  if (!LINUX_S_ISDIR(din.i_mode)) {
    *dir_post_out = dpre; *dir_post_ok = 1;
    return NFS3ERR_NOTDIR;
  }
  if (!valid_leaf_name(name)) {
    *dir_post_out = dpre; *dir_post_ok = 1;
    return NFS3ERR_INVAL;
  }

  ext2_ino_t cino = 0;
  errcode_t ec = ext2fs_lookup(g_ext, (ext2_ino_t)dir_ino, name,
                               (int)strlen(name), NULL, &cino);
  if (ec != 0) {
    *dir_post_out = dpre; *dir_post_ok = 1;
    return ext_err_to_nfs3(ec);
  }
  if (cino == EXT2_ROOT_INO) {
    *dir_post_out = dpre; *dir_post_ok = 1;
    return NFS3ERR_ACCES;
  }

  struct ext2_inode cin;
  st = ext_read_valid_inode((uint64_t)cino, &cin);
  if (st != NFS3_OK) {
    *dir_post_out = dpre; *dir_post_ok = 1;
    return st;
  }
  if (!LINUX_S_ISDIR(cin.i_mode)) {
    *dir_post_out = dpre; *dir_post_ok = 1;
    return NFS3ERR_NOTDIR;
  }

  int non_empty = 0;
  ec = ext2fs_dir_iterate2(g_ext, cino, 0, NULL, ext_check_empty_cb, &non_empty);
  if (ec != 0) {
    *dir_post_out = dpre; *dir_post_ok = 1;
    return ext_err_to_nfs3(ec);
  }
  if (non_empty) {
    *dir_post_out = dpre; *dir_post_ok = 1;
    return NFS3ERR_NOTEMPTY;
  }

  ec = ext2fs_unlink(g_ext, (ext2_ino_t)dir_ino, name, cino, 0);
  if (ec != 0) {
    *dir_post_out = dpre; *dir_post_ok = 1;
    return ext_err_to_nfs3(ec);
  }

  uint32_t now = ext_now_sec();
  (void)ext2fs_punch(g_ext, cino, &cin, NULL, 0, ~0ULL);
  cin.i_links_count = 0;
  cin.i_size = 0;
  cin.i_size_high = 0;
  cin.i_dtime = now;
  cin.i_ctime = now;
  (void)ext2fs_write_inode(g_ext, cino, &cin);
  ext2fs_inode_alloc_stats2(g_ext, cino, -1, 1);

  if (ext2fs_read_inode(g_ext, (ext2_ino_t)dir_ino, &din) == 0) {
    if (din.i_links_count > 2) din.i_links_count--;
    din.i_mtime = now;
    din.i_ctime = now;
    (void)ext2fs_write_inode(g_ext, (ext2_ino_t)dir_ino, &din);
  }
  *dir_post_ok = (ext_op_getattr(dir_ino, dir_post_out) == NFS3_OK);
  return NFS3_OK;
}

static int ext_set_dotdot_cb(ext2_ino_t dir, int entry,
                             struct ext2_dir_entry *dirent, int offset,
                             int blocksize, char *buf, void *priv_data) {
  (void)dir; (void)entry; (void)offset; (void)blocksize; (void)buf;
  unsigned int nlen = (unsigned int)(dirent->name_len & 0xFFU);
  if (nlen == 2U && dirent->name[0] == '.' && dirent->name[1] == '.') {
    ext2_ino_t new_parent = *(ext2_ino_t *)priv_data;
    dirent->inode = new_parent;
    return DIRENT_CHANGED | DIRENT_ABORT;
  }
  return 0;
}

static int ext_mode_to_ft(uint16_t mode) {
  if (LINUX_S_ISDIR(mode))  return EXT2_FT_DIR;
  if (LINUX_S_ISLNK(mode))  return EXT2_FT_SYMLINK;
  if (LINUX_S_ISCHR(mode))  return EXT2_FT_CHRDEV;
  if (LINUX_S_ISBLK(mode))  return EXT2_FT_BLKDEV;
  if (LINUX_S_ISFIFO(mode)) return EXT2_FT_FIFO;
  if (LINUX_S_ISSOCK(mode)) return EXT2_FT_SOCK;
  return EXT2_FT_REG_FILE;
}

static uint32_t ext_op_rename(uint64_t from_dir_ino, const char *from_name,
                              uint64_t to_dir_ino, const char *to_name,
                              WccPre *from_pre_out, NfsAttr *from_post_out, int *from_post_ok,
                              WccPre *to_pre_out, NfsAttr *to_post_out, int *to_post_ok) {
  memset(from_pre_out, 0, sizeof(*from_pre_out));
  memset(to_pre_out, 0, sizeof(*to_pre_out));
  *from_post_ok = 0;
  *to_post_ok = 0;

  NfsAttr fpre, tpre;
  int fok = (ext_op_getattr(from_dir_ino, &fpre) == NFS3_OK);
  int tok = (ext_op_getattr(to_dir_ino, &tpre) == NFS3_OK);
  attr_to_wcc_pre(fok, &fpre, from_pre_out);
  attr_to_wcc_pre(tok, &tpre, to_pre_out);
  if (!fok || !tok) return NFS3ERR_STALE;
  if (fpre.type != NF3DIR || tpre.type != NF3DIR) {
    *from_post_out = fpre; *from_post_ok = 1;
    *to_post_out = tpre;   *to_post_ok = 1;
    return NFS3ERR_NOTDIR;
  }
  if (!valid_leaf_name(from_name) || !valid_leaf_name(to_name)) {
    *from_post_out = fpre; *from_post_ok = 1;
    *to_post_out = tpre;   *to_post_ok = 1;
    return NFS3ERR_INVAL;
  }
  if (strlen(from_name) > 255U || strlen(to_name) > 255U) {
    *from_post_out = fpre; *from_post_ok = 1;
    *to_post_out = tpre;   *to_post_ok = 1;
    return NFS3ERR_NAMETOOLONG;
  }

  ext2_ino_t from_ino = 0;
  errcode_t ec = ext2fs_lookup(g_ext, (ext2_ino_t)from_dir_ino, from_name,
                               (int)strlen(from_name), NULL, &from_ino);
  if (ec != 0) {
    *from_post_out = fpre; *from_post_ok = 1;
    *to_post_out = tpre;   *to_post_ok = 1;
    return ext_err_to_nfs3(ec);
  }
  struct ext2_inode fin;
  uint32_t st = ext_read_valid_inode((uint64_t)from_ino, &fin);
  if (st != NFS3_OK) return st;

  ext2_ino_t to_ino = 0;
  errcode_t tec = ext2fs_lookup(g_ext, (ext2_ino_t)to_dir_ino, to_name,
                                (int)strlen(to_name), NULL, &to_ino);
  if (tec == 0) {
    if (to_ino == from_ino) {
      *from_post_out = fpre; *from_post_ok = 1;
      *to_post_out = tpre;   *to_post_ok = 1;
      return NFS3_OK;
    }
    struct ext2_inode tin;
    st = ext_read_valid_inode((uint64_t)to_ino, &tin);
    if (st != NFS3_OK) return st;

    int from_dir = LINUX_S_ISDIR(fin.i_mode);
    int to_dir = LINUX_S_ISDIR(tin.i_mode);
    if (from_dir && !to_dir) {
      *from_post_out = fpre; *from_post_ok = 1;
      *to_post_out = tpre;   *to_post_ok = 1;
      return NFS3ERR_NOTDIR;
    }
    if (!from_dir && to_dir) {
      *from_post_out = fpre; *from_post_ok = 1;
      *to_post_out = tpre;   *to_post_ok = 1;
      return NFS3ERR_ISDIR;
    }
    WccPre d_pre;
    NfsAttr d_post;
    int d_ok = 0;
    st = to_dir ? ext_op_rmdir(to_dir_ino, to_name, &d_pre, &d_post, &d_ok)
                : ext_op_remove(to_dir_ino, to_name, &d_pre, &d_post, &d_ok);
    if (st != NFS3_OK) {
      *from_post_ok = (ext_op_getattr(from_dir_ino, from_post_out) == NFS3_OK);
      *to_post_ok = (ext_op_getattr(to_dir_ino, to_post_out) == NFS3_OK);
      return st;
    }
  } else if (tec != EXT2_ET_FILE_NOT_FOUND) {
    *from_post_out = fpre; *from_post_ok = 1;
    *to_post_out = tpre;   *to_post_ok = 1;
    return ext_err_to_nfs3(tec);
  }

  if (ext_link_with_expand((ext2_ino_t)to_dir_ino, to_name, from_ino,
                           ext_mode_to_ft(fin.i_mode), &ec) != 0) {
    *from_post_ok = (ext_op_getattr(from_dir_ino, from_post_out) == NFS3_OK);
    *to_post_ok = (ext_op_getattr(to_dir_ino, to_post_out) == NFS3_OK);
    return ext_err_to_nfs3(ec);
  }

  ec = ext2fs_unlink(g_ext, (ext2_ino_t)from_dir_ino, from_name, from_ino, 0);
  if (ec != 0) {
    *from_post_ok = (ext_op_getattr(from_dir_ino, from_post_out) == NFS3_OK);
    *to_post_ok = (ext_op_getattr(to_dir_ino, to_post_out) == NFS3_OK);
    return ext_err_to_nfs3(ec);
  }

  if (LINUX_S_ISDIR(fin.i_mode) && from_dir_ino != to_dir_ino) {
    ext2_ino_t np = (ext2_ino_t)to_dir_ino;
    (void)ext2fs_dir_iterate2(g_ext, from_ino, 0, NULL, ext_set_dotdot_cb, &np);
    struct ext2_inode fdin, tdin;
    if (ext2fs_read_inode(g_ext, (ext2_ino_t)from_dir_ino, &fdin) == 0) {
      if (fdin.i_links_count > 2) fdin.i_links_count--;
      (void)ext2fs_write_inode(g_ext, (ext2_ino_t)from_dir_ino, &fdin);
    }
    if (ext2fs_read_inode(g_ext, (ext2_ino_t)to_dir_ino, &tdin) == 0) {
      if (tdin.i_links_count >= 2 && tdin.i_links_count < 65000U) tdin.i_links_count++;
      (void)ext2fs_write_inode(g_ext, (ext2_ino_t)to_dir_ino, &tdin);
    }
  }

  ext_touch_dir((ext2_ino_t)from_dir_ino);
  if (to_dir_ino != from_dir_ino) ext_touch_dir((ext2_ino_t)to_dir_ino);
  cache_set_parent((uint64_t)from_ino, to_dir_ino);
  *from_post_ok = (ext_op_getattr(from_dir_ino, from_post_out) == NFS3_OK);
  *to_post_ok = (ext_op_getattr(to_dir_ino, to_post_out) == NFS3_OK);
  return NFS3_OK;
}

static uint32_t ext_op_link(uint64_t ino, uint64_t dir_ino, const char *name,
                            NfsAttr *file_attr_out, int *file_attr_ok,
                            WccPre *dir_pre_out, NfsAttr *dir_post_out,
                            int *dir_post_ok) {
  *file_attr_ok = 0;
  memset(dir_pre_out, 0, sizeof(*dir_pre_out));
  *dir_post_ok = 0;

  struct ext2_inode din, fin;
  uint32_t st = ext_read_valid_inode(dir_ino, &din);
  if (st != NFS3_OK) return st;
  NfsAttr dpre;
  ext_fill_attr(dir_ino, &din, &dpre);
  attr_to_wcc_pre(1, &dpre, dir_pre_out);

  st = ext_read_valid_inode(ino, &fin);
  if (st != NFS3_OK) return st;
  ext_fill_attr(ino, &fin, file_attr_out);
  *file_attr_ok = 1;

  if (!LINUX_S_ISDIR(din.i_mode)) {
    *dir_post_out = dpre; *dir_post_ok = 1;
    return NFS3ERR_NOTDIR;
  }
  if (LINUX_S_ISDIR(fin.i_mode)) {
    *dir_post_out = dpre; *dir_post_ok = 1;
    return NFS3ERR_ISDIR;
  }
  if (!valid_leaf_name(name)) {
    *dir_post_out = dpre; *dir_post_ok = 1;
    return NFS3ERR_INVAL;
  }
  if (strlen(name) > 255U) {
    *dir_post_out = dpre; *dir_post_ok = 1;
    return NFS3ERR_NAMETOOLONG;
  }

  ext2_ino_t existing = 0;
  if (ext2fs_lookup(g_ext, (ext2_ino_t)dir_ino, name,
                    (int)strlen(name), NULL, &existing) == 0) {
    *dir_post_out = dpre; *dir_post_ok = 1;
    return NFS3ERR_EXIST;
  }

  errcode_t ec = 0;
  if (ext_link_with_expand((ext2_ino_t)dir_ino, name, (ext2_ino_t)ino,
                           ext_mode_to_ft(fin.i_mode), &ec) != 0) {
    *dir_post_ok = (ext_op_getattr(dir_ino, dir_post_out) == NFS3_OK);
    return ext_err_to_nfs3(ec);
  }

  fin.i_links_count++;
  fin.i_ctime = ext_now_sec();
  (void)ext2fs_write_inode(g_ext, (ext2_ino_t)ino, &fin);
  ext_fill_attr(ino, &fin, file_attr_out);
  *file_attr_ok = 1;
  ext_touch_dir((ext2_ino_t)dir_ino);
  *dir_post_ok = (ext_op_getattr(dir_ino, dir_post_out) == NFS3_OK);
  return NFS3_OK;
}

typedef struct {
  uint64_t dir_ino;
  uint64_t dotdot_ino;
  DirEntryList *list;
  int oom;
} ExtReaddirCtx;

static int ext_readdir_cb(ext2_ino_t dir, int entry,
                          struct ext2_dir_entry *dirent, int offset,
                          int blocksize, char *buf, void *priv_data) {
  (void)dir; (void)entry; (void)offset; (void)blocksize; (void)buf;
  if (dirent->inode == 0) return 0;
  ExtReaddirCtx *ctx = (ExtReaddirCtx *)priv_data;
  unsigned int nlen = (unsigned int)(dirent->name_len & 0xFFU);
  if (nlen == 0 || nlen > 255U) return 0;

  char name[256];
  memcpy(name, dirent->name, nlen);
  name[nlen] = '\0';

  if (strcmp(name, ".") == 0) return 0;
  if (strcmp(name, "..") == 0) {
    ctx->dotdot_ino = (uint64_t)dirent->inode;
    return 0;
  }

  cache_set_parent((uint64_t)dirent->inode, ctx->dir_ino);
  if (dirlist_push(ctx->list, (uint64_t)dirent->inode, name) != 0) {
    ctx->oom = 1;
    return DIRENT_ABORT;
  }
  return 0;
}

static uint32_t ext_op_readdir(uint64_t dir_ino, DirEntryList *list_out,
                               NfsAttr *dir_attr_out, int *dir_attr_ok) {
  *dir_attr_ok = 0;
  memset(list_out, 0, sizeof(*list_out));

  struct ext2_inode din;
  uint32_t st = ext_read_valid_inode(dir_ino, &din);
  if (st != NFS3_OK) return st;
  ext_fill_attr(dir_ino, &din, dir_attr_out);
  *dir_attr_ok = 1;
  if (!LINUX_S_ISDIR(din.i_mode)) return NFS3ERR_NOTDIR;

  uint64_t pino = EXT2_ROOT_INO;
  ext2_ino_t dotdot = 0;
  if (ext2fs_lookup(g_ext, (ext2_ino_t)dir_ino, "..", 2, NULL, &dotdot) == 0 &&
      dotdot >= EXT2_ROOT_INO) {
    pino = (uint64_t)dotdot;
  }
  if (dirlist_push(list_out, dir_ino, ".") != 0 ||
      dirlist_push(list_out, pino, "..") != 0) {
    dirlist_free(list_out);
    return NFS3ERR_SERVERFAULT;
  }

  ExtReaddirCtx rctx = {dir_ino, pino, list_out, 0};
  errcode_t ec = ext2fs_dir_iterate2(g_ext, (ext2_ino_t)dir_ino, 0, NULL,
                                     ext_readdir_cb, &rctx);
  if (rctx.oom) {
    dirlist_free(list_out);
    return NFS3ERR_SERVERFAULT;
  }
  if (ec != 0) {
    dirlist_free(list_out);
    return ext_err_to_nfs3(ec);
  }
  list_out->items[1].fileid = rctx.dotdot_ino;
  return NFS3_OK;
}

static uint32_t ext_op_fsstat(uint64_t ino, FsStatInfo *st_out,
                              NfsAttr *attr_out, int *attr_ok) {
  *attr_ok = (ext_op_getattr(ino, attr_out) == NFS3_OK);
  if (!*attr_ok) return NFS3ERR_STALE;

  uint64_t bs = (uint64_t)g_ext->blocksize;
  uint64_t total_b = (uint64_t)ext2fs_blocks_count(g_ext->super);
  uint64_t free_b = (uint64_t)ext2fs_free_blocks_count(g_ext->super);
  if (free_b > total_b) free_b = total_b;

  st_out->tbytes = total_b * bs;
  st_out->fbytes = free_b * bs;
  st_out->abytes = free_b * bs;
  st_out->tfiles = (uint64_t)g_ext->super->s_inodes_count;
  st_out->ffiles = (uint64_t)g_ext->super->s_free_inodes_count;
  st_out->afiles = (uint64_t)g_ext->super->s_free_inodes_count;
  return NFS3_OK;
}

static uint32_t ext_op_commit(uint64_t ino, WccPre *pre_out,
                              NfsAttr *post_out, int *post_ok) {
  memset(pre_out, 0, sizeof(*pre_out));
  *post_ok = (ext_op_getattr(ino, post_out) == NFS3_OK);
  if (!*post_ok) return NFS3ERR_STALE;
  attr_to_wcc_pre(1, post_out, pre_out);
  errcode_t ec = ext2fs_write_bitmaps(g_ext);
  if (ec == 0) ec = ext2fs_flush(g_ext);
  if (ec == 0 && g_ext->io) ec = io_channel_flush(g_ext->io);
  return ext_err_to_nfs3(ec);
}

/* =========================================================================
 * Unified FS dispatch & ONC RPC v2 / MOUNTv3 / NFSv3 handlers
 * ========================================================================= */

static uint64_t fs_root_ino(void) {
  return (g_fskind == FS_KIND_NTFS) ? (uint64_t)FILE_root : (uint64_t)EXT2_ROOT_INO;
}

static void fs_sync_all(void) {
  if (g_fskind == FS_KIND_NTFS && g_ntfs) {
    if (g_ntfs->dev && g_ntfs->dev->d_ops && g_ntfs->dev->d_ops->sync) {
      (void)g_ntfs->dev->d_ops->sync(g_ntfs->dev);
    }
  } else if (g_fskind == FS_KIND_EXT && g_ext) {
    (void)ext2fs_write_bitmaps(g_ext);
    (void)ext2fs_flush(g_ext);
    if (g_ext->io) (void)io_channel_flush(g_ext->io);
  }
}

static uint32_t fs_getattr(uint64_t ino, NfsAttr *out) {
  return (g_fskind == FS_KIND_NTFS) ? ntfs_op_getattr(ino, out)
                                    : ext_op_getattr(ino, out);
}

static uint32_t fs_setattr(uint64_t ino, const SAttr3 *s, NfsAttr *post_out) {
  return (g_fskind == FS_KIND_NTFS) ? ntfs_op_setattr(ino, s, post_out)
                                    : ext_op_setattr(ino, s, post_out);
}

static uint32_t fs_lookup(uint64_t dir_ino, const char *name,
                          uint64_t *child_ino_out, NfsAttr *child_attr_out) {
  return (g_fskind == FS_KIND_NTFS)
             ? ntfs_op_lookup(dir_ino, name, child_ino_out, child_attr_out)
             : ext_op_lookup(dir_ino, name, child_ino_out, child_attr_out);
}

static uint32_t fs_readlink(uint64_t ino, char *out, size_t cap,
                            NfsAttr *attr_out, int *attr_ok) {
  return (g_fskind == FS_KIND_NTFS)
             ? ntfs_op_readlink(ino, out, cap, attr_out, attr_ok)
             : ext_op_readlink(ino, out, cap, attr_out, attr_ok);
}

static uint32_t fs_read(uint64_t ino, uint64_t offset, uint32_t count,
                        uint8_t *buf, uint32_t *got_out, int *eof_out,
                        NfsAttr *attr_out, int *attr_ok) {
  return (g_fskind == FS_KIND_NTFS)
             ? ntfs_op_read(ino, offset, count, buf, got_out, eof_out, attr_out, attr_ok)
             : ext_op_read(ino, offset, count, buf, got_out, eof_out, attr_out, attr_ok);
}

static uint32_t fs_write(uint64_t ino, uint64_t offset, const uint8_t *buf,
                         uint32_t count, uint32_t stable,
                         uint32_t *written_out, uint32_t *committed_out,
                         WccPre *pre_out, NfsAttr *post_out, int *post_ok) {
  return (g_fskind == FS_KIND_NTFS)
             ? ntfs_op_write(ino, offset, buf, count, stable, written_out,
                             committed_out, pre_out, post_out, post_ok)
             : ext_op_write(ino, offset, buf, count, stable, written_out,
                            committed_out, pre_out, post_out, post_ok);
}

static uint32_t fs_create(uint64_t dir_ino, const char *name,
                          uint32_t create_mode, const SAttr3 *sattr,
                          uint64_t *new_ino_out, NfsAttr *obj_attr_out,
                          WccPre *dir_pre_out, NfsAttr *dir_post_out,
                          int *dir_post_ok) {
  return (g_fskind == FS_KIND_NTFS)
             ? ntfs_op_create(dir_ino, name, create_mode, sattr, new_ino_out,
                              obj_attr_out, dir_pre_out, dir_post_out, dir_post_ok)
             : ext_op_create(dir_ino, name, create_mode, sattr, new_ino_out,
                             obj_attr_out, dir_pre_out, dir_post_out, dir_post_ok);
}

static uint32_t fs_mkdir(uint64_t dir_ino, const char *name,
                         const SAttr3 *sattr, uint64_t *new_ino_out,
                         NfsAttr *obj_attr_out, WccPre *dir_pre_out,
                         NfsAttr *dir_post_out, int *dir_post_ok) {
  return (g_fskind == FS_KIND_NTFS)
             ? ntfs_op_mkdir(dir_ino, name, sattr, new_ino_out, obj_attr_out,
                             dir_pre_out, dir_post_out, dir_post_ok)
             : ext_op_mkdir(dir_ino, name, sattr, new_ino_out, obj_attr_out,
                            dir_pre_out, dir_post_out, dir_post_ok);
}

static uint32_t fs_symlink(uint64_t dir_ino, const char *name,
                           const SAttr3 *sattr, const char *target,
                           uint64_t *new_ino_out, NfsAttr *obj_attr_out,
                           WccPre *dir_pre_out, NfsAttr *dir_post_out,
                           int *dir_post_ok) {
  return (g_fskind == FS_KIND_NTFS)
             ? ntfs_op_symlink(dir_ino, name, sattr, target, new_ino_out,
                               obj_attr_out, dir_pre_out, dir_post_out, dir_post_ok)
             : ext_op_symlink(dir_ino, name, sattr, target, new_ino_out,
                              obj_attr_out, dir_pre_out, dir_post_out, dir_post_ok);
}

static uint32_t fs_remove(uint64_t dir_ino, const char *name,
                          WccPre *dir_pre_out, NfsAttr *dir_post_out,
                          int *dir_post_ok) {
  return (g_fskind == FS_KIND_NTFS)
             ? ntfs_op_remove(dir_ino, name, dir_pre_out, dir_post_out, dir_post_ok)
             : ext_op_remove(dir_ino, name, dir_pre_out, dir_post_out, dir_post_ok);
}

static uint32_t fs_rmdir(uint64_t dir_ino, const char *name,
                         WccPre *dir_pre_out, NfsAttr *dir_post_out,
                         int *dir_post_ok) {
  return (g_fskind == FS_KIND_NTFS)
             ? ntfs_op_rmdir(dir_ino, name, dir_pre_out, dir_post_out, dir_post_ok)
             : ext_op_rmdir(dir_ino, name, dir_pre_out, dir_post_out, dir_post_ok);
}

static uint32_t fs_rename(uint64_t from_dir_ino, const char *from_name,
                          uint64_t to_dir_ino, const char *to_name,
                          WccPre *from_pre_out, NfsAttr *from_post_out, int *from_post_ok,
                          WccPre *to_pre_out, NfsAttr *to_post_out, int *to_post_ok) {
  return (g_fskind == FS_KIND_NTFS)
             ? ntfs_op_rename(from_dir_ino, from_name, to_dir_ino, to_name,
                              from_pre_out, from_post_out, from_post_ok,
                              to_pre_out, to_post_out, to_post_ok)
             : ext_op_rename(from_dir_ino, from_name, to_dir_ino, to_name,
                             from_pre_out, from_post_out, from_post_ok,
                             to_pre_out, to_post_out, to_post_ok);
}

static uint32_t fs_link(uint64_t ino, uint64_t dir_ino, const char *name,
                        NfsAttr *file_attr_out, int *file_attr_ok,
                        WccPre *dir_pre_out, NfsAttr *dir_post_out,
                        int *dir_post_ok) {
  return (g_fskind == FS_KIND_NTFS)
             ? ntfs_op_link(ino, dir_ino, name, file_attr_out, file_attr_ok,
                            dir_pre_out, dir_post_out, dir_post_ok)
             : ext_op_link(ino, dir_ino, name, file_attr_out, file_attr_ok,
                           dir_pre_out, dir_post_out, dir_post_ok);
}

static uint32_t fs_readdir(uint64_t dir_ino, DirEntryList *list_out,
                           NfsAttr *dir_attr_out, int *dir_attr_ok) {
  return (g_fskind == FS_KIND_NTFS)
             ? ntfs_op_readdir(dir_ino, list_out, dir_attr_out, dir_attr_ok)
             : ext_op_readdir(dir_ino, list_out, dir_attr_out, dir_attr_ok);
}

static uint32_t fs_fsstat(uint64_t ino, FsStatInfo *st_out,
                          NfsAttr *attr_out, int *attr_ok) {
  return (g_fskind == FS_KIND_NTFS)
             ? ntfs_op_fsstat(ino, st_out, attr_out, attr_ok)
             : ext_op_fsstat(ino, st_out, attr_out, attr_ok);
}

static uint32_t fs_commit(uint64_t ino, WccPre *pre_out,
                          NfsAttr *post_out, int *post_ok) {
  return (g_fskind == FS_KIND_NTFS)
             ? ntfs_op_commit(ino, pre_out, post_out, post_ok)
             : ext_op_commit(ino, pre_out, post_out, post_ok);
}

/* Returns 0 on success, -1 on XDR garbage args, -2 on proc unavailable */
static int handle_mount_proc(uint32_t proc, XdrReader *r, XdrBuf *out) {
  char dirpath[1025];
  switch (proc) {
    case MOUNTPROC3_NULL:
      return 0;

    case MOUNTPROC3_MNT:
      if (xdr_get_str(r, dirpath, sizeof(dirpath)) != 0) return -1;
      if (strcmp(dirpath, "/") != 0 && strcmp(dirpath, "") != 0) {
        xdr_put_u32(out, MNT3ERR_NOENT);
        return 0;
      }
      xdr_put_u32(out, MNT3_OK);
      xdr_put_fh(out, fs_root_ino());
      xdr_put_u32(out, 2U);
      xdr_put_u32(out, AUTH_SYS);
      xdr_put_u32(out, AUTH_NONE);
      return 0;

    case MOUNTPROC3_DUMP:
      xdr_put_u32(out, 0U);
      return 0;

    case MOUNTPROC3_UMNT:
      if (xdr_get_str(r, dirpath, sizeof(dirpath)) != 0) return -1;
      fs_sync_all();
      return 0;

    case MOUNTPROC3_UMNTALL:
      fs_sync_all();
      return 0;

    case MOUNTPROC3_EXPORT:
      xdr_put_u32(out, 1U);
      xdr_put_str(out, "/");
      xdr_put_u32(out, 1U);
      xdr_put_str(out, "127.0.0.1");
      xdr_put_u32(out, 0U);
      xdr_put_u32(out, 0U);
      return 0;

    default:
      return -2;
  }
}

static size_t xdr_padded_strlen(const char *s) {
  size_t n = strlen(s);
  return 4U + n + ((4U - (n & 3U)) & 3U);
}

/* Returns 0 on success, -1 on XDR garbage args, -2 on proc unavailable */
static int handle_nfs_proc(uint32_t proc, XdrReader *r, XdrBuf *out) {
  uint64_t ino = 0, dir_ino = 0, to_dir_ino = 0, child_ino = 0;
  int bad_fh = 0, bad_fh2 = 0;
  char name[1025], name2[1025], sym_path[PATH_MAX];
  NfsAttr attr, attr2, dir_attr, to_dir_attr;
  WccPre pre, pre2;
  int post_ok = 0, post_ok2 = 0;
  SAttr3 sattr;
  uint32_t st = NFS3_OK;

  switch (proc) {
    case NFSPROC3_NULL:
      return 0;

    case NFSPROC3_GETATTR:
      if (xdr_get_fh(r, &ino, &bad_fh) != 0) return -1;
      st = bad_fh ? NFS3ERR_BADHANDLE : fs_getattr(ino, &attr);
      xdr_put_u32(out, st);
      if (st == NFS3_OK) xdr_put_fattr3(out, &attr);
      return 0;

    case NFSPROC3_SETATTR: {
      if (xdr_get_fh(r, &ino, &bad_fh) != 0) return -1;
      if (xdr_get_sattr3(r, &sattr) != 0) return -1;
      uint32_t guard_check = xdr_get_u32(r);
      uint32_t g_sec = 0, g_nsec = 0;
      if (guard_check) {
        g_sec = xdr_get_u32(r);
        g_nsec = xdr_get_u32(r);
      }
      if (r->err) return -1;
      if (bad_fh) {
        xdr_put_u32(out, NFS3ERR_BADHANDLE);
        xdr_put_wcc_data(out, NULL, 0, NULL);
        return 0;
      }
      int pre_ok = (fs_getattr(ino, &attr) == NFS3_OK);
      attr_to_wcc_pre(pre_ok, &attr, &pre);
      if (!pre_ok) {
        xdr_put_u32(out, NFS3ERR_STALE);
        xdr_put_wcc_data(out, &pre, 0, NULL);
        return 0;
      }
      if (guard_check && (attr.ctime_sec != g_sec || attr.ctime_nsec != g_nsec)) {
        xdr_put_u32(out, NFS3ERR_NOT_SYNC);
        xdr_put_wcc_data(out, &pre, 1, &attr);
        return 0;
      }
      st = fs_setattr(ino, &sattr, &attr2);
      post_ok = (st == NFS3_OK) ? 1 : (fs_getattr(ino, &attr2) == NFS3_OK);
      xdr_put_u32(out, st);
      xdr_put_wcc_data(out, &pre, post_ok, &attr2);
      return 0;
    }

    case NFSPROC3_LOOKUP: {
      if (xdr_get_fh(r, &dir_ino, &bad_fh) != 0) return -1;
      if (xdr_get_str(r, name, sizeof(name)) != 0) return -1;
      if (bad_fh) {
        xdr_put_u32(out, NFS3ERR_BADHANDLE);
        xdr_put_post_op_attr(out, 0, NULL);
        return 0;
      }
      st = fs_lookup(dir_ino, name, &child_ino, &attr);
      int dir_ok = (fs_getattr(dir_ino, &dir_attr) == NFS3_OK);
      xdr_put_u32(out, st);
      if (st == NFS3_OK) {
        xdr_put_fh(out, child_ino);
        xdr_put_post_op_attr(out, 1, &attr);
      }
      xdr_put_post_op_attr(out, dir_ok, &dir_attr);
      return 0;
    }

    case NFSPROC3_ACCESS: {
      if (xdr_get_fh(r, &ino, &bad_fh) != 0) return -1;
      uint32_t req_acc = xdr_get_u32(r);
      if (r->err) return -1;
      if (bad_fh) {
        xdr_put_u32(out, NFS3ERR_BADHANDLE);
        xdr_put_post_op_attr(out, 0, NULL);
        return 0;
      }
      st = fs_getattr(ino, &attr);
      xdr_put_u32(out, st);
      xdr_put_post_op_attr(out, (st == NFS3_OK), &attr);
      if (st == NFS3_OK) {
        uint32_t allowed = ACCESS3_READ | ACCESS3_MODIFY | ACCESS3_EXTEND;
        if (attr.type == NF3DIR) {
          allowed |= ACCESS3_LOOKUP | ACCESS3_DELETE;
        } else {
          allowed |= ACCESS3_EXECUTE;
        }
        xdr_put_u32(out, req_acc & allowed);
      }
      return 0;
    }

    case NFSPROC3_READLINK:
      if (xdr_get_fh(r, &ino, &bad_fh) != 0) return -1;
      if (bad_fh) {
        xdr_put_u32(out, NFS3ERR_BADHANDLE);
        xdr_put_post_op_attr(out, 0, NULL);
        return 0;
      }
      st = fs_readlink(ino, sym_path, sizeof(sym_path), &attr, &post_ok);
      xdr_put_u32(out, st);
      xdr_put_post_op_attr(out, post_ok, &attr);
      if (st == NFS3_OK) xdr_put_str(out, sym_path);
      return 0;

    case NFSPROC3_READ: {
      if (xdr_get_fh(r, &ino, &bad_fh) != 0) return -1;
      uint64_t offset = xdr_get_u64(r);
      uint32_t count = xdr_get_u32(r);
      if (r->err) return -1;
      if (bad_fh) {
        xdr_put_u32(out, NFS3ERR_BADHANDLE);
        xdr_put_post_op_attr(out, 0, NULL);
        return 0;
      }
      if (count > MOUNTD_MAX_IO_SIZE) count = MOUNTD_MAX_IO_SIZE;
      uint8_t *rbuf = count ? malloc(count) : NULL;
      if (count && !rbuf) {
        xdr_put_u32(out, NFS3ERR_SERVERFAULT);
        xdr_put_post_op_attr(out, 0, NULL);
        return 0;
      }
      uint32_t got = 0;
      int eof = 0;
      st = fs_read(ino, offset, count, rbuf, &got, &eof, &attr, &post_ok);
      xdr_put_u32(out, st);
      xdr_put_post_op_attr(out, post_ok, &attr);
      if (st == NFS3_OK) {
        xdr_put_u32(out, got);
        xdr_put_u32(out, eof ? 1U : 0U);
        xdr_put_opaque(out, rbuf, got);
      }
      free(rbuf);
      return 0;
    }

    case NFSPROC3_WRITE: {
      if (xdr_get_fh(r, &ino, &bad_fh) != 0) return -1;
      uint64_t offset = xdr_get_u64(r);
      uint32_t count = xdr_get_u32(r);
      uint32_t stable = xdr_get_u32(r);
      const uint8_t *wptr = NULL;
      uint32_t data_len = 0;
      if (xdr_get_opaque_ptr(r, &wptr, &data_len, MOUNTD_MAX_IO_SIZE) != 0) return -1;
      if (bad_fh) {
        xdr_put_u32(out, NFS3ERR_BADHANDLE);
        xdr_put_wcc_data(out, NULL, 0, NULL);
        return 0;
      }
      uint32_t to_write = (count < data_len) ? count : data_len;
      uint32_t written = 0, committed = FILE_SYNC;
      st = fs_write(ino, offset, wptr, to_write, stable, &written, &committed,
                    &pre, &attr, &post_ok);
      xdr_put_u32(out, st);
      xdr_put_wcc_data(out, &pre, post_ok, &attr);
      if (st == NFS3_OK) {
        xdr_put_u32(out, written);
        xdr_put_u32(out, committed);
        xdr_put_fixed(out, g_writeverf, 8U);
      }
      return 0;
    }

    case NFSPROC3_CREATE: {
      if (xdr_get_fh(r, &dir_ino, &bad_fh) != 0) return -1;
      if (xdr_get_str(r, name, sizeof(name)) != 0) return -1;
      uint32_t cmode = xdr_get_u32(r);
      uint8_t cverf[8];
      int have_sattr = 0;
      if (cmode == CREATE_UNCHECKED || cmode == CREATE_GUARDED) {
        if (xdr_get_sattr3(r, &sattr) != 0) return -1;
        have_sattr = 1;
      } else if (cmode == CREATE_EXCLUSIVE) {
        if (xdr_get_fixed(r, cverf, 8U) != 0) return -1;
      } else {
        return -1;
      }
      if (bad_fh) {
        xdr_put_u32(out, NFS3ERR_BADHANDLE);
        xdr_put_wcc_data(out, NULL, 0, NULL);
        return 0;
      }
      st = fs_create(dir_ino, name, cmode, have_sattr ? &sattr : NULL,
                     &child_ino, &attr, &pre, &dir_attr, &post_ok);
      xdr_put_u32(out, st);
      if (st == NFS3_OK) {
        xdr_put_post_op_fh(out, 1, child_ino);
        xdr_put_post_op_attr(out, 1, &attr);
      }
      xdr_put_wcc_data(out, &pre, post_ok, &dir_attr);
      return 0;
    }

    case NFSPROC3_MKDIR:
      if (xdr_get_fh(r, &dir_ino, &bad_fh) != 0) return -1;
      if (xdr_get_str(r, name, sizeof(name)) != 0) return -1;
      if (xdr_get_sattr3(r, &sattr) != 0) return -1;
      if (bad_fh) {
        xdr_put_u32(out, NFS3ERR_BADHANDLE);
        xdr_put_wcc_data(out, NULL, 0, NULL);
        return 0;
      }
      st = fs_mkdir(dir_ino, name, &sattr, &child_ino, &attr,
                    &pre, &dir_attr, &post_ok);
      xdr_put_u32(out, st);
      if (st == NFS3_OK) {
        xdr_put_post_op_fh(out, 1, child_ino);
        xdr_put_post_op_attr(out, 1, &attr);
      }
      xdr_put_wcc_data(out, &pre, post_ok, &dir_attr);
      return 0;

    case NFSPROC3_SYMLINK:
      if (xdr_get_fh(r, &dir_ino, &bad_fh) != 0) return -1;
      if (xdr_get_str(r, name, sizeof(name)) != 0) return -1;
      if (xdr_get_sattr3(r, &sattr) != 0) return -1;
      if (xdr_get_str(r, sym_path, sizeof(sym_path)) != 0) return -1;
      if (bad_fh) {
        xdr_put_u32(out, NFS3ERR_BADHANDLE);
        xdr_put_wcc_data(out, NULL, 0, NULL);
        return 0;
      }
      st = fs_symlink(dir_ino, name, &sattr, sym_path, &child_ino, &attr,
                      &pre, &dir_attr, &post_ok);
      xdr_put_u32(out, st);
      if (st == NFS3_OK) {
        xdr_put_post_op_fh(out, 1, child_ino);
        xdr_put_post_op_attr(out, 1, &attr);
      }
      xdr_put_wcc_data(out, &pre, post_ok, &dir_attr);
      return 0;

    case NFSPROC3_MKNOD: {
      if (xdr_get_fh(r, &dir_ino, &bad_fh) != 0) return -1;
      if (xdr_get_str(r, name, sizeof(name)) != 0) return -1;
      uint32_t ftype = xdr_get_u32(r);
      if (ftype == NF3CHR || ftype == NF3BLK) {
        if (xdr_get_sattr3(r, &sattr) != 0) return -1;
        (void)xdr_get_u32(r);
        (void)xdr_get_u32(r);
      } else if (ftype == NF3SOCK || ftype == NF3FIFO) {
        if (xdr_get_sattr3(r, &sattr) != 0) return -1;
      }
      if (r->err) return -1;
      post_ok = (!bad_fh && fs_getattr(dir_ino, &dir_attr) == NFS3_OK);
      attr_to_wcc_pre(post_ok, &dir_attr, &pre);
      xdr_put_u32(out, bad_fh ? NFS3ERR_BADHANDLE : NFS3ERR_NOTSUPP);
      xdr_put_wcc_data(out, &pre, post_ok, &dir_attr);
      return 0;
    }

    case NFSPROC3_REMOVE:
      if (xdr_get_fh(r, &dir_ino, &bad_fh) != 0) return -1;
      if (xdr_get_str(r, name, sizeof(name)) != 0) return -1;
      if (bad_fh) {
        xdr_put_u32(out, NFS3ERR_BADHANDLE);
        xdr_put_wcc_data(out, NULL, 0, NULL);
        return 0;
      }
      st = fs_remove(dir_ino, name, &pre, &dir_attr, &post_ok);
      xdr_put_u32(out, st);
      xdr_put_wcc_data(out, &pre, post_ok, &dir_attr);
      return 0;

    case NFSPROC3_RMDIR:
      if (xdr_get_fh(r, &dir_ino, &bad_fh) != 0) return -1;
      if (xdr_get_str(r, name, sizeof(name)) != 0) return -1;
      if (bad_fh) {
        xdr_put_u32(out, NFS3ERR_BADHANDLE);
        xdr_put_wcc_data(out, NULL, 0, NULL);
        return 0;
      }
      st = fs_rmdir(dir_ino, name, &pre, &dir_attr, &post_ok);
      xdr_put_u32(out, st);
      xdr_put_wcc_data(out, &pre, post_ok, &dir_attr);
      return 0;

    case NFSPROC3_RENAME:
      if (xdr_get_fh(r, &dir_ino, &bad_fh) != 0) return -1;
      if (xdr_get_str(r, name, sizeof(name)) != 0) return -1;
      if (xdr_get_fh(r, &to_dir_ino, &bad_fh2) != 0) return -1;
      if (xdr_get_str(r, name2, sizeof(name2)) != 0) return -1;
      if (bad_fh || bad_fh2) {
        xdr_put_u32(out, NFS3ERR_BADHANDLE);
        xdr_put_wcc_data(out, NULL, 0, NULL);
        xdr_put_wcc_data(out, NULL, 0, NULL);
        return 0;
      }
      st = fs_rename(dir_ino, name, to_dir_ino, name2,
                     &pre, &dir_attr, &post_ok,
                     &pre2, &to_dir_attr, &post_ok2);
      xdr_put_u32(out, st);
      xdr_put_wcc_data(out, &pre, post_ok, &dir_attr);
      xdr_put_wcc_data(out, &pre2, post_ok2, &to_dir_attr);
      return 0;

    case NFSPROC3_LINK:
      if (xdr_get_fh(r, &ino, &bad_fh) != 0) return -1;
      if (xdr_get_fh(r, &dir_ino, &bad_fh2) != 0) return -1;
      if (xdr_get_str(r, name, sizeof(name)) != 0) return -1;
      if (bad_fh || bad_fh2) {
        xdr_put_u32(out, NFS3ERR_BADHANDLE);
        xdr_put_post_op_attr(out, 0, NULL);
        xdr_put_wcc_data(out, NULL, 0, NULL);
        return 0;
      }
      st = fs_link(ino, dir_ino, name, &attr, &post_ok,
                   &pre, &dir_attr, &post_ok2);
      xdr_put_u32(out, st);
      xdr_put_post_op_attr(out, post_ok, &attr);
      xdr_put_wcc_data(out, &pre, post_ok2, &dir_attr);
      return 0;

    case NFSPROC3_READDIR: {
      if (xdr_get_fh(r, &dir_ino, &bad_fh) != 0) return -1;
      uint64_t cookie = xdr_get_u64(r);
      uint8_t cverf[8], zverf[8] = {0};
      if (xdr_get_fixed(r, cverf, 8U) != 0) return -1;
      uint32_t max_bytes = xdr_get_u32(r);
      if (r->err) return -1;
      if (bad_fh) {
        xdr_put_u32(out, NFS3ERR_BADHANDLE);
        xdr_put_post_op_attr(out, 0, NULL);
        return 0;
      }
      DirEntryList list;
      st = fs_readdir(dir_ino, &list, &dir_attr, &post_ok);
      if (st != NFS3_OK) {
        xdr_put_u32(out, st);
        xdr_put_post_op_attr(out, post_ok, &dir_attr);
        return 0;
      }
      if (max_bytes < 128U) {
        dirlist_free(&list);
        xdr_put_u32(out, NFS3ERR_TOOSMALL);
        xdr_put_post_op_attr(out, post_ok, &dir_attr);
        return 0;
      }
      size_t start_len = out->len;
      xdr_put_u32(out, NFS3_OK);
      xdr_put_post_op_attr(out, post_ok, &dir_attr);
      xdr_put_fixed(out, zverf, 8U);

      size_t idx = (cookie < (uint64_t)list.count) ? (size_t)cookie : list.count;
      int emitted = 0;
      for (; idx < list.count; idx++) {
        size_t entry_sz = 4U + 8U + xdr_padded_strlen(list.items[idx].name) + 8U;
        size_t used = out->len - start_len;
        if (emitted > 0 && used + entry_sz + 8U > (size_t)max_bytes) break;
        xdr_put_u32(out, 1U);
        xdr_put_u64(out, list.items[idx].fileid);
        xdr_put_str(out, list.items[idx].name);
        xdr_put_u64(out, (uint64_t)(idx + 1U));
        emitted++;
      }
      xdr_put_u32(out, 0U);
      xdr_put_u32(out, (idx >= list.count) ? 1U : 0U);
      dirlist_free(&list);
      return 0;
    }

    case NFSPROC3_READDIRPLUS: {
      if (xdr_get_fh(r, &dir_ino, &bad_fh) != 0) return -1;
      uint64_t cookie = xdr_get_u64(r);
      uint8_t cverf[8], zverf[8] = {0};
      if (xdr_get_fixed(r, cverf, 8U) != 0) return -1;
      uint32_t dircount = xdr_get_u32(r);
      uint32_t maxcount = xdr_get_u32(r);
      (void)dircount;
      if (r->err) return -1;
      if (bad_fh) {
        xdr_put_u32(out, NFS3ERR_BADHANDLE);
        xdr_put_post_op_attr(out, 0, NULL);
        return 0;
      }
      DirEntryList list;
      st = fs_readdir(dir_ino, &list, &dir_attr, &post_ok);
      if (st != NFS3_OK) {
        xdr_put_u32(out, st);
        xdr_put_post_op_attr(out, post_ok, &dir_attr);
        return 0;
      }
      if (maxcount < 256U) {
        dirlist_free(&list);
        xdr_put_u32(out, NFS3ERR_TOOSMALL);
        xdr_put_post_op_attr(out, post_ok, &dir_attr);
        return 0;
      }
      size_t start_len = out->len;
      xdr_put_u32(out, NFS3_OK);
      xdr_put_post_op_attr(out, post_ok, &dir_attr);
      xdr_put_fixed(out, zverf, 8U);

      size_t idx = (cookie < (uint64_t)list.count) ? (size_t)cookie : list.count;
      int emitted = 0;
      for (; idx < list.count; idx++) {
        size_t entry_sz = 4U + 8U + xdr_padded_strlen(list.items[idx].name) +
                          8U + 88U + 16U;
        size_t used = out->len - start_len;
        if (emitted > 0 && used + entry_sz + 8U > (size_t)maxcount) break;

        NfsAttr eattr;
        int eok = (fs_getattr(list.items[idx].fileid, &eattr) == NFS3_OK);
        xdr_put_u32(out, 1U);
        xdr_put_u64(out, list.items[idx].fileid);
        xdr_put_str(out, list.items[idx].name);
        xdr_put_u64(out, (uint64_t)(idx + 1U));
        xdr_put_post_op_attr(out, eok, &eattr);
        xdr_put_post_op_fh(out, eok, list.items[idx].fileid);
        emitted++;
      }
      xdr_put_u32(out, 0U);
      xdr_put_u32(out, (idx >= list.count) ? 1U : 0U);
      dirlist_free(&list);
      return 0;
    }

    case NFSPROC3_FSSTAT: {
      if (xdr_get_fh(r, &ino, &bad_fh) != 0) return -1;
      if (bad_fh) {
        xdr_put_u32(out, NFS3ERR_BADHANDLE);
        xdr_put_post_op_attr(out, 0, NULL);
        return 0;
      }
      FsStatInfo fsi;
      memset(&fsi, 0, sizeof(fsi));
      st = fs_fsstat(ino, &fsi, &attr, &post_ok);
      xdr_put_u32(out, st);
      xdr_put_post_op_attr(out, post_ok, &attr);
      if (st == NFS3_OK) {
        xdr_put_u64(out, fsi.tbytes);
        xdr_put_u64(out, fsi.fbytes);
        xdr_put_u64(out, fsi.abytes);
        xdr_put_u64(out, fsi.tfiles);
        xdr_put_u64(out, fsi.ffiles);
        xdr_put_u64(out, fsi.afiles);
        xdr_put_u32(out, 0U);
      }
      return 0;
    }

    case NFSPROC3_FSINFO:
      if (xdr_get_fh(r, &ino, &bad_fh) != 0) return -1;
      if (bad_fh) {
        xdr_put_u32(out, NFS3ERR_BADHANDLE);
        xdr_put_post_op_attr(out, 0, NULL);
        return 0;
      }
      st = fs_getattr(ino, &attr);
      xdr_put_u32(out, st);
      xdr_put_post_op_attr(out, (st == NFS3_OK), &attr);
      if (st == NFS3_OK) {
        xdr_put_u32(out, MOUNTD_MAX_IO_SIZE); /* rtmax */
        xdr_put_u32(out, 65536U);             /* rtpref */
        xdr_put_u32(out, 4096U);              /* rtmult */
        xdr_put_u32(out, MOUNTD_MAX_IO_SIZE); /* wtmax */
        xdr_put_u32(out, 65536U);             /* wtpref */
        xdr_put_u32(out, 4096U);              /* wtmult */
        xdr_put_u32(out, 65536U);             /* dtpref */
        xdr_put_u64(out, 0x7FFFFFFFFFFFFFFFULL); /* maxfilesize */
        xdr_put_u32(out, 1U);                 /* time_delta.seconds */
        xdr_put_u32(out, 0U);                 /* time_delta.nseconds */
        xdr_put_u32(out, 0x0001U | FSF3_SYMLINK | FSF3_HOMOGENEOUS | FSF3_CANSETTIME);
      }
      return 0;

    case NFSPROC3_PATHCONF:
      if (xdr_get_fh(r, &ino, &bad_fh) != 0) return -1;
      if (bad_fh) {
        xdr_put_u32(out, NFS3ERR_BADHANDLE);
        xdr_put_post_op_attr(out, 0, NULL);
        return 0;
      }
      st = fs_getattr(ino, &attr);
      xdr_put_u32(out, st);
      xdr_put_post_op_attr(out, (st == NFS3_OK), &attr);
      if (st == NFS3_OK) {
        xdr_put_u32(out, 1023U); /* linkmax */
        xdr_put_u32(out, 255U);  /* name_max */
        xdr_put_u32(out, 1U);    /* no_trunc */
        xdr_put_u32(out, 0U);    /* chown_restricted */
        xdr_put_u32(out, 0U);    /* case_insensitive */
        xdr_put_u32(out, 1U);    /* case_preserving */
      }
      return 0;

    case NFSPROC3_COMMIT:
      if (xdr_get_fh(r, &ino, &bad_fh) != 0) return -1;
      (void)xdr_get_u64(r); /* offset */
      (void)xdr_get_u32(r); /* count */
      if (r->err) return -1;
      if (bad_fh) {
        xdr_put_u32(out, NFS3ERR_BADHANDLE);
        xdr_put_wcc_data(out, NULL, 0, NULL);
        return 0;
      }
      st = fs_commit(ino, &pre, &attr, &post_ok);
      xdr_put_u32(out, st);
      xdr_put_wcc_data(out, &pre, post_ok, &attr);
      if (st == NFS3_OK) {
        xdr_put_fixed(out, g_writeverf, 8U);
      }
      return 0;

    default:
      return -2;
  }
}

static int dispatch_rpc_call(const uint8_t *msg, size_t msg_len, XdrBuf *reply) {
  XdrReader r = {msg, msg_len, 0, 0};
  uint32_t xid = xdr_get_u32(&r);
  uint32_t mtype = xdr_get_u32(&r);
  if (r.err || mtype != RPC_CALL) return -1;

  uint32_t rpcvers = xdr_get_u32(&r);
  if (r.err) return -1;
  if (rpcvers != RPC_VERSION_2) {
    xdr_put_u32(reply, xid);
    xdr_put_u32(reply, RPC_REPLY);
    xdr_put_u32(reply, RPC_MSG_DENIED);
    xdr_put_u32(reply, RPC_MISMATCH);
    xdr_put_u32(reply, RPC_VERSION_2);
    xdr_put_u32(reply, RPC_VERSION_2);
    return reply->err ? -1 : 0;
  }

  uint32_t prog = xdr_get_u32(&r);
  uint32_t vers = xdr_get_u32(&r);
  uint32_t proc = xdr_get_u32(&r);
  (void)xdr_get_u32(&r); /* cred flavor */
  if (xdr_skip_opaque(&r, 400U) != 0) return -1;
  (void)xdr_get_u32(&r); /* verf flavor */
  if (xdr_skip_opaque(&r, 400U) != 0) return -1;

  xdr_put_u32(reply, xid);
  xdr_put_u32(reply, RPC_REPLY);
  xdr_put_u32(reply, RPC_MSG_ACCEPTED);
  xdr_put_u32(reply, AUTH_NONE);
  xdr_put_u32(reply, 0U);

  if (prog != MOUNT_PROGRAM && prog != NFS_PROGRAM) {
    xdr_put_u32(reply, RPC_PROG_UNAVAIL);
    return reply->err ? -1 : 0;
  }
  uint32_t want_vers = (prog == MOUNT_PROGRAM) ? MOUNT_V3 : NFS_V3;
  if (vers != want_vers) {
    xdr_put_u32(reply, RPC_PROG_MISMATCH);
    xdr_put_u32(reply, want_vers);
    xdr_put_u32(reply, want_vers);
    return reply->err ? -1 : 0;
  }

  size_t stat_pos = reply->len;
  xdr_put_u32(reply, RPC_SUCCESS);
  int hrc = (prog == MOUNT_PROGRAM) ? handle_mount_proc(proc, &r, reply)
                                    : handle_nfs_proc(proc, &r, reply);
  if (hrc == -1) {
    reply->len = stat_pos;
    xdr_put_u32(reply, RPC_GARBAGE_ARGS);
  } else if (hrc == -2) {
    reply->len = stat_pos;
    xdr_put_u32(reply, RPC_PROC_UNAVAIL);
  }
  return reply->err ? -1 : 0;
}

/* =========================================================================
 * Single-threaded 127.0.0.1 TCP record-marking server & main()
 * ========================================================================= */

#define MAX_RPC_CLIENTS 32

typedef struct {
  int fd;
  uint8_t hdr[4];
  size_t hdr_got;
  uint32_t frag_rem;
  int frag_last;
  int have_hdr;
  uint8_t *msg;
  size_t msg_len;
  size_t msg_cap;
} RpcClient;

static void on_stop_signal(int sig) {
  (void)sig;
  g_stop = 1;
}

static void client_reset_slots(RpcClient *c) {
  c->fd = -1;
  c->hdr_got = 0;
  c->frag_rem = 0;
  c->frag_last = 0;
  c->have_hdr = 0;
  c->msg_len = 0;
  c->msg_cap = 0;
  c->msg = NULL;
}

static void client_close(RpcClient *c) {
  if (c->fd >= 0) {
    close(c->fd);
    c->fd = -1;
  }
  free(c->msg);
  c->msg = NULL;
  c->msg_len = 0;
  c->msg_cap = 0;
  c->hdr_got = 0;
  c->frag_rem = 0;
  c->frag_last = 0;
  c->have_hdr = 0;
}

static int send_all_loopback(int fd, const uint8_t *buf, size_t len) {
  size_t off = 0;
  while (off < len) {
    ssize_t w = send(fd, buf + off, len - off, 0);
    if (w > 0) {
      off += (size_t)w;
      continue;
    }
    if (w < 0 && errno == EINTR) continue;
    if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      struct pollfd pfd = {fd, POLLOUT, 0};
      int pr = poll(&pfd, 1, 5000);
      if (pr > 0) continue;
    }
    return -1;
  }
  return 0;
}

static int send_rpc_record(int fd, const uint8_t *payload, size_t len) {
  if (len > 0x7FFFFFFFU) return -1;
  uint32_t rm = 0x80000000U | (uint32_t)len;
  uint8_t hdr[4] = {
    (uint8_t)(rm >> 24),
    (uint8_t)(rm >> 16),
    (uint8_t)(rm >> 8),
    (uint8_t)rm
  };
  if (send_all_loopback(fd, hdr, 4U) != 0) return -1;
  if (len > 0 && send_all_loopback(fd, payload, len) != 0) return -1;
  return 0;
}

static int client_on_readable(RpcClient *c) {
  for (;;) {
    if (!c->have_hdr) {
      while (c->hdr_got < 4U) {
        ssize_t r = recv(c->fd, c->hdr + c->hdr_got, 4U - c->hdr_got, 0);
        if (r == 0) return -1;
        if (r < 0) {
          if (errno == EINTR) continue;
          if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
          return -1;
        }
        c->hdr_got += (size_t)r;
      }
      uint32_t rm = ((uint32_t)c->hdr[0] << 24) | ((uint32_t)c->hdr[1] << 16) |
                    ((uint32_t)c->hdr[2] << 8)  | (uint32_t)c->hdr[3];
      c->frag_last = ((rm & 0x80000000U) != 0);
      c->frag_rem = (rm & 0x7FFFFFFFU);
      c->hdr_got = 0;
      c->have_hdr = 1;

      if (c->frag_rem > MOUNTD_MAX_RPC_MSG ||
          c->msg_len + (size_t)c->frag_rem > MOUNTD_MAX_RPC_MSG) {
        return -1;
      }
      size_t need = c->msg_len + (size_t)c->frag_rem;
      if (need > c->msg_cap) {
        size_t ncap = c->msg_cap ? c->msg_cap * 2U : 4096U;
        while (ncap < need) ncap *= 2U;
        uint8_t *nb = realloc(c->msg, ncap);
        if (!nb) return -1;
        c->msg = nb;
        c->msg_cap = ncap;
      }
    }

    while (c->frag_rem > 0) {
      ssize_t r = recv(c->fd, c->msg + c->msg_len, c->frag_rem, 0);
      if (r == 0) return -1;
      if (r < 0) {
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
        return -1;
      }
      c->msg_len += (size_t)r;
      c->frag_rem -= (uint32_t)r;
    }

    c->have_hdr = 0;
    if (c->frag_last) {
      XdrBuf reply = {NULL, 0, 0, 0};
      int drc = dispatch_rpc_call(c->msg, c->msg_len, &reply);
      c->msg_len = 0;
      if (drc != 0) {
        free(reply.data);
        return -1;
      }
      int src = send_rpc_record(c->fd, reply.data, reply.len);
      free(reply.data);
      if (src != 0) return -1;
    }
  }
}

static int set_nonblock_cloexec(int fd) {
  int fl = fcntl(fd, F_GETFL, 0);
  if (fl < 0 || fcntl(fd, F_SETFL, fl | O_NONBLOCK) < 0) return -1;
  int fdfl = fcntl(fd, F_GETFD, 0);
  if (fdfl < 0 || fcntl(fd, F_SETFD, fdfl | FD_CLOEXEC) < 0) return -1;
  return 0;
}

static void detach_stdio_to_devnull(void) {
  int null_fd = open("/dev/null", O_RDWR);
  if (null_fd >= 0) {
    (void)dup2(null_fd, STDIN_FILENO);
    (void)dup2(null_fd, STDOUT_FILENO);
    (void)dup2(null_fd, STDERR_FILENO);
    if (null_fd > STDERR_FILENO) close(null_fd);
  }
}

/* Acquire a non-blocking exclusive whole-device POSIX write lock (F_SETLK).
 * Note: on macOS (XNU), flock() conflicts with libntfs-3g's internal
 * fcntl(F_SETLK, F_WRLCK) across separate open() descriptors in the same
 * process, whereas POSIX F_SETLK is keyed by (pid, vnode), coexists cleanly
 * with libntfs-3g and libext2fs in-process on both image files and /dev/disk*
 * nodes, and immediately blocks any concurrent rufux-mountd writer process. */
static int acquire_exclusive_dev_lock(int fd) {
  struct flock flk;
  memset(&flk, 0, sizeof(flk));
  flk.l_type = F_WRLCK;
  flk.l_whence = SEEK_SET;
  flk.l_start = 0;
  flk.l_len = 0;
  return fcntl(fd, F_SETLK, &flk);
}

int main(int argc, char **argv) {
  const char *device = NULL;
  const char *fstype = NULL;
  const char *pidfile = NULL;
  int ready_fd = -1;

  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "--device") == 0 && i + 1 < argc) {
      device = argv[++i];
    } else if (strcmp(argv[i], "--fstype") == 0 && i + 1 < argc) {
      fstype = argv[++i];
    } else if (strcmp(argv[i], "--ready-fd") == 0 && i + 1 < argc) {
      ready_fd = atoi(argv[++i]);
    } else if (strcmp(argv[i], "--pidfile") == 0 && i + 1 < argc) {
      pidfile = argv[++i];
    } else {
      fprintf(stderr,
              "usage: rufux-mountd --device <path> --fstype ntfs|ext "
              "[--ready-fd <fd>] [--pidfile <path>]\n");
      return 2;
    }
  }

  if (!device || !device[0] || !fstype || !fstype[0]) {
    fprintf(stderr, "rufux-mountd: --device and --fstype are required\n");
    return 2;
  }

  if (strcmp(fstype, "ntfs") == 0) {
    g_fskind = FS_KIND_NTFS;
  } else if (strcmp(fstype, "ext") == 0 || strncmp(fstype, "ext", 3) == 0) {
    g_fskind = FS_KIND_EXT;
  } else {
    fprintf(stderr, "rufux-mountd: unsupported --fstype '%s'\n", fstype);
    return 2;
  }

  g_uid = (uint32_t)geteuid();
  g_gid = (uint32_t)getegid();
  uint64_t boot_token = ((uint64_t)time(NULL) << 32) ^ (uint64_t)getpid();
  g_fsid ^= (uint64_t)getpid();
  for (int i = 0; i < 8; i++) {
    g_writeverf[i] = (uint8_t)(boot_token >> ((7 - i) * 8));
  }

  signal(SIGPIPE, SIG_IGN);
  struct sigaction sa;
  memset(&sa, 0, sizeof(sa));
  sa.sa_handler = on_stop_signal;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = 0;
  sigaction(SIGTERM, &sa, NULL);
  sigaction(SIGINT, &sa, NULL);

  int dev_lock_fd = open(device, O_RDWR | O_CLOEXEC);
  if (dev_lock_fd < 0) {
    fprintf(stderr, "rufux-mountd: open('%s') failed: %s\n",
            device, strerror(errno));
    return 1;
  }
  if (acquire_exclusive_dev_lock(dev_lock_fd) != 0) {
    fprintf(stderr,
            "rufux-mountd: device '%s' is already locked by another writer: %s\n",
            device, strerror(errno));
    close(dev_lock_fd);
    return 1;
  }

  if (g_fskind == FS_KIND_NTFS) {
    errno = 0;
    g_ntfs = ntfs_mount(device, NTFS_MNT_RECOVER);
    if (!g_ntfs) {
      errno = 0;
      g_ntfs = ntfs_mount(device, 0);
    }
    if (!g_ntfs) {
      fprintf(stderr, "rufux-mountd: ntfs_mount('%s') failed: %s\n",
              device, strerror(errno));
      close(dev_lock_fd);
      return 1;
    }
    if (acquire_exclusive_dev_lock(dev_lock_fd) != 0) {
      fprintf(stderr, "rufux-mountd: lost exclusive lock on '%s': %s\n",
              device, strerror(errno));
      (void)ntfs_umount(g_ntfs, FALSE);
      g_ntfs = NULL;
      close(dev_lock_fd);
      return 1;
    }
    (void)ntfs_volume_get_free_space(g_ntfs);
    cache_set_parent(FILE_root, FILE_root);
  } else {
    int ext_flags = EXT2_FLAG_RW | EXT2_FLAG_64BITS | EXT2_FLAG_EXCLUSIVE;
    errcode_t ec = ext2fs_open(device, ext_flags, 0, 0, unix_io_manager, &g_ext);
    if (ec != 0 || !g_ext) {
      fprintf(stderr, "rufux-mountd: ext2fs_open('%s') failed (%ld)\n",
              device, (long)ec);
      close(dev_lock_fd);
      return 1;
    }
    if (acquire_exclusive_dev_lock(dev_lock_fd) != 0) {
      fprintf(stderr, "rufux-mountd: lost exclusive lock on '%s': %s\n",
              device, strerror(errno));
      (void)ext2fs_close_free(&g_ext);
      close(dev_lock_fd);
      return 1;
    }
    ec = ext2fs_read_bitmaps(g_ext);
    if (ec != 0) {
      fprintf(stderr, "rufux-mountd: ext2fs_read_bitmaps('%s') failed (%ld)\n",
              device, (long)ec);
      (void)ext2fs_close_free(&g_ext);
      close(dev_lock_fd);
      return 1;
    }
    cache_set_parent(EXT2_ROOT_INO, EXT2_ROOT_INO);
  }

  int lfd = socket(AF_INET, SOCK_STREAM, 0);
  if (lfd < 0 || set_nonblock_cloexec(lfd) != 0) {
    fprintf(stderr, "rufux-mountd: socket failed: %s\n", strerror(errno));
    if (lfd >= 0) close(lfd);
    goto cleanup_fail;
  }

  int one = 1;
  (void)setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

  struct sockaddr_in sin;
  memset(&sin, 0, sizeof(sin));
  sin.sin_family = AF_INET;
  sin.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  sin.sin_port = htons(0);

  if (bind(lfd, (struct sockaddr *)&sin, sizeof(sin)) != 0 ||
      listen(lfd, 16) != 0) {
    fprintf(stderr, "rufux-mountd: bind/listen 127.0.0.1:0 failed: %s\n",
            strerror(errno));
    close(lfd);
    goto cleanup_fail;
  }

  socklen_t slen = sizeof(sin);
  if (getsockname(lfd, (struct sockaddr *)&sin, &slen) != 0) {
    fprintf(stderr, "rufux-mountd: getsockname failed: %s\n", strerror(errno));
    close(lfd);
    goto cleanup_fail;
  }
  uint16_t bound_port = ntohs(sin.sin_port);

  if (pidfile && pidfile[0]) {
    FILE *pf = fopen(pidfile, "w");
    if (!pf) {
      fprintf(stderr, "rufux-mountd: fopen pidfile '%s' failed: %s\n",
              pidfile, strerror(errno));
      close(lfd);
      goto cleanup_fail;
    }
    fprintf(pf, "%ld\n", (long)getpid());
    fclose(pf);
  }

  if (ready_fd >= 0) {
    char pbuf[32];
    int pn = snprintf(pbuf, sizeof(pbuf), "%u\n", (unsigned)bound_port);
    if (pn > 0) {
      (void)write(ready_fd, pbuf, (size_t)pn);
    }
    close(ready_fd);
    ready_fd = -1;
    detach_stdio_to_devnull();
  }

  RpcClient clients[MAX_RPC_CLIENTS];
  for (int i = 0; i < MAX_RPC_CLIENTS; i++) client_reset_slots(&clients[i]);

  while (!g_stop) {
    struct pollfd pfds[1 + MAX_RPC_CLIENTS];
    int map_idx[1 + MAX_RPC_CLIENTS];
    nfds_t nfds = 0;

    pfds[nfds].fd = lfd;
    pfds[nfds].events = POLLIN;
    pfds[nfds].revents = 0;
    map_idx[nfds] = -1;
    nfds++;

    for (int i = 0; i < MAX_RPC_CLIENTS; i++) {
      if (clients[i].fd >= 0) {
        pfds[nfds].fd = clients[i].fd;
        pfds[nfds].events = POLLIN;
        pfds[nfds].revents = 0;
        map_idx[nfds] = i;
        nfds++;
      }
    }

    int pr = poll(pfds, nfds, 1000);
    if (pr < 0) {
      if (errno == EINTR) continue;
      break;
    }
    if (pr == 0) continue;

    if ((pfds[0].revents & POLLIN) != 0) {
      for (;;) {
        struct sockaddr_in peer;
        socklen_t plen = sizeof(peer);
        int cfd = accept(lfd, (struct sockaddr *)&peer, &plen);
        if (cfd < 0) {
          if (errno == EINTR) continue;
          break;
        }
        /* Strictly accept connections from 127.0.0.1 only */
        if (peer.sin_family != AF_INET ||
            peer.sin_addr.s_addr != htonl(INADDR_LOOPBACK) ||
            set_nonblock_cloexec(cfd) != 0) {
          close(cfd);
          continue;
        }
        int nd = 1;
        int sndbuf = 1048576;
        (void)setsockopt(cfd, IPPROTO_TCP, TCP_NODELAY, &nd, sizeof(nd));
        (void)setsockopt(cfd, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));
        (void)setsockopt(cfd, SOL_SOCKET, SO_RCVBUF, &sndbuf, sizeof(sndbuf));

        int placed = 0;
        for (int i = 0; i < MAX_RPC_CLIENTS; i++) {
          if (clients[i].fd < 0) {
            clients[i].fd = cfd;
            placed = 1;
            break;
          }
        }
        if (!placed) close(cfd);
      }
    }

    for (nfds_t k = 1; k < nfds; k++) {
      int ci = map_idx[k];
      if (ci < 0 || clients[ci].fd < 0) continue;
      if ((pfds[k].revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
        client_close(&clients[ci]);
        continue;
      }
      if ((pfds[k].revents & POLLIN) != 0) {
        if (client_on_readable(&clients[ci]) != 0) {
          client_close(&clients[ci]);
        }
      }
    }
  }

  for (int i = 0; i < MAX_RPC_CLIENTS; i++) client_close(&clients[i]);
  close(lfd);

  int exit_rc = 0;
  if (g_fskind == FS_KIND_NTFS && g_ntfs) {
    if (ntfs_umount(g_ntfs, FALSE) != 0) exit_rc = 1;
    g_ntfs = NULL;
  } else if (g_fskind == FS_KIND_EXT && g_ext) {
    if (ext2fs_write_bitmaps(g_ext) != 0) exit_rc = 1;
    if (ext2fs_flush(g_ext) != 0) exit_rc = 1;
    if (ext2fs_close_free(&g_ext) != 0) exit_rc = 1;
  }
  close(dev_lock_fd);
  if (pidfile && pidfile[0]) (void)unlink(pidfile);
  return exit_rc;

cleanup_fail:
  if (g_fskind == FS_KIND_NTFS && g_ntfs) {
    (void)ntfs_umount(g_ntfs, FALSE);
    g_ntfs = NULL;
  } else if (g_fskind == FS_KIND_EXT && g_ext) {
    (void)ext2fs_close_free(&g_ext);
  }
  close(dev_lock_fd);
  return 1;
}
