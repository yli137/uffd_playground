#include <stdlib.h>
#include <stdint.h>
#include <sched.h>
#include <sys/types.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <linux/userfaultfd.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <assert.h>
#include <string.h>
#include <pthread.h>
#include <sys/ioctl.h>

// Structure to pass to the fault handler thread
struct fault_handler_args {
    int uffd;
    size_t length;
    void* address;
};

static void *handler(void *arg)
{
	printf("Handler thread is running on CPU core: %d\n", sched_getcpu());
	struct fault_handler_args* fargs = (struct fault_handler_args*) arg;
	struct uffd_msg msg;
	ssize_t nread;

	struct pollfd pollfd;
	pollfd.fd = fargs->uffd;
	pollfd.events = POLLIN;
				
	int page_size = 4096;
	// Allocate and map a new page
	void *new_page = mmap(NULL, page_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (new_page == MAP_FAILED) {
		perror("mmap");
		exit(EXIT_FAILURE);
	}

	int iter = 0;
	while (poll(&pollfd, 1, -1) > 0) {
		printf("iteration %d\n", iter++);
		nread = read(fargs->uffd, &msg, sizeof(msg));
		
		if (nread == 0 || nread == -1) {
			perror("Error reading userfaultfd event");
			continue;
		}

		// Handle the write fault
		if (msg.event == UFFD_EVENT_PAGEFAULT) {
			//printf("fault flags %lld Write %d WP %d MINOR %d Write+WP %d\n",
			//		msg.arg.pagefault.flags, 
			//		UFFD_PAGEFAULT_FLAG_WRITE,
			//		UFFD_PAGEFAULT_FLAG_WP,
			//		UFFD_PAGEFAULT_FLAG_MINOR,
			//		UFFD_PAGEFAULT_FLAG_WRITE | UFFD_PAGEFAULT_FLAG_WP);
			
			if (msg.arg.pagefault.flags == UFFD_PAGEFAULT_FLAG_WP) {
				unsigned long fault_address = msg.arg.pagefault.address;

				struct uffdio_writeprotect uffdio_wp;
				uffdio_wp.range.start = fault_address & ~(page_size - 1);
				uffdio_wp.range.len = page_size;
				uffdio_wp.mode = 0;

				if (ioctl(fargs->uffd, UFFDIO_WRITEPROTECT, &uffdio_wp) == -1) {
					perror("UFFDIO_WRITEPROTECT");
					exit(EXIT_FAILURE);
				}

				uffdio_wp.mode = UFFDIO_WRITEPROTECT_MODE_WP;
				if (ioctl(fargs->uffd, UFFDIO_WRITEPROTECT, &uffdio_wp) == -1) {
					perror("UFFDIO_WRITEPROTECT (reapply)");
					exit(EXIT_FAILURE);
				}

			} else if (msg.arg.pagefault.flags == (UFFD_PAGEFAULT_FLAG_WP | UFFD_PAGEFAULT_FLAG_WRITE)){
				unsigned long fault_address = msg.arg.pagefault.address;

				struct uffdio_writeprotect uffdio_wp;
				uffdio_wp.range.start = fault_address & ~(page_size - 1);
				uffdio_wp.range.len = page_size;
				uffdio_wp.mode = 0;

				if (ioctl(fargs->uffd, UFFDIO_WRITEPROTECT, &uffdio_wp) == -1) {
					perror("UFFDIO_WRITEPROTECT");
					exit(EXIT_FAILURE);
				}
				
				usleep(1);
				uffdio_wp.mode = UFFDIO_WRITEPROTECT_MODE_WP;
				if (ioctl(fargs->uffd, UFFDIO_WRITEPROTECT, &uffdio_wp) == -1) {
					perror("UFFDIO_WRITEPROTECT");
					exit(EXIT_FAILURE);
				}

			} else if (msg.arg.pagefault.flags == UFFD_PAGEFAULT_FLAG_WRITE){
				unsigned long fault_address = msg.arg.pagefault.address;

				// Allocate a new page to handle the missing fault (which may be due to a write access)
				void *new_page = mmap(NULL, page_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
				if (new_page == MAP_FAILED) {
					perror("mmap");
					exit(EXIT_FAILURE);
				}

				// Optionally, initialize the page (e.g., zero-fill the page)
				memset(new_page, 0, page_size);

				// Copy the page to the faulting address using UFFDIO_COPY
				struct uffdio_copy uffdio_copy;
				uffdio_copy.src = (unsigned long)new_page;
				uffdio_copy.dst = fault_address & ~(page_size - 1);  // Align to page boundary
				uffdio_copy.len = page_size;
				uffdio_copy.mode = 0;

				if (ioctl(fargs->uffd, UFFDIO_COPY, &uffdio_copy) == -1) {
					perror("UFFDIO_COPY");
					exit(EXIT_FAILURE);
				}

				// Free the temporary page after copying
				munmap(new_page, page_size);


				struct uffdio_writeprotect uffdio_wp;
				uffdio_wp.range.start = fault_address & ~(page_size - 1);
				uffdio_wp.range.len = page_size;
				uffdio_wp.mode = UFFDIO_WRITEPROTECT_MODE_WP;

				if (ioctl(fargs->uffd, UFFDIO_WRITEPROTECT, &uffdio_wp) == -1) {
					perror("UFFDIO_WRITEPROTECT");
					exit(EXIT_FAILURE);
				}


			} else {
				unsigned long fault_address = msg.arg.pagefault.address;

				// Optionally initialize the page (e.g., zero-fill or load data)
				memset(new_page, 0, page_size);  // Zero-fill the page

				// Copy the page to the faulting address using UFFDIO_COPY
				struct uffdio_copy uffdio_copy;
				uffdio_copy.src = (unsigned long)new_page;
				uffdio_copy.dst = fault_address & ~(page_size - 1);  // Align to page boundary
				uffdio_copy.len = page_size;
				uffdio_copy.mode = 0;
				uffdio_copy.copy = 0;

				if (ioctl(fargs->uffd, UFFDIO_COPY, &uffdio_copy) == -1) {
					perror("UFFDIO_COPY");
					exit(EXIT_FAILURE);
				}

				struct uffdio_writeprotect uffdio_wp;
				uffdio_wp.range.start = fault_address & ~(page_size - 1);
				uffdio_wp.range.len = page_size;
				uffdio_wp.mode = UFFDIO_WRITEPROTECT_MODE_WP;
				if (ioctl(fargs->uffd, UFFDIO_WRITEPROTECT, &uffdio_wp) == -1) {
					perror("UFFDIO_WRITEPROTECT (reapply)");
					exit(EXIT_FAILURE);
				}
			}
		}
	}

	return NULL;
}


int main(int argc, char **argv)
{
	printf("Main thread is running on CPU core: %d\n", sched_getcpu());
	// Step 1: Create a userfaultfd object
	int uffd = syscall(SYS_userfaultfd, O_CLOEXEC | O_NONBLOCK);
	assert(uffd != -1);

	int region_size = 4096;
	int *data = (int*)mmap(NULL, region_size, PROT_READ | PROT_WRITE,
			MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if(data == MAP_FAILED){
		fprintf(stderr, "mapping failed\n");
		exit(0);
	}

	// Step 2: Register the memory with userfaultfd
	struct uffdio_api uffdio_api;
	uffdio_api.api = UFFD_API;
	uffdio_api.features = UFFD_FEATURE_PAGEFAULT_FLAG_WP;
	assert(ioctl(uffd, UFFDIO_API, &uffdio_api) != -1);

	// Step 3: set up address and flags
	struct uffdio_register uffdio_register;
	uffdio_register.range.start = (unsigned long)data;
	uffdio_register.range.len = 4096;
	uffdio_register.mode = UFFDIO_REGISTER_MODE_MISSING | UFFDIO_REGISTER_MODE_WP; 

	// if missing is not registered, when writing to a missing page, WP will not be caught
	// uffdio_register.mode = UFFDIO_REGISTER_MODE_WP; 

	assert(ioctl(uffd, UFFDIO_REGISTER, &uffdio_register) != -1);

	// Step 4: Spawn a thread to handle page faults
	pthread_t uffd_thread;
	struct fault_handler_args args;
	args.uffd = uffd;
	args.length = 4096;
	args.address = (void*)data;

	assert(pthread_create(&uffd_thread, NULL, handler, &args) == 0);

	cpu_set_t cpuset;
	CPU_ZERO(&cpuset);
	CPU_SET(16, &cpuset);

	printf("start writing data\n");


	for(int j = 0; j < 1000000; j++){
		for(int i = 0; i < 1000; i++){
			if(data[i] > 1000)
				data[i] = 0;
			data[i] += 1;
			for(int k = 0; k < 100000; k++)
				data[i] += (int)(4.0 / 3.27 + 9.99 / 1.24);
		}
	}
	printf("done\n");

	printf("Verify data\n");
	for(int i = 0; i < 10; i++){
		printf("%d ", data[i]);
	}
	printf("\ndone\n");

	return 0;
}

