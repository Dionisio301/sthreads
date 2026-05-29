#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <sthread.h>

/* TESTES de MUTEXES 
 * verifica que o lock impede uma outra tarefa de avançar 
 * e que o unlock a liberta correctamente 
 *
 * Joao Garcia, 2008 */

static sthread_mutex_t mutex = NULL;
static int i = 0;
static int sucesso = 0;

void *thread_start(void *);

int main(int argc, char **argv) {
	int j=0;
	
	printf("Testando sthread_mutex_*, impl: %s\n",
		   (sthread_get_impl() == STHREAD_PTHREAD_IMPL) ? "pthread" : "user");
	
	sthread_init();
	
	mutex = sthread_mutex_init();
	
	printf("thread 1 do mutex\n");
	sthread_mutex_lock(mutex);
	printf("thread1 dentro do mutex\n");
	if (sthread_create(thread_start, (void*)NULL) == NULL) {
		printf("sthread_create falhou\n");
		exit(1);
	}
	sthread_yield();
	i = 1;
	sthread_mutex_unlock(mutex);
	printf("thread1 depois do mutex\n");
	for(j=0;j< 300000000;j++) {}
	if (sucesso==1) {
		printf("\nSUCESSO!\n");
	} else {
		printf("teste falhado...\n");
	}
	return 1;
}

void *thread_start(void *arg) {
	printf("thread2 antes do mutex\n");
	sthread_mutex_lock(mutex);
	printf("thread2 dentro do mutex\n");
	if (i==1) {
		sucesso = 1;
	}
	sthread_mutex_unlock(mutex);
	printf("thread2 depois do mutex\n");
	return 0;
}
