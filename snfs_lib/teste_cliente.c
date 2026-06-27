/*
 * teste_cliente.c
 * 
 * Programa de teste do SNFS.
 * Compila com:
 *   gcc -g -O0 -Wall -m32 -std=c99 -I../include -I. teste_cliente.c myfs.c queue.c snfs_api.c -o teste_cliente
 */

#include <stdio.h>
#include <string.h>
#include <myfs.h>

int main() {
    printf("=== TESTE SNFS ===\n");

    /* 1. Inicializar */
    printf("[1] Inicializar biblioteca... ");
    if (my_init_lib() < 0) {
        printf("ERRO!\n");
        return -1;
    }
    printf("OK\n");

    /* 2. Criar directoria */
    printf("[2] Criar directoria /teste... ");
    if (my_mkdir("/teste") < 0) printf("ERRO\n");
    else printf("OK\n");

    /* 3. Criar ficheiro */
    printf("[3] Criar ficheiro /teste/ola.txt... ");
    int fd = my_open("/teste/ola.txt", 1); /* 1 = O_CREATE */
    if (fd < 0) { printf("ERRO\n"); return -1; }
    printf("OK (fd=%d)\n", fd);

    /* 4. Escrever */
    char* msg = "Ola mundo!";
    printf("[4] Escrever \"%s\"... ", msg);
    int nw = my_write(fd, msg, strlen(msg));
    if (nw < 0) printf("ERRO\n");
    else printf("OK (%d bytes)\n", nw);
    my_close(fd);

    /* 5. Ler */
    printf("[5] Ler /teste/ola.txt... ");
    fd = my_open("/teste/ola.txt", 0); /* 0 = O_RDONLY */
    if (fd < 0) { printf("ERRO\n"); return -1; }
    char buf[256];
    memset(buf, 0, sizeof(buf));
    int nr = my_read(fd, buf, sizeof(buf)-1);
    if (nr < 0) printf("ERRO\n");
    else printf("OK lido: \"%s\"\n", buf);
    my_close(fd);

    /* 6. Listar directoria */
    printf("[6] Listar /teste... ");
    char* filenames;
    int numFiles;
    if (my_listdir("/teste", &filenames, &numFiles) < 0) {
        printf("ERRO\n");
    } else {
        printf("OK (%d ficheiro(s))\n", numFiles);
        char* p = filenames;
        for (int i = 0; i < numFiles; i++) {
            printf("    -> %s\n", p);
            p += strlen(p) + 1;
        }
        free(filenames);
    }

    printf("=== FIM DOS TESTES ===\n");
    return 0;
}
