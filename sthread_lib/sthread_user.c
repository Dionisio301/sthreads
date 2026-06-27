/* Simplethreads Instructional Thread Package
 * 
 * sthread_user.c - Implements the sthread API using user-level threads.
 *
 * Change Log:
 * 2002-04-15        rick
 *   - Initial version.
 * 2005-10-12        jccc
 *   - Added semaphores, deleted conditional variables
 * 2025             student
 *   - Extended with Linux 2.6.8.1-style O(1) scheduler
 */

#include <stdlib.h>
#include <assert.h>
#include <stdio.h>

#include <sthread.h>
#include <sthread_user.h>
#include <sthread_ctx.h>
#include <sthread_time_slice.h>
#include "queue.h"

/* =========================================================
 * PCB – Process Control Block de cada thread
 * ========================================================= */
struct _sthread {
  sthread_ctx_t          *saved_ctx; //guarda estado thread
  sthread_start_func_t    start_routine_ptr;
  void *args;
  long  wake_time;
  int   join_tid;
  void *join_ret;
  int  tid;
  int  priority;       /* prioridade actual (0=max, 14=min) */
  int  base_priority;  /* prioridade inicial (nunca muda)   */
  int  quantum;        /* ticks de CPU disponíveis nesta época */
  int  nice;           /* 0 a10,  penaliza a prioridade      */
};

/* ==========================================
 * Constantes do escalonador
 * ================================================*/
#define NUM_PRIORITIES  15   /* 0 a 4 estáticas, 5 a 14 dinâmicas */
#define QUANTUM_BASE     5
#define STATIC_MAX_PRIO  4   /* prioridades 0-4 são estáticas    */
#define DYN_MIN_PRIO     5   /* prioridades 5-14 são dinâmicas   */

/* ============================================
 * Runqueue: array de 15 filas FIFO, uma por nível de prio
 * ==================================================== */
typedef struct {
  queue_t *filas[NUM_PRIORITIES];
} runqueue_t;

/* Globais do escalonador */ 
static runqueue_t *active_rq;
static runqueue_t *expired_rq;
static queue_t *blocked_list;    /* todas as threads bloqueadas (monitores) */
static queue_t *dead_thr_list; //threads q terminaram
static queue_t *sleep_thr_list; //threads a dormir
static queue_t *join_thr_list; //threads a espera q outra termine
static queue_t *zombie_thr_list; //thread terminaram sem ninguem a espera
static struct _sthread *active_thr; // thread q esta a executar agora
static int    tid_gen;

#define CLOCK_TICK 100
static long Clock;

/* =========================================================
 * Auxiliares de runqueue
 * ========================================================= */
static runqueue_t *criar_runqueue(void) {
  runqueue_t *rq = malloc(sizeof(runqueue_t));
  int i;
  for (i = 0; i < NUM_PRIORITIES; i++)
    rq->filas[i] = create_queue();
  return rq;
}

static void rq_inserir(runqueue_t *rq, struct _sthread *t) {
  queue_insert(rq->filas[t->priority], t);
}

/* Retira a thread de maior prioridade (menor índice não vazio) */
static struct _sthread *rq_retirar(runqueue_t *rq) {
  int i;
  for (i = 0; i < NUM_PRIORITIES; i++)
    if (!queue_is_empty(rq->filas[i]))
      return queue_remove(rq->filas[i]);
  return NULL;
}

/* Retorna a PRIORIDADE mais alta (menor índice) com threads disponíveis,
   ou -1 se a runqueue estiver vazia */
static int rq_melhor_prio(runqueue_t *rq) {
  int i;
  for (i = 0; i < NUM_PRIORITIES; i++)
    if (!queue_is_empty(rq->filas[i]))
      return i;
  return -1;
}

static int rq_vazia(runqueue_t *rq) {
  return rq_melhor_prio(rq) == -1;
}

/* Troca as duas runqueues (O(1) — apenas ponteiros) */
static void trocar_runqueues(void) {
  runqueue_t *tmp = active_rq;
  active_rq  = expired_rq;
  expired_rq = tmp;
}

/* =========================================================
 * Cálculo de quantum e prioridade dinâmicos
 * ========================================================= */
static void recalcular(struct _sthread *t) {
  if (t->priority <= STATIC_MAX_PRIO)
    return; /* prioridade estática: não altera nada */

  int qpu = t->quantum; /* quantum ainda por usar nesta época */

  /* Quantum para a próxima época */
  int novo_q = QUANTUM_BASE + qpu / 2;
  if (novo_q < 1) novo_q = 1;
  t->quantum = novo_q;

  /* Nova prioridade para a próxima época */
  int nova_p = t->base_priority - qpu + t->nice;
  if (nova_p < DYN_MIN_PRIO) nova_p = DYN_MIN_PRIO;
  if (nova_p > 14)            nova_p = 14;
  t->priority = nova_p;
}

/* =========================================================
 * Inicialização
 * ========================================================= */
void sthread_aux_start(void) {
  splx(LOW);
  active_thr->start_routine_ptr(active_thr->args);
  sthread_user_exit((void *)0);
}

void sthread_user_dispatcher(void);

void sthread_user_init(void) {
  active_rq = criar_runqueue();
  expired_rq = criar_runqueue();
  blocked_list = create_queue();
  dead_thr_list = create_queue();
  sleep_thr_list = create_queue();
  join_thr_list = create_queue();
  zombie_thr_list = create_queue();
  tid_gen = 1;

  /* Thread principal */
  struct _sthread *main_thread = malloc(sizeof(struct _sthread)); //PCB thread principal
  main_thread->start_routine_ptr = NULL;
  main_thread->args = NULL;
  main_thread->saved_ctx = sthread_new_blank_ctx();
  main_thread->wake_time = 0;
  main_thread->join_tid = 0;
  main_thread->join_ret = NULL;
  main_thread->tid  = tid_gen++;
  main_thread->priority = 7;   /* prioridade dinâmica média */
  main_thread->base_priority = 7;
  main_thread->quantum  = QUANTUM_BASE;
  main_thread->nice  = 0;

  active_thr = main_thread;//definir thread principal como activa
  //arranque do relogio
  Clock = 1;
  sthread_time_slices_init(sthread_user_dispatcher, CLOCK_TICK);//func q configura o relogio do sistema/ a cada 100 ms chama dispatcher
}

/* =========================================================
 * Criação de threads
 * ========================================================= */

/* Versão interna com prioridade explícita */
sthread_t sthread_user_create_p(sthread_start_func_t start_routine, void *arg, int priority) {
  if (priority < 0) priority = 0;
  if (priority > 14) priority = 14;
//reservar memmoria para o PCB e inicializr campos
  struct _sthread *t = malloc(sizeof(struct _sthread));
  t->args = arg;
  t->start_routine_ptr = start_routine;
  t->wake_time = 0;
  t->join_tid = 0;
  t->join_ret = NULL;
  t->saved_ctx  = sthread_new_ctx(sthread_aux_start);
  t->priority  = priority;
  t->base_priority = priority;
  t->quantum  = QUANTUM_BASE;
  t->nice = 0;

  splx(HIGH);
  t->tid = tid_gen++;
  rq_inserir(active_rq, t);
  splx(LOW);
  return t;
}

/* Versão pública (API original sem priority) — usa prioridade dinâmica média */
threads_t sthread_user_create(sthread_start_func_t start_routine, void *arg) {
  return sthread_user_create_p(start_routine, arg, 7);//para o pcb da thread criada
}

/* =========================================
 * sthread_nice ,  modifica o nice da thread activa
 * Retorna a prioridade que a thread terá na próxima época
 * se esgotar o quantum agora.
 * ========================================================= */
int sthread_user_nice(int nice) {
  if (nice < 0)  nice = 0;
  if (nice > 10) nice = 10;
  active_thr->nice = nice;

  /* simula o cálculo para a próxima época */
  int qpu   = active_thr->quantum;
  int nova_p = active_thr->base_priority - qpu + nice;
  if (nova_p < DYN_MIN_PRIO) nova_p = DYN_MIN_PRIO;
  if (nova_p > 14)            nova_p = 14;
  return nova_p;
}

/* =========================================================
 * Exit
 * ============================================ */
void sthread_user_free(struct _sthread *thread);

void sthread_user_exit(void *ret) {
  splx(HIGH);

  int is_zombie = 1;

  /* Desbloqueia quem estava em join à espera desta thread */
  queue_t *tmp = create_queue();
  while (!queue_is_empty(join_thr_list)) {
    struct _sthread *t = queue_remove(join_thr_list);
    if (t->join_tid == active_thr->tid) {
      t->join_ret = ret;
      rq_inserir(active_rq, t);
      is_zombie = 0;
    } else {
      queue_insert(tmp, t);
    }
  }
  delete_queue(join_thr_list);
  join_thr_list = tmp;

  if (is_zombie)
    queue_insert(zombie_thr_list, active_thr);
  else
    queue_insert(dead_thr_list, active_thr);

  /* Sem mais nenhuma thread para executar → programa termina */
  if (rq_vazia(active_rq) && rq_vazia(expired_rq)) {
    printf("Todas as threads terminaram.\n");
    exit(0);
  }

  if (rq_vazia(active_rq))
    trocar_runqueues();

  struct _sthread *old = active_thr;
  active_thr = rq_retirar(active_rq);
  sthread_switch(old->saved_ctx, active_thr->saved_ctx);
  splx(LOW);
}

/* =========================================================
 * Dispatcher , chamado em cada tick do relógio
 * ========================================================= */
void sthread_user_dispatcher(void) {
  Clock++;

  /* Acorda threads que dormiram o tempo suficiente */
  queue_t *tmp = create_queue();
  while (!queue_is_empty(sleep_thr_list)) {
    struct _sthread *t = queue_remove(sleep_thr_list);
    if (t->wake_time <= Clock) {
      t->wake_time = 0;
      t->quantum   = QUANTUM_BASE;   /* recomeça com quantum base */
      rq_inserir(active_rq, t);
    } else {
      queue_insert(tmp, t);//vai para fila temporaria
    }
  }
  delete_queue(sleep_thr_list);
  sleep_thr_list = tmp;//so threads que nao devem acordar

  /* Decrementa quantum da thread activa */
  active_thr->quantum--;
//verificar se quantum aunda nao esgotou
  if (active_thr->quantum > 0) {
    /* Verificar preempção: existe thread activa com prioridade superior? */
    int melhor = rq_melhor_prio(active_rq);//qul melhor prioridade disponivel?
    if (melhor != -1 && melhor < active_thr->priority) {
      /* Preempção: volta para activos sem recalcular (quantum ainda disponível) */
      struct _sthread *old = active_thr;
      rq_inserir(active_rq, old);
      active_thr = rq_retirar(active_rq);
      sthread_switch(old->saved_ctx, active_thr->saved_ctx);
    }
    return;
  }

  /* Quantum esgotado: recalcula e move para expirados */
  active_thr->quantum = 0; /* garante que não fica negativo no recalculo */
  recalcular(active_thr);
  rq_inserir(expired_rq, active_thr);

  if (rq_vazia(active_rq))
    trocar_runqueues();

  struct _sthread *old = active_thr;
  active_thr = rq_retirar(active_rq);
  sthread_switch(old->saved_ctx, active_thr->saved_ctx);
}

/* ===============================================
 * Yield ,  cedência voluntária do CPU
 * Comportamento: bloquear como se fosse E/S 
 *  recalcula prioridade/quantum e vai para activos
 * ========================================================= */
void sthread_user_yield(void) {
  splx(HIGH);//desliga as interrupções

  struct _sthread *old = active_thr;

  /* Recalcula com o quantum ainda disponível (favorece threads que cederem cedo) */
  recalcular(old);

  /* Vai para activos (não expirados), porque cedeu voluntariamente */
  rq_inserir(active_rq, old);

  if (rq_vazia(active_rq)) {
    /* Só havia esta thread - continua */
    active_thr = rq_retirar(active_rq);
    if (active_thr == old) {
      splx(LOW);
      return;
    }
  }

  active_thr = rq_retirar(active_rq);
  sthread_switch(old->saved_ctx, active_thr->saved_ctx);
  splx(LOW);
}

/* ============================================
 * Free
 * ========================================================= */
void sthread_user_free(struct _sthread *thread) {
  sthread_free_ctx(thread->saved_ctx);
  free(thread);
}

/* =========================================================
 * sthread_dump — imprime estado completo do escalonador
 * ========================================================= */
void sthread_user_dump(void) {
  int i;
  queue_element_t *qe;

  printf("=== dump start ===\n");
  printf("active thread\n");
  printf("id: %d\n", active_thr->tid);
  printf("priority: %d\n", active_thr->priority);
  printf("quantum: %d\n", active_thr->quantum);

  printf("active runqueue\n");
  for (i = 0; i < NUM_PRIORITIES; i++) {
    printf("[%d]", i);
    qe = active_rq->filas[i]->first;
    while (qe != NULL) {
      printf(" %d,%d", qe->thread->tid, qe->thread->quantum);
      qe = qe->next;
    }
    printf("\n");
  }

  printf("expired runqueue\n");
  for (i = 0; i < NUM_PRIORITIES; i++) {
    printf("[%d]", i);
    qe = expired_rq->filas[i]->first;
    while (qe != NULL) {
      printf(" %d,%d", qe->thread->tid, qe->thread->quantum);
      qe = qe->next;
    }
    printf("\n");
  }

  printf("blocked list\n");
  qe = blocked_list->first;
  while (qe != NULL) {
    printf("%d,%d ", qe->thread->tid, qe->thread->quantum);
    qe = qe->next;
  }
  printf("\n=== dump end ===\n");
}

/* =======================================================
 * Join e Sleep
 * ========================================================= */
int sthread_user_join(sthread_t thread, void **value_ptr) {
  splx(HIGH);

  /* Verifica se a thread já terminou como zombie */
  int found = 0;
  queue_t *tmp = create_queue();
  while (!queue_is_empty(zombie_thr_list)) {
    struct _sthread *z = queue_remove(zombie_thr_list);
    if (z->tid == thread->tid) {
      *value_ptr = z->join_ret;
      queue_insert(dead_thr_list, z);
      found = 1;
    } else {
      queue_insert(tmp, z);
    }
  }
  delete_queue(zombie_thr_list);
  zombie_thr_list = tmp;

  if (found) { splx(LOW); return 0; }

  /* Procura a thread em todas as filas */
  if (active_thr->tid == thread->tid) found = 1;

  queue_element_t *qe;
  int i;
  for (i = 0; i < NUM_PRIORITIES && !found; i++) {
    qe = active_rq->filas[i]->first;
    while (qe && !found) { if (qe->thread->tid == thread->tid) found = 1; qe = qe->next; }
  }
  for (i = 0; i < NUM_PRIORITIES && !found; i++) {
    qe = expired_rq->filas[i]->first;
    while (qe && !found) { if (qe->thread->tid == thread->tid) found = 1; qe = qe->next; }
  }
  qe = sleep_thr_list->first;
  while (qe && !found) { if (qe->thread->tid == thread->tid) found = 1; qe = qe->next; }
  qe = join_thr_list->first;
  while (qe && !found) { if (qe->thread->tid == thread->tid) found = 1; qe = qe->next; }

  if (!found) { splx(LOW); return -1; }

  active_thr->join_tid = thread->tid;
  struct _sthread *old = active_thr;
  queue_insert(join_thr_list, old);

  if (rq_vazia(active_rq)) trocar_runqueues();

  active_thr = rq_retirar(active_rq);
  sthread_switch(old->saved_ctx, active_thr->saved_ctx);

  *value_ptr = thread->join_ret;
  splx(LOW);
  return 0;
}

int sthread_user_sleep(int time) {
  splx(HIGH);

  long num_ticks = 10L * time / CLOCK_TICK;
  if (num_ticks == 0) { splx(LOW); return 0; }

  active_thr->wake_time = Clock + num_ticks;
  queue_insert(sleep_thr_list, active_thr);

  struct _sthread *old = active_thr;

  if (rq_vazia(active_rq)) trocar_runqueues();

  active_thr = rq_retirar(active_rq);
  sthread_switch(old->saved_ctx, active_thr->saved_ctx);
  splx(LOW);
  return 0;
}

/* ================================================
 * Mutex
 * ========================================================= */
struct _sthread_mutex {
  lock_t          l;
  struct _sthread *thr;
  queue_t        *queue;
};

sthread_mutex_t sthread_user_mutex_init(void) {
  sthread_mutex_t lock = malloc(sizeof(struct _sthread_mutex));//reserva memoria para o mutex
  if (!lock) { printf("Error in creating mutex\n"); return NULL; }
  lock->l     = 0;
  lock->thr   = NULL;
  lock->queue = create_queue();//fila de espera vazia
  return lock; //devolve o mutex criado
}

void sthread_user_mutex_free(sthread_mutex_t lock) {
  delete_queue(lock->queue);
  free(lock);
}

void sthread_user_mutex_lock(sthread_mutex_t lock) {
  while (atomic_test_and_set(&(lock->l))) {}//verificar lock, se 0 muda a 1, se 1 fica em loop

  if (lock->thr == NULL) {
    lock->thr = active_thr;
    atomic_clear(&(lock->l));
  } else {
    /* Bloqueia: recalcula prioridade/quantum e vai para lista de bloqueados */
    recalcular(active_thr);
    queue_insert(lock->queue, active_thr);//fila de espera do mutex
    queue_insert(blocked_list, active_thr);//para o dump
    atomic_clear(&(lock->l));//liberar lock e baixo nivel, outras podem acessar

    splx(HIGH); //desligar interrupcoes
    struct _sthread *old = active_thr;
    if (rq_vazia(active_rq)) trocar_runqueues();//se nao ha threads activas, trocar runqueue
    active_thr = rq_retirar(active_rq);//tirar proxima thread
    sthread_switch(old->saved_ctx, active_thr->saved_ctx);//troca fisica. a actual dorme
    splx(LOW);//ligar
  }
}

void sthread_user_mutex_unlock(sthread_mutex_t lock) {
  if (lock->thr != active_thr) {//verificar se quem esta a fazr unlock dtm o mutex
    printf("unlock without lock!\n");
    return;
  }

  while (atomic_test_and_set(&(lock->l))) {}//adquirir o lock d baixo nivel para mudar mutex

  if (queue_is_empty(lock->queue)) {//se nao tiver ninguem a espera
    lock->thr = NULL;//mutex livre
  } else {
    struct _sthread *t = queue_remove(lock->queue);

    /* Remove da lista de bloqueados */
    queue_t *tmp = create_queue();
    while (!queue_is_empty(blocked_list)) {
      struct _sthread *b = queue_remove(blocked_list);
      if (b->tid != t->tid) queue_insert(tmp, b);
    }
    delete_queue(blocked_list);
    blocked_list = tmp;

    lock->thr = t;
    rq_inserir(active_rq, t);

    /* Preempção: se a thread desbloqueada tem maior prioridade */
    if (t->priority < active_thr->priority) {
      atomic_clear(&(lock->l));
      struct _sthread *old = active_thr;
      rq_inserir(active_rq, old);
      active_thr = rq_retirar(active_rq);
      sthread_switch(old->saved_ctx, active_thr->saved_ctx);
      return;
    }
  }

  atomic_clear(&(lock->l));
}

/* =========================================================
 * Monitor
 * ========================================================= */
struct _sthread_mon {
  sthread_mutex_t mutex;
  queue_t        *queue;
};

sthread_mon_t sthread_user_monitor_init(void) {
  sthread_mon_t mon = malloc(sizeof(struct _sthread_mon));
  if (!mon) { printf("Error creating monitor\n"); return NULL; }
  mon->mutex = sthread_user_mutex_init();
  mon->queue = create_queue();
  return mon;
}

void sthread_user_monitor_free(sthread_mon_t mon) {
  sthread_user_mutex_free(mon->mutex);
  delete_queue(mon->queue);
  free(mon);
}

void sthread_user_monitor_enter(sthread_mon_t mon) {
  sthread_user_mutex_lock(mon->mutex);
}

void sthread_user_monitor_exit(sthread_mon_t mon) {
  sthread_user_mutex_unlock(mon->mutex);
}

void sthread_user_monitor_wait(sthread_mon_t mon) {
  if (mon->mutex->thr != active_thr) {//verificar se a thread esta dentro do monitor
    printf("monitor wait called outside monitor\n");
    return;
  }

  /* Recalcula e regista como bloqueada */
  recalcular(active_thr);
  queue_insert(mon->queue, active_thr);
  queue_insert(blocked_list, active_thr);

  /* Liberta o monitor */
  sthread_user_mutex_unlock(mon->mutex);//libertar o monitor . Apos o wait o monitor deve ser liberto

  splx(HIGH);
  struct _sthread *old = active_thr;
  if (rq_vazia(active_rq)) trocar_runqueues();
  active_thr = rq_retirar(active_rq);
  sthread_switch(old->saved_ctx, active_thr->saved_ctx);
  splx(LOW);
}

void sthread_user_monitor_signal(sthread_mon_t mon) {
  if (mon->mutex->thr != active_thr) {
    printf("monitor signal called outside monitor\n");
    return;
  }

  while (atomic_test_and_set(&(mon->mutex->l))) {}

  if (!queue_is_empty(mon->queue)) {
    struct _sthread *t = queue_remove(mon->queue);

    /* Remove da lista de bloqueados */
    queue_t *tmp = create_queue();
    while (!queue_is_empty(blocked_list)) {
      struct _sthread *b = queue_remove(blocked_list);
      if (b->tid != t->tid) queue_insert(tmp, b);
    }
    delete_queue(blocked_list);
    blocked_list = tmp;

    /* Move para fila de espera do mutex (entra quando o mutex for libertado) */
    queue_insert(mon->mutex->queue, t);
  }

  atomic_clear(&(mon->mutex->l));
}

/* ================================================
 * Dummies para pthreads (não modificar)
 * ========================================================= */
sthread_mon_t sthread_dummy_monitor_init(void) {
  printf("WARNING: pthreads do not include monitors!\n"); return NULL;
}
void sthread_dummy_monitor_free(sthread_mon_t mon) {
  printf("WARNING: pthreads do not include monitors!\n");
}
void sthread_dummy_monitor_enter(sthread_mon_t mon) {
  printf("WARNING: pthreads do not include monitors!\n");
}
void sthread_dummy_monitor_exit(sthread_mon_t mon) {
  printf("WARNING: pthreads do not include monitors!\n");
}
void sthread_dummy_monitor_wait(sthread_mon_t mon) {
  printf("WARNING: pthreads do not include monitors!\n");
}
void sthread_dummy_monitor_signal(sthread_mon_t mon) {
  printf("WARNING: pthreads do not include monitors!\n");
}
