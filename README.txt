
  O código disponibilizado funciona por omissão com a implementação das tarefas nativas - pthreads.

  Para usar as tarefas em modo utilizador devem "descomentar" a linha referente ao uso das pthreads 
no ficheiro include/config.h (no fim do ficheiro).

  Para que a 2ª parte do trabalho funcione com as tarefas em modo utilizador (sthreads) com o escalonamento 
pedido na 1ª parte do projecto, será necessário o seguinte:

- Descompactar o pacote da biblioteca snfs dado por nós;

- Copiar todos os ficheiros *.c e *.h por vocês alterados para as directorias respectivas tendo em atenção o seguinte:
  - Foram feitas alterações nos ficheiros sthread_pthreads.c, sthread_pthreads.h e sthread.c. 
    Caso tenham alterado algum destes ficheiros têm que ter cuidado ao fazer o merge. Basicamente o que estes ficheiros 
    têm a mais em relação ao código disponibilizado para a 1ª parte é a implementação dos monitores e do sleep nas pthreads.
  - o ficheiro include/config.h foi também alterado para permitir o undefine da variável USE_PTHREADS.

- A Makefiles foram também alteradas em relação ao que foi dado para a 1ª parte do projecto. Ter em atenção estas 
  alterações ao fazer o merge das Makefiles:
  - Na Makefile que está na raiz da biblioteca foram acrescentadas as directorias referentes ao snfs
  - Na Makefile do sthread_lib foi acrescentado o ficheiro sthread_pthread e acrescentado também a definição do USE_PTHREADS.

======================================================================================================
 
 Para o lançamento da aplicação servidor:
   1) Executa do comando make na directoria snfs+sthreads
   2) muda para a directoria snfs_server
   3) lançar na linha comandos ./server  (pode ser também ./server <io_delay> , io_delay é um inteiro positivo)


  Os testes devem ser descompactados na directoria snfs+sthreads e uma vez compilados (comando make) 
devem  ser executados num terminal depois de que o servidor já estiver a executar noutro terminal.
 Para cada teste é preciso executar de novo o servidor no respectivo terminal.


   