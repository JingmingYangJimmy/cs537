#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <string.h>
#include <sys/stat.h>
#include "wfs.h"
#include <time.h> 

void initialize_filesystem(const char *disk_path)
{
    int fd;
    struct wfs_sb *sb;
    struct wfs_log_entry *log_entry;

    size_t disk_size = 1048576; // 1MB disk size

    // Open the disk file
    fd = open(disk_path, O_RDWR);
    if (fd == -1)
    {
        perror("Error opening disk file");
        exit(EXIT_FAILURE);
    }

    // Map the disk file to memory
    void *disk_map = mmap(NULL, disk_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (disk_map == MAP_FAILED)
    {
        perror("Error mapping disk file");
        close(fd);
        exit(EXIT_FAILURE);
    }
    sb = (struct wfs_sb *)disk_map;
    sb->magic = WFS_MAGIC;
    sb->head = sizeof(struct wfs_sb);

    log_entry = (struct wfs_log_entry *)((char *)disk_map + sb->head);
    log_entry->inode.inode_number = 0;                                                     // Root inode number
    log_entry->inode.deleted = 0;                                                          // Not deleted
    log_entry->inode.mode = S_IFDIR;                                                       // Directory mode
    log_entry->inode.uid = 0;                                                              // Root user
    log_entry->inode.gid = 0;                                                              // Root group
    log_entry->inode.flags = 0;                                                            // Default flags
    log_entry->inode.size = 0;                                                             // Initial size (empty directory)
    log_entry->inode.atime = log_entry->inode.mtime = log_entry->inode.ctime = time(NULL); // Set current time
    log_entry->inode.links = 1;                                                            // At least one link

    sb->head += sizeof(struct wfs_inode) + sizeof(struct wfs_log_entry);
    if (msync(disk_map, disk_size, MS_SYNC) == -1)
    {
        perror("Error syncing memory to disk");
    }

    munmap(disk_map, disk_size);
    close(fd);
}

int main(int argc, char *argv[])
{
    if (argc < 2)
    {
        fprintf(stderr, "not correct number of arguments");
        exit(EXIT_FAILURE);
    }

    initialize_filesystem(argv[1]);

    return 0;
}

