/*
 * supermercado.c
 *
 * Problema do Supermercado com a biblioteca sthreads
 * e a nova política de escalonamento Linux 2.6.8.1.
 *
 * Duas caixas (filas), N clientes, M empregados.
 * Regras:
 *  - Cliente escolhe a fila com menos clientes.
 *  - Empregado atende a sua fila; se vazia, rouba da outra.
 *  - Se ambas vazias, o empregado bloqueia-se.
 */

#include <stdio.h>
#include <stdlib.h>
#include <sthread.h>


/* ── Parâmetros ── */
#define NUM_CAIXAS    2
#define NUM_CLIENTES  8
#define ATENDER_TICKS 3   /* ticks simulados para atender */
#define ESPERA_TICKS  2   /* ticks que cliente espera antes de sair */

/* ── Estado partilhado de cada fila ─ */
typedef struct {
  int          fila_id;
  int          num_clientes;
  sthread_mon_t mon;           /* monitor que protege esta fila */
} caixa_t;

static caixa_t caixas[NUM_CAIXAS];

/* ── Funções de simulação de tempo ─ */
static void Atender(int tempo) {
  int i;
  for (i = 0; i < tempo; i++)
    sthread_yield();
}

static void SerAtendido(int tempo) {
  int i;
  for (i = 0; i < tempo; i++)
    sthread_yield();
}

/* ── Escolha da fila pelo cliente ─ */
static int EscolherFila(void) {
  /* Escolhe a fila com menos clientes */
  return (caixas[0].num_clientes <= caixas[1].num_clientes) ? 0 : 1;
}

/* ── Thread do Cliente  */
void *cliente(void *arg) {
  int id = (int)(long)arg;
  int fila_id = EscolherFila();
  int tempo_atendimento = ATENDER_TICKS;

  printf("[Cliente %d] Escolheu fila %d\n", id, fila_id);

  sthread_monitor_enter(caixas[fila_id].mon);
  caixas[fila_id].num_clientes++;
  sthread_monitor_signal(caixas[fila_id].mon); /* acorda empregado se bloqueado */
  sthread_monitor_wait(caixas[fila_id].mon);   /* espera ser atendido */
  caixas[fila_id].num_clientes--;
  sthread_monitor_exit(caixas[fila_id].mon);

  SerAtendido(tempo_atendimento);
  printf("[Cliente %d] Foi atendido na fila %d\n", id, fila_id);

  /* Espera antes de sair */
  int i;
  for (i = 0; i < ESPERA_TICKS; i++)
    sthread_yield();

  return NULL;
}

/* ── Thread do Empregado ─── */
void *empregado(void *arg) {
  int fila_id  = (int)(long)arg;
  int outra    = 1 - fila_id;   /* a outra fila */

  printf("[Empregado %d] Começou a trabalhar na fila %d\n", fila_id, fila_id);

  while (1) {
    int atender_fila = -1;

    /* Primeiro tenta a sua fila */
    sthread_monitor_enter(caixas[fila_id].mon);
    while (caixas[fila_id].num_clientes == 0) {
      /* Tenta roubar da outra fila */
      sthread_monitor_exit(caixas[fila_id].mon);
      sthread_monitor_enter(caixas[outra].mon);
      if (caixas[outra].num_clientes > 0) {
        atender_fila = outra;
        break;
      }
      sthread_monitor_exit(caixas[outra].mon);

      /* Ninguém para atender , bloqueia na sua fila */
      sthread_monitor_enter(caixas[fila_id].mon);
      if (caixas[fila_id].num_clientes == 0) {
        printf("[Empregado %d] Bloqueado à espera de clientes\n", fila_id);
        sthread_monitor_wait(caixas[fila_id].mon);
        printf("[Empregado %d] Acordou\n", fila_id);
      }
    }

    if (atender_fila == -1) {
      atender_fila = fila_id;
    }

    /* Sinaliza o cliente que vai ser atendido */
    sthread_monitor_signal(caixas[atender_fila].mon);
    sthread_monitor_exit(caixas[atender_fila].mon);

    printf("[Empregado %d] A atender cliente na fila %d\n", fila_id, atender_fila);
    Atender(ATENDER_TICKS);
  }

  return NULL;
}

/* ── Main ────── */
int main(void) {
  int i;

  printf("=== Supermercado (sthreads + Linux 2.6.8.1 scheduler) ===\n");

  sthread_init();

  /* Inicializa as caixas */
  for (i = 0; i < NUM_CAIXAS; i++) {
    caixas[i].fila_id      = i;
    caixas[i].num_clientes = 0;
    caixas[i].mon          = sthread_monitor_init();
  }

  /* Lança os empregados (prioridade dinâmica baixa = 8) */
  for (i = 0; i < NUM_CAIXAS; i++)
    sthread_create_p(empregado, (void*)(long)i, 8);

  /* Lança os clientes (prioridade dinâmica = 7) */
  for (i = 0; i < NUM_CLIENTES; i++)
    sthread_create_p(cliente, (void*)(long)i, 7);

  /* Thread principal faz dump periódico e aguarda */
  int rounds = 0;
  while (rounds < 5) {
    sthread_yield();
    sthread_dump();
    rounds++;
  }

  printf("=== Main terminou ===\n");
  return 0;
}
