#define MAX_MMAPS 32

/* Define mmap flags */
#define MAP_PRIVATE 0x0001
#define MAP_SHARED 0x0002
#define MAP_ANONYMOUS 0x0004
#define MAP_ANON MAP_ANONYMOUS
#define MAP_FIXED 0x0008
#define MAP_GROWSUP 0x0010
//#define mmap_START 0x60000000
//#define mmap_END 0x80000000
/* Protections on memory mapping */
#define PROT_READ 0x1
#define PROT_WRITE 0x2
void *mmap(void *addr, int length, int prot, int flags, int fd, int offset);
int munmap(void *addr, int length);
