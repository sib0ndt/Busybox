#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <linux/ext4.h>
#include <linux/fs.h>
#include <linux/magic.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/ioctl.h>
#include <sys/statfs.h>
#include <sys/types.h>
#include <unistd.h>

enum {
	RC_NO_CHANGE = 0,
	RC_CHANGED = 10,
	RC_UNSUPPORTED = 11,
	RC_ERROR = 12,
};

int main(int argc, char **argv)
{
	const char *device_path;
	const char *mountpoint;
	struct statfs fs_info;
	uint64_t device_size_bytes;
	uint64_t target_blocks;
	uint64_t current_blocks;
	uint64_t block_size;
	int device_fd;
	int mount_fd;

	if (argc != 3) {
		fprintf(stderr, "usage: %s <block-device> <mountpoint>\n", argv[0]);
		return RC_ERROR;
	}

	device_path = argv[1];
	mountpoint = argv[2];

	device_fd = open(device_path, O_RDONLY | O_CLOEXEC);
	if (device_fd < 0) {
		perror("open block device");
		return RC_ERROR;
	}
	if (ioctl(device_fd, BLKGETSIZE64, &device_size_bytes) != 0) {
		perror("BLKGETSIZE64");
		close(device_fd);
		return RC_ERROR;
	}
	close(device_fd);

	if (statfs(mountpoint, &fs_info) != 0) {
		perror("statfs");
		return RC_ERROR;
	}
	if (fs_info.f_type != EXT2_SUPER_MAGIC && fs_info.f_type != EXT4_SUPER_MAGIC) {
		return RC_UNSUPPORTED;
	}

	block_size = fs_info.f_bsize != 0 ? (uint64_t)fs_info.f_bsize : (uint64_t)fs_info.f_frsize;
	if (block_size == 0) {
		return RC_ERROR;
	}

	current_blocks = (uint64_t)fs_info.f_blocks;
	target_blocks = device_size_bytes / block_size;
	if (target_blocks <= current_blocks) {
		return RC_NO_CHANGE;
	}

	mount_fd = open(mountpoint, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	if (mount_fd < 0) {
		perror("open mountpoint");
		return RC_ERROR;
	}
	if (ioctl(mount_fd, EXT4_IOC_RESIZE_FS, &target_blocks) != 0) {
		if (errno == ENOTTY || errno == EOPNOTSUPP) {
			close(mount_fd);
			return RC_UNSUPPORTED;
		}
		perror("EXT4_IOC_RESIZE_FS");
		close(mount_fd);
		return RC_ERROR;
	}
	close(mount_fd);

	return RC_CHANGED;
}
