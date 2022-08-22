#include "../utils/utils.h"
#include "../master_book/master_book.h"
#include "user.h"
#include "../pid_list/pid_list.h"
#include "../master/master.h"

#include <signal.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/ipc.h>
#include <sys/msg.h>
#include <sys/stat.h>
#include <sys/shm.h>
#include <sys/sem.h>
#include <errno.h>
#include <time.h>
#include <string.h>
#include <strings.h>

/*Variabili statici per ogni processo utente*/
extern int SO_BUDGET_INIT;
extern int SO_USERS_NUM;
extern int SO_REWARD;
extern int SO_RETRY;
extern int SO_MIN_TRANS_GEN_NSEC;
extern int SO_MAX_TRANS_GEN_NSEC;

static int bilancio_corrente;
static int cont_try = 0;

void usr_handler(int);
int calcola_bilancio(int, struct master_book, int*);

void init_user(int* users, int shm_nodes_array, int shm_nodes_size, int shm_book_id, int shm_book_size_id)
{
  int sem_id;

  struct nodes nodes;
  struct master_book book;

  struct sembuf sops;
  int block_reached = 0;
  struct sigaction sa;
  int queue_id;
  sigset_t mask;
  bilancio_corrente = SO_BUDGET_INIT;
  bzero(&sops, sizeof(struct sembuf));
  sigemptyset(&mask);
  sigaddset(&mask, SIGINT);

  sa.sa_flags = 0;
  sa.sa_mask = mask;
  sa.sa_handler = usr_handler;
  if (sigaction(SIGUSR1, &sa, NULL) == -1) {
    fprintf(ERR_FILE, "user u%d: cannot associate handler to SIGUSR1\n", getpid());
    exit(EXIT_FAILURE);
  }
  if (sigaction(SIGUSR2, &sa, NULL) == -1) {
    fprintf(ERR_FILE, "user u%d: cannot associate handler to SIGUSR2\n", getpid());
    exit(EXIT_FAILURE);
  }
  if (sigaction(SIGTERM, &sa, NULL) == -1) {
    fprintf(ERR_FILE, "user u%d: cannot associate handler to SIGTERM\n", getpid());
    exit(EXIT_FAILURE);
  }
  if (sigaction(SIGSEGV, &sa, NULL) < 0) {
    fprintf(ERR_FILE, "user u%d: could not associate handler to SIGSEGV.\n", getpid());
    exit(EXIT_FAILURE);
  }

  if ((sem_id = semget(getppid(), 0, S_IRUSR | S_IWUSR)) == -1) {
    TEST_ERROR;
    exit(EXIT_FAILURE);
  }

  if ((nodes.array = attach_shm_memory(shm_nodes_array, SHM_RDONLY)) == NULL) {
    fprintf(ERR_FILE, "user u%d: the process cannot be attached to the nodes array shared memory.\n", getpid());
    exit(EXIT_FAILURE);
  }
  if ((nodes.size = attach_shm_memory(shm_nodes_size, SHM_RDONLY)) == NULL) {
    fprintf(ERR_FILE, "user u%d: the process cannot be attached to the nodes size shared memory.\n", getpid());
    exit(EXIT_FAILURE);
  }
  if ((book.blocks = attach_shm_memory(shm_book_id, SHM_RDONLY)) == NULL) {
    fprintf(ERR_FILE, "user u%d: the process cannot be attached to the registry shared memory.\n", getpid());
    exit(EXIT_FAILURE);
  }
  if ((book.size = attach_shm_memory(shm_book_size_id, SHM_RDONLY)) == NULL) {
    fprintf(ERR_FILE, "user u%d: the process cannot be attached to the book size shared memory.\n", getpid());
    exit(EXIT_FAILURE);
  }

  while (cont_try < SO_RETRY)
  {
    if ((bilancio_corrente = calcola_bilancio(bilancio_corrente, book, &block_reached)) >= 2) {
      int size = 0;
      transaction t;
      /* estrazione di un destinatario casuale*/
      /* estrazione di un nodo casuale*/
      int cifra_utente, random_user, random_node, total_quantity, reward;
      struct msg* message = malloc(sizeof(struct msg));
      if (message == NULL) {
        TEST_ERROR;
        CHILD_STOP_SIMULATION;
        exit(EXIT_FAILURE);
      }
      random_user = random_element(users, SO_USERS_NUM);
      if (random_user == -1) {
        fprintf(LOG_FILE, "init_user u%d:  all other users have terminated. Ending successfully.\n", getpid());
        exit(EXIT_SUCCESS);
      }
      size = *(nodes.size);
      random_node = random_element(nodes.array, size);
      if (random_node == -1) {
        fprintf(ERR_FILE, "init_user u%d: all nodes have terminated, cannot send transaction.\n", getpid());
        exit(EXIT_FAILURE);
      }

      /* generazione di un importo da inviare*/
      srand(getpid() + clock());
      total_quantity = (rand() % (bilancio_corrente - 2 + 1)) + 2;
      reward = total_quantity / 100 * SO_REWARD;
      if (reward == 0)
        reward = 1;
      cifra_utente = total_quantity - reward;

      /* system V message queue */
      /* ricerca della coda di messaggi del nodo random */
      if ((queue_id = msgget(random_node, 0)) == -1) {
        fprintf(ERR_FILE, "init_user u%d: cannot retrieve message queue of node %d (%s)\n", getpid(), random_node, strerror(errno));
        CHILD_STOP_SIMULATION;
        exit(EXIT_FAILURE);
      }
      /* creazione di una transazione e invio di tale al nodo generato*/
      new_transaction(&t, getpid(), random_user, cifra_utente, reward);

      message->mtype = 1;
      message->mtext = t;
      if (msgsnd(queue_id, message, sizeof(struct msg) - sizeof(long), IPC_NOWAIT) == -1) {
        if (errno != EAGAIN) {
          fprintf(ERR_FILE, "[%ld] init_user u%d: recieved an unexpected error while sending transaction at queue with id %d of node %d: %s.\n", clock() / CLOCKS_PER_SEC, getpid(), queue_id, random_node, strerror(errno));
          CHILD_STOP_SIMULATION;
          exit(EXIT_FAILURE);
        }
        else {
          cont_try++;
        }
      }
      else {
        bilancio_corrente -= total_quantity;
        kill(random_node, SIGUSR1);
      }
      free(message);
    }
    else {
      cont_try++;
    }

    /*tempo di attesa dopo l'invio di una transazione*/
    sleep_random_from_range(SO_MIN_TRANS_GEN_NSEC, SO_MAX_TRANS_GEN_NSEC);
  }
  exit(EARLY_FAILURE);
}

int calcola_bilancio(int bilancio, struct master_book book, int* block_reached)
{
  /* block_reached e book.size codificati come indici di blocchi (come se fosse un'array di blocchi)
    necessaria conversione in indice per l'array nella shm (array di transazioni) */
  int i = (*block_reached) * SO_BLOCK_SIZE;
  int size = (*book.size) * SO_BLOCK_SIZE;
  while (i < size) {
    transaction t = book.blocks[i];
    if (t.receiver == getpid()) {
      bilancio += t.quantita;
    }
    i++;
  }
  /* codifico block_reached come un indice del blocco raggiunto */
  *block_reached = i / SO_BLOCK_SIZE;
  return bilancio;
}

void usr_handler(int signal) {
  switch (signal) {
  case SIGUSR1:
  {
    int transaction_q = 0;
    struct msg incoming;
    transaction refused_t;
    if ((transaction_q = msgget(getppid(), 0)) == -1) {
      TEST_ERROR;
      exit(EXIT_FAILURE);
    }
    if (msgrcv(transaction_q, &incoming, sizeof(struct msg) - sizeof(long), getpid(), IPC_NOWAIT) == -1) {
      if (errno != ENOMSG) {
        TEST_ERROR;
        exit(EXIT_FAILURE);
      }
      fprintf(ERR_FILE, "u%d: no msg was found with my pid type\n", getpid());
    }
    refused_t = incoming.mtext;
    bilancio_corrente += (refused_t.quantita + refused_t.reward);
    /* fprintf(LOG_FILE, "u%d: transazione rifiutata, posso ancora mandare %d volte\n", getpid(), (SO_RETRY - cont_try)); */
    cont_try++;

  }
  break;
  case SIGUSR2:
    break;
  case SIGTERM:
    /* fprintf(LOG_FILE, "u%d: killed by parent. Ending successfully\n", getpid()); */
    exit(EXIT_SUCCESS);
    break;
  case SIGSEGV:
    fprintf(ERR_FILE, "u%d: recieved a SIGSEGV, stopping simulation.\n", getpid());
    exit(EXIT_FAILURE);
    break;
  default:
    fprintf(ERR_FILE, "user %d: an unexpected signal was recieved.\n", getpid());
    break;
  }
}

/*TODO: noi al bilancio togliano due volte la stessa transazione inviata:
la prima volta con quando generiamo la transazione
la seconda volta quando leggiamo nel libro mastro*/