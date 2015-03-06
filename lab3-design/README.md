Lab 3 Design Problem
Kalin Khemka and Vir Thakor

In this lab, our goal was to design a user level program to analyze an OSPFS filesystem 
image, and fix any problems with that file. In order to do this, we checked if any of the
four invariants were violated:
  1. All blocks used for exactly one purpose
  2. All blocks initialized before referenced
  3. All referenced blocks marked not free
  4. All unreferenced blocks marked free
We also ran sanity-checks on all the disk structures involved on the filesystem image 
(superblock, inodes, and directory entries). Many of our tests were done together. 
For example, checking the first invariant involving blocks being used for only one 
purpose was done in conjunction with all the rest of our tests. 

Our program loads in an image file, then runs analyzes_ospfs() which returns FS_BROKEN
or FS_FIXED depending on how successful we were to fix a file system image. In this we 
check 5 main components of the file system: the superblock, inode table, referenced 
blocks, directories, and the bitmap.

First, we checked the superblock. We ran multiple checks, checking whether: there was
enough space in the image for a superblock, the magic number is correct, the number of
bitmap blocks is enough (if the number is incorrect, we add additional memory to the 
number of bitmap bytes to accommodate extra data), the first inode location is correct,
and the total number of inodres is less than the capacity.

Second, we checked the inodes for errors. For each inode we: ignore empty inodes, then
separate inode by type (regular, directory, or symlink). So for a directory inode we
check that its size doens't exceed the maximum file size, and set the block pointers to
direct, indirect, or double indirect (checked within referenced blocks section, which is 
why we check inodes first). For a symlink inode, we check the length of the link name. 
While iterating through each inode, we create a list of inodes so we can later check that
directory entries are referenced correctly (checked within the directories section, which
is why we check inodes before directories)

Thirdly, we check the blocks referenced by the inodes for errors. Before checking the
referenced blocks, we allocate enough memory for a bitmap to accommodate all of the blocks,
then go through and marks enough bits for used blocks and unused blocks. In order to check
the referenced blocks, we iterate through all inodes and check (non empty inodes): skip 
inodes that are regular or directory inodes, and instead checks duplicate block references
by checking if the inode is a direct, indirect, or double indirect reference. For each 
direct block reference, if we reach the end of the inode within the loop, we truncate it, 
else of we find our block number in the bitmap, otherwise we truncate the inode and mark the
block as referenced. For the indirect block reference, if we reach the end of file, we
truncate the inode, otherwise if we detect a duplicate reference we truncate the inode to
avoid the duplicate. For all indirect blocks, we truncate if we reach the EOF, else we
truncate if we find a duplicate reference, otherwise we mark the block referenced. For
double indirect references, we truncate if we reach the end of file or find a duplicate,
otherwise we loop through each indirect block and truncating for EOF and duplicate then 
looping through the blocks referenced by this indirect block and truncating.

Fourth, we check the directories. To check for errors, we loop through each directory's
directory entries and check the entry refers to an existing inode, and has a valid name.

Finally, we check the bitmap. Returns FS_OK if the bitmap we creat equals the bitmap of the
image file.
