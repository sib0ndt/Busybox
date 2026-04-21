#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/fs.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define MBR_SIZE 512
#define MBR_SIGNATURE_OFFSET 510
#define PARTITION_TABLE_OFFSET 446
#define PARTITION_ENTRY_SIZE 16
#define MAX_PRIMARY_PARTITIONS 4

enum {
	RC_NO_CHANGE = 0,
	RC_CHANGED = 10,
	RC_UNSUPPORTED = 11,
	RC_ERROR = 12,
	RC_REREAD_NEEDED = 13,
};

struct partition_entry {
	uint8_t bootable;
	uint8_t start_chs[3];
	uint8_t type;
	uint8_t end_chs[3];
	uint32_t start_lba;
	uint32_t sector_count;
};

static uint32_t load_le32(const uint8_t *value)
{
	return ((uint32_t)value[0])
		| ((uint32_t)value[1] << 8)
		| ((uint32_t)value[2] << 16)
		| ((uint32_t)value[3] << 24);
}

static void store_le32(uint8_t *value, uint32_t data)
{
	value[0] = (uint8_t)(data & 0xffU);
	value[1] = (uint8_t)((data >> 8) & 0xffU);
	value[2] = (uint8_t)((data >> 16) & 0xffU);
	value[3] = (uint8_t)((data >> 24) & 0xffU);
}

static int read_line(const char *path, char *buffer, size_t size)
{
	FILE *file;

	file = fopen(path, "r");
	if (file == NULL) {
		return -1;
	}
	if (fgets(buffer, (int)size, file) == NULL) {
		fclose(file);
		return -1;
	}
	fclose(file);
	buffer[strcspn(buffer, "\n")] = '\0';
	return 0;
}

static int read_uint64_file(const char *path, uint64_t *value)
{
	char buffer[64];
	char *endptr;
	unsigned long long parsed;

	if (read_line(path, buffer, sizeof(buffer)) != 0) {
		return -1;
	}
	errno = 0;
	parsed = strtoull(buffer, &endptr, 10);
	if (errno != 0 || endptr == buffer || *endptr != '\0') {
		return -1;
	}
	*value = (uint64_t)parsed;
	return 0;
}

static int resolve_block_names(
	const char *partition_device,
	char *partition_name,
	size_t partition_name_size,
	char *disk_name,
	size_t disk_name_size,
	unsigned *partition_number,
	uint64_t *disk_sectors
)
{
	const char *basename_part;
	char sysfs_partition[PATH_MAX];
	char parent_path[PATH_MAX];
	char partition_number_path[PATH_MAX];
	char disk_size_path[PATH_MAX];
	char buffer[64];
	char *resolved_parent;
	char *last_slash;

	basename_part = strrchr(partition_device, '/');
	basename_part = basename_part == NULL ? partition_device : basename_part + 1;

	if (snprintf(partition_name, partition_name_size, "%s", basename_part) >= (int)partition_name_size) {
		return -1;
	}
	if (snprintf(sysfs_partition, sizeof(sysfs_partition), "/sys/class/block/%s", partition_name) >= (int)sizeof(sysfs_partition)) {
		return -1;
	}
	if (snprintf(partition_number_path, sizeof(partition_number_path), "%s/partition", sysfs_partition) >= (int)sizeof(partition_number_path)) {
		return -1;
	}
	if (read_line(partition_number_path, buffer, sizeof(buffer)) != 0) {
		return -1;
	}
	errno = 0;
	*partition_number = (unsigned)strtoul(buffer, &last_slash, 10);
	if (errno != 0 || last_slash == buffer || *last_slash != '\0') {
		return -1;
	}

	if (snprintf(parent_path, sizeof(parent_path), "%s/..", sysfs_partition) >= (int)sizeof(parent_path)) {
		return -1;
	}
	resolved_parent = realpath(parent_path, NULL);
	if (resolved_parent == NULL) {
		return -1;
	}

	last_slash = strrchr(resolved_parent, '/');
	if (last_slash == NULL || *(last_slash + 1) == '\0') {
		free(resolved_parent);
		return -1;
	}
	if (snprintf(disk_name, disk_name_size, "%s", last_slash + 1) >= (int)disk_name_size) {
		free(resolved_parent);
		return -1;
	}
	free(resolved_parent);

	if (snprintf(disk_size_path, sizeof(disk_size_path), "/sys/class/block/%s/size", disk_name) >= (int)sizeof(disk_size_path)) {
		return -1;
	}
	if (read_uint64_file(disk_size_path, disk_sectors) != 0) {
		return -1;
	}

	return 0;
}

static int read_mbr(int disk_fd, uint8_t mbr[MBR_SIZE])
{
	ssize_t bytes_read;

	bytes_read = pread(disk_fd, mbr, MBR_SIZE, 0);
	return bytes_read == MBR_SIZE ? 0 : -1;
}

static int read_gpt_signature(int disk_fd)
{
	uint8_t signature[8];
	ssize_t bytes_read;

	bytes_read = pread(disk_fd, signature, sizeof(signature), 512);
	if (bytes_read != (ssize_t)sizeof(signature)) {
		return 0;
	}
	return memcmp(signature, "EFI PART", sizeof(signature)) == 0;
}

static void parse_partition(const uint8_t *entry_bytes, struct partition_entry *entry)
{
	entry->bootable = entry_bytes[0];
	memcpy(entry->start_chs, entry_bytes + 1, sizeof(entry->start_chs));
	entry->type = entry_bytes[4];
	memcpy(entry->end_chs, entry_bytes + 5, sizeof(entry->end_chs));
	entry->start_lba = load_le32(entry_bytes + 8);
	entry->sector_count = load_le32(entry_bytes + 12);
}

static int is_extended_partition(uint8_t type)
{
	return type == 0x05 || type == 0x0f || type == 0x85;
}

int main(int argc, char **argv)
{
	char partition_name[NAME_MAX];
	char disk_name[NAME_MAX];
	char disk_path[PATH_MAX];
	uint8_t mbr[MBR_SIZE];
	struct partition_entry entries[MAX_PRIMARY_PARTITIONS];
	uint64_t disk_sectors;
	uint64_t current_end;
	uint64_t new_sector_count;
	unsigned partition_number;
	unsigned index;
	int disk_fd;
	int ioctl_rc;

	if (argc != 2) {
		fprintf(stderr, "usage: %s <root-partition-device>\n", argv[0]);
		return RC_ERROR;
	}

	if (resolve_block_names(argv[1], partition_name, sizeof(partition_name), disk_name, sizeof(disk_name), &partition_number, &disk_sectors) != 0) {
		fprintf(stderr, "failed to resolve block metadata for %s\n", argv[1]);
		return RC_ERROR;
	}
	if (partition_number == 0 || partition_number > MAX_PRIMARY_PARTITIONS) {
		return RC_UNSUPPORTED;
	}
	if (disk_sectors == 0 || disk_sectors > UINT32_MAX) {
		return RC_UNSUPPORTED;
	}
	if (snprintf(disk_path, sizeof(disk_path), "/dev/%s", disk_name) >= (int)sizeof(disk_path)) {
		return RC_ERROR;
	}

	disk_fd = open(disk_path, O_RDWR | O_CLOEXEC);
	if (disk_fd < 0) {
		perror("open disk");
		return RC_ERROR;
	}

	if (read_mbr(disk_fd, mbr) != 0) {
		perror("read mbr");
		close(disk_fd);
		return RC_ERROR;
	}
	if (mbr[MBR_SIGNATURE_OFFSET] != 0x55 || mbr[MBR_SIGNATURE_OFFSET + 1] != 0xaa) {
		close(disk_fd);
		return RC_UNSUPPORTED;
	}
	if (read_gpt_signature(disk_fd)) {
		close(disk_fd);
		return RC_UNSUPPORTED;
	}

	for (index = 0; index < MAX_PRIMARY_PARTITIONS; ++index) {
		parse_partition(mbr + PARTITION_TABLE_OFFSET + (index * PARTITION_ENTRY_SIZE), &entries[index]);
	}

	index = partition_number - 1;
	if (entries[index].type == 0 || entries[index].sector_count == 0 || is_extended_partition(entries[index].type)) {
		close(disk_fd);
		return RC_UNSUPPORTED;
	}

	current_end = (uint64_t)entries[index].start_lba + (uint64_t)entries[index].sector_count;
	if (current_end > disk_sectors) {
		close(disk_fd);
		return RC_UNSUPPORTED;
	}

	for (unsigned other = 0; other < MAX_PRIMARY_PARTITIONS; ++other) {
		uint64_t other_end;

		if (other == index || entries[other].type == 0 || entries[other].sector_count == 0) {
			continue;
		}
		other_end = (uint64_t)entries[other].start_lba + (uint64_t)entries[other].sector_count;
		if (entries[other].start_lba > entries[index].start_lba || other_end > current_end) {
			close(disk_fd);
			return RC_UNSUPPORTED;
		}
	}

	new_sector_count = disk_sectors - (uint64_t)entries[index].start_lba;
	if (new_sector_count <= (uint64_t)entries[index].sector_count) {
		close(disk_fd);
		return RC_NO_CHANGE;
	}
	if (new_sector_count > UINT32_MAX) {
		close(disk_fd);
		return RC_UNSUPPORTED;
	}

	mbr[PARTITION_TABLE_OFFSET + (index * PARTITION_ENTRY_SIZE) + 5] = 0xfe;
	mbr[PARTITION_TABLE_OFFSET + (index * PARTITION_ENTRY_SIZE) + 6] = 0xff;
	mbr[PARTITION_TABLE_OFFSET + (index * PARTITION_ENTRY_SIZE) + 7] = 0xff;
	store_le32(mbr + PARTITION_TABLE_OFFSET + (index * PARTITION_ENTRY_SIZE) + 12, (uint32_t)new_sector_count);

	if (pwrite(disk_fd, mbr, MBR_SIZE, 0) != MBR_SIZE) {
		perror("write mbr");
		close(disk_fd);
		return RC_ERROR;
	}
	if (fsync(disk_fd) != 0) {
		perror("fsync");
		close(disk_fd);
		return RC_ERROR;
	}

	ioctl_rc = ioctl(disk_fd, BLKRRPART);
	if (ioctl_rc != 0) {
		perror("BLKRRPART");
		close(disk_fd);
		return RC_REREAD_NEEDED;
	}

	close(disk_fd);
	return RC_CHANGED;
}
