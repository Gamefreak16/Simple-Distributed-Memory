#define _GNU_SOURCE
#include <sys/types.h>
#include <stdio.h>
#include <linux/userfaultfd.h>
#include <pthread.h>
#include <errno.h>
#include <unistd.h>
#include <stdlib.h>
#include <fcntl.h>
#include <signal.h>
#include <poll.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/ioctl.h>
#include <poll.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define errExit(msg)    do { perror(msg); exit(EXIT_FAILURE);	\
	} while (0)

static int page_size;
static char* msi;
static long PAGETOP;

static void *listen_thread(void* arg){
	ssize_t nread;
	int sock = (int) arg;
	char request;
	void* addr;
	char readbuf[page_size];
	int workPage;

	for(;;){
		memset(readbuf, 0, page_size);
		struct pollfd pollfd;
		int nready;
		pollfd.fd = sock;
		pollfd.events = POLLIN;
		nready = poll(&pollfd, 1, -1);
		if (nready == -1)
			errExit("poll");

		// nread = read(sock, &readbuf, 10);
		// printf("%s\n", readbuf);



		nread = read(sock, &readbuf, (7));//Constant sized request
		if (nread == 0) {
			printf("EOF on userfaultfd!\n");
			exit(EXIT_FAILURE);
		}

		request = *readbuf;
		memcpy(&addr, (readbuf+1), sizeof(void *));

		//printf("%c", request);

		workPage =(int)((int)(((char*)addr)-PAGETOP)/page_size);

		//request and address
		//requests: Fetch (sends page, both become S), 
		//Invalidate (this becomes I and calls smadvise, they are M)
		if(request == 'F'){
			//printf("Fetch Address recieved: %p\n", addr);



			if(msi[workPage] != 'I'){
				*readbuf = 'M';
				send(sock, readbuf, 1, 0);
				memcpy(readbuf, addr, page_size);
				send(sock,readbuf, page_size,0);
			}
			else if(msi[workPage] == 'I'){
				*readbuf = 'I';
				send(sock, readbuf, 1, 0);

			}


			msi[workPage] = 'S';
		}
		else if(request == 'I'){
			
			//printf("Invalidate Address recieved: %p\n", addr);
			madvise(addr, page_size, MADV_DONTNEED);
			msi[workPage] = 'I';
		}

	}
}

static void *fault_handler_thread(void *arg)
{
	static struct uffd_msg msg;   /* Data read from userfaultfd */
	long uffd;                    /* userfaultfd file descriptor */
	static char *page = NULL;
	struct uffdio_copy uffdio_copy;
	ssize_t nread;
	int sock;
	int workPage;
	char sendbuf[page_size];

	uffd = ((long*)arg)[0];
	sock = ((long*)arg)[1];
	free(arg);
	if (page == NULL) {
		page = mmap(NULL, page_size, PROT_READ | PROT_WRITE,
			    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		if (page == MAP_FAILED)
			errExit("mmap");
	}

	for (;;) {

		struct pollfd pollfd;
		int nready;
		pollfd.fd = uffd;
		pollfd.events = POLLIN;
		nready = poll(&pollfd, 1, -1);
		if (nready == -1)
			errExit("poll");
		
		nread = read(uffd, &msg, sizeof(msg));
		if (nread == 0) {
			printf("EOF on userfaultfd!\n");
			exit(EXIT_FAILURE);
		}

		printf(" [x] PAGEFAULT\n");

		if (nread == -1)
			errExit("read");

		if (msg.event != UFFD_EVENT_PAGEFAULT) {
			fprintf(stderr, "Unexpected event on userfaultfd\n");
			exit(EXIT_FAILURE);
		}

		workPage =(int)((int)(msg.arg.pagefault.address-PAGETOP)/page_size);
		memset(&sendbuf, 0, page_size);
		
		//printf("%p and %p\n", PAGETOP, msg.arg.pagefault.address);
		memset(page, 0, page_size);//Defaults to setting the page to 0's
		if(msi[workPage] == 'S' || msi[workPage] == 'M'){

		}
		else if(msi[workPage] == 'I'){//The fetch request
		//If page invalid, send fetch request, and copy returned page onto memory

			*sendbuf = 'F';
			memcpy((sendbuf+1), &msg.arg.pagefault.address, sizeof(void*));
			send(sock, sendbuf,(7), 0);
			read(sock, &sendbuf, 1);
			if(*sendbuf == 'I'){

				//Do nothing? Let it just become 0's
			}
			else if(*sendbuf == 'M'){
				read(sock, &sendbuf, page_size);
				memcpy(page, sendbuf, page_size);
			}
			msi[workPage] = 'S';
			
		}
		
		// char* hello = "Hello";
		// send(sock, hello, strlen(hello), 0);
		uffdio_copy.src = (unsigned long) page;
		uffdio_copy.dst = (unsigned long) msg.arg.pagefault.address &
			~(page_size - 1);
		uffdio_copy.len = page_size;
		uffdio_copy.mode = 0;
		uffdio_copy.copy = 0;

		if (ioctl(uffd, UFFDIO_COPY, &uffdio_copy) == -1)
			errExit("ioctl-UFFDIO_COPY");	
	}

}

int main(int argc, const char *argv[])
{
	int server_fd, new_socket;
	struct sockaddr_in address;
	int opt = 1;
	int addrlen = sizeof(address);
	unsigned long listenPort, connectPort;
	int first = 0;
	char buffer[256] = {0};
	int numPages;
	unsigned long pageLength;
	char *page_addr, *temp_addr;
	int readval;
    long uffd;         
	pthread_t thr;
	struct uffdio_api uffdio_api;
	struct uffdio_register uffdio_register;
    int threadret;
	int bool=1;
	char c;

	int connSock = 0;
	struct sockaddr_in serv_addr;

    if (argc != 3) {
		fprintf(stderr, "Usage: %s [Listening Port] [Connecting Port]\n", argv[0]);
		exit(EXIT_FAILURE);
	}

	listenPort = strtoul(argv[1], NULL, 0);
	connectPort = strtoul(argv[2], NULL, 0);

	if ((server_fd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
		perror("Server Socket failed");
		exit(EXIT_FAILURE);
	}
	if ((connSock = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
		printf("\n Connection Socket creation error \n");
		return -1;
	}


	if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR | SO_REUSEPORT,
		       &opt, sizeof(opt))) {
		perror("setsockopt");
		exit(EXIT_FAILURE);
	}
	address.sin_family = AF_INET;
	address.sin_addr.s_addr = INADDR_ANY;
	address.sin_port = htons(listenPort);
	if (bind(server_fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
		perror("bind failed");
		exit(EXIT_FAILURE);
	}
	if (listen(server_fd, 3) < 0) {
		perror("listen");
		exit(EXIT_FAILURE);
	}

	memset(&serv_addr, '0', sizeof(serv_addr));
	serv_addr.sin_family = AF_INET;
	serv_addr.sin_port = htons(connectPort);

	if(inet_pton(AF_INET, "127.0.0.1", &serv_addr.sin_addr) <= 0) {
		printf("\nInvalid address/ Address not supported \n");
		return -1;
	}

	if (connect(connSock, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
		first = 1;
		if ((new_socket = accept(server_fd, (struct sockaddr *)&address,
				 (socklen_t*)&addrlen)) < 0) {
			perror("accept");
			exit(EXIT_FAILURE);
		}
		
	}
	//printf("first: %d\n", first);
	page_size = sysconf(_SC_PAGE_SIZE);
	char write_buffer[page_size];

	if(!first){

		if ((new_socket = accept(server_fd, (struct sockaddr *)&address,
				 (socklen_t*)&addrlen)) < 0) {
			perror("accept");
			exit(EXIT_FAILURE);
		}
		readval = read(new_socket, buffer, (7));
		numPages = (int) *buffer;
		pageLength = numPages * page_size;
		// for(int i = 0; i < 8; i++){
		// 	printf("Value buf : %d\n", buffer[i]);
		// }
		
		memcpy(&page_addr, (buffer+1), sizeof(void *));
		page_addr = mmap(page_addr, pageLength, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		//write_buffer = malloc(pageLength);
		printf("Process 2:\n\tmmap'd region : %p\n\tmmap'd size : %lu\n", page_addr, pageLength);
	}
	else{
		while(bool){
			bool = 0;
			if (connect(connSock, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
				bool = 1;
			}
		}
	}
	


	uffd = syscall(__NR_userfaultfd, O_CLOEXEC | O_NONBLOCK);
	if (uffd == -1)
		errExit("userfaultfd");

	uffdio_api.api = UFFD_API;
	uffdio_api.features = 0;
	if (ioctl(uffd, UFFDIO_API, &uffdio_api) == -1)
		errExit("ioctl-UFFDIO_API");
		


	if(first){

		printf(" > How many pages would you like to allocate (greater than 0)? ");
		fflush(stdout);
		readval = read(0, buffer, 256);
		numPages = atoi((char*)buffer);
		if(numPages <= 0) return -1;
		
		
		pageLength = numPages * page_size;
		//write_buffer[page_size] = {0};

		*buffer = numPages;
		page_addr = mmap(NULL, pageLength, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

		printf("Process 1:\n\tmmap'd region : %p\n\tmmap'd size : %lu\n", page_addr, pageLength);

		

		memcpy((buffer+1), &page_addr, sizeof(void *));
		// for(int i = 0; i < 8; i++){
		// 	printf("Value buf : %d\n", buffer[i]);
		// }
		
		send(connSock, buffer, (7), 0);
	}
	PAGETOP = page_addr;
    char te[numPages];
    msi = te;
	for(int i = 0; i < numPages; i++){
		msi[i] = 'I';
	}
    
	bool = 1;

	uffdio_register.range.start = (unsigned long) page_addr;
	uffdio_register.range.len = pageLength;
	uffdio_register.mode = UFFDIO_REGISTER_MODE_MISSING;
	if (ioctl(uffd, UFFDIO_REGISTER, &uffdio_register) == -1)
		errExit("ioctl-UFFDIO_REGISTER");

	long* items = (long*) malloc(2*sizeof(long));
	items[0] = uffd;
	items[1] = connSock;
	threadret = pthread_create(&thr, NULL, fault_handler_thread, (void *) items);
	if (threadret != 0) {
		errno = threadret;
		errExit("pthread_create");
	}

	threadret = pthread_create(&thr, NULL, listen_thread, (void *) new_socket);
	if (threadret != 0) {
		errno = threadret;
		errExit("pthread_create");
	}
	

	for(;;){
		printf(" > Which command should I run? (r:read, w:write v:view msi array): ");
		fflush(stdout);
		
		memset(buffer, 0, 256);
		//printf("Before Num: %d Char: %d    %d", readval, *buffer, 'r');
		readval = read(0, buffer, 256);
		c = *buffer;
		//printf("Read: %d   Matching: %d", c, 'r');
		
		if(readval != 2){
			printf("\nPlease enter either 'r' for read or 'w' for write.\n");
		}
		else if(c == 'r'){

			printf(" > For which page? (0-%i, or -1 for all): ", numPages-1);
			fflush(stdout);
			memset(buffer, 0, 256);
			readval = read(0, buffer, 256);
			if(readval <= 1) {}
			readval = strtol(buffer, &temp_addr, 10);
			if((char *)buffer == temp_addr || readval > numPages-1 || readval < -1){errno = 0; printf("Invalid Page\n");}
			else{
				
				temp_addr = page_addr;
				memset(write_buffer, 0, page_size);

				if(readval == -1){
					for(int i = 0; i < numPages; i++){
						memcpy(write_buffer, temp_addr, page_size);
						printf(" [*] Page %i:\n%s\n",i,write_buffer);
						temp_addr += page_size;
					}
						
				}
				else{
					temp_addr += page_size*readval;
					memcpy(write_buffer, temp_addr, page_size);

					printf(" [*] Page %i:\n%s\n",readval,write_buffer);
				}
			}

		}
		else if( c == 'w'){
			
			printf(" > For which page? (0-%i, or -1 for all): ", numPages-1);
			fflush(stdout);
			memset(buffer, 0, 256);
			readval = read(0, buffer, 256);
			if(readval <= 1) {}
			bool = strtol((char *)buffer, NULL, 10);
			if(buffer == temp_addr || bool > numPages-1 || bool < -1 || readval <= 1){errno = 0; printf("\nInvalid Page\n");}
			else{
				temp_addr = page_addr;
				memset(buffer, 0, 256);
				*buffer = 'I';
				
				printf(" > Type your new message: ");
				fflush(stdout);
				memset(write_buffer, 0, page_size);

				if(bool == -1){
					readval = read(0, write_buffer, page_size);
					write_buffer[readval] = '\0';
					write_buffer[page_size-1] = '\0';

					for(int i = 0; i < numPages; i++){
					
						memcpy(temp_addr, write_buffer, page_size);

						printf(" [*] Page %i:\n%s\n",i, temp_addr);

						memcpy((buffer+1), &temp_addr, sizeof(void*));
						send(connSock, buffer,(7), 0);
						msi[i] = 'M';
						//1 + sizeof(char *)*6
						temp_addr += page_size;
					}
					
				}
				else{
					readval = read(0, write_buffer, page_size);
					write_buffer[readval] = '\0';
					write_buffer[page_size-1] = '\0';
					msi[bool]='M';
					temp_addr += page_size*bool;
					memcpy(temp_addr, write_buffer, page_size);
					memcpy((buffer+1), &temp_addr, sizeof(void*));
					send(connSock, buffer,(7), 0);

					printf(" [*] Page %i:\n%s\n",bool, temp_addr);
				}
				//Send the invalidate message from here, as it was modified



			}
		}
		else if( c == 'v'){
			printf(" [*] MSI Array:\n\t[");

			for(int i = 0; i < numPages; i++){
				printf("%c", msi[i]);
				if(i < numPages-1){
					printf(", ");
				}
			}
			printf("]\n");
		}
		else{
			printf("\nPlease enter either 'r' for read, 'w' for write, or 'v' to view MSI array.\n");
		}
	}

	return 0;
}