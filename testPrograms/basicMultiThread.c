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
	heap_storage[10] = 1;
	pthread_mutex_unlock(&mutex_g);
	printf("heap_storage index 10: %d \n", heap_storage[10]);
}

int main() {
	int i;
	pthread_t tid;

	heap_storage = detector_malloc(4096*sizeof(int));   // same, without repeating the type name

	// Let us create three threads
	for (i = 0; i < 3; i++)
		pthread_create(&tid, NULL, myThreadFun, (void *)tid);

	pthread_exit(NULL);
	return 0;
}
