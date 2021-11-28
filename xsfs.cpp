/*
  FUSE: Filesystem in Userspace
  Copyright (C) 2001-2007  Miklos Szeredi <miklos@szeredi.hu>
*/

/** @file
 *
 * This minimal "filesystem" provides XSearch capabilities
 *
 * Compile with:
 *
 *     g++ -Wall xsfs.cpp `pkg-config fuse3 --cflags --libs` -o xsfs
 * 
 * Additional compiler flags for warnings:
 * 		-Wwrite-strings -Wformat-extra-args -Wformat=
 */

#define FUSE_USE_VERSION 31

#ifdef linux
/* For pread()/pwrite() */
#define _XOPEN_SOURCE 700
#endif

extern "C" {
	#include <fuse.h>
	#include <fuse_lowlevel.h>
}

#include <iostream>
// #include <cmath>
#include <cerrno>
#include <cstring>
#include <clocale>
#include <dirent.h>
#include <sys/time.h>
#include <unistd.h>
#include <stdio.h>
#include <sys/socket.h>
#include <stdlib.h>
#include <netinet/in.h>
#include <string.h>
#include <thread>
#include <atomic>
// #include <chrono>
#include <memory>

#include "ouroboroslib/include/ouroboros.hpp"

using namespace std;
using namespace ouroboros;

#define QUEUE_SIZE_RATIO 2
#define BLOCK_ADDON_SIZE 4096
#define BLOCK_SIZE 1024
#define PAGE_SIZE 4096
#define DELIMITERS " \t\n"
#define MAX_RESULTS 2

#define BUFFER_SIZE 1024
#define PORT 8080

/* XSearch functions */
// --------------------

void work_index(MemoryComponentManager* manager,
                atomic<long>* total_num_tokens,
                unsigned int queue_id,
                unsigned int index_id,
                int block_size)
{
	TFIDFIndexMemoryComponent* componentIndex;
	shared_ptr<BaseTFIDFIndex> index;
	FileDualQueueMemoryComponent* componentQueue;
	DualQueue<FileDataBlock*> *queue;
	FileDataBlock *dataBlock;
	BranchLessTokenizer *tokenizer;
	CTokBLock *tokBlock;
	char *buffer;
	char **tokens;
	char delims[32] = DELIMITERS;
	int length;

	// allocate the buffers, the list of tokens for the tokenizer data block and create the 
	buffer = new char[block_size + 1];
	tokens = new char*[block_size / 2 + 1];
	tokBlock = new CTokBLock();
	tokBlock->buffer = buffer;
	tokBlock->tokens = tokens;
	tokenizer = new BranchLessTokenizer(delims);

	// get the paged string store component identified by worker_id from the manager
	componentIndex = (TFIDFIndexMemoryComponent*) 
						manager->getMemoryComponent(MemoryComponentType::TFIDF_INDEX, index_id);
	// get the store from the store component
	index = componentIndex->getTFIDFIndex();
	// get the queue component identified by numa_id from the manager
	componentQueue = (FileDualQueueMemoryComponent*)
						manager->getMemoryComponent(MemoryComponentType::DUALQUEUE, queue_id);
	// get the queue from the queue component
	queue = componentQueue->getDualQueue();

	// load balancing is achieved through the queue
	while (true) {
		// pop full data block from the queue
		dataBlock = queue->pop_full();
		// if the data in the block has a length greater than 0 then tokenize, otherwise exit the while loop
		length = dataBlock->length;
		if (length > 0) {
			tokenizer->getTokens(dataBlock, tokBlock);

			for (long i = 0; i < tokBlock->numTokens; i++) {
				index->insert(tokBlock->tokens[i], dataBlock->fileIdx);
			}
		}
		
		queue->push_empty(dataBlock);

		if (length == -1) {
			break;
		}
	}

	delete tokenizer;
	delete tokBlock;
	delete[] tokens;
	delete[] buffer;

	*total_num_tokens += index->getNumTerms();
}

void work_read(MemoryComponentManager* manager,
               char *filename,
               unsigned int queue_id,
               int block_size)
{
	FileDualQueueMemoryComponent* componentQueue;
	FileIndexMemoryComponent* componentFileIndex;
	DualQueue<FileDataBlock*> *queue;
	shared_ptr<BaseFileIndex> index;
	WaveFileReaderDriver *reader;
	FileDataBlock *dataBlock;
	char delims[32] = DELIMITERS;
	int i, length;
	long fileIdx;

	// get the queue component identified by queue_id
	componentQueue = (FileDualQueueMemoryComponent*)
						manager->getMemoryComponent(MemoryComponentType::DUALQUEUE, queue_id);
	// get the queue from the queue component
	queue = componentQueue->getDualQueue();
	// get the file index component identified by queue_id
	componentFileIndex = (FileIndexMemoryComponent*)
							manager->getMemoryComponent(MemoryComponentType::FILE_INDEX, queue_id);
	// get the file index from the file index component
	index = componentFileIndex->getFileIndex();
	// create a new reader driver for the current file
	reader = new WaveFileReaderDriver((filename, block_size, BLOCK_ADDON_SIZE, delims);

	// try to open the file in the reader driver; if it errors out print message and terminate
	try {
		reader->open();
		fileIdx = index->insert(filename);
	} catch (exception &e) {
		cout << "ERR: could not open file " << filename << endl;
		delete reader;
		return;
	}

	while (true) {
		// pop empty data block from the queue
		dataBlock = queue->pop_empty();
		// read a block of data from the file into data block buffer
		reader->readNextBlock(dataBlock);
		dataBlock->fileIdx = fileIdx;
		length = dataBlock->length;
		// push full data block to queue (in this case it pushed to the empty queue since there is no consumer)
		queue->push_full(dataBlock);
		// if the reader driver reached the end of the file break from the while loop and read next file
		if (length == 0) {
			break;
			}
		}

		// close the reader and free memory
		reader->close();
		delete reader;
}

void work_init_indexes(MemoryComponentManager* manager,
                       unsigned int index_id,
                       long page_size,
                       long initial_capacity,
                       TFIDFIndexMemoryComponentType store_type)
{
    TFIDFIndexMemoryComponent* component;
	unsigned long numBuckets;
	size_t bucketSize = ChainedHashTable<const char*,
						PagedVersatileIndex<TFIndexEntry, IDFIndexEntry>*,
						cstr_hash,
						cstr_equal>::getBucketSizeInBytes();
	numBuckets = get_next_prime_number(initial_capacity / bucketSize);
	// create a new page string store component; the component is responsible with storing the terms sequentially
	component = new TFIDFIndexMemoryComponent(page_size, store_type, numBuckets);
	// add the string store component to the manager identified by the current worker_id
	manager->addMemoryComponent(MemoryComponentType::TFIDF_INDEX, index_id, component);
}

void work_init_queues(MemoryComponentManager* manager,
                      unsigned int queue_id,
                      int queue_size,
                      int block_size)
{
	FileDualQueueMemoryComponent* componentQueue;
	FileIndexMemoryComponent* componentFileIndex;

	// create a new queue component; the component is responsible with intializing the queue and the queue elements
	componentQueue = new FileDualQueueMemoryComponent(queue_size, block_size + BLOCK_ADDON_SIZE);
	// create a new file index component; the component is responsible for maintaining the indexes of file paths
	componentFileIndex = new FileIndexMemoryComponent(FileIndexMemoryComponentType::STD);
	// add the queue component to the manager, identified by the current queue_id
	manager->addMemoryComponent(MemoryComponentType::DUALQUEUE, queue_id, componentQueue);
	// add the file index component to the manager, identified by the current queue_id
	manager->addMemoryComponent(MemoryComponentType::FILE_INDEX, queue_id, componentFileIndex);
}

/* Server for XSearch queries */
// -----------------------------

void server() {

	char *msg = "Success!";
    int server_fd, new_socket, valread;
	struct sockaddr_in address;
	int opt = 1;
	int addrlen = sizeof(address);
	
	// Creating socket file descriptor
	if ((server_fd = socket(AF_INET, SOCK_STREAM, 0)) == 0)
	{
		perror("socket failed");
		exit(EXIT_FAILURE);
	}
	
	// Forcefully attaching socket to the port 8080
	if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR | SO_REUSEPORT,
												&opt, sizeof(opt)))
	{
		perror("setsockopt");
		exit(EXIT_FAILURE);
	}
	address.sin_family = AF_INET;
	address.sin_addr.s_addr = INADDR_ANY;
	address.sin_port = htons( PORT );
	
	// Forcefully attaching socket to the port 8080
	if (bind(server_fd, (struct sockaddr *)&address,
								sizeof(address)) < 0)
	{
		perror("bind failed");
		exit(EXIT_FAILURE);
	}
	if (listen(server_fd, 3) < 0)
	{
		perror("listen");
		exit(EXIT_FAILURE);
	}

    while ( 1 )
	{
		if ((new_socket = accept(server_fd, (struct sockaddr *)&address,
						(socklen_t*)&addrlen)) < 0)
		{
			perror("accept");
			exit(EXIT_FAILURE);
		}

		// create empty buffer every time
		char buffer[BUFFER_SIZE] = {0};
		valread = read(new_socket, buffer, BUFFER_SIZE);    
		std::cout << "[server] " << buffer << std::endl; 


		// TODO : process query 
 
		send(new_socket, msg, strlen(msg), 0);
    }

}

/* FUSE operations */
// ------------------

/* Get file attributes. Similar to stat() */
static int xs_getattr(const char *path, struct stat *stbuf,
			struct fuse_file_info *fi)
{
	printf( "[getattr] from (node) %s\n", path );

	(void) fi;
	int res;

	res = lstat(path, stbuf);
	if (res == -1)
		return - errno;

	return 0;
}

// static int xs_mknod(const char *path, mode_t mode, dev_t rdev);

static int xs_mkdir(const char *path, mode_t mode)
{
	printf( "[mkdir] at (dir) %s\n", path );

	int res;

	res = mkdir(path, mode);
	if (res == -1)
		return -errno;

	return 0;
}

/* Remove a file. Not related to symbolic/hard links */
static int xs_unlink(const char *path)
{
	printf( "[unlink] at (file) %s\n", path );

	int res;

	res = unlink(path);
	if (res == -1)
		return -errno;

	return 0;
}

static int xs_rmdir(const char *path)
{
	printf( "[rmdir] at (dir) %s\n", path );

	int res;

	res = rmdir(path);
	if (res == -1)
		return -errno;

	return 0;
}

static int xs_rename(const char *from, const char *to, unsigned int flags)
{
	printf( "[rename] %s to %s\n", from, to );

	int res;

	if (flags)
		return -EINVAL;

	res = rename(from, to);
	if (res == -1)
		return -errno;

	return 0;
}

static int xs_open(const char *path, struct fuse_file_info *fi)
{
	printf( "[open] (file) at %s\n", path );

	int res;

	res = open(path, fi->flags);
	if (res == -1)
		return -errno;

	fi->fh = res;
	return 0;
}

static int xs_read(const char *path, char *buf, size_t size, off_t offset,
		    struct fuse_file_info *fi)
{
	printf( "[read] %i bytes from (file) %s\n", size, path );

	int fd;
	int res;

	if(fi == NULL)
		fd = open(path, O_RDONLY);
	else
		fd = fi->fh;
	
	if (fd == -1)
		return -errno;

	res = pread(fd, buf, size, offset);
	if (res == -1)
		res = -errno;

	if(fi == NULL)
		close(fd);
	return res;
}

static int xs_write(const char *path, const char *buf, size_t size,
		    off_t offset, struct fuse_file_info *fi)
{
	printf( "[write] %i bytes in (file) %s\n", size, path );

	int fd;
	int res;

	(void) fi;
	if(fi == NULL)
		fd = open(path, O_WRONLY);
	else
		fd = fi->fh;
	
	if (fd == -1)
		return -errno;

	res = pwrite(fd, buf, size, offset);
	if (res == -1)
		res = -errno;

	if(fi == NULL)
		close(fd);
	return res;
}

static int xs_statfs(const char *path, struct statvfs *stbuf)
{
	printf( "[statfs] called\n", path );

	int res;

	res = statvfs(path, stbuf);
	if (res == -1)
		return -errno;

	return 0;
}

/* Release an open file when there are no more references to it */
static int xs_release(const char *path, struct fuse_file_info *fi)
{
	printf( "[release] (file) at %s\n", path );

	(void) path;
	close(fi->fh);
	return 0;
}

static int xs_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
		       off_t offset, struct fuse_file_info *fi,
		       enum fuse_readdir_flags flags)
{
	printf( "[readdir] at (dir) %s\n", path );

	DIR *dp;
	struct dirent *de;

	(void) offset;
	(void) fi;
	(void) flags;

	dp = opendir(path);
	if (dp == NULL)
		return -errno;

	while ((de = readdir(dp)) != NULL) {
		struct stat st;
		memset(&st, 0, sizeof(st));
		st.st_ino = de->d_ino;
		st.st_mode = de->d_type << 12;
	}

	closedir(dp);
	return 0;
}

static void *xs_init(struct fuse_conn_info *conn,
		      struct fuse_config *cfg)
{
	printf("[init] filesystem\n");

	(void) conn;
	cfg->use_ino = 1;

	/* Pick up changes from lower filesystem right away.
	   Also necessary for better hardlink support */
	cfg->entry_timeout = 0;
	cfg->attr_timeout = 0;
	cfg->negative_timeout = 0;

	return NULL;
}

/* Check file access permissions */
static int xs_access(const char *path, int mask)
{
	printf( "[access] to path %s\n", path );

	int res;

	res = access(path, mask);
	if (res == -1)
		return -errno;

	return 0;
}

static int xs_create(const char *path, mode_t mode,
		    struct fuse_file_info *fi)
{
	printf( "[create] (file) at %s\n", path );

	int res;

	res = open(path, fi->flags, mode);
	if (res == -1)
		return -errno;

	fi->fh = res;
	return 0;
}

/* Change the access and modification times of a file with ns resolution */
#ifdef HAVE_UTIMENSAT
static int xs_utimens(const char *path, const struct timespec ts[2],
		       struct fuse_file_info *fi)
{
	printf( "[utimens] called on %s\n", path );

	(void) fi;
	int res;

	/* don't use utime/utimes since they follow symlinks */
	res = utimensat(0, path, ts, AT_SYMLINK_NOFOLLOW);
	if (res == -1)
		return -errno;

	return 0;
}
#endif

/* Allocates space for an open file */
#ifdef HAVE_POSIX_FALLOCATE
static int xs_fallocate(const char *path, int mode,
			off_t offset, off_t length, struct fuse_file_info *fi)
{
	printf( "[fallocate] file %s\n", path );
	
	int fd;
	int res;

	(void) fi;

	if (mode)
		return -EOPNOTSUPP;

	if(fi == NULL)
		fd = open(path, O_WRONLY);
	else
		fd = fi->fh;
	
	if (fd == -1)
		return -errno;

	res = -posix_fallocate(fd, offset, length);

	if(fi == NULL)
		close(fd);
	return res;
}
#endif

/* Find next data or hole after the specified offset */
static off_t xs_lseek(const char *path, off_t off, int whence, struct fuse_file_info *fi)
{
	printf( "[lseek] %i at %s\n", whence, path );

	int fd;
	off_t res;

	if (fi == NULL)
		fd = open(path, O_RDONLY);
	else
		fd = fi->fh;

	if (fd == -1)
		return -errno;

	res = lseek(fd, off, whence);
	if (res == -1)
		res = -errno;

	if (fi == NULL)
		close(fd);
	return res;
}

/* Note: out-of-order designated initialization is supported 
   in the C programming language, but is not allowed in C++. */
static const struct fuse_operations xs_oper = {
	.getattr	= xs_getattr,
		// .mknod		= xs_mknod,
	.mkdir		= xs_mkdir,
	.unlink		= xs_unlink,
	.rmdir		= xs_rmdir,
	.rename		= xs_rename,
	.open		= xs_open,
	.read		= xs_read,
	.write		= xs_write,
	.statfs		= xs_statfs,
	.release	= xs_release,
	.readdir	= xs_readdir,
	.init       = xs_init,
	.access		= xs_access,
	.create 	= xs_create,
#ifdef HAVE_UTIMENSAT
	.utimens	= xs_utimens,
#endif
#ifdef HAVE_POSIX_FALLOCATE
	.fallocate	= xs_fallocate,
#endif
	.lseek		= xs_lseek,
};

int main(int argc, char *argv[])
{
    int num_readers = 2;
    int num_indexers = 2;
    int queue_size = QUEUE_SIZE_RATIO * num_indexers / num_readers;;
    int block_size = BLOCK_SIZE;
    long page_size = PAGE_SIZE;
    TFIDFIndexMemoryComponentType store_type = TFIDFIndexMemoryComponentType::STD;

	// the manager is used to store and provide access to NUMA sensitive components (i.e. the queues)
    MemoryComponentManager* manager;
    FileDualQueueMemoryComponent* component;
    DualQueue<FileDataBlock*> *queue;
    FileDataBlock *finalBlock;
    // list of threads that initialize the NUMA sensitive components
    vector<thread> threads_init;
    // list of threads that will read the contents of the input files
    vector<thread> threads_read;
    // list of threads that will tokenize the contents of the input files
    vector<thread> threads_tok;
	// create each queue in a specific NUMA node and add the queues to the manager
    manager = new MemoryComponentManager();

	for (int i = 0; i < num_readers; i++) {
        threads_init.push_back(thread(work_init_queues,
                                      manager,
                                      i,
                                      queue_size,
                                      block_size));
    }
    for (int i = 0; i < num_indexers; i++) {
        threads_init.push_back(thread(work_init_indexes,
                                      manager,
                                      i,
                                      page_size,
                                      total_size / num_indexers,
                                      store_type));
    }
    for (int i = 0; i < num_readers + num_indexers; i++) {
        threads_init[i].join();
    }

    // Launch server on separate thread 
    thread fooThread(server); 

	umask(0);
	return fuse_main(argc, argv, &xs_oper, NULL);
}