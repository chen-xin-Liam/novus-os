#include <stdio.h>
#include <stddef.h>

typedef unsigned char  UINT8;
typedef unsigned short UINT16;
typedef unsigned int   UINT32;
typedef unsigned long long UINT64;

#define NOVUSFS_PACKED __attribute__((packed))
#define NOVUSFS_BLOCK_SIZE  4096u
#define NOVUSFS_NDIR          7u
#define NOVUSFS_INODE_SIZE       128u

typedef struct NOVUSFS_PACKED novusfs_inode {
  UINT16 mode;
  UINT16 pad0;
  UINT32 ino;
  UINT32 size;
  UINT32 nlinks;
  UINT32 blocks;
  UINT64 mtime;
  UINT64 dblock[NOVUSFS_NDIR];
  UINT64 iblk;
  UINT64 diblk;
} novusfs_inode_t;

int main(void) {
  printf("sizeof = %zu\n", sizeof(novusfs_inode_t));
  printf("mode    %zu\n", offsetof(novusfs_inode_t, mode));
  printf("ino     %zu\n", offsetof(novusfs_inode_t, ino));
  printf("size    %zu\n", offsetof(novusfs_inode_t, size));
  printf("blocks  %zu\n", offsetof(novusfs_inode_t, blocks));
  printf("mtime   %zu\n", offsetof(novusfs_inode_t, mtime));
  printf("dblock0 %zu\n", offsetof(novusfs_inode_t, dblock[0]));
  printf("iblk    %zu\n", offsetof(novusfs_inode_t, iblk));
  printf("diblk   %zu\n", offsetof(novusfs_inode_t, diblk));
  novusfs_inode_t in;
  in.dblock[0] = 164;
  in.mtime = 163;
  unsigned char *p = (unsigned char*)&in;
  printf("after set: byte32=%02x byte33=%02x byte34=%02x byte35=%02x\n",
         p[32],p[33],p[34],p[35]);
  printf("byte28=%02x byte29=%02x byte30=%02x byte31=%02x\n",
         p[28],p[29],p[30],p[31]);
  return 0;
}
