#ifndef OSPFSFIX_H
#define OSPFSFIX_H

#include <errno.h>
#include <stdint.h>
#include "ospfs.h"

#define MAX_FILENAME_LEN 1024

//Define Error Messages
#define ERROR(msg) printf("%s in %s, Line:%d\n", msg, __FILE__, __LINE__)
#define UNFIXABLE(msg) printf("Error Unfixable: %s\n", msg)
//Define success message
#define FIXED(msg) printf("Fixed Error: %s\n", msg)
//Define progress check messages
#define CHECK(msg) printf("TESTING %s\n", msg)
#define CORRECT(msg) printf("PASSED %s CHECKS\n", msg)

//Return values for the function
#define FS_OK 0
#define FS_FIXED 1
#define FS_BROKEN 2

// holds info about file system, data read from input, and ospfs structures
typedef struct file_system{
	char filename[MAX_FILENAME_LEN + 1];
	void *buffer;
	uint32_t buffer_size;

	ospfs_super_t super;
	ospfs_inode_t *inodes;

	uint32_t num_bitmap_blocks;
	void *bitmap;
} file_system_t;

// global structure that holds data for file system
extern file_system_t fs;

//runs primary functions to try and fix file system
int fix_file_system();

// Function used to analyze filesystem image
static int checks_superblock();
static int checks_inodes();
static int checks_referenced_blocks();
static int checks_directories();
static int checks_bitmap();

// helper functions
static int bitmap_get(uint32_t block_num);
static void bitmap_set(uint32_t block_num, int value);
static int check_inode(uint32_t ino);
static int check_direct_refs(ospfs_inode_t *inode);
static int check_indirect_refs(ospfs_inode_t *inode);
static int check_twice_indirect_refs(ospfs_inode_t *inode);
static int truncates_inode(ospfs_inode_t *inode, int n);

void *block_pointer(uint32_t block_num);
void *block_offset(uint32_t block_num, uint32_t offset);

static inline uint32_t ospfs_inode_blockno(ospfs_inode_t *oi, uint32_t offset);
static inline void *ospfs_inode_data(ospfs_inode_t *oi, uint32_t offset);

static inline void bitvector_set(void *vector, int i);
static inline void bitvector_clear(void *vector, int i);
static inline int bitvector_test(const void *vector, int i);

#endif
