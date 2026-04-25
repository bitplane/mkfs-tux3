#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <libgen.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#ifdef DT_UNKNOWN
#undef DT_UNKNOWN
#endif
#ifdef DT_FIFO
#undef DT_FIFO
#endif
#ifdef DT_CHR
#undef DT_CHR
#endif
#ifdef DT_DIR
#undef DT_DIR
#endif
#ifdef DT_BLK
#undef DT_BLK
#endif
#ifdef DT_REG
#undef DT_REG
#endif
#ifdef DT_LNK
#undef DT_LNK
#endif
#ifdef DT_SOCK
#undef DT_SOCK
#endif
#ifdef DT_WHT
#undef DT_WHT
#endif

#include "user/tux3user.h"
#include "user/diskio.h"

static void usage(const char *progname)
{
	fprintf(stderr,
		"Usage: %s [-b blocksize] [-s image-size] IMAGE [SOURCE_DIR]\n"
		"\n"
		"Create a Tux3 filesystem image and optionally populate it from SOURCE_DIR.\n"
		"image-size accepts plain bytes or K/M/G/T suffixes.\n",
		progname);
}

static void die_errno(const char *what, const char *path)
{
	if (path)
		fprintf(stderr, "%s '%s': %s\n", what, path, strerror(errno));
	else
		fprintf(stderr, "%s: %s\n", what, strerror(errno));
	exit(1);
}

static void die_msg(const char *msg)
{
	fprintf(stderr, "%s\n", msg);
	exit(1);
}

static unsigned parse_blocksize(const char *value)
{
	char *end;
	unsigned long blocksize = strtoul(value, &end, 0);
	if (*end != '\0' || blocksize == 0 || blocksize > (1UL << 20))
		die_msg("invalid block size");
	if ((blocksize & (blocksize - 1)) != 0)
		die_msg("block size must be a power of two");
	return (unsigned)blocksize;
}

static loff_t parse_size(const char *value)
{
	char *end;
	unsigned long long size = strtoull(value, &end, 0);
	unsigned long long multiplier = 1;

	if (end == value)
		die_msg("invalid image size");

	if (*end) {
		switch (*end) {
		case 'k': case 'K': multiplier = 1ULL << 10; break;
		case 'm': case 'M': multiplier = 1ULL << 20; break;
		case 'g': case 'G': multiplier = 1ULL << 30; break;
		case 't': case 'T': multiplier = 1ULL << 40; break;
		default: die_msg("invalid image size suffix");
		}
		end++;
		if (*end == 'i' || *end == 'I')
			end++;
		if (*end == 'b' || *end == 'B')
			end++;
		if (*end != '\0')
			die_msg("invalid image size suffix");
	}

	if (size == 0 || size > (ULLONG_MAX / multiplier))
		die_msg("invalid image size");
	return (loff_t)(size * multiplier);
}

static int open_image(const char *path, loff_t requested_size, loff_t *actual_size)
{
	int flags = O_RDWR | O_CREAT;
	int fd = open(path, flags, 0644);
	if (fd < 0)
		die_errno("open", path);

	if (requested_size) {
		if (ftruncate(fd, requested_size) < 0)
			die_errno("truncate", path);
		*actual_size = requested_size;
	} else {
		if (fdsize64(fd, actual_size))
			die_errno("get size", path);
		if (*actual_size == 0)
			die_msg("image is empty; pass -s SIZE or pre-size the image");
	}

	return fd;
}

static void init_iattr_from_stat(struct tux_iattr *iattr, const struct stat *st)
{
	memset(iattr, 0, sizeof(*iattr));
	iattr->uid = make_kuid(&init_user_ns, st->st_uid);
	iattr->gid = make_kgid(&init_user_ns, st->st_gid);
	iattr->mode = st->st_mode;
	iattr->rdev = st->st_rdev;
}

static void write_file_data(struct inode *inode, const char *host_path)
{
	int fd = open(host_path, O_RDONLY);
	if (fd < 0)
		die_errno("open source", host_path);

	struct file file = FILE_INIT(inode, 0);
	char buffer[1 << 16];

	for (;;) {
		ssize_t got = read(fd, buffer, sizeof(buffer));
		if (got < 0)
			die_errno("read source", host_path);
		if (got == 0)
			break;

		char *cursor = buffer;
		ssize_t remaining = got;
		while (remaining > 0) {
			int written = tuxwrite(&file, cursor, remaining);
			if (written < 0) {
				fprintf(stderr, "write tux3 '%s': %s\n", host_path,
					strerror(-written));
				exit(1);
			}
			if (written == 0)
				die_msg("short tux3 write");
			cursor += written;
			remaining -= written;
		}
	}

	close(fd);
}

static void populate_entry(struct inode *parent, const char *host_path,
				   const char *name);

static void populate_directory_contents(struct inode *dir_inode,
					const char *host_path)
{
	DIR *dir = opendir(host_path);
	if (!dir)
		die_errno("opendir", host_path);

	for (;;) {
		errno = 0;
		struct dirent *entry = readdir(dir);
		if (!entry)
			break;
		if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
			continue;

		char *child_path;
		if (asprintf(&child_path, "%s/%s", host_path, entry->d_name) < 0)
			die_errno("asprintf", NULL);
		populate_entry(dir_inode, child_path, entry->d_name);
		free(child_path);
	}

	if (errno)
		die_errno("readdir", host_path);
	closedir(dir);
}

static void populate_symlink(struct inode *parent, const char *host_path,
			     const char *name, struct tux_iattr *iattr)
{
	struct stat st;
	if (lstat(host_path, &st) < 0)
		die_errno("lstat symlink", host_path);

	size_t size = st.st_size ? (size_t)st.st_size + 1 : PATH_MAX;
	char *target = malloc(size);
	if (!target)
		die_errno("malloc", NULL);

	ssize_t len = readlink(host_path, target, size - 1);
	if (len < 0)
		die_errno("readlink", host_path);
	target[len] = '\0';

	int err = tuxsymlink(parent, name, strlen(name), iattr, target);
	if (err) {
		fprintf(stderr, "symlink tux3 '%s': %s\n", host_path, strerror(-err));
		exit(1);
	}
	free(target);
}

static void populate_entry(struct inode *parent, const char *host_path,
				   const char *name)
{
	struct stat st;
	struct tux_iattr iattr;

	if (lstat(host_path, &st) < 0)
		die_errno("lstat", host_path);
	init_iattr_from_stat(&iattr, &st);

	if (S_ISLNK(st.st_mode)) {
		populate_symlink(parent, host_path, name, &iattr);
		return;
	}

	struct inode *inode = tuxmknod(parent, name, strlen(name), &iattr);
	if (IS_ERR(inode)) {
		fprintf(stderr, "create tux3 '%s': %s\n", host_path,
			strerror(-PTR_ERR(inode)));
		exit(1);
	}

	if (S_ISDIR(st.st_mode))
		populate_directory_contents(inode, host_path);
	else if (S_ISREG(st.st_mode))
		write_file_data(inode, host_path);

	iput(inode);
}

int main(int argc, char **argv)
{
	unsigned blocksize = 1 << 12;
	loff_t requested_size = 0;
	int opt;

	while ((opt = getopt(argc, argv, "b:s:h")) != -1) {
		switch (opt) {
		case 'b': blocksize = parse_blocksize(optarg); break;
		case 's': requested_size = parse_size(optarg); break;
		case 'h': usage(argv[0]); return 0;
		default: usage(argv[0]); return 1;
		}
	}

	if (argc - optind < 1 || argc - optind > 2) {
		usage(argv[0]);
		return 1;
	}

	const char *image_path = argv[optind];
	const char *source_path = argc - optind == 2 ? argv[optind + 1] : NULL;

	loff_t image_size;
	int fd = open_image(image_path, requested_size, &image_size);

	int blockbits = ffs(blocksize) - 1;
	if (((loff_t)1 << blockbits) != blocksize)
		die_msg("block size must be a power of two");
	if ((image_size >> blockbits) == 0)
		die_msg("image is too small for selected block size");

	int err = tux3_init_mem(1 << 28, 2);
	if (err) {
		fprintf(stderr, "tux3_init_mem: %s\n", strerror(-err));
		return 1;
	}

	struct dev dev = { .fd = fd, .bits = blockbits };
	struct sb *sb = rapid_sb(&dev);
	sb->super = INIT_DISKSB(blockbits, image_size >> blockbits);

	err = mkfs_tux3(sb);
	if (err) {
		fprintf(stderr, "mkfs_tux3: %s\n", strerror(-err));
		return 1;
	}

	if (source_path)
		populate_directory_contents(sb->rootdir, source_path);

	err = sync_super(sb);
	if (err) {
		fprintf(stderr, "sync_super: %s\n", strerror(-err));
		return 1;
	}

	put_super(sb);
	tux3_exit_mem();
	close(fd);
	return 0;
}
