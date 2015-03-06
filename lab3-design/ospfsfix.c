/***********************************************************************
** OSPFSFIX.C											
**
** Kalin Khemka and Vir Thakor
**
** Runs analysis on a file system image to check for violations of the
** four invariants and runs sanity checks on all disk structures.
***********************************************************************/

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "ospfsfix.h"

// Data for our file system
file_system_t fs;

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

static inline uint32_t ospfs_inode_blockno(ospfs_inode_t *oi, uint32_t offset);
static inline void *ospfs_inode_data(ospfs_inode_t *oi, uint32_t offset);
static inline int bitvector_test(const void *vector, int i);

//This function analyzes the file system
int fix_file_system() {
	int retval;
	int temp;
	
	// Superblock check
	retval = temp = checks_superblock();
	if (temp == FS_BROKEN)
		return temp;
	else if (temp == FS_FIXED)
		retval = FS_FIXED;
	
	// Inode check
	temp = checks_inodes();
	if (temp == FS_BROKEN)
		return temp;
	else if (temp == FS_FIXED)
		retval = FS_FIXED;
	
	// Referenced blocks check
	temp = checks_referenced_blocks();
	if (temp == FS_BROKEN)
		return temp;
	else if (temp == FS_FIXED)
		retval = FS_FIXED;
	
	// Directories check
	temp = checks_directories();
	if (temp == FS_BROKEN)
		return temp;
	else if (temp == FS_FIXED)
		retval = FS_FIXED;
	
	// Bitmap check
	temp = checks_bitmap();
	if (temp == FS_BROKEN)
		return temp;
	else if (temp == FS_FIXED)
		retval = FS_FIXED;
	
	return retval;
}

//Checks the superblock of the file system (Invariant 1, kind of)
static int checks_superblock() {

	CHECK("SUPERBLOCK");

	int retval = FS_OK;

	//Check size of FS, if space for superblock
	if (fs.buffer_size < (2 * OSPFS_BLKSIZE)) {
		UNFIXABLE("Not enough space for superblock (Sanity Check)");
		return FS_BROKEN;
	}

	ospfs_super_t *super = (ospfs_super_t *) block_pointer(1);

	//Check the magic number
	fs.super.os_magic = OSPFS_MAGIC;
	if (super->os_magic != OSPFS_MAGIC) {
		retval = FS_FIXED;
		FIXED("Incorrect magic number (Sanity Check)");
	}

	//Check number of blocks in image
	fs.super.os_nblocks = fs.buffer_size / OSPFS_BLKSIZE;
	if (fs.buffer_size != super->os_nblocks * OSPFS_BLKSIZE) {
		retval = FS_FIXED;
		FIXED("Incorrect number of blocks (Sanity Check)");
	}

	//Check number of bitmap blocks and first inode block
	uint32_t num_bitmap_blocks;
	uint32_t num_bitmap_bytes = fs.super.os_nblocks / 8;
	uint32_t block_mod = num_bitmap_bytes % OSPFS_BLKSIZE;
	if (block_mod != 0) {
		num_bitmap_bytes -= block_mod;
		num_bitmap_bytes += OSPFS_BLKSIZE;
	}
	num_bitmap_blocks = num_bitmap_bytes / OSPFS_BLKSIZE;
	fs.num_bitmap_blocks = num_bitmap_bytes;
	fs.super.os_firstinob = OSPFS_FREEMAP_BLK + num_bitmap_blocks;
	if (fs.super.os_firstinob != super->os_firstinob) {
		retval = FS_FIXED;
		FIXED("First inode is incorrect (Sanity Check)");
	}

	//Number of inodes
	uint32_t num_inodes = OSPFS_BLKINODES * (fs.super.os_nblocks - fs.super.os_firstinob - 1);
	if (num_inodes < super->os_ninodes) {
		UNFIXABLE("Filesystem contains too many inodes (Sanity Check)");
		return FS_BROKEN;
	} else
		fs.super.os_ninodes = super->os_ninodes;

	CORRECT("SUPERBLOCK");

	printf("-Superblock magic number: %x\n", fs.super.os_magic);
	printf("-Superblock number of blocks:: %d\n", fs.super.os_nblocks);
	printf("-Superblock number of inodes:: %d\n", fs.super.os_ninodes);
	printf("-Superblock first inode location: %d\n", fs.super.os_firstinob);

	return retval;
}

//Checks all the inodes for errors (Invariant 1)
static int checks_inodes() {
	int retval = FS_OK;
	uint32_t ino_no;

	CHECK("INODES");

	//Creates a new set of inodes to put the correct data
	fs.inodes = malloc(sizeof(ospfs_inode_t) * fs.super.os_ninodes);
	memset(fs.inodes, 0, sizeof(ospfs_inode_t) * fs.super.os_ninodes);

	//Goes through every inode, checking for all references.
	//Also fills in the set of inodes that are being used to check directories later
	int check;
	for (ino_no = 1; ino_no < fs.super.os_ninodes; ino_no++) {
		check = check_inode(ino_no);
		if (check == FS_BROKEN)
			return FS_BROKEN;
		else if (check == FS_FIXED)
			retval = FS_FIXED;
	}
	CORRECT("INODES");
	return retval;
}

//Checks the inodes and the referenced blocks (Invariants 1, 2, 3, 4)
static int checks_referenced_blocks() {
	int retval = FS_OK;
	int blocks_used;
	int inode_ref;
	int i;
	uint32_t ino;
	int bitmap_block_size = fs.super.os_nblocks / OSPFS_BLKBITSIZE;
	int inode_blocks = fs.super.os_ninodes / OSPFS_BLKINODES;

	CHECK("REFERENCED BLOCKS (Invariants 2/3/4)");
	
	//Creating a new bitmap to store correct data
	if ((fs.super.os_nblocks % OSPFS_BLKBITSIZE) > 0)
		bitmap_block_size++;
	fs.bitmap = malloc(bitmap_block_size * OSPFS_BLKSIZE);
	
	//Sets the new bitmap with data from the filesystem image
	if ((fs.super.os_ninodes % OSPFS_BLKINODES) > 0)
		inode_blocks++;
	blocks_used = fs.super.os_firstinob + inode_blocks;
	
	//Sets all the used inodes (Invariant 3)
	for (i = 0; i < blocks_used; i++)
		bitmap_set(i, 0);

	//All other blocks are set to free (Invariant 4)
	for (; i < fs.super.os_nblocks; i++)
		bitmap_set(i, 1); 
	
	//Checks all the referenced blocks for inconsistencies
	for (ino = 1; ino < fs.super.os_ninodes; ino++)
	{
		ospfs_inode_t *inode = &fs.inodes[ino];

		//Skip nodes that are empty
		if (inode->oi_nlink == 0)
			continue;
			
		//Skip nodes that aren't regular files or directories (AKA symlinks)
		if (inode->oi_ftype != OSPFS_FTYPE_REG && inode->oi_ftype != OSPFS_FTYPE_DIR)
			continue;
		
		//Checks for duplicate references throughout direct, indirect, and indirect2 blocks(Invariant 1)
		inode_ref = check_direct_refs(inode);
		if (inode_ref == FS_BROKEN)
			return FS_BROKEN;
		else if (inode_ref == FS_FIXED)
			retval = FS_FIXED;
		
		inode_ref = check_indirect_refs(inode);
		if (inode_ref == FS_BROKEN)
			return FS_BROKEN;
		else if (inode_ref == FS_FIXED)
			retval = FS_FIXED;

		inode_ref = check_twice_indirect_refs(inode);
		if (inode_ref == FS_BROKEN)
			return FS_BROKEN;
		else if (inode_ref == FS_FIXED)
			retval = FS_FIXED;
	}

	CORRECT("REFERENCED BLOCKS");
	return retval;
}

//Checks directory inodes to ensure all inodes exist and there are no repeated filenames
static int checks_directories() {
	int retval = FS_OK;
	int i, direntry_num;
	ospfs_inode_t *curr_inode;
	ospfs_direntry_t *curr_direntry;

	CHECK("DIRECTORIES");

	//Reset the values of all oi_nlink variables in every inode so that we can set correct value later
	for (i = 0; i < fs.super.os_ninodes; i++)
		fs.inodes[i].oi_nlink = 0;

	//Checking every directory
	for (i = 0; i < fs.super.os_ninodes; i++) {
		curr_inode = &fs.inodes[i];
		
		//We only want directory inodes becase they contain all the information
		if (curr_inode->oi_ftype != OSPFS_FTYPE_DIR)
			continue;

		//Go through all the directory's entries
		for (direntry_num = 0; direntry_num < curr_inode->oi_size; direntry_num+= OSPFS_DIRENTRY_SIZE) {
			curr_direntry = ospfs_inode_data(curr_inode, direntry_num);

			//Empty directory, skip it
			if (curr_direntry->od_ino == 0)
				continue;

			//Check to make sure inode referenced exists
			if (fs.super.os_ninodes <= curr_direntry->od_ino) {
				retval = FS_FIXED;
				FIXED("Inode out of range (Invariant 2)");
				continue;
			}

			//Check for a valid directory name, with the null byte at end
			if (strchr(curr_direntry->od_name, '\0') == NULL) {
				retval = FS_FIXED;
				FIXED("Invalid directory name (Sanity Check)");
				continue;
			}

			printf("-Directory %d references inode %d -> %s\n", direntry_num
				, curr_direntry->od_ino, curr_direntry->od_name);

			//If the inode is proper, then update the link value of it
			fs.inodes[curr_direntry->od_ino].oi_nlink++;
		}
	}

	CORRECT("DIRECTORIES");
	return retval;
}

//Checks the bitmap to make sure it is correct (Invariants 3, 4)
static int checks_bitmap() {
	CHECK("FREE BLOCK BITMAP");

	uint32_t i;
	int old_used = 0;
	int old_free = 0;
	int new_used = 0;
	int new_free = 0;
	void *start_bitmap = block_pointer(OSPFS_FREEMAP_BLK);

	for (i = OSPFS_FREEMAP_BLK; i < fs.super.os_nblocks; i++){
		if (bitvector_test(start_bitmap, i))
			old_free++;
		else
			old_used++;
	}

	for (i = OSPFS_FREEMAP_BLK; i < fs.super.os_nblocks; i++){
		if (bitmap_get(i))
			new_free++;
		else
			new_used++;
	}

	if (new_free - old_free < 0){
		if (-(new_free - old_free) < 2)
			new_free = old_free;
	} else{
		if (-(old_free - new_free) < 2)
			old_free = new_free;
	}

	if (new_used - old_used < 0){
		if (-(new_used - old_used) < 2)
			new_used = old_used;
	} else{
		if (-(old_used - new_used) < 2)
			old_used = new_used;
	}

	if (new_free == old_free && new_used == old_used)
		return FS_OK;
	else{
		printf("-OLD USED:%d\n", old_used);
		printf("-NEW USED:%d\n", new_used);
		printf("-OLD FREE:%d\n", old_free);
		printf("-NEW FREE:%d\n", new_free);
		FIXED("Incorrect free block bitmap values (Invariant 3/4)");
		return FS_FIXED;
	}

	CORRECT("FREE BLOCK BITMAP");
}

//Checks the inode whose number was passed in to check for validity
static int check_inode(uint32_t ino) {
	int retval = FS_OK;
	int i;
	ospfs_symlink_inode_t *new_sym_inode, *fs_sym_inode;
	ospfs_inode_t *new_inode;
	ospfs_inode_t *fs_inode = block_offset(fs.super.os_firstinob, ino * OSPFS_INODESIZE);

	//Skip if the inode is empty
	if (fs_inode->oi_nlink == 0)
		return retval;

	//A new copy of the inode
	new_inode = &fs.inodes[ino];

	//Checks the type of inode
	switch(fs_inode->oi_ftype) {
		//Regular files and directories have the same checks
		case OSPFS_FTYPE_REG:
		case OSPFS_FTYPE_DIR:
			//Check the make sure the size is valid
			if (fs_inode->oi_size > OSPFS_MAXFILESIZE) {
				new_inode->oi_size = OSPFS_MAXFILESIZE;
				FIXED("Inode size too large (Sanity Check)");
				retval = FS_FIXED;
			} else
				new_inode->oi_size = fs_inode->oi_size;

			//Ports over the mode
			new_inode->oi_mode = fs_inode->oi_mode;

			//Sets all the pointers depending on the size of the inode
			if (new_inode->oi_size > ((OSPFS_NDIRECT + OSPFS_NINDIRECT) * OSPFS_BLKSIZE))
				new_inode->oi_indirect2 = fs_inode->oi_indirect2;

			if (new_inode->oi_size > (OSPFS_NDIRECT * OSPFS_BLKSIZE))
				new_inode->oi_indirect = fs_inode->oi_indirect;

			//For the direct block, we have to copy every part over
			for (i = 0; i < OSPFS_NDIRECT; i++)
				new_inode->oi_direct[i] = fs_inode->oi_direct[i];
			
			break;
		case OSPFS_FTYPE_SYMLINK:
			//Set the current symlink inode and a new copy of it
			fs_sym_inode = (ospfs_symlink_inode_t *) fs_inode;
			new_sym_inode = (ospfs_symlink_inode_t *) new_inode;

			//Set the size to be 0 and then put in the size and destination manually
			new_sym_inode->oi_size = 0;
			for (i = 0; i < OSPFS_MAXSYMLINKLEN; i++) {
				new_sym_inode->oi_symlink[i] = fs_sym_inode->oi_symlink[i];
				if (fs_sym_inode->oi_symlink[i] == '\0')
					break;
				else
					new_sym_inode->oi_size++;
			}
			//If the file name is too large, then it truncates it at the max
			if (new_sym_inode->oi_symlink[i] != '\0') {
				new_sym_inode->oi_symlink[i] = '\0';
				FIXED("Symbolic link value too long (Sanity Check)");
				retval = FS_FIXED;
			}
			break;
		default:
			FIXED("Incorrect inode type (Sanity Check)");
			return FS_FIXED;
	}

	//Sets the values of oi_ftype and oi_nlink to complete the new inode
	new_inode->oi_ftype = fs_inode->oi_ftype;
	new_inode->oi_nlink = fs_inode->oi_nlink;

	printf("-Inode %d is correct\n", ino);

	return retval;
}

//Returns the bit value from the bitmap location representing the block
static int bitmap_get(uint32_t block_num) {
	// Finds the byte to check and creates a mask
	uint8_t *byte = (uint8_t *) fs.bitmap + (block_num / 8);
	uint8_t mask = 0x01 << (7 - (block_num % 8));
	if (*byte & mask)
		return 1;
	else
		return 0;
}

//Sets the bit value in the bitmap location representing the block
static void bitmap_set(uint32_t block_num, int value) {
	// Finds the byte to modify and creates a mask
	uint8_t *byte = (uint8_t *) fs.bitmap + (block_num / 8);
	uint8_t mask = 0x01 << (7 - (block_num % 8));
	// Modifies the byte.
	if (value)
		*byte = *byte | mask;
	else
		*byte = *byte & (~mask);
}

//Checks the direct block references and puts them in a set of "proper" blocks
//If a block has already been referenced, it will truncate the file there so
//as to minimize any damage caused
int check_direct_refs(ospfs_inode_t * inode) {
	int i;
	for (i = 0; i < OSPFS_NDIRECT; i++)
	{
		uint32_t block_num = inode->oi_direct[i];
		if (block_num == 0)
			return truncates_inode(inode, i);

		if (bitmap_get(block_num) == 0)
		{
			printf("Direct block %d referenced multiple times\n", block_num);
			truncates_inode(inode, i);
			return FS_FIXED;
		}	

		bitmap_set(block_num, 0);
	}
	return FS_OK;
}

//Checks the indirect block references and puts them in a set of "proper" blocks
//If a block has already been referenced, it will truncate the file there so
//as to minimize any damage caused
int check_indirect_refs(ospfs_inode_t * inode) {
	int i;
	
	if (inode->oi_indirect == 0)
		return truncates_inode(inode, OSPFS_NDIRECT);

	if (bitmap_get(inode->oi_indirect) == 0)
	{
		FIXED("Indirect block referenced multiple times");
		truncates_inode(inode, OSPFS_NDIRECT);
		return FS_FIXED;
	}

	uint32_t *indirect_blocks = block_pointer(inode->oi_indirect);
	for (i = 0; i < OSPFS_NINDIRECT; i++)
	{
		if (indirect_blocks[i] == 0)
			return truncates_inode(inode, OSPFS_NDIRECT + i);

		if (bitmap_get(indirect_blocks[i]) == 0)
		{
			FIXED("Indirect direct block referenced multiple times");
			truncates_inode(inode, OSPFS_NDIRECT + i);
			return FS_FIXED;
		}

		bitmap_set(indirect_blocks[i], 0);
	}
	return FS_OK;
}

//Checks the doubly indirect block references and puts them in a set of "proper" blocks
//If a block has already been referenced, it will truncate the file there so
//as to minimize any damage caused
int check_twice_indirect_refs(ospfs_inode_t * inode) {
	int i, j, block_num;

	if (inode->oi_indirect2 == 0)
		return truncates_inode(inode, OSPFS_NDIRECT + OSPFS_NINDIRECT);

	if (bitmap_get(inode->oi_indirect2) == 0) {
		FIXED("Doubly indirect block referenced multiple times");
		truncates_inode(inode, OSPFS_NDIRECT + OSPFS_NINDIRECT);
		return FS_FIXED;
	}

	uint32_t *twice_indirect_block = block_pointer(inode->oi_indirect2);
	block_num = OSPFS_NDIRECT + OSPFS_NINDIRECT;
	for (i = 0; i < OSPFS_NINDIRECT; i++) {
		
		if (twice_indirect_block[i] == 0)
			return truncates_inode(inode, block_num);

		if (bitmap_get(twice_indirect_block[i] == 0)) {
			FIXED("Doubly indirect indirect block referenced multiple times");
			truncates_inode(inode, block_num);
			return FS_FIXED;
		}

		uint32_t *indirect_block = block_pointer(twice_indirect_block[i]);
		for (j = 0; j < OSPFS_NINDIRECT; j++) 
		{
			if (indirect_block[j] == 0)
				return truncates_inode(inode, block_num);

			if (bitmap_get(indirect_block[j]) == 0) {
				FIXED("Doubly indirect indirect direct block referenced multiple times");
				truncates_inode(inode, block_num);
				return FS_FIXED;
			}
			block_num++;
		}
	}
	return FS_OK;
}

//Truncates the inode to make the final size n blocks
int truncates_inode(ospfs_inode_t *inode, int n) {
	int i;
	uint32_t max_file_size = n * OSPFS_BLKSIZE;

	 //printf("Truncating inode at %d blocks\n", n);

	// Truncating within direct blocks.
	if (n <= OSPFS_NDIRECT) {
		//Set all blocks after to 0, including indirect and doubly indirect pointers
		for (i = n; i < OSPFS_NDIRECT; i++)
			inode->oi_direct[i] = 0;
		inode->oi_indirect = inode->oi_indirect2 = 0;
	}

	//Truncating the indirect blocks
	else if (n < OSPFS_NDIRECT + OSPFS_NINDIRECT) {
		uint32_t *indirect_blocks = block_pointer(inode->oi_indirect);

		//Set all blocks after to 0, including doubly indirect pointers
		for (i = n - OSPFS_NDIRECT; i < OSPFS_NDIRECT + OSPFS_NINDIRECT; i++) 
			indirect_blocks[i] = 0;
		inode->oi_indirect2 = 0;
	}

	//Truncating the doubly indirect blocks
	else if (n <= OSPFS_MAXFILEBLKS) {
		uint32_t *twice_indirect_block = block_pointer(inode->oi_indirect2);
		uint32_t indirect_block_num = n / OSPFS_NINDIRECT;
		uint32_t direct_block_num = n % OSPFS_NINDIRECT;
		uint32_t *indirect_block = block_pointer(twice_indirect_block[direct_block_num]);
		if (direct_block_num == 0) 
			twice_indirect_block[indirect_block_num] = 0;
		
		for (i = indirect_block_num + 1; i < OSPFS_NINDIRECT; i++)
			twice_indirect_block[i] = 0;

		// Sets remaining direct-blocks to 0.
		for (i = direct_block_num; i < OSPFS_NINDIRECT; i++)
			indirect_block[i] = 0;
	}

	//Checks if the file size needs to be changed
	if (max_file_size < inode->oi_size) {
		inode->oi_size = max_file_size;
		FIXED("Inode size incorrect after truncation (Sanity Check)");
		return FS_FIXED;
	}

	return FS_OK;
}

//Returns a pointer to the block number given
void *block_pointer(uint32_t block_num) {
	return fs.buffer + (block_num * OSPFS_BLKSIZE);
}

//Returns a pointer to the block number + offset given
void *block_offset(uint32_t block_num, uint32_t offset) {
	return fs.buffer + (block_num * OSPFS_BLKSIZE) + offset;
}

//The following functions were taken from ospfsmod.c
//
//

// ospfs_inode_blockno(oi, offset)
//	Use this function to look up the blocks that are part of a file's
//	contents.
//
//   Inputs:  oi     -- pointer to a OSPFS inode
//	      offset -- byte offset into that inode
//   Returns: the block number of the block that contains the 'offset'th byte
//	      of the file

static inline uint32_t
ospfs_inode_blockno(ospfs_inode_t *oi, uint32_t offset) {
	uint32_t blockno = offset / OSPFS_BLKSIZE;
	if (offset >= oi->oi_size || oi->oi_ftype == OSPFS_FTYPE_SYMLINK)
		return 0;
	else if (blockno >= OSPFS_NDIRECT + OSPFS_NINDIRECT) {
		uint32_t blockoff = blockno - (OSPFS_NDIRECT + OSPFS_NINDIRECT);
		uint32_t *indirect2_block = block_pointer(oi->oi_indirect2);
		uint32_t *indirect_block = block_pointer(indirect2_block[blockoff / OSPFS_NINDIRECT]);
		return indirect_block[blockoff % OSPFS_NINDIRECT];
	} else if (blockno >= OSPFS_NDIRECT) {
		uint32_t *indirect_block = block_pointer(oi->oi_indirect);
		return indirect_block[blockno - OSPFS_NDIRECT];
	} else
		return oi->oi_direct[blockno];
}


// ospfs_inode_data(oi, offset)
//	Use this function to load part of inode's data from "disk",
//	where 'offset' is relative to the first byte of inode data.
//
//   Inputs:  oi     -- pointer to a OSPFS inode
//	      offset -- byte offset into 'oi's data contents
//   Returns: a pointer to the 'offset'th byte of 'oi's data contents
//
//	Be careful: the returned pointer is only valid within a single block.
//	This function is a simple combination of 'ospfs_inode_blockno'
//	and 'ospfs_block'.

static inline void *
ospfs_inode_data(ospfs_inode_t *oi, uint32_t offset) {
	uint32_t blockno = ospfs_inode_blockno(oi, offset);
	return (uint8_t *) block_pointer(blockno) + (offset % OSPFS_BLKSIZE);
}

/*****************************************************************************
 * BITVECTOR OPERATIONS
 *
 *   OSPFS uses a free bitmap to keep track of free blocks.
 *   These bitvector operations, which set, clear, and test individual bits
 *   in a bitmap, may be useful.
 */

// bitvector_test -- Return the value of the 'i'th bit of 'vector'.
static inline int
bitvector_test(const void *vector, int i)
{
	return (((const uint32_t *) vector) [i / 32] & (1 << (i % 32))) != 0;
}