#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <sthread.h>

/* TESTES DE MONITORES
 * sequencia de enter/exit 
 * 
 * Joao Garcia, 2008 */



int main(int argc, char **argv) {
	sthread_mon_t mon = NULL;
	int i = 0;
	
	printf("Testando sthread_monitor_*, impl: %s\n",
		   (sthread_get_impl() == STHREAD_PTHREAD_IMPL) ? "pthread" : "user");
	
	sthread_init();
	
	mon = sthread_monitor_init();
	while (i < 10) {
		sthread_monitor_enter(mon);
		printf("i=%i\n",i);
		i++;
		sthread_monitor_exit(mon);
	}
	printf("\nSUCESSO!\n");
	return 1;
}
