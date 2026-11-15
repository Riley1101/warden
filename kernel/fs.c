#include "fs.h"

#include <abi/errno.h>
#include <kstring.h>
#include <path.h>
#include <slab.h>
#include <tar.h>

struct node {
  char name[NAME_MAX + 1];
  uint32_t type;
  uint64_t ino;
  struct node *parent;      /* NULL for the root, and once unlinked */
  struct node *child, *next; /* a directory's entries, in creation order */
  uint8_t *data;            /* a file's contents: size bytes of cap */
  size_t size, cap;
  int refs;   /* open files */
  int linked; /* in the tree */
  struct label writer;   /* fs_note_writer: who wrote it */
  uint32_t writer_max;   /* the writer's session limit, the lowest seen */
};

static struct node *root;
static uint64_t next_ino = 1;

static int streq(const char *a, const char *b) {
  size_t n = strlen(a);
  return n == strlen(b) && memcmp(a, b, n) == 0;
}

static struct node *node_new(const char *name, size_t len, uint32_t type) {
  struct node *n = kzalloc(sizeof *n);
  if (n == NULL)
    return NULL;
  memcpy(n->name, name, len);
  n->name[len] = '\0';
  n->type = type;
  n->ino = next_ino++;
  return n;
}

static void node_free(struct node *n) {
  kfree(n->data);
  kfree(n);
}

/* Free `n` if nothing holds it any more. */
static void maybe_free(struct node *n) {
  if (!n->linked && n->refs == 0)
    node_free(n);
}

static void attach(struct node *dir, struct node *n) {
  struct node **link = &dir->child;
  while (*link != NULL)
    link = &(*link)->next;
  *link = n;
  n->next = NULL;
  n->parent = dir;
  n->linked = 1;
}

static void detach(struct node *n) {
  struct node **link = &n->parent->child;
  while (*link != n)
    link = &(*link)->next;
  *link = n->next;
  n->next = NULL;
  n->parent = NULL;
  n->linked = 0;
}

static struct node *child_named(struct node *dir, const char *name,
                                size_t len) {
  for (struct node *c = dir->child; c != NULL; c = c->next)
    if (strlen(c->name) == len && memcmp(c->name, name, len) == 0)
      return c;
  return NULL;
}

int fs_init(void) {
  root = node_new("", 0, FT_DIR);
  if (root == NULL)
    return -ENOMEM;
  root->linked = 1; /* never unlinked */
  return 0;
}

/* ---- paths ---------------------------------------------------------------- */

/* `path`, normalized; a relative path is -ENOENT (there is no cwd). */
static int normalize(const char *path, char *out) {
  int err = path_normalize(path, out, PATH_MAX);
  return err == -EINVAL ? -ENOENT : err;
}

/* The node at normalized path `p`. */
static int walk(const char *p, struct node **out) {
  struct node *cur = root;
  while (*p == '/')
    p++;
  while (*p != '\0') {
    const char *end = p;
    while (*end != '\0' && *end != '/')
      end++;
    if (cur->type != FT_DIR)
      return -ENOTDIR;
    if ((size_t)(end - p) > NAME_MAX)
      return -ENAMETOOLONG;
    cur = child_named(cur, p, end - p);
    if (cur == NULL)
      return -ENOENT;
    p = *end == '/' ? end + 1 : end;
  }
  *out = cur;
  return 0;
}

static int lookup(const char *path, struct node **out) {
  char p[PATH_MAX];
  int err = normalize(path, p);
  return err != 0 ? err : walk(p, out);
}

/*
 * Split `path` into its directory (which must exist) and last component.
 * `name` gets the component (NAME_MAX + 1 bytes). The root has no last
 * component: -EINVAL.
 */
static int lookup_parent(const char *path, struct node **dir, char *name) {
  char p[PATH_MAX];
  int err = normalize(path, p);
  if (err != 0)
    return err;
  if (streq(p, "/"))
    return -EINVAL;
  size_t len = strlen(p), slash = len;
  while (p[slash - 1] != '/')
    slash--;
  if (len - slash > NAME_MAX)
    return -ENAMETOOLONG;
  memcpy(name, p + slash, len - slash + 1);
  p[slash > 1 ? slash - 1 : 1] = '\0'; /* "/a/b" -> "/a"; "/a" -> "/" */
  err = walk(p, dir);
  if (err != 0)
    return err;
  return (*dir)->type == FT_DIR ? 0 : -ENOTDIR;
}

/* ---- the tree ------------------------------------------------------------- */

static int create(struct node *dir, const char *name, uint32_t type,
                  struct node **out) {
  struct node *n = node_new(name, strlen(name), type);
  if (n == NULL)
    return -ENOMEM;
  attach(dir, n);
  *out = n;
  return 0;
}

int fs_open(const char *path, int flags, struct node **out) {
  struct node *dir, *n;
  char name[NAME_MAX + 1];
  int err = lookup(path, &n);
  if (err == -ENOENT && (flags & O_CREAT)) {
    err = lookup_parent(path, &dir, name);
    if (err == 0)
      err = create(dir, name, FT_FILE, &n);
  }
  if (err != 0)
    return err;
  int writes = (flags & O_ACCMODE) != O_RDONLY;
  if (n->type == FT_DIR && (writes || (flags & O_TRUNC)))
    return -EISDIR;
  if ((flags & O_TRUNC) && writes)
    n->size = 0;
  n->refs++;
  *out = n;
  return 0;
}

void fs_put(struct node *n) {
  n->refs--;
  maybe_free(n);
}

int fs_mkdir(const char *path) {
  struct node *dir, *n;
  char name[NAME_MAX + 1];
  int err = lookup_parent(path, &dir, name);
  if (err == -EINVAL)
    return -EEXIST; /* "/" */
  if (err != 0)
    return err;
  if (child_named(dir, name, strlen(name)) != NULL)
    return -EEXIST;
  return create(dir, name, FT_DIR, &n);
}

/* Take `n` out of the tree; it lives on while open. */
static void drop(struct node *n) {
  detach(n);
  maybe_free(n);
}

int fs_unlink(const char *path) {
  struct node *n;
  int err = lookup(path, &n);
  if (err != 0)
    return err;
  if (n == root)
    return -EBUSY;
  if (n->type == FT_DIR && n->child != NULL)
    return -ENOTEMPTY;
  drop(n);
  return 0;
}

/* Is `n` `anc`, or somewhere below it? */
static int within(struct node *n, struct node *anc) {
  for (; n != NULL; n = n->parent)
    if (n == anc)
      return 1;
  return 0;
}

int fs_rename(const char *from, const char *to) {
  struct node *n, *dir;
  char name[NAME_MAX + 1];
  int err = lookup(from, &n);
  if (err != 0)
    return err;
  if (n == root)
    return -EBUSY;
  err = lookup_parent(to, &dir, name);
  if (err != 0)
    return err == -EINVAL ? -EBUSY : err; /* onto "/" */
  if (n->type == FT_DIR && within(dir, n))
    return -EINVAL; /* a directory into itself */

  struct node *old = child_named(dir, name, strlen(name));
  if (old == n)
    return 0;
  if (old != NULL) {
    /* Replace it, as rename(2) does: a file by a file, an empty directory
     * by a directory. */
    if (old->type == FT_DIR && n->type != FT_DIR)
      return -EISDIR;
    if (old->type != FT_DIR && n->type == FT_DIR)
      return -ENOTDIR;
    if (old->type == FT_DIR && old->child != NULL)
      return -ENOTEMPTY;
    drop(old);
  }
  detach(n);
  memcpy(n->name, name, strlen(name) + 1);
  attach(dir, n);
  return 0;
}

void fs_fstat(struct node *n, struct stat *st) {
  uint64_t entries = 0;
  for (struct node *c = n->child; c != NULL; c = c->next)
    entries++;
  int agent = n->writer.origin == ORIGIN_AGENT;
  *st = (struct stat){
      .type = n->type,
      .agent = agent,
      .ino = n->ino,
      .size = n->type == FT_DIR ? entries : n->size,
      .session = agent ? n->writer.session : 0,
      .caps = agent ? n->writer.caps : 0,
  };
}

int fs_stat(const char *path, struct stat *st) {
  struct node *n;
  int err = lookup(path, &n);
  if (err == 0)
    fs_fstat(n, st);
  return err;
}

int fs_is_dir(struct node *n) { return n->type == FT_DIR; }
uint64_t fs_size(struct node *n) { return n->size; }
const void *fs_data(struct node *n) { return n->data; }

/* ---- contents ------------------------------------------------------------- */

int64_t fs_read(struct node *n, uint64_t off, void *dst, size_t len) {
  if (n->type == FT_DIR)
    return -EISDIR;
  if (off >= n->size)
    return 0;
  if (len > n->size - off)
    len = n->size - off;
  memcpy(dst, n->data + off, len);
  return (int64_t)len;
}

/* Room for `want` bytes: grow by doubling, so appends stay cheap. */
static int reserve(struct node *n, size_t want) {
  if (want <= n->cap)
    return 0;
  size_t cap = n->cap < 64 ? 64 : n->cap;
  while (cap < want)
    cap *= 2;
  if (cap > FS_FILE_MAX)
    cap = FS_FILE_MAX;
  uint8_t *data = kmalloc(cap);
  if (data == NULL)
    return -ENOMEM;
  memcpy(data, n->data, n->size);
  kfree(n->data);
  n->data = data;
  n->cap = cap;
  return 0;
}

int64_t fs_write(struct node *n, uint64_t off, const void *src, size_t len) {
  if (n->type == FT_DIR)
    return -EISDIR;
  if (off > FS_FILE_MAX || len > FS_FILE_MAX - off)
    return -EFBIG;
  int err = reserve(n, off + len);
  if (err != 0)
    return err;
  if (off > n->size)
    memset(n->data + n->size, 0, off - n->size); /* the gap reads as zeros */
  memcpy(n->data + off, src, len);
  if (off + len > n->size)
    n->size = off + len;
  return (int64_t)len;
}

int fs_readdir(struct node *n, uint64_t index, struct dirent *out) {
  if (n->type != FT_DIR)
    return -ENOTDIR;
  struct node *c = n->child;
  for (; c != NULL && index > 0; index--)
    c = c->next;
  if (c == NULL)
    return 0;
  out->type = c->type;
  memcpy(out->name, c->name, sizeof out->name);
  return 1;
}

/* ---- the initramfs -------------------------------------------------------- */

/* mkdir -p: every directory on the way to `path`, and `path` itself. */
static int mkdirs(const char *path) {
  char p[PATH_MAX];
  int err = normalize(path, p);
  if (err != 0)
    return err;
  for (char *s = p + 1;; s++) {
    if (*s != '/' && *s != '\0')
      continue;
    char c = *s;
    *s = '\0';
    err = fs_mkdir(p);
    *s = c;
    if (err != 0 && err != -EEXIST)
      return err;
    if (c == '\0')
      break;
  }
  struct node *n;
  err = walk(p, &n);
  return err != 0 ? err : n->type == FT_DIR ? 0 : -ENOTDIR;
}

static int load_member(void *ctx, const char *path, enum tar_type type,
                       const void *data, size_t size) {
  (void)ctx;
  if (streq(path, "/"))
    return 0;
  if (type == TAR_DIR)
    return mkdirs(path);

  /* its directory first: an archive needn't list it */
  char dir[PATH_MAX];
  size_t slash = strlen(path);
  while (path[slash] != '/')
    slash--;
  memcpy(dir, path, slash);
  dir[slash] = '\0'; /* "/a/b" -> "/a"; "/a" -> "" (the root) */
  int err = dir[0] != '\0' ? mkdirs(dir) : 0;
  struct node *n;
  if (err == 0)
    err = fs_open(path, O_WRONLY | O_CREAT | O_TRUNC, &n);
  if (err != 0)
    return err;
  int64_t w = fs_write(n, 0, data, size);
  fs_put(n);
  return w < 0 ? (int)w : 0;
}

int fs_load_tar(const void *tar, size_t len) {
  return tar_each(tar, len, load_member, NULL);
}

int fs_note_writer(struct node *n, const struct label *l, uint32_t max_procs) {
  if (l->origin == ORIGIN_HUMAN)
    return 0;
  int err = label_meet(&n->writer, l);
  if (err != 0)
    return err;
  if (n->writer_max == 0 || max_procs < n->writer_max)
    n->writer_max = max_procs;
  return 0;
}

const struct label *fs_writer(struct node *n, uint32_t *max_procs) {
  *max_procs = n->writer_max;
  return &n->writer;
}
