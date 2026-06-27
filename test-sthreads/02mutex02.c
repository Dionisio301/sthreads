#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <sthread.h>

/* TESTES de MUTEXES 
 * lock/unlock intervalado com yield 
 *
 * Joao Garcia, 2008 */


int main(int argc, char **argv) {
	sthread_mutex_t mutex = NULL;
	int i = 0;
	
	printf("Testing sthread_mutex_*, impl: %s\n",
		   (sthread_get_impl() == STHREAD_PTHREAD_IMPL) ? "pthread" : "user");
	
	sthread_init();
	
	mutex = sthread_mutex_init();
	while (i < 10) {
		sthread_mutex_lock(mutex);
		printf("i=%i\n",i);
		i++;
		sthread_yield();
		sthread_mutex_unlock(mutex);
	}
	printf("\nSUCESSO!\n");
	return 1;
}
