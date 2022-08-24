#include "../utils/utils.h"
#include "../master_book/master_book.h"
#include "user.h"
#include "../ipc_functions/ipc_functions.h"
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

#define TEST_ERROR_AND_FAIL    if (errno && errno != ESRCH && errno != EAGAIN) {fprintf(ERR_FILE, \
                       "%s:%d: PID=%5d: Error %d (%s)\n",\
                       __FILE__,\
                       __LINE__,\
                       getpid(),\
                       errno,\
                       strerror(errno)); \
                       exit(EXIT_FAILURE); }

/*Variabili statici per ogni processo utente*/
extern int SO_BUDGET_INIT;
extern int SO_USERS_NUM;
extern int SO_REWARD;
extern int SO_RETRY;
extern int SO_MIN_TRANS_GEN_NSEC;
extern int SO_MAX_TRANS_GEN_NSEC;

static int block_reached;
static int cont_try;

static int bilancio_corrente;

static struct nodes nodes;
static struct master_book book;
static int* users;


void usr_handler(int);
int calcola_bilancio(int, struct master_book, int*);
void generate_and_send_transaction(struct nodes nodes, int* users, struct master_book book, int* block_reached);

void init_user(int* users_a, int shm_nodes_array, int shm_nodes_size, int shm_book_id, int shm_book_size_id)
{
  struct sigaction sa;
  sigset_t mask;

  users = users_a;
  bilancio_corrente = SO_BUDGET_INIT;
  sigemptyset(&mask);
  sigaddset(&mask, SIGINT);

  nodes.array = shmat(shm_nodes_array, NULL, SHM_RDONLY);
  TEST_ERROR_AND_FAIL;

  nodes.size = shmat(shm_nodes_size, NULL, SHM_RDONLY);
  TEST_ERROR_AND_FAIL;

  book.blocks = shmat(shm_book_id, NULL, SHM_RDONLY);
  TEST_ERROR_AND_FAIL;
  book.size = shmat(shm_book_size_id, NULL, SHM_RDONLY);
  TEST_ERROR_AND_FAIL;

  sa.sa_flags = 0;
  sa.sa_mask = mask;
  sa.sa_handler = usr_handler;
  sigaction(SIGUSR1, &sa, NULL);
  TEST_ERROR_AND_FAIL;
  sigaction(SIGUSR2, &sa, NULL);
  TEST_ERROR_AND_FAIL;
  sigaction(SIGTERM, &sa, NULL);
  TEST_ERROR_AND_FAIL;
  sigaction(SIGSEGV, &sa, NULL);
  TEST_ERROR_AND_FAIL;

  alarm(1);
  while (cont_try < SO_RETRY)
  {
    generate_and_send_transaction(nodes, users, book, &block_reached);

    /*tempo di attesa dopo l'invio di una transazione*/
    sleep_random_from_range(SO_MIN_TRANS_GEN_NSEC, SO_MAX_TRANS_GEN_NSEC);
  }

  exit(EXIT_SUCCESS);
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
    /*transazione rifiutata, recupero del credito, incremento del count_try*/
  {
    int transaction_q = 0;
    struct msg incoming;
    transaction refused_t;
    transaction_q = msgget(MSG_Q, 0);
    TEST_ERROR_AND_FAIL;

    msgrcv(transaction_q, &incoming, sizeof(struct msg) - sizeof(long), getpid(), IPC_NOWAIT);
    if (errno != ENOMSG) {
      TEST_ERROR_AND_FAIL;
    }
    else
      fprintf(ERR_FILE, "u%d: no msg was found with my pid type\n", getpid());
    refused_t = incoming.mtext;
    bilancio_corrente += (refused_t.quantita + refused_t.reward);
    /* fprintf(LOG_FILE, "u%d: transazione rifiutata, posso ancora mandare %d volte\n", getpid(), (SO_RETRY - cont_try)); */
    cont_try++;
  }
  break;
  case SIGUSR2:
    /*generazione di una transazione da un segnale*/
    fprintf(LOG_FILE, "recieved SIGUSR2, generation of transaction in progress...\n");
    generate_and_send_transaction(nodes, users, book, &block_reached);
    break;
  case SIGTERM:
    /*segnale per la terminazione dell'esecuzione*/
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

void generate_and_send_transaction(struct nodes nodes, int* users, struct master_book book, int* block_reached) {
  if ((bilancio_corrente = calcola_bilancio(bilancio_corrente, book, block_reached)) >= 2) {
    int size = 0, queue_id, cifra_utente, random_user, random_node, total_quantity, reward;
    transaction t;
    struct msg message;
    /* estrazione di un destinatario casuale*/
    random_user = random_element(users, SO_USERS_NUM);
    if (random_user == -1) {
      fprintf(LOG_FILE, "init_user u%d:  all other users have terminated. Ending successfully.\n", getpid());
      exit(EXIT_SUCCESS);
    }
    /* estrazione di un nodo casuale*/
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


    /* creazione di una transazione e invio di tale al nodo generato*/
    new_transaction(&t, getpid(), random_user, cifra_utente, reward);
    /* system V message queue */
    /* ricerca della coda di messaggi del nodo random */
    if ((queue_id = msgget(random_node, 0)) == -1) {
      fprintf(ERR_FILE, "init_user u%d: cannot retrieve message queue of node %d (%s)\n", getpid(), random_node, strerror(errno));
      CHILD_STOP_SIMULATION;
      exit(EXIT_FAILURE);
    }
    message.mtype = 1;
    message.mtext = t;
    if (msgsnd(queue_id, &message, sizeof(struct msg) - sizeof(long), IPC_NOWAIT) == -1) {
      if (errno != EAGAIN) {
        fprintf(ERR_FILE, "init_user u%d: recieved an unexpected error while sending transaction at queue with id %d of node %d: %s.\n", getpid(), queue_id, random_node, strerror(errno));
        CHILD_STOP_SIMULATION;
        exit(EXIT_FAILURE);
      }
      else {
        cont_try++;
      }
    }
    else {
      cont_try = 0;
      bilancio_corrente -= total_quantity;
      kill(random_node, SIGUSR1);
    }
  }
  else {
    cont_try++;

  }
}