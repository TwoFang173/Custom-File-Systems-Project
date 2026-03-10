/**************************************************************
 * Class::  CSC-415-02 Fall 2025
 * Name:: Kenneth Luong, Gabriella Pasami,
 * 	Jiaming Yu, Nicholas Blackson,
 * Student IDs:: 924199104, 922822157, 923430323, 923752775
 * GitHub-Name:: kyuuris
 * Group-Name:: File Managers
 * Project:: Basic File System
 *
 * File:: b_io.c
 *
 * Description:: Basic File System - Key File I/O Operations
 *
 **************************************************************/

#include <stdio.h>
#include <unistd.h>
#include <stdlib.h> // for malloc
#include <string.h> // for memcpy
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include "b_io.h"
#include <time.h>

#include <errno.h>
#include "mfs.h"

#define MAXFCBS 20

typedef struct b_fcb
{
	/** TODO add al the information you need in the file control block **/
	char *buffer; // holds the open file buffer
	int index;	  // holds the current position in the buffer
	int buflen;	  // holds how many valid bytes are in the buffer
	// custom data
	dir_entry *de;	// contains file level data
	int file_pos;	// stores current position in a file
	int flags;		// stores flag info
	int dirty;		// whether file has been modified
	int cur_extent; // extent section currently in use
	int cur_block;	// current block within extent
} b_fcb;

b_fcb fcbArray[MAXFCBS];

int startup = 0; // Indicates that this has not been initialized

// helper function prototypes for b_open()
// uses pars_path to find the directory entry for the file requested
// creates a file entry if necessary based on flags or other input
dir_entry *fs_create_entry_by_path(const char *path, dir_entry **out_parent);
// free the extents and set size to 0 if truncating a file
int fs_truncate_file(dir_entry *de);
// update parent directory
static int write_parent_dir(dir_entry *parent);

// helper function prototypes for b_write()
// verify file has capacity for a write
int ensure_file_capacity(b_fcb *fcb, int new_size);
// convert the logical block to physical for disk storage
int logical_to_physical(dir_entry *de, int logical_block, uint64_t *phys_block);
// read a block into buffer
int load_block_into_fcb(b_fcb *fcb, uint64_t phys_block);
// append a new ext to an existing ext in the file
int append_extent_to_file(dir_entry *de, ext *newext);
// flush dirty blocks to disk
int flush_fcb_block(b_fcb *fcb, uint64_t phys_block);
// find an ext in the dir_entry provided a logical block
int find_extent_for_block(dir_entry *de, int logical_block);

// Method to initialize our file system
void b_init()
{
	// init fcbArray to all free
	for (int i = 0; i < MAXFCBS; i++)
	{
		fcbArray[i].buffer = NULL; // indicates a free fcbArray
	}

	startup = 1;
}

// Method to get a free FCB element
b_io_fd b_getFCB()
{
	for (int i = 0; i < MAXFCBS; i++)
	{
		if (fcbArray[i].buffer == NULL)
		{
			return i; // Not thread safe (But do not worry about it for this assignment)
		}
	}
	return (-1); // all in use
}

// Interface to open a buffered file
// Modification of interface for this assignment, flags match the Linux flags for open
// O_RDONLY, O_WRONLY, or O_RDWR
b_io_fd b_open(char *filename, int flags)
{
	b_io_fd fd;

	//*** TODO ***:  Modify to save or set any information needed
	//
	//

	if (startup == 0)
		b_init(); // Initialize our system

	fd = b_getFCB(); // get our own file descriptor
	if (fd < 0)
		return -1;

	// verify slot in array
	memset(&fcbArray[fd], 0, sizeof(fcbArray[fd]));

	// try to find file
	// dir_entry *de = fs_find_entry_by_path(filename);
	path_result pr = fs_find_entry_by_path(filename);
	dir_entry *de = pr.entry;
	dir_entry *parent_dir = pr.parent;
	int index = pr.index;

	// file not found; create if O_CREAT is set
	if (de == NULL)
	{
		if (flags & O_CREAT)
		{
			// de = fs_create_entry_by_path(filename);
			dir_entry *new_parent;
			de = fs_create_entry_by_path(filename, &new_parent);
			parent_dir = new_parent;
			// sanity check
			if (!de)
			{
				fprintf(stderr, "Error: file creation failed.\n");
				return -1;
			}
		}
		else
		{
			fprintf(stderr, "Error 404: file not found.\n");
			return -1; // file not found and no O_CREAT
		}
	}

	// refuse to open a directory as a file
	if (de->is_dir)
	{
		errno = EISDIR;
		return -1;
	}

	// handle O_TRUNC: truncate file to length 0 & free extents
	if ((flags & O_TRUNC) && (flags & (O_WRONLY | O_RDWR)))
	{
		if (fs_truncate_file(de) < 0)
		{
			// truncation failed
			fprintf(stderr, "Error: truncation failed.\n");
			return -1;
		}
		// caller should also write parent back to disk
		// write_parent_dir(de);
		write_parent_dir(parent_dir);
	}

	// allocate the buffer for this FD
	fcbArray[fd].buffer = malloc(g_vcb->block_size);
	if (fcbArray[fd].buffer == NULL)
	{
		return -1;
	}

	// initialize fcb
	fcbArray[fd].de = de;
	fcbArray[fd].index = 0;
	fcbArray[fd].buflen = 0;
	fcbArray[fd].file_pos = 0;
	fcbArray[fd].flags = flags;
	fcbArray[fd].dirty = 0;
	fcbArray[fd].cur_extent = 0;
	fcbArray[fd].cur_block = -1;

	// O_APPEND starts position at end of file
	if (flags & O_APPEND)
	{
		fcbArray[fd].file_pos = de->size;

		// verify that the append is located in the write extent and block
		int remaining = fcbArray[fd].file_pos;
		for (int i = 0; i < MAX_EXTENT; i++)
		{
			int e_blocks = de->location[i].num_blocks * g_vcb->block_size;
			if (remaining < e_blocks)
			{
				fcbArray[fd].cur_extent = i;
				fcbArray[fd].cur_block = remaining / g_vcb->block_size;
				break;
			}
			remaining -= e_blocks;
		}
	}

	// update access time of file and verify write persists
	de->accessed = time(NULL);
	// write_parent_dir(de);
	write_parent_dir(parent_dir);

	return fd;
}

// Interface to seek function
int b_seek(b_io_fd fd, off_t offset, int whence)
{
	if (startup == 0)
		b_init(); // Initialize our system

	// check that fd is between 0 and (MAXFCBS-1)
	if ((fd < 0) || (fd >= MAXFCBS))
	{
		return (-1); // invalid file descriptor
	}

	int new_pos = 0;
	// SEEK_SET = 0, SEEK_CUR = 1, SEEK_END = 2;
	switch (whence)
	{
	case SEEK_SET:
		new_pos = offset;
		break;
	case SEEK_CUR:
		new_pos = fcbArray[fd].file_pos + offset;
		break;
	case SEEK_END:
		new_pos = fcbArray[fd].de->size + offset;
		break;
	default:
		return -1;
	}

	if (new_pos < 0)
	{
		return -1;
	}

	fcbArray[fd].file_pos = new_pos;

	return (new_pos);
}

// Interface to write function
int b_write(b_io_fd fd, char *buffer, int count)
{
	if (startup == 0)
		b_init(); // Initialize our system

	// check that fd is between 0 and (MAXFCBS-1)
	if ((fd < 0) || (fd >= MAXFCBS))
	{
		return (-1); // invalid file descriptor
	}

	b_fcb *fcb = &fcbArray[fd];
	dir_entry *de = fcb->de;

	// check permissions
	if (!(fcb->flags & (O_WRONLY | O_RDWR)))
	{
		return -1;
	}

	int bytes_written = 0;
	int block_size = g_vcb->block_size;

	// append goes to end
	if (fcb->flags & O_APPEND)
	{
		fcb->file_pos = de->size;
	}

	while (bytes_written < count)
	{
		int need = count - bytes_written;
		int needed_size = fcb->file_pos + need;

		// grow file if needed
		if (ensure_file_capacity(fcb, needed_size) < 0)
		{
			break;
		}

		int logical_block = fcb->file_pos / block_size;
		int offset = fcb->file_pos % block_size;
		uint64_t phys_block;

		if (logical_to_physical(de, logical_block, &phys_block) < 0)
		{
			break;
		}

		// load new block if different
		if (fcb->cur_block != (int)phys_block)
		{
			if (fcb->dirty && fcb->cur_block >= 0)
			{
				// flush old block before switching
				if (flush_fcb_block(fcb, fcb->cur_block) < 0)
				{
					break;
				}
			}

			// load correct block
			if (load_block_into_fcb(fcb, phys_block) < 0)
			{
				break;
			}
		}

		int room = block_size - offset;
		int chunk = need;

		if (chunk > room)
		{
			chunk = room;
		}

		memcpy(fcb->buffer + offset, buffer + bytes_written, chunk);
		fcb->dirty = 1;

		fcb->file_pos += chunk;
		bytes_written += chunk;

		if (fcb->file_pos > de->size)
		{
			de->size = fcb->file_pos;
		}
	}

	// update meta info
	de->modified = time(NULL);
	de->accessed = de->modified;

	return bytes_written;
}

// Interface to read a buffer

// Filling the callers request is broken into three parts
// Part 1 is what can be filled from the current buffer, which may or may not be enough
// Part 2 is after using what was left in our buffer there is still 1 or more block
//        size chunks needed to fill the callers request.  This represents the number of
//        bytes in multiples of the blocksize.
// Part 3 is a value less than blocksize which is what remains to copy to the callers buffer
//        after fulfilling part 1 and part 2.  This would always be filled from a refill
//        of our buffer.
//  +-------------+------------------------------------------------+--------+
//  |             |                                                |        |
//  | filled from |  filled direct in multiples of the block size  | filled |
//  | existing    |                                                | from   |
//  | buffer      |                                                |refilled|
//  |             |                                                | buffer |
//  |             |                                                |        |
//  | Part1       |  Part 2                                        | Part3  |
//  +-------------+------------------------------------------------+--------+
int b_read(b_io_fd fd, char *buffer, int count)
{
	if (startup == 0)
		b_init(); // Initialize our system

	// check that fd is between 0 and (MAXFCBS-1)
	if ((fd < 0) || (fd >= MAXFCBS))
	{
		return (-1); // invalid file descriptor
	}

	b_fcb *fcb = &fcbArray[fd];
	dir_entry *de = fcb->de;

	if (!de)
		return -1;

	// refuse read on write-only
	if (fcb->flags & O_WRONLY)
		return -1;

	int file_size = de->size;
	int block_size = g_vcb->block_size;

	// EOF
	if (fcb->file_pos >= file_size)
		return 0;

	// clamp count
	int available = file_size - fcb->file_pos;
	if (count > available)
		count = available;

	int bytes_read = 0;

	while (bytes_read < count)
	{
		int logical_block = fcb->file_pos / block_size;
		int offset = fcb->file_pos % block_size;
		uint64_t phys_block;

		if (logical_to_physical(de, logical_block, &phys_block) < 0)
		{
			break;
		}

		// load correct block
		if (fcb->cur_block != (int)phys_block)
		{
			if (load_block_into_fcb(fcb, phys_block) < 0)
			{
				break;
			}
		}

		int room = block_size - offset;
		int remaining = count - bytes_read;
		int chunk = remaining;

		if (chunk > room)
		{
			chunk = room;
		}

		memcpy(buffer + bytes_read, fcb->buffer + offset, chunk);

		fcb->file_pos += chunk;
		bytes_read += chunk;
	}

	if (bytes_read > 0)
		de->accessed = time(NULL);

	return bytes_read;
}

// Interface to Close the file
int b_close(b_io_fd fd)
{
	if (startup == 0)
		b_init(); // Initialize our system

	if (fd < 0 || fd >= MAXFCBS)
		return -1;

	b_fcb *fcb = &fcbArray[fd];

	// flush if dirty
	if (fcb->dirty && fcb->cur_block >= 0)
	{
		flush_fcb_block(fcb, fcb->cur_block);
	}

	// free buffer
	if (fcb->buffer)
	{
		free(fcb->buffer);
		fcb->buffer = NULL;
	}

	// reset whole struct
	memset(fcb, 0, sizeof(b_fcb));

	return 0;
}

// helper function bodies
// return pointer to directory entry for path or NULL if not found
// dir_entry *fs_find_entry_by_path(const char *path)
path_result fs_find_entry_by_path(const char *path)
{
	path_result res = {0};

	// sanity check
	if (!path)
		// return NULL;
		return res;
	// temp storage for working path
	char tmp[PATH_MAX];
	strncpy(tmp, path, sizeof(tmp) - 1);
	tmp[sizeof(tmp) - 1] = '\0';

	// verify valid path
	ppinfo ppi;
	if (parse_path(tmp, &ppi) != 0)
		// return NULL;
		return res;

	res.parent = ppi.parent;
	res.index = ppi.index;
	// error checking
	if (ppi.index == -2)
	{
		// path was a directory reference to parent (root or parent returned)
		// return that reference
		// return &ppi.parent[0];
		return res;
	}
	// otherwise return file directory entry
	if (ppi.index >= 0) //&& ppi.parent)
	{
		res.entry = &ppi.parent[ppi.index];
		// return &ppi.parent[ppi.index];
		return res;
	}
	// return NULL;
	return res;
}

// create a new file entry and return a pointer to the directory entry
dir_entry *fs_create_entry_by_path(const char *path, dir_entry **out_parent)
{
	// sanity check
	if (!path)
		return NULL;

	// make a working copy of the path
	char tmp[PATH_MAX];
	strncpy(tmp, path, sizeof(tmp) - 1);
	tmp[sizeof(tmp) - 1] = '\0';

	// parse the path to find parent and leaf name
	ppinfo ppi;
	if (parse_path(tmp, &ppi) != 0)
		return NULL;
	if (!ppi.parent)
		return NULL;

	*out_parent = ppi.parent;

	const char *leaf = ppi.last_element_name;
	if (!leaf || !leaf[0])
		return NULL;

	// if an entry already exists, return it
	if (ppi.index >= 0)
		return &ppi.parent[ppi.index];

	// compute how many entries the parent directory can hold
	int parent_blocks = ppi.parent[0].location[0].num_blocks;
	if (parent_blocks <= 0)
		return NULL;

	int entries_per_block = g_vcb->block_size / (int)sizeof(dir_entry);
	int total_entries = parent_blocks * entries_per_block;

	if (total_entries <= 2)
		return NULL;

	// find a free slot skip . and ..
	int free_idx = -1;
	for (int i = 2; i < total_entries; i++)
	{
		if (!ppi.parent[i].is_used)
		{
			free_idx = i;
			break;
		}
	}

	if (free_idx < 0)
		return NULL;

	// create a new file entry in the parent directory
	dir_entry *e = &ppi.parent[free_idx];
	memset(e, 0, sizeof(*e));

	strncpy(e->name, leaf, FS_MAX_NAME - 1);
	e->name[FS_MAX_NAME - 1] = '\0';

	e->is_used = 1;
	e->is_dir = 0;
	e->size = 0;

	time_t t = time(NULL);
	e->created = t;
	e->accessed = t;
	e->modified = t;

	// initialize all extents as empty
	for (int k = 0; k < MAX_EXTENT; k++)
	{
		e->location[k].start_block = -1;
		e->location[k].num_blocks = 0;
	}

	// write the parent directory back to disk
	int p_start = ppi.parent[0].location[0].start_block;
	int p_blocks = ppi.parent[0].location[0].num_blocks;

	if (p_start < 0 || p_blocks <= 0)
	{
		e->is_used = 0;
		return NULL;
	}

	if (LBAwrite(ppi.parent, (uint64_t)p_blocks, (uint64_t)p_start) != (uint64_t)p_blocks)
	{
		fprintf(stderr, "Error: failed to write new file to disk.\n");
		e->is_used = 0;
		return NULL;
	}

	ppi.parent[0].size = p_blocks * g_vcb->block_size;

	for (int i = 2; i < total_entries; i++)
	{
		if (ppi.parent[i].is_used && ppi.parent[i].is_dir)
		{
			dir_entry *child = load_dir(&ppi.parent[i]);
			if (!child)
				continue;

			child[1].size = ppi.parent[0].size;
			child[1].location[0].start_block = ppi.parent[0].location[0].start_block;
			child[1].location[0].num_blocks = ppi.parent[0].location[0].num_blocks;

			uint64_t written = LBAwrite(child,
					 ppi.parent[i].location[0].num_blocks,
					 ppi.parent[i].location[0].start_block);
			if (written != (uint64_t)ppi.parent[i].location[0].num_blocks)
			{
				fprintf(stderr, "Error: failed to write new file to disk.\n");
				return NULL;
			}
			free(child);
		}
	}
	ppi.parent = load_dir(&ppi.parent[0]);
	*out_parent = ppi.parent;
	if (LBAwrite(g_vcb, 1, 0) != 1)
    {
        fprintf(stderr, "Error: failed to sync VCB in parent write.\n");
		return NULL;
    }
	return e;
}

// free extents and set size to 0
int fs_truncate_file(dir_entry *de)
{
	// sanity check directory entry exists
	if (!de)
		return -1;

	// track total blocks freed
	int total_freed = 0;

	// temporary extent array for freeing
	ext temp_extents[MAX_EXTENT];
	int temp_count = 0;

	// store file extent information to free the assoicated blocks
	for (int i = 0; i < MAX_EXTENT; i++)
	{
		if (de->location[i].start_block >= 0 && de->location[i].num_blocks > 0)
		{
			temp_extents[temp_count].start_block = de->location[i].start_block;
			temp_extents[temp_count].num_blocks = de->location[i].num_blocks;
			temp_count++;

			// clear directory entry extent information
			de->location[i].start_block = -1;
			de->location[i].num_blocks = 0;
		}
	}

	// free all blocks associated with the file
	if (temp_count > 0)
	{
		int freed = free_block(fsm, temp_extents, g_vcb);
		if (freed < 0)
		{
			fprintf(stderr, "Warning: failed to free some blocks in fs_truncate_file.\n");
		}
		else
		{
			total_freed += freed;
		}
	}

	// set file size to 0 and update time
	de->size = 0;
	de->modified = time(NULL);

	// ***NOTE***
	// caller must write the parent directory back to disk

	return total_freed; // return number of blocks freed
}

// writes a parent directory's updated contents back to disk
static int write_parent_dir(dir_entry *parent)
{
	// sanity check
	if (!parent)
		return -1;

	// get parent metadata
	int p_blocks = parent[0].location[0].num_blocks;
	int p_start = parent[0].location[0].start_block;

	// santiy check meta data
	if (p_blocks <= 0 || p_start < 0)
		return -1;

	// write directory entries back to disk
	int written = LBAwrite(parent, (uint64_t)p_blocks, (uint64_t)p_start);
	if (written != p_blocks)
	{
		fprintf(stderr, "Error: failed to write parent dir.\n");
		return -1;
	}

	// sync vcb
	if (LBAwrite(g_vcb, 1, 0) != 1)
	{
		fprintf(stderr, "Error: failed to sync VCB in parent write.\n");
		return -1;
	}

	return 0;
}

// verify file has capacity for a write
int ensure_file_capacity(b_fcb *fcb, int new_size)
{
	// load directory entry
	dir_entry *de = fcb->de;

	// find current blocks used and blocks required after write
	int current_blocks = (de->size + g_vcb->block_size - 1) / g_vcb->block_size;
	int required_blocks = (new_size + g_vcb->block_size - 1) / g_vcb->block_size;

	// if not enough blocks, allocate more
	while (current_blocks < required_blocks)
	{
		// get next available block from free space
		int new_block = find_first_free(fsm, g_vcb);

		// return error when there is no more space
		if (new_block < 0)
			return -1;

		// figure out witch ext complies to logical block we are allocating
		int extent_index = current_blocks == 0 ? 0 : find_extent_for_block(de, current_blocks);

		// load working ext
		ext *ex = &de->location[extent_index];

		// situations for unused ext, contiguous blocks, and starting a new ext
		if (ex->num_blocks == 0)
		{
			ex->start_block = new_block;
			ex->num_blocks = 1;
		}
		else if (ex->start_block + ex->num_blocks == new_block)
		{
			ex->num_blocks++;
		}
		else
		{
			extent_index++;
			if (extent_index >= MAX_EXTENT)
				return -1;

			ex = &de->location[extent_index];
			ex->start_block = new_block;
			ex->num_blocks = 1;
		}
		// update block count
		current_blocks++;
	}
	// success
	return 0;
}

// convert the logical block to physical for disk storage
int logical_to_physical(dir_entry *de, int logical_block, uint64_t *phys_block)
{
	// tracker for blocks that come before current ext
	int block_accum = 0;

	// iterate ext in directory entry
	for (int e = 0; e < MAX_EXTENT; e++)
	{
		ext *ex = &de->location[e];

		// no ext exist
		if (ex->num_blocks == 0)
			return -1;

		// verify logical block is within ext range
		if (logical_block < block_accum + ex->num_blocks)
		{
			// return physical block for updating file
			*phys_block = ex->start_block + (logical_block - block_accum);
			return 0;
		}

		block_accum += ex->num_blocks;
	}
	// return error
	return -1;
}

// read a block into buffer
int load_block_into_fcb(b_fcb *fcb, uint64_t phys_block)
{
	// load a block into the buffer
	if (LBAread(fcb->buffer, 1, phys_block) != 1)
    {
        fprintf(stderr, "Error: failed to read block from disk.\n");
        return -1;
    }

	// reset
	fcb->buflen = g_vcb->block_size;
	fcb->index = 0;
	fcb->cur_block = phys_block;
	fcb->cur_extent = find_extent_for_block(fcb->de, fcb->file_pos / g_vcb->block_size);
	return 0;
}

// append a new ext to an existing ext in the file
int append_extent_to_file(dir_entry *de, ext *newext)
{
	// step through each extent
	for (int i = 0; i < MAX_EXTENT; i++)
	{
		// skip unused slots
		if (newext[i].num_blocks == 0)
			continue;

		int new_start = newext[i].start_block;
		int new_len = newext[i].num_blocks;

		// tracker for sucessful merge
		int merged = 0;

		// attempt to merge with most recent available extent
		int last_used = -1;

		// find last non-empty extent
		for (int j = 0; j < MAX_EXTENT; j++)
		{
			if (de->location[j].num_blocks > 0)
				last_used = j;
		}

		if (last_used != -1)
		{
			ext *last = &de->location[last_used];

			// check if contiguous
			if (last->start_block + last->num_blocks == new_start)
			{
				// merge into final file extent
				last->num_blocks += new_len;
				merged = 1;
			}
		}

		if (merged)
			continue;

		// cannot merge ext for continuity, append to first free slot
		int appended = 0;

		for (int j = 0; j < MAX_EXTENT; j++)
		{
			if (de->location[j].num_blocks == 0)
			{
				// copy the new extent into the free slot
				de->location[j].start_block = new_start;
				de->location[j].num_blocks = new_len;
				appended = 1;
				break;
			}
		}

		if (!appended)
		{
			// no free slots returns failure
			return -1;
		}
	}

	return 0;
}

// flush dirty blocks to disk
int flush_fcb_block(b_fcb *fcb, uint64_t phys_block)
{
	if (!fcb->dirty)
		return 0;

	if (phys_block < 0)
		return -1;

	if (LBAwrite(fcb->buffer, 1, phys_block) != 1)
    {
        fprintf(stderr, "Error: failed to sync VCB in parent write.\n");
		return -1;
    }

	fcb->dirty = 0;

	return 0;
}

// find which extent contains the given logical block
int find_extent_for_block(dir_entry *de, int logical_block)
{
	// sanity check
	if (!de || logical_block < 0)
		return -1;

	// count blocks so far
	int block_accum = 0;

	for (int i = 0; i < MAX_EXTENT; i++)
	{
		ext *ex = &de->location[i];

		// skip unused extents
		if (ex->num_blocks == 0)
			continue;

		// found the extent containing the logical block
		if (logical_block < block_accum + ex->num_blocks)
			return i;

		block_accum += ex->num_blocks;
	}

	// block not found in any extent
	return -1;
}