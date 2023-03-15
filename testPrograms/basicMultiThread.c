#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <pthread.h>

int g = 0;
int *heap_storage;
pthread_mutex_t mutex_g = PTHREAD_MUTEX_INITIALIZER;

void *detector_malloc(size_t size) {
	return malloc(size);
}

void *myThreadFun(void *tid) {
	int myid = (int)tid;

	pthread_mutex_lock(&mutex_g);
	printf("heap_storage index 10: %d \n", heap_storage[10]);
	heap_storage[10] = 1;
	pthread_mutex_unlock(&mutex_g);
}

int main() {
	int i;
	pthread_t tid;

	heap_storage = detector_malloc(4096*sizeof(int));   // same, without repeating the type name

	int last_tid;
	// Let us create three threads
	for (i = 0; i < 3; i++)
		last_tid = pthread_create(&tid, NULL, myThreadFun, (void *)tid);
		// pthread_join(last_tid, NULL);

	// insures that the program doesn't quit before the threads didn't end
	// pthread_joind confuses Dynamorio and wait syscalls get blocked so this is an ugly solution
	for (i = 0; i <= 10000000; i++) {}
	return 0;
}
