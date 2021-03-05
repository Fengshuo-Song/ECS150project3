#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#include "disk.h"
#include "fs.h"

static uint16_t FAT_EOC = 0xFFFF;

struct super_block {
    uint8_t     signature[8];
    uint16_t    total_amount_of_blocks;
    uint16_t    rdir_block_index;
    uint16_t    data_block_start_index;
    uint16_t    amount_of_data_blocks;
    uint8_t     number_of_fat_blocks;
    uint8_t     padding[4079];
} sb;

static int fs_invalid = 1;

// Root Directory
struct entry {
    uint8_t     file_name[16];
    uint32_t    file_size;
    uint16_t    first_data_block_index;
    uint8_t     padding[10];
};

static uint8_t root_directory[BLOCK_SIZE];
struct entry * ptr_entry_list;

struct file_descriptor {
    int32_t index;
    uint32_t offset;
} fdl[FS_OPEN_MAX_COUNT];

// Check whether the file name is legal.
static int validate_file_name(const char *file_name) {
    int ret = 0;

    // Check if file name exists.
    if (file_name == NULL) {
        ret = -1;
    } else {
        int len = strlen(file_name);
        if (len > 15)
            ret = -1;
    }

    return ret;
}

// Check whether the entry is empty.
static int entry_is_empty(struct entry * p) {
    return p->file_name[0] == 0;
}

// Read the content of FAT at index `which` and assign it to `*value`
static int read_fat(uint16_t which, uint16_t * value) {
    // Check whether the file system is vaild.
    if (fs_invalid)
        return -1;

    // Check whether FAT index is out of range.
    if (which >= sb.amount_of_data_blocks)
        return -1;

    // Get the block index.
    size_t block = (which * 2) / BLOCK_SIZE;
    // Check whether block index is out of range.
    if (block >= sb.number_of_fat_blocks)
        return -1;

    uint8_t fat[BLOCK_SIZE];

    // Read the block and store it in FAT and check if successful.
    // Since super block takes up the first block, so read the
    // block indexing `block`+1 and check if successful.
    if (block_read(1+block, fat) == -1)
        return -1;

    // `offset` is the index of FAT when each entry is treated as 8 bits.
    int offset = (which * 2) % BLOCK_SIZE;
    *value = *((uint16_t*)(fat+offset));
    return 0;
}

// Write the content of FAT at index `which` and assign it to `*value`.
static int write_fat(uint16_t which, uint16_t value) {
    // Check whether the file system is vaild.
    if (fs_invalid)
        return -1;

    // Check whether FAT index is out of range.
    if (which >= sb.amount_of_data_blocks)
        return -1;

    size_t block = (which * 2) / BLOCK_SIZE;
    // Check whether block index is out of range.
    if (block >= sb.number_of_fat_blocks)
        return -1;

    // Read the block and store it in FAT.
    uint8_t fat[BLOCK_SIZE];
    if (block_read(1+block, fat) == -1)
        return -1;

    int offset = (which * 2) % BLOCK_SIZE;
    // Change the value at given index.
    *((uint16_t*)(fat+offset)) = value;

    // Write the new FAT to the block.
    return block_write(1+block, fat);
}

// Get `count` number of free spaces and store their indices in FAT in `pt`.
// `return_count` is the actual number of spaces obtained.
static int get_free_fat(uint16_t count, uint16_t * pt,
                        uint16_t * return_count) {
    // Initialize `return_count`.
    *return_count = 0;
    // Check whether the file system is vaild.
    if (fs_invalid)
        return -1;

    if (count <= 0)
        return -1;

    // `total` stores the amount of space actually obtained.
    // `number` is the index in FAT.
    uint16_t total = 0;
    uint16_t number = 0;
    int i;
    for (i = 0; i < sb.number_of_fat_blocks; i++) {
        // Read each block in FAT.
        uint8_t fat[BLOCK_SIZE];
        block_read(1+i, fat);
        // `result` stores the value in FAT.
        uint16_t result = 0;
        int offset;
        for (offset = 0; offset < BLOCK_SIZE &&
             number < sb.amount_of_data_blocks; offset += 2, number++) {
            result = *((uint16_t*)(fat+offset));

            // A free item is found.
            if (result == 0) {
                *(pt+total++) = number;
                if (total == count) {
                    *return_count = total;
                    return 0;
                }
            }
        }
    }
    *return_count = total;
    return -1;
}

// Write the entire chain.
static int write_chain_fat(uint16_t *pt, uint16_t count) {
    // Check whether the file system is vaild.
    if (fs_invalid)
        return -1;

    // Iterate each free entry in `pt`.
    for (uint16_t index = 0; index < count; ++index) {
        // Current index
        uint16_t value = pt[index];
        // The value at current index is the index of next free entry if
        // it is not the end of the chain; and 0xFFFF otherwise.
        uint16_t next_value = (index < count-1 ? pt[index+1]:(uint16_t)0xFFFF);
        size_t block = value / (BLOCK_SIZE/2);
        size_t offset = (value % (BLOCK_SIZE/2)) * 2;
        uint8_t data[BLOCK_SIZE];

        if (block_read(1+block, data) == -1)
            return -1;

        *((uint16_t*)(data+offset)) = next_value;

        // Write `data` to the block and check if successful.
        if (block_write(1+block, data) == -1)
            return -1;
    }

    return 0;
}

// Delete the entire chain.
static int delete_chain_fat(uint16_t start) {
    // Check whether the file system is vaild.
    if (fs_invalid)
        return -1;

    if (start < 1)
        return -1;

    uint16_t cur = start;
    uint16_t next = 0;
    do {
        // Read the block at `cur` and store it in `next`.
        if (read_fat(cur, &next) == -1)
            return -1;
        if (next == 0)
            return -1;
        // Change the value at `cur` to 0.
        if (write_fat(cur, 0) != 0)
            return -1;
        cur = next;
    } while (cur != FAT_EOC);

    return 0;
}

// Read the entire chain. If `pt` is empty, return the length of the chain
// only; otherwise, return the length and store the content of the chain in
// `pt`.
static int read_chain_fat(uint16_t start, uint16_t* pt) {
    // Check whether the file system is vaild.
    if (fs_invalid)
        return -1;

    if (start < 1)
        return -1;

    int total = 0;
    uint16_t* p = pt;
    uint16_t cur = start;
    uint16_t next = 0;
    do {
        // Read the content at `cur` and store it in `next`.
        if (read_fat(cur, &next) == -1)
            return -1;
        if (next == 0)
            return -1;
        // Store `next` in `p`.
        if (p != NULL)
            *p++ = cur;
        total++;
        cur = next;
    } while (cur != FAT_EOC);

    return total;
}

// Change the file information in root directory.
static int change_file_size_rdir(int index, uint32_t file_size,
                                 uint16_t first_data_block_index) {
    // Check whether the file system is vaild.
    if (fs_invalid)
        return -1;
    if (index < 0 || index >= FS_FILE_MAX_COUNT)
        return -1;
    ptr_entry_list[index].file_size = file_size;
    ptr_entry_list[index].first_data_block_index = first_data_block_index;
    if (block_write(sb.rdir_block_index, root_directory) == -1)
        return -1;
    return 0;
}

// Get the number of free FAT entries.
static int get_free_fat_count(uint16_t *count) {
    // Check whether the file system is vaild.
    if (fs_invalid)
        return -1;

    // Initialize `*count`.
    *count = 0;
    uint16_t number = 0;
    int i;
    for (i = 0; i < sb.number_of_fat_blocks; i++) {
        // Read each block in FAT.
        uint8_t fat[BLOCK_SIZE];
        block_read(1+i, fat);
        // `result` stores the value in FAT.
        uint16_t result = 0;

        for (int offset = 0; offset < BLOCK_SIZE &&
             number < sb.amount_of_data_blocks; offset += 2, number++) {
            result = *((uint16_t*)(fat+offset));
            // A free item is found.
            if (result == 0)
                (*count)++;
        }
    }

    return 0;
}

// Get how many more files that can be created in the directory.
static int get_free_entry_count(uint16_t *count) {
    // Check whether the file system is vaild.
    if (fs_invalid)
        return -1;

    // Initialize `*count`.
    *count = 0;
    int i;
    for (i = 0; i < FS_FILE_MAX_COUNT; ++i) {
        if (entry_is_empty(&ptr_entry_list[i]))
            (*count)++;
    }

    return 0;
}


int fs_mount(const char *disk_name) {
    // The virtual is not opened. Set the file system to be invaild.
    fs_invalid = 1;
    // Check if the block is open.
    if (block_disk_open(disk_name) == -1)
        return -1;
    // Read the super block and check if successful.
    if (block_read(0, &sb) == -1)
        return -1;

    // Read the root directory.
    if (block_read(sb.rdir_block_index, root_directory) == -1) {
        memset(&sb, 0, sizeof(sb));
        return -1;
    }
    ptr_entry_list = (struct entry *)root_directory;
    // Initialize file descriptor.
    for (int i = 0; i < FS_OPEN_MAX_COUNT; ++i) {
        fdl[i].index = -1;
        fdl[i].offset = 0;
    }
    fs_invalid = 0;
    return 0;
}

int fs_umount(void) {
    // Check whether the file system is vaild.
    if (fs_invalid)
        return -1;
    // Set all values in super block to 0.
    memset(&sb, 0, sizeof(sb));
    fs_invalid = 1;
    return block_disk_close();
}

int fs_info(void) {
    // Check whether the file system is vaild.
    if (fs_invalid)
        return -1;
    printf("FS Info:\n");
    printf("total_blk_count=%d\n", sb.total_amount_of_blocks);
    printf("fat_blk_count=%d\n", sb.number_of_fat_blocks);
    printf("rdir_blk=%d\n", sb.rdir_block_index);
    printf("data_blk=%d\n", sb.data_block_start_index);
    printf("data_blk_count=%d\n", sb.amount_of_data_blocks);
    uint16_t free_fat, free_rdir;
    get_free_fat_count(&free_fat);
    get_free_entry_count(&free_rdir);
    // The number of data blocks equals the number of entries in FAT.
    printf("fat_free_ratio=%d/%d\n", free_fat, sb.amount_of_data_blocks);
    printf("rdir_free_ratio=%d/128\n", free_rdir);

    return 0;
}

int fs_create(const char *file_name) {
    // Check whether the file system is vaild.
    if (fs_invalid)
        return -1;
    // Check whether the file name is legal.
    if (validate_file_name(file_name) == -1)
        return -1;

    // Find the first free index for the new file and also
    // check whether this file already exists.
    int first_free_index = -1;
    int find_index = -1;
    for (int i = 0; i < FS_FILE_MAX_COUNT; ++i) {
        if (entry_is_empty(&ptr_entry_list[i])) {
            if (first_free_index == -1)
                first_free_index = i;
        } else {
            if (memcmp(file_name, ptr_entry_list[i].file_name,
                       strlen(file_name)+1) == 0)
                find_index = i;
        }
    }
    if (find_index != -1)
        return -1;
    if (first_free_index == -1) {
        return -1;
    } else {
        // Update file name at the free index of the list.
        memcpy(ptr_entry_list[first_free_index].file_name, file_name,
               strlen(file_name)+1);
        // Initialize file size and first data block index.
        ptr_entry_list[first_free_index].file_size = 0;
        ptr_entry_list[first_free_index].first_data_block_index = FAT_EOC;
        return block_write(sb.rdir_block_index, root_directory);
    }
}

int fs_delete(const char *file_name) {
    // Check whether the file system is vaild.
    if (fs_invalid)
        return -1;
    // Check whether the file name is legal.
    if (validate_file_name(file_name) == -1)
        return -1;
    // Check whether the file exists.
    int find_index = -1;
    for (int i = 0; i < FS_FILE_MAX_COUNT && find_index == -1; ++i) {
        if (memcmp(file_name, ptr_entry_list[i].file_name,
                   strlen(file_name)+1) == 0)
            find_index = i;
    }
    if (find_index == -1) {
        return -1;
    } else {
        // Check whether the file is open.
        for (int i = 0; i < FS_OPEN_MAX_COUNT; ++i) {
            if (fdl[i].index == find_index)
                return -1;
        }

        // Delete the data in the file.
        int start = ptr_entry_list[find_index].first_data_block_index;
        if (start != FAT_EOC)
            delete_chain_fat(start);
        // Reset the information in root directory corresponding to the file.
        memset(root_directory+32*find_index, 0, 32);
        int result = block_write(sb.rdir_block_index,
                                 root_directory);
        return result;
    }
}

int fs_ls(void) {
    // Check whether the file system is vaild.
    if (fs_invalid)
        return -1;
    printf("FS Ls:\n");
    // List the information of all files in the root directory.
    int i;
    for (i = 0; i < FS_FILE_MAX_COUNT; ++i) {
        if (!entry_is_empty(&ptr_entry_list[i]))
            printf("file: %s, size: %d, data_blk: %d\n",
                   ptr_entry_list[i].file_name, ptr_entry_list[i].file_size,
                   ptr_entry_list[i].first_data_block_index);
    }

    return 0;
}

int fs_open(const char *file_name) {
    // Check whether the file system is vaild.
    if (fs_invalid)
        return -1;
    // Check whether the file name is legal.
    if (validate_file_name(file_name) == -1)
        return -1;
    // Check whether the file exists.
    int find_index = -1;
    for (int i = 0; i < FS_FILE_MAX_COUNT && find_index == -1; ++i) {
        if (memcmp(file_name, ptr_entry_list[i].file_name,
                   strlen(file_name)+1) == 0)
            find_index = i;
    }
    if (find_index == -1) {
        return -1;
    } else {
        // Find a free file descriptor in the file descriptor list.
        int find_first_free_fdl = -1;
        int i;
        for (i = 0; i < FS_OPEN_MAX_COUNT && find_first_free_fdl == -1; ++i) {
            if (fdl[i].index == -1)
                find_first_free_fdl = i;
        }
        if (find_first_free_fdl == -1)
            return -1;
        // Update the value in the given file descriptor.
        fdl[find_first_free_fdl].index = find_index;
        fdl[find_first_free_fdl].offset = 0;

        return find_first_free_fdl;
    }
}

int fs_close(int fd) {
    // Check whether the file system is vaild.
    if (fs_invalid)
        return -1;
    // Check whether fd is out of range.
    if (fd < 0 || fd >= FS_OPEN_MAX_COUNT)
        return -1;
    // Check whether the file is open.
    if (fdl[fd].index == -1)
        return -1;

    // Change the index to -1.
    fdl[fd].index = -1;
    fdl[fd].offset = 0;
    return 0;
}

int fs_stat(int fd) {
    // Check whether the file system is vaild.
    if (fs_invalid)
        return -1;
    // Check whether fd is out of range.
    if (fd < 0 || fd >= FS_OPEN_MAX_COUNT)
        return -1;
    // Check whether the file is open.
    if (fdl[fd].index == -1)
        return -1;
    // Check whether the file descriptor is empty.
    if (entry_is_empty(&ptr_entry_list[fdl[fd].index]))
        return -1;

    return ptr_entry_list[fdl[fd].index].file_size;
}

int fs_lseek(int fd, size_t offset) {
    // Check whether the file system is vaild.
    if (fs_invalid)
        return -1;
    // Check whether fd is out of range.
    if (fd < 0 || fd >= FS_OPEN_MAX_COUNT)
        return -1;
    // Check whether the file is open.
    if (fdl[fd].index == -1)
        return -1;
    // Check whether the file descriptor is empty.
    if (entry_is_empty(&ptr_entry_list[fdl[fd].index]))
        return -1;
    // Check whether the offset is within the bound.
    if (offset > ptr_entry_list[fdl[fd].index].file_size)
        return -1;
    // Update `offset`.
    fdl[fd].offset = offset;
    return 0;
}

int fs_write(int fd, void *buf, size_t count) {
    // The number of bytes actually written
    int write_bytes = 0;
    // Check whether the file system is vaild.
    if (fs_invalid)
        return -1;
    // Check if `buf` is empty.
    if (buf == NULL)
        return -1;
    // Check whether fd is out of range.
    if (fd < 0 || fd >= FS_OPEN_MAX_COUNT)
        return -1;
    // Check whether the file is open.
    if (fdl[fd].index == -1)
        return -1;
    // Special case: write 0 data.
    if (count == 0)
        return 0;
    // Check whether the file descriptor is empty.
    if (entry_is_empty(&ptr_entry_list[fdl[fd].index]))
        return write_bytes;
    // Check whether the offset is within the bound.
    if (fdl[fd].offset > ptr_entry_list[fdl[fd].index].file_size)
        return write_bytes;

    // Get the file sizes of both original and new file.
    uint32_t old_file_size = ptr_entry_list[fdl[fd].index].file_size;
    // Get the block indeices and offsets of both start and end.
    uint16_t start_block = fdl[fd].offset / BLOCK_SIZE;
    uint16_t start_offset = fdl[fd].offset % BLOCK_SIZE;
    uint16_t end_block = (fdl[fd].offset + count -1) / BLOCK_SIZE;
    uint16_t end_offset = (fdl[fd].offset + count -1) % BLOCK_SIZE;

    // If the original file is empty, write the content directly to the file.
    if (old_file_size == 0) {
        uint16_t total_blocks = end_block-start_block+1;
        uint16_t return_total_blocks = 0;
        uint16_t free_fat_list[total_blocks];

        // This happens when there is not enough space for writing.
        if (get_free_fat(total_blocks, free_fat_list,
                         &return_total_blocks) == -1) {
            // This means there is no free space.
            if (return_total_blocks == 0) {
                return write_bytes;
            } else {
                // Store the number of blocks that can actually use.
                total_blocks = return_total_blocks;
                count = return_total_blocks * BLOCK_SIZE;

                // Update the end block index and offset.
                end_block = (fdl[fd].offset + count -1) / BLOCK_SIZE;
                end_offset = (fdl[fd].offset + count -1) % BLOCK_SIZE;
            }
        }

        // Check if the entry corresponding to the file is empty.
        if (entry_is_empty(&ptr_entry_list[fdl[fd].index]))
            return write_bytes;

        // Write the data to data blocks.
        // iterator of the FAT list
        int no = 0;

        // Since the last data block, which is `end_block`, may not be used
        // completely, we divide this task into 2 parts: writing full-block
        // data into blocks indexing from `start_block` to `end_block`-1 and
        // then writing the remaining data to the last data block indexing
        // `end_block`.

        // Part 1
        for (uint16_t block = start_block; block < end_block; ++block) {
            // Write the data to corresponding data block and check if
            // successful.
            if (block_write(sb.data_block_start_index+free_fat_list[no++],
                            buf+block*BLOCK_SIZE) == -1) {
                // Update the information in root directory.
                change_file_size_rdir(fdl[fd].index, write_bytes,
                                                free_fat_list[0]);
                // Update the FAT blocks.
                write_chain_fat(free_fat_list, write_bytes/BLOCK_SIZE);
                return write_bytes;
            }

            // Update the bytes written and offset.
            write_bytes += BLOCK_SIZE;
            fdl[fd].offset += BLOCK_SIZE;
        }

        // Part 2
        uint8_t data[BLOCK_SIZE];
        memset(data, 0, BLOCK_SIZE);
        memcpy(data, buf+end_block*BLOCK_SIZE, end_offset+1);
        if (block_write(sb.data_block_start_index+free_fat_list[no],
                        data) == -1) {
            // Update the information in root directory.
            change_file_size_rdir(fdl[fd].index, write_bytes,
                                            free_fat_list[0]);
            // Update the FAT blocks.
            write_chain_fat(free_fat_list, write_bytes/BLOCK_SIZE);
            return write_bytes;
        }

        // Update the bytes written and offset.
        write_bytes += end_offset+1;
        fdl[fd].offset += end_offset+1;
        // Update the information in root directory.
        change_file_size_rdir(fdl[fd].index, write_bytes,
                                        free_fat_list[0]);
        // Update the FAT blocks.
        write_chain_fat(free_fat_list, total_blocks);
        return write_bytes;
    } else {
        // Check whether the file descriptor is empty.
        if (entry_is_empty(&ptr_entry_list[fdl[fd].index]))
            return write_bytes;

        // Get the length of the chain in the original file.
        int old_len = read_chain_fat(
            ptr_entry_list[fdl[fd].index].first_data_block_index, NULL);
        // This happens when the reading fails.
        if (old_len == -1)
            return write_bytes;
        int new_len = old_len;
        // When the newly added data can be written in the same block, there is
        // no need to update the length.
        if (new_len < end_block + 1)
            new_len = end_block + 1;
        uint16_t fat_list[new_len];
        read_chain_fat(ptr_entry_list[fdl[fd].index].first_data_block_index,
                       fat_list);

        if (old_len < new_len) {
            uint16_t return_total_blocks = 0;
            // This happens when there is not enough space for writing.
            if (get_free_fat(new_len-old_len, fat_list+old_len,
                             &return_total_blocks) == -1) {
                // Get the number of 8-bit items.
                if (start_offset == 0)
                    count = return_total_blocks * BLOCK_SIZE;
                else
                    count = BLOCK_SIZE-start_offset + return_total_blocks *
                            BLOCK_SIZE;

                // Update the end block index, block offset and length.
                end_block = (fdl[fd].offset + count -1) / BLOCK_SIZE;
                end_offset = (fdl[fd].offset + count -1) % BLOCK_SIZE;
                new_len = end_block + 1;

                // Special case: write 0 data.
                if (count == 0)
                    return 0;
            }
        }

        uint8_t data[BLOCK_SIZE];

        // This happens when all new data can be written within one block.
        if (start_block == end_block) {
            // Get the original data in the data block and check if successful.
            if (block_read(sb.data_block_start_index + fat_list[start_block],
                           data) == -1)
                return write_bytes;
            // Update the data.
            memcpy(data+start_offset, buf, count);
            // Update the data block by writing the new data to the block.
            if (block_write(sb.data_block_start_index + fat_list[start_block],
                            data) == -1)
                return write_bytes;

            // Update the bytes written and offset.
            write_bytes += count;
            fdl[fd].offset += count;

            // Update the root directory and FAT blocks.
            change_file_size_rdir(fdl[fd].index, fdl[fd].offset,
                ptr_entry_list[fdl[fd].index].first_data_block_index);
            write_chain_fat(fat_list, new_len);
            return write_bytes;
        } else {
            // When the new data cannot be written in one block, we divide this
            // task into 3 parts: writing data into the starting data block
            // indexing `start_block`, writing full-block data into blocks
            // indexing from `start_block`+1 to `end_block`-1 and then writing
            // the remaining data to the last data block indexing `end_block`.

            // Part1
            // Get the original data in the data block and check if successful.
            if (block_read(sb.data_block_start_index + fat_list[start_block],
                           data) == -1)
                return write_bytes;
            // Update the data.
            memcpy(data+start_offset, buf, BLOCK_SIZE - start_offset);
            // Update the data block by writing the new data to the block.
            if (block_write(sb.data_block_start_index + fat_list[start_block],
                            data) == -1)
                return write_bytes;

            // Update the bytes written and offset.
            write_bytes += BLOCK_SIZE-start_offset;
            fdl[fd].offset += BLOCK_SIZE-start_offset;

            // Part 2
            for (uint16_t index = start_block+1; index < end_block; ++index) {
                // Write the data to corresponding data block and check if
                // successful.
                if (block_write(sb.data_block_start_index + fat_list[index],
                                buf + (BLOCK_SIZE - start_offset) + (index -
                                (start_block + 1)) * BLOCK_SIZE) == -1) {
                    // Update the root directory and FAT blocks.
                    change_file_size_rdir(fdl[fd].index, fdl[fd].offset,
                        ptr_entry_list[fdl[fd].index].first_data_block_index);
                    write_chain_fat(fat_list, fdl[fd].offset / BLOCK_SIZE);
                    return write_bytes;
                }

                // Update the bytes written and offset.
                write_bytes += BLOCK_SIZE;
                fdl[fd].offset += BLOCK_SIZE;
            }

            // Part 3
            // Get the data block and check if successful.
            if (block_read(sb.data_block_start_index+fat_list[end_block],
                           data) == -1) {
                // Update the root directory and FAT blocks.
                change_file_size_rdir(fdl[fd].index, fdl[fd].offset,
                    ptr_entry_list[fdl[fd].index].first_data_block_index);
                write_chain_fat(fat_list, fdl[fd].offset / BLOCK_SIZE);
                return write_bytes;
            }
            // Update the data.
            memcpy(data, buf+count-(end_offset+1), end_offset+1);
            // Update the data block by writing the new data to the block.
            if (block_write(sb.data_block_start_index + fat_list[end_block],
                            data) == -1) {
                // Update the root directory and FAT blocks.
                change_file_size_rdir(fdl[fd].index, fdl[fd].offset,
                    ptr_entry_list[fdl[fd].index].first_data_block_index);
                write_chain_fat(fat_list, fdl[fd].offset / BLOCK_SIZE);
                return write_bytes;
            }

            // Update the bytes written and offset.
            write_bytes += end_offset+1;
            fdl[fd].offset += end_offset+1;
            // Update the root directory and FAT blocks.
            change_file_size_rdir(fdl[fd].index, fdl[fd].offset,
                ptr_entry_list[fdl[fd].index].first_data_block_index);
            write_chain_fat(fat_list, new_len);
            return write_bytes;
        }
    }
}

int fs_read(int fd, void *buf, size_t count) {
    // The number of bytes actually written
    int read_bytes = 0;
    // Check whether the file system is vaild.
    if (fs_invalid)
        return -1;
    // Check if `buf` is empty.
    if (buf == NULL)
        return -1;
    // Check whether `fd` is out of range.
    if (fd < 0 || fd >= FS_OPEN_MAX_COUNT)
        return -1;
    // Check whether the file is open.
    if (fdl[fd].index == -1)
        return -1;
    // Special case: read 0 data.
    if (count == 0)
        return 0;
    // Check whether the file descriptor is empty.
    if (entry_is_empty(&ptr_entry_list[fdl[fd].index]))
        return read_bytes;
    // Check whether the offset is within the bound.
    if (fdl[fd].offset >= ptr_entry_list[fdl[fd].index].file_size)
        return read_bytes;

    // Get the start block index and offset.
    uint16_t start_block = fdl[fd].offset / BLOCK_SIZE;
    uint16_t start_offset = fdl[fd].offset % BLOCK_SIZE;

    // If the amount of data to be read is beyond the amount of data the file
    // has after the offset, then read till the end of the file.
    if (count > ptr_entry_list[fdl[fd].index].file_size - fdl[fd].offset)
        count = ptr_entry_list[fdl[fd].index].file_size - fdl[fd].offset;

    // Calculate the end block index and offset.
    uint16_t end_block = (fdl[fd].offset + count -1) / BLOCK_SIZE;
    uint16_t end_offset = (fdl[fd].offset + count -1) % BLOCK_SIZE;

    // Get the length of the data to be read.
    int old_len = read_chain_fat(
        ptr_entry_list[fdl[fd].index].first_data_block_index, NULL);
    // This happens when the reading fails.
    if (old_len == -1)
        return read_bytes;

    // Get the chain in FAT.
    uint16_t fat_list[old_len];
    read_chain_fat(ptr_entry_list[fdl[fd].index].first_data_block_index,
                   fat_list);

    uint8_t data[BLOCK_SIZE];

    // This happens when the data to be read is within one block.
    if (start_block == end_block) {
        // Read the block and check if successful.
        if (block_read(sb.data_block_start_index+fat_list[start_block],
                       data) == -1)
            return read_bytes;
        // Store the data in `buf`.
        memcpy(buf+read_bytes, data+start_offset, count);
        // Update the byte already read and the offset.
        read_bytes += count;
        fdl[fd].offset += count;
        return read_bytes;
    } else {
        // When the new data to be read takes up more than one block, we divide
        // this task into 3 parts: reading data from the starting data block
        // indexing `start_block`, reading full-block data from blocks indexing
        // from `start_block`+1 to `end_block`-1 and then reading the remaining
        // data from the last data block indexing `end_block`.

        // Read the block and check if successful.
        if (block_read(sb.data_block_start_index+fat_list[start_block],
                       data) == -1)
            return read_bytes;
        // Store the data in `buf`.
        memcpy(buf+read_bytes, data+start_offset, BLOCK_SIZE-start_offset);
        // Update the byte already read and the offset.
        read_bytes += BLOCK_SIZE-start_offset;
        fdl[fd].offset += BLOCK_SIZE-start_offset;

        for (uint16_t index = start_block+1; index < end_block; ++index) {
            // Read the block and check if successful.
            if (block_read(sb.data_block_start_index +
                           fat_list[index], data) == -1)
                return read_bytes;
            // Store the data in `buf`.
            memcpy(buf + read_bytes, data, BLOCK_SIZE);
            // Update the byte already read and the offset.
            read_bytes += BLOCK_SIZE;
            fdl[fd].offset += BLOCK_SIZE;
        }

        // Read the block and check if successful.
        if (block_read(sb.data_block_start_index +
                       fat_list[end_block], data) == -1)
            return read_bytes;
        // Store the data in `buf`.
        memcpy(buf + read_bytes, data, end_offset + 1);
        // Update the byte already read and the offset.
        read_bytes += end_offset + 1;
        fdl[fd].offset += end_offset + 1;
        return read_bytes;
    }

    return read_bytes;
}
