/*
  FUSE: Filesystem in Userspace
  Copyright (C) 2001-2007  Miklos Szeredi <miklos@szeredi.hu>

  This program can be distributed under the terms of the GNU GPL.
  See the file COPYING.



  This is "sortfs", a FUSE FS which mirrors an underlying filesystem,
  while presenting all of the directories as sorted. It was based on one
  of the FUSE examples.

  This is a silly idea unless you have a piece of software which assumes
  directories are sorted (perhaps, Windows software?) which you can't fix.

  At which point you find yourself wishing you had this.



  gcc -Wall sortfs.c -o sortfs `pkg-config fuse --cflags --libs` -lulockmgr

  sortfs -o modules=subdir,subdir=/origin /mountpoint
*/

#define FUSE_USE_VERSION 26

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#define _GNU_SOURCE

#include <ctype.h>
#include <fuse.h>
#include <ulockmgr.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <errno.h>
#include <sys/time.h>
#ifdef HAVE_SETXATTR
#include <sys/xattr.h>
#endif

static int sort_getattr(const char *path, struct stat *stbuf)
{
	int res;

	res = lstat(path, stbuf);
	if (res == -1)
		return -errno;

	return 0;
}

static int sort_fgetattr(const char *path, struct stat *stbuf,
			struct fuse_file_info *fi)
{
	int res;

	(void) path;

	res = fstat(fi->fh, stbuf);
	if (res == -1)
		return -errno;

	return 0;
}

static int sort_access(const char *path, int mask)
{
	int res;

	res = access(path, mask);
	if (res == -1)
		return -errno;

	return 0;
}

static int sort_readlink(const char *path, char *buf, size_t size)
{
	int res;

	res = readlink(path, buf, size - 1);
	if (res == -1)
		return -errno;

	buf[res] = '\0';
	return 0;
}

struct sort_dirp {
	DIR *dp;
	struct dirent *entries;
        off_t size, allocated;
};

static int sort_opendir(const char *path, struct fuse_file_info *fi)
{
	int res;
	struct sort_dirp *d = malloc(sizeof(struct sort_dirp));

	if (d == NULL)
		return -ENOMEM;

	d->dp = opendir(path);
	if (d->dp == NULL) {
		res = -errno;
		free(d);
		return res;
	}
	d->entries = NULL;
        d->size = 0;
        d->allocated = 0;

	fi->fh = (unsigned long) d;
	return 0;
}

static inline struct sort_dirp *get_dirp(struct fuse_file_info *fi)
{
	return (struct sort_dirp *) (uintptr_t) fi->fh;
}

int dirent_simple_comparison(const void *a, const void *b)
{
  const char *s1 = ((const struct dirent *)a)->d_name;
  const char *s2 = ((const struct dirent *)b)->d_name;
  return strcasecmp(s1, s2);
}

int dirent_natural_comparison(const void *a, const void *b)
{
  const char *s1 = ((const struct dirent *)a)->d_name;
  const char *s2 = ((const struct dirent *)b)->d_name;

  // This is Norman Ramsey's natural sort
  // http://stackoverflow.com/a/1344071/32878
  // thanks!
  for (;;) {
    if (*s2 == '\0')
      return *s1 != '\0';
    else if (*s1 == '\0')
      return 1;
    else if (!(isdigit(*s1) && isdigit(*s2))) {
      if (*s1 != *s2)
        return (int)*s1 - (int)*s2;
      else
        (++s1, ++s2);
    } else {
      char *lim1, *lim2;
      unsigned long n1 = strtoul(s1, &lim1, 10);
      unsigned long n2 = strtoul(s2, &lim2, 10);
      if (n1 > n2)
        return 1;
      else if (n1 < n2)
        return -1;
      s1 = lim1;
      s2 = lim2;
    }
  }
}

static int load_dir_contents(struct sort_dirp *d)
{
  int ret;
  void *allocation;
  struct dirent *result;

  d->entries = malloc(sizeof(struct dirent)*3);
  if(d->entries == NULL)
    return -ENOMEM;
  d->allocated = 3;

  while (!(ret=readdir_r(d->dp, &d->entries[d->size], &result))) {
    if(result==NULL)
      break;

    d->size++;

    if(d->size >= d->allocated)
      {
        int newsize = 2*d->allocated;
        allocation = realloc(d->entries, newsize * sizeof(struct dirent));
        if(allocation == NULL) {
          free(d->entries);
          return -ENOMEM;
        }
        d->entries = allocation;
        d->allocated = newsize;
      }
  }

  if(ret) {
    // some sort of failure occured
    free(d->entries);
    return -ret;
  }

  qsort(d->entries, d->size, sizeof(struct dirent),
        dirent_simple_comparison);

  return 0;
}

static int sort_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
		       off_t offset, struct fuse_file_info *fi)
{
	struct sort_dirp *d = get_dirp(fi);
        off_t idx;

        if(!d->size) {
          int ret;
          ret = load_dir_contents(d);
          if(ret)
            return ret;
        }

        // have everything sorted. now pull out the part they want.

	(void) path;
        for(idx = offset; idx < d->size; idx++) {
		struct stat st;
		memset(&st, 0, sizeof(st));
		st.st_ino = d->entries[idx].d_ino;
		st.st_mode = d->entries[idx].d_type << 12;
		if (filler(buf, d->entries[idx].d_name, &st, idx+1))
			break;
	}

	return 0;
}

static int sort_releasedir(const char *path, struct fuse_file_info *fi)
{
	struct sort_dirp *d = get_dirp(fi);
	(void) path;
	closedir(d->dp);
        free(d->entries);
	free(d);
	return 0;
}

static int sort_mknod(const char *path, mode_t mode, dev_t rdev)
{
	int res;

	if (S_ISFIFO(mode))
		res = mkfifo(path, mode);
	else
		res = mknod(path, mode, rdev);
	if (res == -1)
		return -errno;

	return 0;
}

static int sort_mkdir(const char *path, mode_t mode)
{
	int res;

	res = mkdir(path, mode);
	if (res == -1)
		return -errno;

	return 0;
}

static int sort_unlink(const char *path)
{
	int res;

	res = unlink(path);
	if (res == -1)
		return -errno;

	return 0;
}

static int sort_rmdir(const char *path)
{
	int res;

	res = rmdir(path);
	if (res == -1)
		return -errno;

	return 0;
}

static int sort_symlink(const char *from, const char *to)
{
	int res;

	res = symlink(from, to);
	if (res == -1)
		return -errno;

	return 0;
}

static int sort_rename(const char *from, const char *to)
{
	int res;

	res = rename(from, to);
	if (res == -1)
		return -errno;

	return 0;
}

static int sort_link(const char *from, const char *to)
{
	int res;

	res = link(from, to);
	if (res == -1)
		return -errno;

	return 0;
}

static int sort_chmod(const char *path, mode_t mode)
{
	int res;

	res = chmod(path, mode);
	if (res == -1)
		return -errno;

	return 0;
}

static int sort_chown(const char *path, uid_t uid, gid_t gid)
{
	int res;

	res = lchown(path, uid, gid);
	if (res == -1)
		return -errno;

	return 0;
}

static int sort_truncate(const char *path, off_t size)
{
	int res;

	res = truncate(path, size);
	if (res == -1)
		return -errno;

	return 0;
}

static int sort_ftruncate(const char *path, off_t size,
			 struct fuse_file_info *fi)
{
	int res;

	(void) path;

	res = ftruncate(fi->fh, size);
	if (res == -1)
		return -errno;

	return 0;
}

static int sort_utimens(const char *path, const struct timespec ts[2])
{
	int res;
	struct timeval tv[2];

	tv[0].tv_sec = ts[0].tv_sec;
	tv[0].tv_usec = ts[0].tv_nsec / 1000;
	tv[1].tv_sec = ts[1].tv_sec;
	tv[1].tv_usec = ts[1].tv_nsec / 1000;

	res = utimes(path, tv);
	if (res == -1)
		return -errno;

	return 0;
}

static int sort_create(const char *path, mode_t mode, struct fuse_file_info *fi)
{
	int fd;

	fd = open(path, fi->flags, mode);
	if (fd == -1)
		return -errno;

        // preallocate for average sized files?

	fi->fh = fd;
	return 0;
}

static int sort_open(const char *path, struct fuse_file_info *fi)
{
	int fd;

	fd = open(path, fi->flags);
	if (fd == -1)
		return -errno;

	fi->fh = fd;
	return 0;
}

static int sort_read(const char *path, char *buf, size_t size, off_t offset,
		    struct fuse_file_info *fi)
{
	int res;

	(void) path;
	res = pread(fi->fh, buf, size, offset);
	if (res == -1)
		res = -errno;

	return res;
}

static int sort_write(const char *path, const char *buf, size_t size,
		     off_t offset, struct fuse_file_info *fi)
{
	int res;

	(void) path;
	res = pwrite(fi->fh, buf, size, offset);
	if (res == -1)
		res = -errno;

	return res;
}

static int sort_statfs(const char *path, struct statvfs *stbuf)
{
	int res;

	res = statvfs(path, stbuf);
	if (res == -1)
		return -errno;

	return 0;
}

static int sort_flush(const char *path, struct fuse_file_info *fi)
{
	int res;

	(void) path;
	/* This is called from every close on an open file, so call the
	   close on the underlying filesystem.	But since flush may be
	   called multiple times for an open file, this must not really
	   close the file.  This is important if used on a network
	   filesystem like NFS which flush the data/metadata on close() */
	res = close(dup(fi->fh));
	if (res == -1)
		return -errno;

	return 0;
}

static int sort_release(const char *path, struct fuse_file_info *fi)
{
	(void) path;
	close(fi->fh);

	return 0;
}

static int sort_fsync(const char *path, int isdatasync,
		     struct fuse_file_info *fi)
{
	int res;
	(void) path;

#ifndef HAVE_FDATASYNC
	(void) isdatasync;
#else
	if (isdatasync)
		res = fdatasync(fi->fh);
	else
#endif
		res = fsync(fi->fh);
	if (res == -1)
		return -errno;

	return 0;
}

#ifdef HAVE_SETXATTR
/* xattr operations are optional and can safely be left unimplemented */
static int sort_setxattr(const char *path, const char *name, const char *value,
			size_t size, int flags)
{
        int res = lsetxattr(path, name, value, size, flags);
	if (res == -1)
		return -errno;
	return 0;
}

static int sort_getxattr(const char *path, const char *name, char *value,
			size_t size)
{
        int res = lgetxattr(path, name, value, size);
	if (res == -1)
		return -errno;
	return res;
}

static int sort_listxattr(const char *path, char *list, size_t size)
{
        int res = llistxattr(path, list, size);
	if (res == -1)
		return -errno;
	return res;
}

static int sort_removexattr(const char *path, const char *name)
{
        int res = lremovexattr(path, name);
	if (res == -1)
		return -errno;
	return 0;
}
#endif /* HAVE_SETXATTR */

static int sort_lock(const char *path, struct fuse_file_info *fi, int cmd,
		    struct flock *lock)
{
	(void) path;

	return ulockmgr_op(fi->fh, cmd, lock, &fi->lock_owner,
			   sizeof(fi->lock_owner));
}

static struct fuse_operations sort_oper = {
	.getattr	= sort_getattr,
	.fgetattr	= sort_fgetattr,
	.access		= sort_access,
	.readlink	= sort_readlink,
	.opendir	= sort_opendir,
	.readdir	= sort_readdir,
	.releasedir	= sort_releasedir,
	.mknod		= sort_mknod,
	.mkdir		= sort_mkdir,
	.symlink	= sort_symlink,
	.unlink		= sort_unlink,
	.rmdir		= sort_rmdir,
	.rename		= sort_rename,
	.link		= sort_link,
	.chmod		= sort_chmod,
	.chown		= sort_chown,
	.truncate	= sort_truncate,
	.ftruncate	= sort_ftruncate,
	.utimens	= sort_utimens,
	.create		= sort_create,
	.open		= sort_open,
	.read		= sort_read,
	.write		= sort_write,
	.statfs		= sort_statfs,
	.flush		= sort_flush,
	.release	= sort_release,
	.fsync		= sort_fsync,
#ifdef HAVE_SETXATTR
	.setxattr	= sort_setxattr,
	.getxattr	= sort_getxattr,
	.listxattr	= sort_listxattr,
	.removexattr	= sort_removexattr,
#endif
	.lock		= sort_lock,

	.flag_nullpath_ok = 1,
};

int main(int argc, char *argv[])
{
	umask(0);
	return fuse_main(argc,
                         argv,
                         &sort_oper,
                         NULL);
}
