#define FUSE_USE_VERSION 30
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <string.h>
#include <sys/stat.h>
#include <errno.h>
#include <fuse.h>
#include "wfs.h"

void *disk;
struct wfs_log_entry *log_entry;
struct wfs_sb *sb;
unsigned long parent_inode_number(const char *path);

static int my_getattr(const char *path, struct stat *stbuf)
{
    memset(stbuf, 0, sizeof(struct stat));

    struct wfs_log_entry *logptr = path_to_logentry(path);
    if (logptr == NULL)
    {
        return -ENOENT; // No such file or directory
    }

    stbuf->st_mode = logptr->inode.mode;
    stbuf->st_nlink = logptr->inode.links;
    stbuf->st_size = logptr->inode.size;
    stbuf->st_uid = logptr->inode.uid;
    stbuf->st_gid = logptr->inode.gid;
    stbuf->st_mtime = logptr->inode.mtime; 

    return 0; // Success
}
struct wfs_log_entry *get_parent_dir(const char *path)
{
    if (path == NULL){
        return NULL;
    }

    if (strcmp(path, "/") == 0 || strcmp(path, "") == 0){
        return get_log_entry(0); 
    }	
	const char *occur = strrchr(path, '/');
    if (occur == NULL){
        return NULL; // if no '/'
    }
    size_t length = occur - path;//get length
    if (length == 0){
        return get_log_entry(0);
    }
    char *parent_path = malloc(length + 1);
    if (parent_path == NULL){
        return NULL; 
    }

    strncpy(parent_path, path, length);
    parent_path[length] = '\0'; 
    struct wfs_log_entry *logptr = path_to_logentry(parent_path);
    free(parent_path);

    return logptr;
}

unsigned int vaild_inode()
{
    char *ptr = NULL;
    ptr = (char *)((char *)disk + sizeof(struct wfs_sb));
    struct wfs_log_entry *lep = (struct wfs_log_entry *)ptr;
    unsigned int vaildnumber = 0;
    
    for (; ptr < (char *)disk + sb->head; ptr += (sizeof(struct wfs_inode) + lep->inode.size))
    {
        lep = (struct wfs_log_entry *)ptr;
        if (lep->inode.inode_number > vaildnumber && lep->inode.deleted == 0){
            vaildnumber = lep->inode.inode_number;
        }
    }
    return vaildnumber;
}

struct wfs_log_entry *get_log_entry(unsigned int inode_number)
{
    
    char *ptr = NULL;
    ptr = (char *)((char *)disk + sizeof(struct wfs_sb));
    struct wfs_log_entry *lep = (struct wfs_log_entry *)ptr;
    struct wfs_log_entry *final = NULL;
    
    for (; ptr < (char *)disk + sb->head; ptr += (sizeof(struct wfs_inode) + lep->inode.size))
    {
        lep = (struct wfs_log_entry *)ptr;
        if (lep->inode.inode_number == inode_number && lep->inode.deleted == 0)
        {
            final = lep;
        }
    }
    return final;
}

struct wfs_log_entry *path_to_logentry(const char *path)
{

    char *path_copy = strdup(path);
    if (path_copy == NULL){
        perror("Failed to duplicate path string");
        return NULL;
    }

    unsigned long inodenum = 0;
    struct wfs_log_entry *logptr = NULL;

    logptr = get_log_entry(inodenum);
    puts(path);

    char *token = strtok(path_copy, "/");
    while (token != NULL)
    {
        if (logptr == NULL)
        {
            return NULL;
        }
        inodenum = get_inode_number(token, logptr);
        if (inodenum == -1)
        {
            return NULL;
        }

        logptr = get_log_entry(inodenum);

        token = strtok(NULL, "/");
    }
    free(path_copy);
    
	return logptr;
}

struct wfs_inode *get_inode(unsigned inode_number)
{

    struct wfs_log_entry *entry = &log_entry[inode_number];

    if (entry->inode.deleted)
    {
        return NULL; 
    }

    return &entry->inode;
}

void update_dir(struct wfs_inode *dir_inode, const char *entry_name, unsigned long inode_number)
{

    if (dir_inode == NULL || (dir_inode->mode & S_IFMT) != S_IFDIR)
    {
        return; 
    }

    struct wfs_dentry *dentry = (struct wfs_dentry *)(log_entry[dir_inode->inode_number].data + dir_inode->size);
    strncpy(dentry->name, entry_name, MAX_FILE_NAME_LEN - 1);
    dentry->name[MAX_FILE_NAME_LEN - 1] = '\0'; // Ensure null termination
    dentry->inode_number = inode_number;

    // Update the size of the directory inode to reflect the new entry
    dir_inode->size += sizeof(struct wfs_dentry);
    // Update timestamps
    dir_inode->mtime = dir_inode->ctime = time(NULL);
}

unsigned long get_inode_number(char *name, struct wfs_log_entry *l)
{
    char *end_ptr = (char *)l + (sizeof(struct wfs_inode) + l->inode.size);
    char *cur = l->data; // begin at dentry
    struct wfs_dentry *d_ptr = (struct wfs_dentry *)cur;
    for (; cur < end_ptr; cur += sizeof(struct wfs_dentry))
    {
        d_ptr = (struct wfs_dentry *)cur;
        if (strcmp(name, d_ptr->name) == 0)
        {
            return d_ptr->inode_number;
        }
    }
    // not found
    return -1;
}



static int my_read(const char *path, char *buf, size_t size, off_t offset, struct fuse_file_info *fi)
{
    int len;
    int ret = 0;
    struct wfs_log_entry *file_entry = path_to_logentry(path);

    len = file_entry->inode.size;

    if (file_entry == NULL)
    {
        // File not found or error
        return -ENOENT;
    }

    if (offset < len)
    {
        if (offset + size > len)
        {
            size = len - offset;
        }
        memcpy(buf, file_entry->data + offset, size);
        ret = size;
    }
    else
    {
        ret = 0; // Offset is past the end of the file, overflow
    }

    return ret;
}
static int my_readdir(const char *path, void *buf, fuse_fill_dir_t filler, off_t offset, struct fuse_file_info *fi)
{
    struct wfs_log_entry *l = path_to_logentry(path);
    struct wfs_log_entry *parent = get_parent_dir(path);

    if (l == NULL || parent == NULL)
    {
        return -ENOENT;
    }

    struct stat st;

    // Currentand parent directory
    memset(&st, 0, sizeof(st));
    st.st_ino = l->inode.inode_number;
    st.st_mode = l->inode.mode;

    if (filler(buf, ".", &st, 0))
        return 0;

    memset(&st, 0, sizeof(st));
    st.st_ino = parent->inode.inode_number;
    st.st_mode = parent->inode.mode;
    if (filler(buf, "..", &st, 0))
        return 0;

    char *end_ptr = (char *)l + (sizeof(struct wfs_inode) + l->inode.size);
    for (char *cur = l->data; cur < end_ptr; cur += sizeof(struct wfs_dentry))
    {
        struct wfs_dentry *d_ptr = (struct wfs_dentry *)cur;

        if (get_log_entry(d_ptr->inode_number)->inode.deleted == 0)
        {
            memset(&st, 0, sizeof(st));
            st.st_ino = d_ptr->inode_number;
            st.st_mode = get_log_entry(d_ptr->inode_number)->inode.mode;

            if (filler(buf, d_ptr->name, &st, 0))
                break;
        }
    }

    return 0;
}

int my_mknod(const char *path, mode_t mode, dev_t dev)
{
    if (path == NULL)
    {
        return -EINVAL;
    }

    const char *signal = strrchr(path, '/');
    const char *file_name;
    if (signal != NULL){
        file_name = signal + 1;
    }
    else{
        file_name = path;
    }

    struct wfs_log_entry *parent_log = get_parent_dir(path);
    unsigned int new_inodenum = vaild_inode() + 1;

    if (get_inode_number((char *)file_name, parent_log) != -1){
        return -EEXIST;
    }
    struct wfs_log_entry *new_parent_log = (struct wfs_log_entry *)((char *)disk + sb->head);

    memcpy((char *)new_parent_log, (char *)parent_log, sizeof(struct wfs_log_entry) + parent_log->inode.size);

    struct wfs_dentry *new_dentry = (void *)((char *)new_parent_log->data + new_parent_log->inode.size);
    strcpy(new_dentry->name, file_name);
    new_dentry->inode_number = new_inodenum;

    new_parent_log->inode.size = new_parent_log->inode.size + sizeof(struct wfs_dentry);
    sb->head = sb->head + (uint32_t)(sizeof(struct wfs_inode) + new_parent_log->inode.size);
    struct wfs_log_entry *new_file_entry = (struct wfs_log_entry *)((char *)disk + sb->head);
    new_file_entry->inode.inode_number = new_inodenum;
    new_file_entry->inode.atime = time(NULL);
    new_file_entry->inode.mtime = time(NULL);
    new_file_entry->inode.ctime = time(NULL);
    new_file_entry->inode.deleted = 0;
    new_file_entry->inode.flags = 0;
    new_file_entry->inode.gid = getuid();
    new_file_entry->inode.uid = getuid();
    new_file_entry->inode.links = 1;
    new_file_entry->inode.mode = mode | S_IFREG;
    new_file_entry->inode.size = 0;

    sb->head = sb->head + (uint32_t)(sizeof(struct wfs_inode));

    return 0;
}
int my_mkdir(const char *path, mode_t mode)
{
    if (path == NULL)
    {
        return -EINVAL;
    }

    const char *signal = strrchr(path, '/');
    const char *file_name;
    if (signal != NULL)
    {
        file_name = signal + 1;
    }
    else
    {
        file_name = path;
    }

    struct wfs_log_entry *parent_log = get_parent_dir(path);
    unsigned int new_inodenum = vaild_inode() + 1;

    if (get_inode_number((char *)file_name, parent_log) != -1)
    {
        return -EEXIST;
    }

    struct wfs_log_entry *new_parent_log = (struct wfs_log_entry *)((char *)disk + sb->head);
    memcpy((char *)new_parent_log, (char *)parent_log, sizeof(struct wfs_log_entry) + parent_log->inode.size);

    struct wfs_dentry *new_dentry = (void *)((char *)new_parent_log->data + new_parent_log->inode.size);
    strcpy(new_dentry->name, file_name);
    new_dentry->inode_number = new_inodenum;

    new_parent_log->inode.size = new_parent_log->inode.size + sizeof(struct wfs_dentry);
    sb->head = sb->head + (uint32_t)(sizeof(struct wfs_inode) + new_parent_log->inode.size);
	struct wfs_log_entry *new_file_entry = (struct wfs_log_entry *)((char *)disk + sb->head);
    new_file_entry->inode.inode_number = new_inodenum;
    new_file_entry->inode.atime = time(NULL);
    new_file_entry->inode.mtime = time(NULL);
    new_file_entry->inode.ctime = time(NULL);
    new_file_entry->inode.deleted = 0;
    new_file_entry->inode.flags = 0;
    new_file_entry->inode.gid = getuid();
    new_file_entry->inode.uid = getuid();
    new_file_entry->inode.links = 1;
    new_file_entry->inode.mode = S_IFDIR | mode;
    new_file_entry->inode.size = 0;

    sb->head = sb->head + (uint32_t)(sizeof(struct wfs_inode));
    return 0;
}

static int my_write(const char *path, const char *buf, size_t size, off_t offset, struct fuse_file_info *fi)
{
	 if (offset < 0)
    {
        return 0; 
    }
	struct wfs_log_entry *currentLogEntry = path_to_logentry(path);
    if (currentLogEntry == NULL)
    {
        return -ENOENT;
    }

	 int updatedSize = currentLogEntry->inode.size;
	 if (offset + size > currentLogEntry->inode.size)
    {
        updatedSize = offset + size;
    }

	// Create new logentry position
    struct wfs_log_entry *newLogEntry = (struct wfs_log_entry *)((char *)disk + sb->head);
    memcpy((char *)newLogEntry, (char *)currentLogEntry, sizeof(struct wfs_inode) + currentLogEntry->inode.size);

    newLogEntry->inode.size = updatedSize;
    memcpy((char *)newLogEntry->data + offset, buf, size);

    newLogEntry->inode.mtime = time(NULL);

    // Move the head of the sb to the end of the new log entry
    sb->head += (uint32_t)(sizeof(struct wfs_inode)) + newLogEntry->inode.size;

    return size; 
}

int my_unlink(const char *path)
{
    struct wfs_log_entry *file_entry = path_to_logentry(path);
    if (file_entry == NULL)
    {
        return -ENOENT; // File not found
    }

    if ((file_entry->inode.mode & S_IFMT) == S_IFDIR)
    {
        return -EISDIR; // Can't unlink a directory
    }

    // Mark the inode as deleted
    file_entry->inode.deleted = 1;
    return 0;
}

static struct fuse_operations my_operations = {
    .getattr = my_getattr,
    .mknod = my_mknod,
    .mkdir = my_mkdir,
    .read = my_read,
    .write = my_write,
    .readdir = my_readdir,
    .unlink = my_unlink,
};

int main(int argc, char *argv[])
{

    // Filter argc and argv here and then pass it to fuse_main
    if (argc < 3)
    {
        printf("Usage: mount.wfs [FUSE options] disk_path mount_point\n");
        return 1;
    }
    // should be like ./mount.wfs -f -s disk mnt or without -f
    int i;
    char *disk_arg = NULL;
    // Filter argc and argv here and then pass it to fuse_main

    // Iterate over arguments
    for (i = 1; i < argc; i++)
    {
        if (!(strcmp(argv[i], "-f") == 0 || strcmp(argv[i], "-s") == 0))
        {
            disk_arg = argv[i]; // Save the disk argument
            // Remove 'disk' from argv
            memmove(&argv[i], &argv[i + 1], (argc - i - 1) * sizeof(char *));
            argc--;
            i--;
            break;
        }
    }

    // open disk
    int fd = open(disk_arg, O_RDWR | O_CREAT, 0666);
    if (fd < 0)
    {
        perror("Failed to open disk image");
        exit(1);
    }

    // get the file size
    struct stat s;
    if (fstat(fd, &s) == -1)
    {
        perror("Failed to get file size");
        close(fd);
        exit(1);
    }

    // mmap disk
    disk = mmap(NULL, s.st_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (disk == MAP_FAILED)
    {
        perror("Failed to map file");
        close(fd);
        exit(1);
    }

    sb = (struct wfs_sb *)disk;
    int fuse_return_value = fuse_main(argc, argv, &my_operations, NULL);

    if (munmap(disk, s.st_size) == -1)
    {
        perror("Failed to unmap memory");
    }

    close(fd);

    return fuse_return_value;
}

