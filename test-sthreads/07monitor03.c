#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <sthread.h>

/* TESTES DE MONITORES
 * duas threads, uma bloqueia-se com wait, a outra liberta com signal 
 *
 * Joao Garcia, 2008 */



#define VALOR_PARAGEM 40

static sthread_mon_t mon = NULL;

void *thread_start(void *arg) {
	printf("thread filha antes do monitor\n");
	sthread_monitor_enter(mon);
	printf("thread filha dentro do monitor, antes do signal\n");
	sthread_monitor_signal(mon);
	printf("thread filha dentro do monitor, depois do signal\n");
	sthread_monitor_exit(mon);
	printf("thread filha depois do monitor\n");
	return NULL;
}

int main(int argc, char **argv) {
	int arg = 1;
	printf("Testing sthread_mutex_*, impl: %s\n",
		   (sthread_get_impl() == STHREAD_PTHREAD_IMPL) ? "pthread" : "user");
	
	sthread_init();
	
	mon = sthread_monitor_init();
	
	sthread_monitor_enter(mon);
	if (sthread_create(thread_start, (void*)&arg) == NULL) {
		printf("sthread_create failed\n");
		exit(1);
	}
	printf("thread principal vai bloquear-se\n");
	sthread_monitor_wait(mon);
	printf("thread principal desbloqueada\n");
	sthread_monitor_exit(mon);
	printf("\nSUCESSO!\n");
	return 1;
}
