#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <sthread.h>

/* TESTES DE MONITORES
 * duas threads tentam alterar o mesmo contador concorrentemente
 *
 * Joao Garcia, 2008 */



#define VALOR_PARAGEM 40

static sthread_mon_t mon = NULL;
static int i = 0;

void *thread_start(void *);

void print_inc(int id) {
	while(i<VALOR_PARAGEM) {
		sthread_monitor_enter(mon);
		printf("id=%i, i=%i\n", id, i);
		if (i%2 == id && i<VALOR_PARAGEM) {
			i++;
		}
		sthread_yield();
		sthread_monitor_exit(mon);
	}
}

int main(int argc, char **argv) {
	int arg = 1;
	printf("Testing sthread_mutex_*, impl: %s\n",
		   (sthread_get_impl() == STHREAD_PTHREAD_IMPL) ? "pthread" : "user");
	
	sthread_init();
	
	mon = sthread_monitor_init();
	
	if (sthread_create(thread_start, (void*)&arg) == NULL) {
		printf("sthread_create failed\n");
		exit(1);
	}
	print_inc(0);
	if (i == VALOR_PARAGEM) {
		printf("\nSUCESSO!\n");
	}
	return 1;
}

void *thread_start(void *arg) {
	int id = *((int *) arg);
	print_inc(id);
	return 0;
}
