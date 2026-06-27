/* 
 * SNFS Server
 * 
 * server.c
 *
 * PARTE 2 - Requisito 3: Sincronização eficiente
 *
 * Melhoria: readers-writers lock sobre o monitor do sthread.
 *   - Operações de leitura (ping, lookup, read, readdir) correm em PARALELO
 *   - Operações de escrita (write, create, mkdir, copy) têm EXCLUSÃO MÚTUA
 *     e esperam que todos os leitores activos terminem
 *
 * O buffer produtor/consumidor mantém o monitor original.
 * O rwlock é separado e só protege a execução dos handlers de FS.
 */

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/time.h>
#include <sys/select.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <stdlib.h>
#include <errno.h>
#include <sthread.h>
#ifdef USE_PTHREADS
#include <pthread.h>
#endif

#include <snfs_proto.h>
#include "snfs.h"

#ifndef SERVER_SOCK
#define SERVER_SOCK "/tmp/server.socket_1"
#endif

/* request descriptor */
struct _req {
   snfs_msg_req_t req;
   struct sockaddr_un cliaddr;
   int reqsz;
   socklen_t clilen;
};
typedef struct _req* req_t;

#define NUM_REQ_HANDLERS 8
#define NUM_TC 5
#define RING_SIZE 10

/* =====================================================================
 * Globals
 * ===================================================================== */

/* monitor para o buffer produtor/consumidor (igual ao original) */
static sthread_mon_t mon = NULL;
static int available_reqs;
req_t ring[RING_SIZE];
int sockfd;

/* -------------------------------------------------------------------
 * Readers-Writers lock implementado com monitor sthread
 *
 * Política: preferência a escritores (evita starvation de writes)
 *   - readers_active : nº de leitores a executar agora
 *   - writer_active  : 1 se um escritor está a executar
 *   - writers_waiting: nº de escritores à espera
 * ------------------------------------------------------------------- */
static sthread_mon_t rw_mon    = NULL;
static int readers_active   = 0;
static int writer_active    = 0;
static int writers_waiting  = 0;

static void rw_read_lock()
{
   sthread_monitor_enter(rw_mon);
   /* espera se há escritor activo ou escritores à espera (preferência a escritores) */
   while (writer_active || writers_waiting > 0)
      sthread_monitor_wait(rw_mon);
   readers_active++;
   sthread_monitor_exit(rw_mon);
}

static void rw_read_unlock()
{
   sthread_monitor_enter(rw_mon);
   readers_active--;
   if (readers_active == 0)
      sthread_monitor_signal(rw_mon); /* acorda escritores à espera */
   sthread_monitor_exit(rw_mon);
}

static void rw_write_lock()
{
   sthread_monitor_enter(rw_mon);
   writers_waiting++;
   while (writer_active || readers_active > 0)
      sthread_monitor_wait(rw_mon);
   writers_waiting--;
   writer_active = 1;
   sthread_monitor_exit(rw_mon);
}

static void rw_write_unlock()
{
   sthread_monitor_enter(rw_mon);
   writer_active = 0;
   sthread_monitor_signal(rw_mon); /* acorda leitores e escritores */
   sthread_monitor_exit(rw_mon);
}

/* =====================================================================
 * Tabela de serviços — indica se cada operação é leitura ou escrita
 * ===================================================================== */

#define OP_READ  0
#define OP_WRITE 1

struct {
   snfs_msg_type_t type;
   snfs_handler_t  handler;
   int             is_write; /* OP_READ ou OP_WRITE */
} Service[NUM_REQ_HANDLERS] = {
   {REQ_PING,    snfs_ping,    OP_READ},
   {REQ_LOOKUP,  snfs_lookup,  OP_READ},
   {REQ_READ,    snfs_read,    OP_READ},
   {REQ_WRITE,   snfs_write,   OP_WRITE},
   {REQ_CREATE,  snfs_create,  OP_WRITE},
   {REQ_MKDIR,   snfs_mkdir,   OP_WRITE},
   {REQ_READDIR, snfs_readdir, OP_READ},
   {REQ_COPY,    snfs_copy,    OP_WRITE}
};

/* =====================================================================
 * Buffer ring (igual ao original)
 * ===================================================================== */

req_t get_req() {
   static short int pos = 0;
   req_t temp = ring[pos];
   pos = (pos + 1) % RING_SIZE;
   return temp;
}

void put_req(req_t req) {
   static short int pos = 0;
   ring[pos] = req;
   pos = (pos + 1) % RING_SIZE;
}

/* =====================================================================
 * Socket helpers (igual ao original)
 * ===================================================================== */

int my_recvfrom(snfs_msg_req_t* req, struct sockaddr_un* cliaddr, socklen_t* clilen) {
   int reqsz;
   *clilen = sizeof(*cliaddr);
   do {
      errno = 0;
      reqsz = recvfrom(sockfd, (void*)req, sizeof(*req), MSG_DONTWAIT,
         (struct sockaddr*)cliaddr, clilen);
      if (errno == EAGAIN) sthread_yield();
   } while (errno == EAGAIN);
   return reqsz;
}

void srv_init_socket(struct sockaddr_un* servaddr)
{
   if ((sockfd = socket(AF_UNIX, SOCK_DGRAM, 0)) < 0) {
      printf("[snfs_srv] socket error: %s.\n", strerror(errno)); exit(-1);
   }
   bzero(servaddr, sizeof(*servaddr));
   servaddr->sun_family = AF_UNIX;
   strcpy(servaddr->sun_path, SERVER_SOCK);
   if (unlink(servaddr->sun_path) < 0 && errno != ENOENT) {
      printf("[snfs_srv] unlink error: %s.\n", strerror(errno)); exit(-1);
   }
   if (bind(sockfd, (struct sockaddr*)servaddr, sizeof(*servaddr)) < 0) {
      printf("[snfs_srv] bind error: %s.\n", strerror(errno)); exit(-1);
   }
}

int srv_recv_request(snfs_msg_req_t* req, struct sockaddr_un* cliaddr, socklen_t* clilen)
{
   int status = my_recvfrom(req, cliaddr, clilen);
   if (status == 0) { printf("[snfs_srv] request error.\n"); return 0; }
   if (status < 0)  { printf("[snfs_srv] recvfrom error: %s.\n", strerror(errno)); exit(-1); }
   return status;
}

void srv_send_response(snfs_msg_res_t* res, int ressz,
   struct sockaddr_un* cliaddr, socklen_t clilen)
{
   int status = sendto(sockfd, res, ressz, 0, (struct sockaddr*)cliaddr, clilen);
   if (status < 0) { printf("[snfs_srv] sendto error: %s.\n", strerror(errno)); exit(-1); }
   if (status != ressz) printf("[snfs_srv] message size mismatch.\n");
}

/* =====================================================================
 * Thread consumidora — agora usa rwlock para executar handlers
 * ===================================================================== */

void* thread_consumer() {
   req_t req_d;
   int ressz, req_i;
   snfs_msg_res_t res;

   while (1) {
      /* retirar pedido do buffer (monitor original) */
      sthread_monitor_enter(mon);
      while (!available_reqs) sthread_monitor_wait(mon);
      req_d = get_req();
      available_reqs--;
      sthread_monitor_signal(mon);
      sthread_monitor_exit(mon);

      memset(&res, 0, sizeof(res));

      /* encontrar handler */
      req_i = -1;
      for (int i = 0; i < NUM_REQ_HANDLERS; i++) {
         if (req_d->req.type == Service[i].type) { req_i = i; break; }
      }

      if (req_i == -1) {
         res.status = RES_UNKNOWN;
         ressz = sizeof(res) - sizeof(res.body);
         printf("[snfs_srv] unknown request.\n");
      } else {
         /* adquirir rwlock conforme o tipo de operação */
         if (Service[req_i].is_write == OP_WRITE) {
            rw_write_lock();
            Service[req_i].handler(&(req_d->req), req_d->reqsz, &res, &ressz);
            rw_write_unlock();
         } else {
            rw_read_lock();
            Service[req_i].handler(&(req_d->req), req_d->reqsz, &res, &ressz);
            rw_read_unlock();
         }
      }

      res.sn = req_d->req.sn;
      srv_send_response(&res, ressz, &(req_d->cliaddr), req_d->clilen);

      free(req_d); req_d = NULL;
      sthread_yield();
   }
}

/* =====================================================================
 * Thread produtora (igual ao original)
 * ===================================================================== */

void* thread_producer() {
   req_t req_d;

   while (1) {
      sthread_monitor_enter(mon);
      while (available_reqs == RING_SIZE) sthread_monitor_wait(mon);

      req_d = (req_t) malloc(sizeof(struct _req));
      memset(req_d, 0, sizeof(struct _req));

      if ((req_d->reqsz = srv_recv_request(&(req_d->req), &(req_d->cliaddr),
            &(req_d->clilen))) == 0) {
         free(req_d);
         sthread_monitor_exit(mon);
         continue;
      }

      put_req(req_d);
      available_reqs++;
      sthread_monitor_signal(mon);
      sthread_monitor_exit(mon);
      sthread_yield();
   }
}

/* =====================================================================
 * main 
 * ===================================================================== */

int main(int argc, char **argv)
{
   sthread_t threads[NUM_TC];
   sthread_t aux;
   int i;
   available_reqs = 0;

   snfs_init(argc, argv);

   struct sockaddr_un servaddr;
   srv_init_socket(&servaddr);

   sthread_init();

   /* monitor do buffer produtor/consumidor */
   mon    = sthread_monitor_init();

   /* monitor do rwlock */
   rw_mon = sthread_monitor_init();

   for (i = 0; i < NUM_TC; i++) {
      threads[i] = sthread_create(thread_consumer, (void*)NULL);
      if (threads[i] == NULL) {
         printf("Error while creating threads. Terminating...\n"); exit(-1);
      }
   }

   aux = sthread_create(thread_producer, (void*)NULL);

   sthread_join(aux, (void**)NULL);
   for (i = 0; i < NUM_TC; i++)
      sthread_join(threads[i], (void**)NULL);

   return 0;
}
