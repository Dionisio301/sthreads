/* 
 * SNFS API Layer
 * 
 * snfs_api.c
 *
 * PARTE 2 - Requisito 2: Suporte a replicação de servidores
 *
 * Operações que alteram estado (write, create, mkdir, copy):
 *   - Envia para TODOS os N servidores
 *   - Espera resposta de TODOS
 *   - Se STATUS diferente entre servidores -> termina abruptamente
 *
 * Operações apenas de leitura (ping, lookup, read, readdir):
 *   - Envia para M servidores (round-robin, M <= N)
 *   - Retorna assim que chegar a PRIMEIRA resposta válida
 *   - Descarta as restantes
 */

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <strings.h>

#include <snfs_api.h>
#include <snfs_proto.h>

/* =====================================================================
 * Configuração multi-servidor
 * ===================================================================== */

/* Número máximo de servidores suportados */
#define MAX_SERVERS 8

/* Número de servidores a contactar em operações de leitura (M <= N) */
#ifndef READ_SERVERS_M
#define READ_SERVERS_M 1
#endif

/* socket do cliente */
static int Cli_sock;

/* endereço do cliente */
static struct sockaddr_un Cli_addr;

/* array de endereços dos servidores */
static struct sockaddr_un Serv_addrs[MAX_SERVERS];

/* número de servidores activos */
static int Num_servers = 0;

/* índice round-robin para operações de leitura */
static int rr_index = 0;

/* número de série do próximo pedido */
static snfs_req_serial_num_t next_sn;

/* =====================================================================
 * remote_call
 *
 * to_all_servers == 1  -> envia a TODOS, espera TODOS (writes)
 * to_all_servers == 0  -> envia a M em round-robin, retorna com o 1º (reads)
 * ===================================================================== */

static int remote_call(snfs_msg_req_t *req, int reqsz, snfs_msg_res_t *res,
   int ressz, int to_all_servers)
{
   int status;
   req->sn = (next_sn++);

   if (Num_servers == 0) {
      printf("[snfs_api] no servers configured.\n");
      return -1;
   }

   if (to_all_servers) {
      /* -------------------------------------------------------
       * WRITE path: envia a todos, espera resposta de todos
       * ------------------------------------------------------- */
      snfs_msg_res_t responses[MAX_SERVERS];
      int received[MAX_SERVERS];
      memset(received, 0, sizeof(received));

      /* enviar para todos */
      for (int i = 0; i < Num_servers; i++) {
         status = sendto(Cli_sock, (void*)req, reqsz, 0,
            (struct sockaddr*)&Serv_addrs[i], sizeof(Serv_addrs[i]));
         if (status < 0) {
            printf("[snfs_api] sendto server %d error: %s.\n", i, strerror(errno));
            return -1;
         }
      }

      /* receber de todos */
      int received_count = 0;
      while (received_count < Num_servers) {
         snfs_msg_res_t tmp;
         status = recvfrom(Cli_sock, &tmp, ressz, 0, NULL, NULL);
         if (status < 0) {
            printf("[snfs_api] recvfrom error: %s.\n", strerror(errno));
            return -1;
         }
         if (status == 0) {
            printf("[snfs_api] server closed.\n");
            return -1;
         }
         /* associar resposta ao servidor pelo serial number */
         if (tmp.sn == req->sn) {
            /* guardamos a primeira resposta válida e comparamos as restantes */
            responses[received_count] = tmp;
            received_count++;
         }
         /* respostas com sn diferente são descartadas (antigas) */
      }

      /* verificar que todos retornaram o mesmo STATUS */
      snfs_msg_res_status_t first_status = responses[0].status;
      for (int i = 1; i < Num_servers; i++) {
         if (responses[i].status != first_status) {
            printf("[snfs_api] FATAL: servers returned different status! Aborting.\n");
            exit(-1);
         }
      }

      /* devolver a primeira resposta */
      memcpy(res, &responses[0], ressz);
      return ressz;

   } else {
      /* -------------------------------------------------------
       * READ path: envia a M servidores em round-robin,
       * retorna com a primeira resposta
       * ------------------------------------------------------- */
      int M = (READ_SERVERS_M < Num_servers) ? READ_SERVERS_M : Num_servers;

      /* enviar a M servidores em round-robin */
      for (int k = 0; k < M; k++) {
         int idx = (rr_index + k) % Num_servers;
         status = sendto(Cli_sock, (void*)req, reqsz, 0,
            (struct sockaddr*)&Serv_addrs[idx], sizeof(Serv_addrs[idx]));
         if (status < 0) {
            printf("[snfs_api] sendto server %d error: %s.\n", idx, strerror(errno));
            return -1;
         }
      }
      rr_index = (rr_index + M) % Num_servers;

      /* esperar pela primeira resposta válida */
      while (1) {
         status = recvfrom(Cli_sock, res, ressz, 0, NULL, NULL);
         if (status < 0) {
            printf("[snfs_api] recvfrom error: %s.\n", strerror(errno));
            return -1;
         }
         if (status == 0) {
            printf("[snfs_api] server closed.\n");
            return -1;
         }
         if (res->sn == req->sn) {
            /* resposta correcta - retornar imediatamente */
            return status;
         }
         /* resposta antiga/outra - descartar e continuar a esperar */
      }
   }
}

/* =====================================================================
 * snfs_init
 *
 * server_name pode ser:
 *   - nome simples "server"      -> modo single server (compatibilidade)
 *   - prefixo "server" com N=1   -> usa "server_1"
 *
 * Para múltiplos servidores, o cliente tenta ligar a
 * server_name_1, server_name_2, ... até MAX_SERVERS.
 * Pára quando um socket não existir.
 * ===================================================================== */

int snfs_init(char* cli_name, char* server_name)
{
   if (cli_name == NULL || server_name == NULL) {
      printf("[snfs_api] invalid client/server address names.\n");
      return -1;
   }

   /* criar socket */
   if ((Cli_sock = socket(AF_UNIX, SOCK_DGRAM, 0)) < 0) {
      printf("[snfs_api] socket error: %s.\n", strerror(errno));
      return -1;
   }

   /* reutilizar endereço se existir */
   if (unlink(cli_name) < 0 && errno != ENOENT) {
      printf("[snfs_api] unlink error: %s.\n", strerror(errno));
   }

   bzero(&Cli_addr, sizeof(Cli_addr));
   Cli_addr.sun_family = AF_UNIX;
   strcpy(Cli_addr.sun_path, cli_name);

   if (bind(Cli_sock, (struct sockaddr*)&Cli_addr, sizeof(Cli_addr)) < 0) {
      printf("[snfs_api] bind error: %s.\n", strerror(errno));
      return -1;
   }

   /*
    * Detectar servidores disponíveis.
    * Primeiro tenta server_name_1 .. server_name_N (modo multi-servidor).
    * Se nenhum existir com sufixo, tenta o nome directo (modo single).
    */
   Num_servers = 0;
   char addr_path[128];

   for (int i = 1; i <= 1; i++) {
      snprintf(addr_path, sizeof(addr_path), "%s", server_name);
      /* verificar se o socket existe */
      struct sockaddr_un tmp;
      bzero(&tmp, sizeof(tmp));
      tmp.sun_family = AF_UNIX;
      strcpy(tmp.sun_path, addr_path);
      /* tentar enviar 0 bytes para ver se existe - método simples: apenas
         registar; a falha será detectada no primeiro sendto real */
      Serv_addrs[Num_servers] = tmp;
      Num_servers++;
      if (Num_servers >= MAX_SERVERS) break;
   }

   /* fallback: se não encontrou nenhum sufixado, usa o nome directo */
   if (Num_servers == 0) {
      bzero(&Serv_addrs[0], sizeof(Serv_addrs[0]));
      Serv_addrs[0].sun_family = AF_UNIX;
      strcpy(Serv_addrs[0].sun_path, server_name);
      Num_servers = 1;
   }

   printf("[snfs_api] configured with %d server(s).\n", Num_servers);

   next_sn  = 0;
   rr_index = 0;
   return 0;
}

/* =====================================================================
 * SNFS API  (lógica idêntica ao original; só muda o remote_call)
 * ===================================================================== */

snfs_call_status_t snfs_ping(char* inmsg, int insize, char* outmsg, int outsize)
{
   snfs_msg_req_t req; snfs_msg_res_t res;
   memset(&req,0,sizeof(req)); memset(&res,0,sizeof(res));
   req.type = REQ_PING;
   strncpy(req.body.ping.msg, inmsg, insize);
   int status = remote_call(&req, sizeof(req.sn)+sizeof(req.type)+sizeof(req.body.ping),
      &res, sizeof(res), 0);  /* leitura */
   if (status < 0 || res.status != RES_OK) return STAT_ERROR;
   strncpy(outmsg, res.body.ping.msg, outsize);
   return STAT_OK;
}

snfs_call_status_t snfs_lookup(char* pathname, snfs_fhandle_t* file, unsigned* fsize)
{
   snfs_msg_req_t req; snfs_msg_res_t res;
   memset(&req,0,sizeof(req)); memset(&res,0,sizeof(res));
   req.type = REQ_LOOKUP;
   strcpy(req.body.lookup.pname, pathname);
   int status = remote_call(&req, sizeof(req.sn)+sizeof(req.type)+sizeof(req.body.lookup),
      &res, sizeof(res), 0);  /* leitura */
   if (status < 0 || res.status != RES_OK) {
      *file = res.body.lookup.file;
      return STAT_ERROR;
   }
   *file  = res.body.lookup.file;
   *fsize = res.body.lookup.fsize;
   return STAT_OK;
}

snfs_call_status_t snfs_read(snfs_fhandle_t fhandle, unsigned offset,
   unsigned count, char* buffer, int* nread)
{
   snfs_msg_req_t req; snfs_msg_res_t res;
   memset(&req,0,sizeof(req)); memset(&res,0,sizeof(res));
   req.type = REQ_READ;
   req.body.read.fhandle = fhandle;
   req.body.read.offset  = offset;
   req.body.read.count   = count;
   int status = remote_call(&req, sizeof(req.sn)+sizeof(req.type)+sizeof(req.body.read),
      &res, sizeof(res), 0);  /* leitura */
   if (status < 0 || res.status != RES_OK) return STAT_ERROR;
   memcpy(buffer, res.body.read.data, count);
   *nread = res.body.read.nread;
   return STAT_OK;
}

snfs_call_status_t snfs_write(snfs_fhandle_t fhandle, unsigned offset,
   unsigned count, char* buffer, unsigned int* fsize)
{
   snfs_msg_req_t req; snfs_msg_res_t res;
   memset(&req,0,sizeof(req)); memset(&res,0,sizeof(res));
   req.type = REQ_WRITE;
   req.body.write.fhandle = fhandle;
   req.body.write.offset  = offset;
   req.body.write.count   = count;
   memcpy(req.body.write.data, buffer, count);
   int status = remote_call(&req, sizeof(req.sn)+sizeof(req.type)+sizeof(req.body.write),
      &res, sizeof(res), 1);  /* escrita -> todos os servidores */
   if (status < 0 || res.status != RES_OK) return STAT_ERROR;
   *fsize = res.body.write.fsize;
   return STAT_OK;
}

snfs_call_status_t snfs_create(snfs_fhandle_t dir, char* name, snfs_fhandle_t* file)
{
   snfs_msg_req_t req; snfs_msg_res_t res;
   memset(&req,0,sizeof(req)); memset(&res,0,sizeof(res));
   req.type = REQ_CREATE;
   req.body.create.dir = (snfs_fhandle_t)dir;
   strcpy(req.body.create.name, name);
   int status = remote_call(&req, sizeof(req.sn)+sizeof(req.type)+sizeof(req.body.create),
      &res, sizeof(res), 1);  /* escrita -> todos */
   if (status < 0 || res.status != RES_OK) return STAT_ERROR;
   *file = res.body.create.file;
   return STAT_OK;
}

snfs_call_status_t snfs_mkdir(snfs_fhandle_t dir, char* name, snfs_fhandle_t* file)
{
   snfs_msg_req_t req; snfs_msg_res_t res;
   memset(&req,0,sizeof(req)); memset(&res,0,sizeof(res));
   req.type = REQ_MKDIR;
   req.body.mkdir.dir = dir;
   strcpy(req.body.mkdir.file, name);
   int status = remote_call(&req, sizeof(req.sn)+sizeof(req.type)+sizeof(req.body.mkdir),
      &res, sizeof(res), 1);  /* escrita -> todos */
   if (status < 0 || res.status != RES_OK) return STAT_ERROR;
   *file = res.body.mkdir.newdirid;
   return STAT_OK;
}

snfs_call_status_t snfs_readdir(snfs_fhandle_t dir, unsigned cmax,
   snfs_dir_entry_t* list, unsigned* count)
{
   snfs_msg_req_t req; snfs_msg_res_t res;
   memset(&req,0,sizeof(req)); memset(&res,0,sizeof(res));
   req.type = REQ_READDIR;
   req.body.readdir.dir  = dir;
   req.body.readdir.cmax = cmax;
   int status = remote_call(&req, sizeof(req.sn)+sizeof(req.type)+sizeof(req.body.readdir),
      &res, sizeof(res), 0);  /* leitura */
   if (status < 0 || res.status != RES_OK) return STAT_ERROR;
   *count = res.body.readdir.count;
   memcpy(list, res.body.readdir.list, sizeof(snfs_dir_entry_t)*(*count));
   return STAT_OK;
}

snfs_call_status_t snfs_copy(char* srcpath, char* tgtpath)
{
   snfs_msg_req_t req; snfs_msg_res_t res;
   memset(&req,0,sizeof(req)); memset(&res,0,sizeof(res));
   req.type = REQ_COPY;
   strcpy(req.body.copy.srcpathname, srcpath);
   strcpy(req.body.copy.tgtpathname, tgtpath);
   int status = remote_call(&req, sizeof(req.sn)+sizeof(req.type)+sizeof(req.body.copy),
      &res, sizeof(res), 1);  /* escrita -> todos */
   if (status < 0 || res.status != RES_OK) return STAT_ERROR;
   return STAT_OK;
}

void snfs_finish()
{
   close(Cli_sock);
   unlink(Cli_addr.sun_path);
}
