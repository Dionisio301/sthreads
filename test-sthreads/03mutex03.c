#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <sthread.h>

/* TESTES de MUTEXES 
 * duas threads tentam alterar o mesmo contador concorrentemente 
 *
 * Joao Garcia, 2008 */


static sthread_mutex_t mutex = NULL;
static int i = 0;

void *thread_start(void *);

void print_inc(int id) {
	while(i<40) {
		sthread_mutex_lock(mutex);
		printf("id=%i, i=%i\n", id, i);
		i++;
		sthread_yield();
		sthread_mutex_unlock(mutex);
	}
}

int main(int argc, char **argv) {
	int arg = 1;
	printf("Testing sthread_mutex_*, impl: %s\n",
		   (sthread_get_impl() == STHREAD_PTHREAD_IMPL) ? "pthread" : "user");
	
	sthread_init();
	
	mutex = sthread_mutex_init();
	
	if (sthread_create(thread_start, (void*)&arg) == NULL) {
		printf("sthread_create failed\n");
		exit(1);
	}
	print_inc(0);
	printf("\nSUCESSO!\n");
	return 1;
}

void *thread_start(void *arg) {
	int id = *((int *) arg);
	print_inc(id);
	return 0;
}
