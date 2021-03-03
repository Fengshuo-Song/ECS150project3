#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#include "disk.h"
#include "fs.h"

#define SIGNATURE "ECS150FS"

static uint16_t FAT_EOC = 0xFFFF;

struct superblock
{
    uint8_t     Signature[8];
    uint16_t    Total_amount_of_blocks;
    uint16_t    Root_directory_block_index;
    uint16_t    Data_block_start_index;
    uint16_t    Amount_of_data_blocks;
    uint8_t     Number_of_blocks;
    uint8_t     Padding[4079];
} sb;

static int fsIsInvalid = 1;

//Root Directory 
struct Entry
{
    uint8_t     Filename[16];
    uint32_t    filesize;
    uint16_t    first_data_block_index;
    uint8_t     Padding[10];
};

static uint8_t RootDirectory[BLOCK_SIZE];
struct Entry * pEntryList;

struct FileDescriptor
{
    int32_t index;
    uint32_t offset;
}fdl[FS_OPEN_MAX_COUNT];

//Check whether the filename is legal.
static int validateFilename(const char *pFilename)
{
    int ret = 0;
    
    //Check if filename exists.
    if (pFilename == NULL)
        ret = -1;
    else
    {
        int len = strlen(pFilename);
        if (len > 15)
            ret= -1;
    }

    return ret;
}

//Check whether the entry is empty.
static int EntryIsEmpty(struct Entry * p)
{
    return p->Filename[0] == 0;
}

//Read the content of FAT at index `which` and assign it to `value`
static int readFAT(uint16_t which, uint16_t * value)
{
	//Check whether the file system is vaild.
    if(fsIsInvalid)
        return -1;
        
    //Check whether FAT index is out of range.
	if(which >= sb.Amount_of_data_blocks)
        return -1;
    
    //Get the block index.
    size_t block = (which * 2) / BLOCK_SIZE;  
	//Check whether block index is out of range.   
    if(block >= sb.Number_of_blocks)
        return -1;
    
    uint8_t FAT[BLOCK_SIZE];
    
    //Read the block and store it in FAT and check if successful.
    //Since superblock takes up the first block, so read the block indexing `block`+1 and 
	//check if successful.
    if(block_read(1+block, FAT) == -1)
        return -1;
    
    //`offset` is the index of FAT when each entry is treated as 8 bits.
    int offset = (which * 2) % BLOCK_SIZE;
    *value = *((uint16_t*)(FAT+offset));
    return 0;   
}

//Write the content of FAT at index `which` and assign it to `value`.
static int writeFAT(uint16_t which, uint16_t value)
{
	//Check whether the file system is vaild.
    if(fsIsInvalid)
        return -1;
        
    //Check whether FAT index is out of range.
    if(which >= sb.Amount_of_data_blocks)
        return -1;
    
    size_t block = (which * 2) / BLOCK_SIZE; 
	//Check whether block index is out of range.   
    if(block >= sb.Number_of_blocks)
        return -1;
    
    //Read the block and store it in FAT.
    uint8_t FAT[BLOCK_SIZE];
    if(block_read(1+block, FAT) == -1)
        return -1;
    
    int offset = (which * 2) % BLOCK_SIZE;
    //Change the value at given index.
    *((uint16_t*)(FAT+offset)) = value;
    
    //Write the new FAT to the block.
    return block_write(1+block, FAT);
}

//Get `count` number of free spaces and store their indices in FAT in `pt`.
//`returncount` is the actual number of spaces obtained.
static int getfreeFAT(uint16_t count, uint16_t * pt, uint16_t * returncount)
{
	//Initialize `returncount`.
    *returncount = 0;
    //Check whether the file system is vaild.
    if(fsIsInvalid)
        return -1;
        
    if(count <= 0)
        return -1;

	//`total` stores the amount of space actually obtained.
	//`number` is the index in FAT.        
    uint16_t total = 0;
    uint16_t number = 0;
    int i;
    for(i = 0; i < sb.Number_of_blocks; i++)
    {
    	//Read each block in FAT.
        uint8_t FAT[BLOCK_SIZE];
        block_read(1+i, FAT);
        //`result` stores the value in FAT.
        uint16_t result = 0;
        int offset; 
        for(offset = 0; offset < BLOCK_SIZE && number < sb.Amount_of_data_blocks; offset += 2, number ++)
        {
            result = *((uint16_t*)(FAT+offset));
            
            //A free item is found.
            if(result == 0)
            {
                *(pt+total++) = number;
                if(total==count)
                {
                    *returncount = total;
                    return 0;
                }
            }
        }
    }
    *returncount = total;
    return -1;
}

//Write the entire chain.
static int writechainFAT(uint16_t *pt, uint16_t count)
{
	//Check whether the file system is vaild.
    if(fsIsInvalid)
        return -1;

	//Iterate each free entry in `pt`.
    for(uint16_t index = 0; index < count; ++ index)
    {
    	//current index
        uint16_t value = pt[index];
		//the value at current index is the index of next free entry if 
		//it is not the end of the chain; and 0xFFFF otherwise.
        uint16_t nextvalue = (index < count-1 ? pt[index+1]:(uint16_t)0xFFFF);
        size_t block = value / (BLOCK_SIZE/2);
        size_t offset = (value % (BLOCK_SIZE/2)) * 2;
        uint8_t data[BLOCK_SIZE];

        if(block_read(1+block, data) == -1)
            return -1;

        *((uint16_t*)(data+offset)) = nextvalue;
        
        //Write `data` to the block and check if successful.
        if(block_write(1+block, data) == -1)
            return -1;
    }

    return 0;        
}

//Delete the entire chain.
static int deletechainFAT(uint16_t start)
{
	//Check whether the file system is vaild.
    if(fsIsInvalid)
        return -1;

    if(start < 1)
        return -1;
        
    uint16_t cur = start;
    uint16_t next = 0;
    do
    {
    	//Read the block at `cur` and store it in `next`.
        if(readFAT(cur, &next) == -1)
            return -1;
        if(next == 0)
            return -1;
        //Change the value at `cur` to 0.
        if(writeFAT(cur,0) != 0)
            return -1;
        cur = next;
    }while(cur != FAT_EOC);
    
    return 0;        
}

//Read the entire chain.
static int readchainFAT(uint16_t start, uint16_t* pt)
{
	//Check whether the file system is vaild.
    if(fsIsInvalid)
        return -1;
        
    if(start < 1)
        return -1;
    
    int total = 0;
    uint16_t* p = pt;
    uint16_t cur = start;
    uint16_t next = 0;
    do
    {
    	//Read the content at `cur` and store it in `next`.
        if(readFAT(cur, &next) == -1)
            return -1;
        if(next == 0)
            return -1;
        //Store `next` in `p`.
        if(p != NULL)
            *p++ = cur;
        total ++;
        cur = next;
    }while(cur != FAT_EOC);
    
    return total;        
}

//Change the file information in root directory.
static int changeFileSize2Rootdirectory(int index, uint32_t filesize, uint16_t first_data_block_index)
{
	//Check whether the file system is vaild.
    if(fsIsInvalid)
        return -1;
    if(index < 0 || index >= FS_FILE_MAX_COUNT)
        return -1;
    pEntryList[index].filesize = filesize;
    pEntryList[index].first_data_block_index = first_data_block_index;
    if(block_write(sb.Root_directory_block_index, RootDirectory) == -1)
        return -1;
    return 0;
}

//Get the number of free FAT entries.
static int getFreeFATCount(uint16_t *count)
{
	//Check whether the file system is vaild.
    if(fsIsInvalid)
        return -1;
    
	//Initialize `*count`.    
    *count = 0;
    uint16_t number = 0;
    int i;
    for(i = 0; i < sb.Number_of_blocks; i++)
    {
    	//Read each block in FAT.
        uint8_t FAT[BLOCK_SIZE];
        block_read(1+i, FAT);
        //`result` stores the value in FAT.
        uint16_t result = 0;
         
        for(int offset = 0; offset < BLOCK_SIZE && number < sb.Amount_of_data_blocks; offset += 2, number ++)
        {
            result = *((uint16_t*)(FAT+offset));
            //A free item is found.
            if(result == 0)
				(*count) ++;
        }
    }
    
    return 0;  
}

//Get the number of free directories.
static int getFreeRootDirCount(uint16_t *count)
{
	//Check whether the file system is vaild.
    if(fsIsInvalid)
        return -1;

    //Initialize `*count`.    
    *count = 0;
    int i;
    for(i = 0; i < FS_FILE_MAX_COUNT; ++ i)     
	{
		if(EntryIsEmpty(&pEntryList[i]))
			(*count) ++;
	}
    
	return 0;
}


int fs_mount(const char *diskname)
{
	//Check whether the file system is vaild.
    fsIsInvalid = 1;
	//Check if the block is open.
    if(block_disk_open(diskname) == -1)
        return -1;
    //Read the superblock and check if successful.
    if(block_read(0, &sb) == -1)
        return -1;
    
    //Read the root directory.
    if(block_read(sb.Root_directory_block_index, RootDirectory) == -1)
    {
        memset(&sb, 0, sizeof(sb));
        return -1;
    }
    pEntryList = (struct Entry *)RootDirectory;
    //Initialize file descriptor.
    for(int i = 0; i < FS_OPEN_MAX_COUNT; ++ i)
    {
        fdl[i].index = -1;
        fdl[i].offset = 0;
    }
    fsIsInvalid = 0;
    return 0;
}

int fs_umount(void)
{
	//Check whether the file system is vaild.
    if(fsIsInvalid)
        return -1;
    //Set all values in superblock to 0.
    memset(&sb, 0, sizeof(sb));
    fsIsInvalid = 1;
    return block_disk_close();
}

int fs_info(void)
{
	//Check whether the file system is vaild.
    if(fsIsInvalid)
        return -1;
    printf("FS Info:\n");
    printf("total_blk_count=%d\n",sb.Total_amount_of_blocks);
    printf("fat_blk_count=%d\n",sb.Number_of_blocks);
    printf("rdir_blk=%d\n",sb.Root_directory_block_index);
    printf("data_blk=%d\n",sb.Data_block_start_index);
    printf("data_blk_count=%d\n",sb.Amount_of_data_blocks);
    uint16_t free_fat, free_rdir;
    getFreeFATCount(&free_fat);
    getFreeRootDirCount(&free_rdir);
    //The number of data blocks equals the number of entries in FAT.
    printf("fat_free_ratio=%d/%d\n",free_fat,sb.Amount_of_data_blocks);
    printf("rdir_free_ratio=%d/128\n",free_rdir);

    return 0;
}

int fs_create(const char *filename)
{
	//Check whether the file system is vaild.
    if(fsIsInvalid)
        return -1;
    //Check whether the filename is legal.
    if(validateFilename(filename) == -1)
        return -1;

	//Find the first free index for the new file and also check whether this file
	//already exists.
    int firstfreeIndex = -1;
    int findIndex = -1;
    for(int i = 0; i < FS_FILE_MAX_COUNT; ++ i)
    {        
        if(EntryIsEmpty(&pEntryList[i]))
        {
            if(firstfreeIndex == -1)
                firstfreeIndex = i;
        }
        else
        {
            if(memcmp(filename, pEntryList[i].Filename, strlen(filename)+1) == 0)
                findIndex = i;
        }
    }
    if(findIndex != -1)
        return -1;
    if(firstfreeIndex == -1)
        return -1;
    else
    {
    	//Update filename at the free index of the list.
        memcpy(pEntryList[firstfreeIndex].Filename, filename, strlen(filename)+1);
        //Initialize filesize and first data block index.
        pEntryList[firstfreeIndex].filesize = 0;
        pEntryList[firstfreeIndex].first_data_block_index = FAT_EOC;
        return block_write(sb.Root_directory_block_index, RootDirectory);
    }
}

int fs_delete(const char *filename)
{
	//Check whether the file system is vaild.
    if(fsIsInvalid)
        return -1;
    //Check whether the filename is legal. 
    if(validateFilename(filename) == -1)
        return -1;
	//Check whether the file exists.
    int findIndex = -1;
    for(int i = 0; i < FS_FILE_MAX_COUNT && findIndex == -1; ++ i)
    {        
        if(memcmp(filename, pEntryList[i].Filename, strlen(filename)+1) == 0)
            findIndex = i;
    }
    if(findIndex == -1)
        return -1;
    else
    {
    	//Check whether the file is open.    	
        for(int i = 0; i < FS_OPEN_MAX_COUNT; ++ i)
        {
            if(fdl[i].index == findIndex)
                return -1;
        }

		//Delete the data in the file.
        int start = pEntryList[findIndex].first_data_block_index;
        if(start != FAT_EOC)
            deletechainFAT(start);
        //Reset the information in the root directory corresponding to the file.
        memset(RootDirectory+32*findIndex, 0, 32);
        int result = block_write(sb.Root_directory_block_index, RootDirectory);
        return result;
    }
}

int fs_ls(void)
{
	//Check whether the file system is vaild.
    if(fsIsInvalid)
        return -1;
    printf("FS Ls:\n");
    //List the information of all files in the root directory.
    int i;
    for(i = 0; i < FS_FILE_MAX_COUNT; ++ i)
        if(!EntryIsEmpty(&pEntryList[i]))
            printf("file: %s, size: %d, data_blk: %d\n", pEntryList[i].Filename, pEntryList[i].filesize, pEntryList[i].first_data_block_index);

    return 0;
}

int fs_open(const char *filename)
{
	//Check whether the file system is vaild.
    if(fsIsInvalid)
        return -1;
    //Check whether the filename is legal.
    if(validateFilename(filename) == -1)
        return -1;
	//Check whether the file exists.
    int findIndex = -1;
    for(int i = 0; i < FS_FILE_MAX_COUNT && findIndex == -1; ++ i)
    {        
        if(memcmp(filename, pEntryList[i].Filename, strlen(filename)+1) == 0)
            findIndex = i;
    }
    if(findIndex == -1)
        return -1;
    else
    {
    	//Find a free file descriptor in the file descriptor list.
        int findfirstfreefdl = -1;
        for(int i = 0; i < FS_OPEN_MAX_COUNT && findfirstfreefdl == -1; ++ i)
        {
            if(fdl[i].index == -1)
                findfirstfreefdl = i;
        }
        if(findfirstfreefdl == -1)
            return -1;
        //Update the value in the given file descriptor.
        fdl[findfirstfreefdl].index = findIndex;
        fdl[findfirstfreefdl].offset = 0;
        
        return findfirstfreefdl;
    }
}

int fs_close(int fd)
{
	//Check whether the file system is vaild.
    if(fsIsInvalid)
        return -1;
    //Check whether fd is out of range.
    if(fd < 0 || fd >= FS_OPEN_MAX_COUNT)
        return -1;
    //Check whether the file is open.
    if(fdl[fd].index == -1)
        return -1;

    //Change the index to -1.
    fdl[fd].index = -1;
    fdl[fd].offset = 0;
    return 0;
}

int fs_stat(int fd)
{
	//Check whether the file system is vaild.
    if(fsIsInvalid)
        return -1;
    //Check whether fd is out of range.
    if(fd < 0 || fd >= FS_OPEN_MAX_COUNT)
        return -1;
    //Check whether the file is open.
    if(fdl[fd].index == -1)
        return -1;
	//Check whether the file descriptor is empty.
    if(EntryIsEmpty(&pEntryList[fdl[fd].index]))
        return -1;

    return pEntryList[fdl[fd].index].filesize;
}

int fs_lseek(int fd, size_t offset)
{
	//Check whether the file system is vaild.
    if(fsIsInvalid)
        return -1;
    //Check whether fd is out of range.
    if(fd < 0 || fd >= FS_OPEN_MAX_COUNT)
        return -1;
    //Check whether the file is open. 
    if(fdl[fd].index == -1)
        return -1;
	//Check whether the file descriptor is empty.
    if(EntryIsEmpty(&pEntryList[fdl[fd].index]))
        return -1;
    //Check whether the offset is within the bound.
    if(offset > pEntryList[fdl[fd].index].filesize) //可以等于，然后追加
        return -1;
    //Update `offset`.    
    fdl[fd].offset = offset;
    return 0;
}

int fs_write(int fd, void *buf, size_t count)
{
    //the number of bytes actually written    
	int writeBytes = 0;
	//Check whether the file system is vaild.
    if(fsIsInvalid)
        return -1;
    //Check if `buf` is empty.
    if(buf == NULL)
        return -1;
    //Check whether fd is out of range. 
    if(fd < 0 || fd >= FS_OPEN_MAX_COUNT)
        return -1;
    //Check whether the file is open.
    if(fdl[fd].index == -1)
        return -1;
	//Special case: write 0 data.
    if(count == 0)
        return 0;
	//Check whether the file descriptor is empty.s
    if(EntryIsEmpty(&pEntryList[fdl[fd].index]))
        return writeBytes;
    //Check whether the offset is within the bound.
    if(fdl[fd].offset > pEntryList[fdl[fd].index].filesize)
        return writeBytes;

	//Get the file sizes of both original and new file.
    uint32_t oldfilesize = pEntryList[fdl[fd].index].filesize;
    //Get the block indeices and offsets of both start and end.
    uint16_t startblock = fdl[fd].offset / BLOCK_SIZE;
    uint16_t startoffset = fdl[fd].offset % BLOCK_SIZE;
    uint16_t endblock = (fdl[fd].offset + count -1) / BLOCK_SIZE;
    uint16_t endoffset = (fdl[fd].offset + count -1) % BLOCK_SIZE;

	//If the original file is empty, then write the content directly to the file.
    if(oldfilesize == 0)
    {
        uint16_t totalBlocks = endblock-startblock+1;
        uint16_t returntotalBlocks = 0;
        uint16_t freeFatList[totalBlocks];

        //This happens when there is not enough space for writing.
        if(getfreeFAT(totalBlocks, freeFatList, &returntotalBlocks) == -1)
        {
        	//This means there is no free space.
            if(returntotalBlocks == 0)
                return writeBytes;
            else
            {
            	//Store the number of blocks that can actually use.
                totalBlocks = returntotalBlocks;
                count = returntotalBlocks * BLOCK_SIZE;
                
                //Update the end block index and offset.
                endblock = (fdl[fd].offset + count -1) / BLOCK_SIZE;
                endoffset = (fdl[fd].offset + count -1) % BLOCK_SIZE;
            }
        }
        
		//Check if the entry corresponding to the file is empty.
        if(EntryIsEmpty(&pEntryList[fdl[fd].index]))
            return writeBytes;

		//Write the data to data blocks.
		//iterator of the FAT list
        int no = 0;

        //Since the last data block, which is `endblock`, may not be used completely, 
        //we divide this task into 2 parts: writing full-block data into blocks indexing
        //from `startblock` to `endblock`-1 and then writing the remaining data to the 
        //last data block indexing `endblock`.
        
        //Part 1
        for(uint16_t block = startblock; block < endblock; ++ block)
        {
        	//Write the data to corresponding data block and check if successful.
            if(block_write(sb.Data_block_start_index+freeFatList[no++], buf+block*BLOCK_SIZE) == -1)
            {
            	//Update the information in root directory.
                changeFileSize2Rootdirectory(fdl[fd].index, writeBytes, freeFatList[0]);
                //Update the FAT blocks.
                writechainFAT(freeFatList, writeBytes/BLOCK_SIZE);
                return writeBytes;
            }
            
            //Update the bytes written and offset.
            writeBytes += BLOCK_SIZE;
            fdl[fd].offset += BLOCK_SIZE;
        }
        
        //Part 2
        uint8_t data[BLOCK_SIZE];
        memset(data, 0, BLOCK_SIZE);
        memcpy(data, buf+endblock*BLOCK_SIZE, endoffset+1);
        if(block_write(sb.Data_block_start_index+freeFatList[no], data) == -1)
        {
        	//Update the information in root directory.
            changeFileSize2Rootdirectory(fdl[fd].index, writeBytes, freeFatList[0]);
            //Update the FAT blocks.
            writechainFAT(freeFatList, writeBytes/BLOCK_SIZE);
            return writeBytes;
        }
        
        //Update the bytes written and offset.
        writeBytes += endoffset+1;
        fdl[fd].offset += endoffset+1;
        //Update the information in root directory.        
        changeFileSize2Rootdirectory(fdl[fd].index, writeBytes, freeFatList[0]);
        //Update the FAT blocks.
        writechainFAT(freeFatList, totalBlocks);
        return writeBytes;
    }
    else
    {
		//Check whether the file descriptor is empty.
        if(EntryIsEmpty(&pEntryList[fdl[fd].index]))
            return writeBytes;

		//Get the length of the chain in the original file.
        int oldlen = readchainFAT(pEntryList[fdl[fd].index].first_data_block_index, NULL);
        //This happens when the reading fails.
		if(oldlen == -1)
            return writeBytes;
        int newlen = oldlen;
        //When the newly added data can be written in the same block, there is no need to
        //update the length.
        if(newlen < endblock + 1)
            newlen = endblock + 1;
        uint16_t FATList[newlen];
        readchainFAT(pEntryList[fdl[fd].index].first_data_block_index, FATList);
		
        if(oldlen < newlen)
        {
            uint16_t returntotalBlocks = 0;
            //This happens when there is not enough space for writing. 
            if(getfreeFAT(newlen-oldlen, FATList+oldlen, &returntotalBlocks) == -1)
            {
				//Get the number of 8-bit items.			 
                if(startoffset == 0)
                    count = returntotalBlocks * BLOCK_SIZE;
                else
                    count = BLOCK_SIZE-startoffset + returntotalBlocks * BLOCK_SIZE;
                    
                //Update the filesize, end block index, block offset and length.
                endblock = (fdl[fd].offset + count -1) / BLOCK_SIZE;
                endoffset = (fdl[fd].offset + count -1) % BLOCK_SIZE;
                newlen = endblock + 1;
				
				//Special case: write 0 data.
                if(count == 0)
                    return 0;
            }

        }

        uint8_t data[BLOCK_SIZE];
        
        //This happens when all new data can be written within one block.
        if(startblock == endblock)
        {
        	//Get the original data in the data block and check if successful.
            if(block_read(sb.Data_block_start_index+FATList[startblock], data) == -1)
                return writeBytes;
            //Update the data.
            memcpy(data+startoffset, buf, count);
            //Update the data block by writing the new data to the block.
            if(block_write(sb.Data_block_start_index+FATList[startblock], data) == -1)
                return writeBytes;
                
            //Update the bytes written and offset.
            writeBytes += count;
            fdl[fd].offset += count;
            
            //Update the root directory and FAT blocks.
            changeFileSize2Rootdirectory(fdl[fd].index, fdl[fd].offset, pEntryList[fdl[fd].index].first_data_block_index);
            writechainFAT(FATList, newlen);
            return writeBytes;
        }
        else
        {
        	//When the new data cannot be written in one block, we divide this task 
			//into 3 parts: writing data into the starting data block indexing `startblock`, 
			//writing full-block data into blocks indexing from `startblock`+1 to `endblock`-1
			//and then writing the remaining data to the last data block indexing `endblock`.
			
			//Part1
			//Get the original data in the data block and check if successful.
            if(block_read(sb.Data_block_start_index+FATList[startblock], data) == -1)
                return writeBytes;
            //Update the data.
            memcpy(data+startoffset, buf, BLOCK_SIZE-startoffset);
            //Update the data block by writing the new data to the block.
            if(block_write(sb.Data_block_start_index+FATList[startblock], data) == -1)
                return writeBytes;
            
            //Update the bytes written and offset.
            writeBytes += BLOCK_SIZE-startoffset;
            fdl[fd].offset += BLOCK_SIZE-startoffset;
                        
            //Part 2
            for(uint16_t index = startblock+1; index < endblock; ++ index)
            {
            	//Write the data to corresponding data block and check if successful.
                if(block_write(sb.Data_block_start_index+FATList[index], buf+(BLOCK_SIZE-startoffset)+(index - (startblock+1))*BLOCK_SIZE) == -1)
                {
                	//Update the root directory and FAT blocks.
                    changeFileSize2Rootdirectory(fdl[fd].index, fdl[fd].offset, pEntryList[fdl[fd].index].first_data_block_index);
                    writechainFAT(FATList, fdl[fd].offset / BLOCK_SIZE);
                    return writeBytes;
                }
                
                //Update the bytes written and offset.
                writeBytes += BLOCK_SIZE;
                fdl[fd].offset += BLOCK_SIZE;
            }
            
			//Part 3
			//Get the data block and check if successful.            
            if(block_read(sb.Data_block_start_index+FATList[endblock], data) == -1)
            {
            	//Update the root directory and FAT blocks.
                changeFileSize2Rootdirectory(fdl[fd].index, fdl[fd].offset, pEntryList[fdl[fd].index].first_data_block_index);
                writechainFAT(FATList, fdl[fd].offset / BLOCK_SIZE);
                return writeBytes;
            }
            //Update the data.
            memcpy(data, buf+count-(endoffset+1), endoffset+1);
            //Update the data block by writing the new data to the block.
            if(block_write(sb.Data_block_start_index+FATList[endblock], data) == -1)
            {
				//Update the root directory and FAT blocks.
                changeFileSize2Rootdirectory(fdl[fd].index, fdl[fd].offset, pEntryList[fdl[fd].index].first_data_block_index);
                writechainFAT(FATList, fdl[fd].offset / BLOCK_SIZE);
                return writeBytes;
            }
            
            //Update the bytes written and offset.
            writeBytes += endoffset+1;
            fdl[fd].offset += endoffset+1;
			//Update the root directory and FAT blocks.
            changeFileSize2Rootdirectory(fdl[fd].index, fdl[fd].offset, pEntryList[fdl[fd].index].first_data_block_index);
            writechainFAT(FATList, newlen);
            return writeBytes;
        }
    }
}

int fs_read(int fd, void *buf, size_t count)
{
	//the number of bytes actually written
    int readBytes = 0;
    //Check whether the file system is vaild.
    if(fsIsInvalid)
        return -1;
	//Check if `buf` is empty.
    if(buf == NULL)
        return -1;
    //Check whether `fd` is out of range.
    if(fd < 0 || fd >= FS_OPEN_MAX_COUNT)
        return -1;
    //Check whether the file is open.
    if(fdl[fd].index == -1)
        return -1;
    //Special case: read 0 data.
    if(count == 0)
        return 0;
    //Check whether the file descriptor is empty.    
    if(EntryIsEmpty(&pEntryList[fdl[fd].index]))
        return readBytes;
	//Check whether the offset is within the bound.
    if(fdl[fd].offset >= pEntryList[fdl[fd].index].filesize)
        return readBytes;

	//Get the start block index and offset.
    uint16_t startblock = fdl[fd].offset / BLOCK_SIZE;
    uint16_t startoffset = fdl[fd].offset % BLOCK_SIZE;

    //If the amount of data to be read is beyond the amount of data the file has
	//after the offset, then read till the end of the file.
    if(count > pEntryList[fdl[fd].index].filesize - fdl[fd].offset)
        count = pEntryList[fdl[fd].index].filesize - fdl[fd].offset;
	
	//Calculate the end block index and offset.
    uint16_t endblock = (fdl[fd].offset + count -1) / BLOCK_SIZE;
    uint16_t endoffset = (fdl[fd].offset + count -1) % BLOCK_SIZE;

	//Get the length of the data to be read.
    int oldlen = readchainFAT(pEntryList[fdl[fd].index].first_data_block_index, NULL);
    //This happens when the reading fails.
    if(oldlen == -1)
        return readBytes;
	
	//Get the chain in FAT.
    uint16_t FATList[oldlen];
    readchainFAT(pEntryList[fdl[fd].index].first_data_block_index, FATList);
      
    uint8_t data[BLOCK_SIZE];
    
    //This happens when the data to be read is within one block.
    if(startblock == endblock)
    {
    	//Read the block and check if successful.
        if(block_read(sb.Data_block_start_index+FATList[startblock], data) == -1)
            return readBytes;
		//Store the data in `buf`.
        memcpy(buf+readBytes, data+startoffset, count);
        //Update the byte already read and the offset.
        readBytes += count;
        fdl[fd].offset += count;
        return readBytes;
    }
    else
    {
    	//When the new data to be read takes up more than one block, we divide this task 
		//into 3 parts: reading data from the starting data block indexing `startblock`, 
		//reading full-block data from blocks indexing from `startblock`+1 to `endblock`-1
		//and then reading the remaining data from the last data block indexing `endblock`.

    	//Read the block and check if successful.
        if(block_read(sb.Data_block_start_index+FATList[startblock], data) == -1)
            return readBytes;
        //Store the data in `buf`.
        memcpy(buf+readBytes, data+startoffset, BLOCK_SIZE-startoffset);
        //Update the byte already read and the offset.
        readBytes += BLOCK_SIZE-startoffset;
        fdl[fd].offset += BLOCK_SIZE-startoffset;
		
        for(uint16_t index = startblock+1; index < endblock; ++ index)
        {
        	//Read the block and check if successful.
            if(block_read(sb.Data_block_start_index+FATList[index], data) == -1)
                return readBytes;
            //Store the data in `buf`.
            memcpy(buf+readBytes, data, BLOCK_SIZE);
            //Update the byte already read and the offset.
            readBytes += BLOCK_SIZE;
            fdl[fd].offset += BLOCK_SIZE;
        }
		
		//Read the block and check if successful.
        if(block_read(sb.Data_block_start_index+FATList[endblock], data) == -1)
            return readBytes;
        //Store the data in `buf`.
        memcpy(buf+readBytes, data, endoffset+1);
        //Update the byte already read and the offset.
        readBytes += endoffset+1;
        fdl[fd].offset += endoffset+1;
        return readBytes;
    }

    return readBytes;
}
