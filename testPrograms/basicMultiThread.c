#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <pthread.h>

// Let us create a global variable to change it in threads
int g = 0;
int *heap_storage;
pthread_mutex_t mutex_g = PTHREAD_MUTEX_INITIALIZER;

// The function to be executed by all threads
void *myThreadFun(void *tid) {
	// Store the value argument passed to this thread
	int myid = (int)tid;

	pthread_mutex_lock(&mutex_g);
	heap_storage[10] = 1;
	pthread_mutex_unlock(&mutex_g);

	// Let us create a static variable to observe its changes
	static int s = 0;

	// Change static and global variables
	++s; 
	++g;

	// Print the argument, static and global variables
	printf("Thread ID: %d, Static: %d, Global: %d\n", myid, ++s, ++g);
}

int main() {
	int i;
	pthread_t tid;

	heap_storage = malloc(4096*sizeof(int));   // same, without repeating the type name

	// Let us create three threads
	for (i = 0; i < 3; i++)
		pthread_create(&tid, NULL, myThreadFun, (void *)tid);

	pthread_exit(NULL);
	return 0;
}
