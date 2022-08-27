#include "../utils/utils.h"
#include "../load_constants/load_constants.h"
#include "../ipc_functions/ipc_functions.h"
#include "../master_book/master_book.h"
#include "master_functions/master_functions.h"
#include "../node/node.h"
#include "../user/user.h"
#include "master.h"

#include <sys/ipc.h>
#include <sys/sem.h>
#include <sys/shm.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/msg.h>

#define SIM_END_SEC 0
#define SIM_END_USR 1
#define SIM_END_SIZ 2

#define MAX_USERS_TO_PRINT 20

#define ID_READY_NODE 2
#define NUM_SEM 3

#define REGISTRY_SIZE sizeof(transaction) * SO_BLOCK_SIZE * SO_REGISTRY_SIZE

extern int SO_USERS_NUM;
extern int SO_NODES_NUM;
extern int SO_SIM_SEC;
extern int SO_NUM_FRIENDS;
extern int SO_BUDGET_INIT;

static int simulation_seconds;

static int shm_book_id = -1;
static int shm_book_size_id = -1;
static struct master_book book;

static int shm_users_array_id = -1;
static int* users = NULL;
static int* user_budget;
static int inactive_users;

static int shm_nodes_array_id = -1;
static int shm_nodes_size_id = -1;
static int* nodes_write_fd;
static int* node_budget;
static struct nodes nodes;

static int queue_id = -1;
static int refused_queue_id = -1;

static int sem_id = -1;

static int periodical_flag;

int periodical_update(int block_reached) {
  int index;
  transaction t;
  int size = *(book.size) * SO_BLOCK_SIZE;

  while (block_reached < size) {
    t = book.blocks[block_reached];
    if (t.sender == SELF_SENDER) {
      int nodes_size = *(nodes.size);
      int index = find_element(nodes.array, nodes_size, t.receiver);
      node_budget[index] += t.quantita;
    }
    else {
      int total_quantity = t.quantita + t.reward;
      index = find_element(users, SO_USERS_NUM, t.sender);
      user_budget[index] -= total_quantity;
      index = find_element(users, SO_USERS_NUM, t.receiver);
      user_budget[index] += t.quantita;
    }
    ++block_reached;
  }

  return block_reached;
}

void stop_simulation() {
  int size;
  int i = 0;
  int child = 0;

  while ((child = users[i]) != 0 && i < SO_USERS_NUM) {
    i++;
    kill(child, SIGTERM);
  }
  i = 0;
  size = *(nodes.size);
  while ((child = nodes.array[i]) != 0 && i < size) {
    i++;
    kill(child, SIGTERM);
  }

  /* resetting errno as some kill might have failed */
  errno = 0;
}

static void cleanup() {
  sigset_t mask;
  int i = 0;
  int sblock[3] = { SIGUSR1, SIGUSR2, SIGINT };
  alarm(0);
  sig_block(sblock, 3);


  if (users != NULL && nodes.array != NULL) {
    stop_simulation();
  }
  errno = 0;
  if (shm_book_id != -1) {
    shmctl(shm_book_id, IPC_RMID, 0);
    TEST_ERROR;
  }
  if (shm_book_size_id != -1) {
    shmctl(shm_book_size_id, IPC_RMID, 0);
    TEST_ERROR;
  }
  if (shm_nodes_array_id != -1) {
    shmctl(shm_nodes_array_id, IPC_RMID, 0);
    TEST_ERROR;
  }
  if (shm_nodes_size_id != -1) {
    shmctl(shm_nodes_size_id, IPC_RMID, 0);
    TEST_ERROR;
  }
  if (shm_users_array_id != -1) {
    shmctl(shm_users_array_id, IPC_RMID, 0);
    TEST_ERROR;
  }
  if (sem_id != -1) {
    semctl(sem_id, IPC_RMID, 0);
    TEST_ERROR;
  }
  if (queue_id != -1) {
    msgctl(queue_id, IPC_RMID, 0);
    TEST_ERROR;
  }
  if (refused_queue_id != -1) {
    msgctl(refused_queue_id, IPC_RMID, 0);
    TEST_ERROR;
  }

  while (nodes_write_fd[i] != 0) {
    close(nodes_write_fd[i]);
    TEST_ERROR;
    i++;
  }

  free(node_budget);
  free(user_budget);
  free(nodes_write_fd);
}

void periodical_print() {
  int i = 0;
  printf("NUMBER OF ACTIVE USERS %d | NUMBER OF ACTIVE NODES %d\n", (SO_USERS_NUM - inactive_users), *(nodes.size));
  /* budget di ogni processo utente (inclusi quelli terminati prematuramente)*/
  printf("USERS (%d) BUDGETS\n", SO_USERS_NUM);
  if (SO_USERS_NUM <= MAX_USERS_TO_PRINT) {
    while (i < SO_USERS_NUM) {
      printf("USER u%d : %d$\n", users[i], user_budget[i]);
      i++;
    }
  }
  else {
    int min_i = 0, max_i = 0;
    printf("[!] Users count is too high to display all budgets [!]\n");
    i = 1;
    while (i < SO_NODES_NUM) {
      if (user_budget[i] < user_budget[min_i]) {
        min_i = i;
      }
      if (user_budget[i] > user_budget[max_i]) {
        max_i = i;
      }
      i++;
    }

    printf("HIGHEST BUDGET: USER u%d : %d$\n", users[max_i], user_budget[max_i]);
    printf("LOWEST BUDGET: USER u%d : %d$\n", users[min_i], user_budget[min_i]);
  }

  {
    int size = *(nodes.size);
    int i = 0;
    /* budget di ogni processo nodo */
    printf("NODES (%d) BUDGETS\n", *(nodes.size));
    while (i < size) {
      printf("NODE n%d : %d$\n", nodes.array[i], node_budget[i]);
      i++;
    }
  }
}

void summary_print(int ending_reason, int* users, int* user_budget, struct nodes nodes, int* node_budget, int* nof_transactions) {
  int i = 0;

  switch (ending_reason)
  {
  case SIM_END_SEC:
    printf("[!] Simulation ending reason: TIME LIMIT REACHED [!]\n\n");
    break;
  case SIM_END_SIZ:
    printf("[!] Simulation ending reason: MASTER BOOK SIZE EXCEEDED [!]\n\n");
    break;
  case SIM_END_USR:
    printf("[!] Simulation ending reason: ALL USERS TERMINATED [!]\n\n");
    break;
  default:
    printf("[!] Simulation ending reason: UNEXPECTED ERRORS [!]\n\n");
    break;
  }
  /* bilancio di ogni processo utente, compresi quelli che sono terminati prematuramente */
  /*printf("USERS BUDGETS\n");
  while (i < SO_USERS_NUM) {
    printf("USER u%d : %d$\n", users[i], user_budget[i]);
    i++;
  }*/
  /* bilancio di ogni processo nodo */
  i = 0;
  printf("NODES (%d) BUDGETS\n", *(nodes.size));
  while (i < *nodes.size) {
    printf("NODE n%d : %d$\n", nodes.array[i], node_budget[i]);
    i++;
  }
  printf("NUMBER OF INACTIVE USERS: %d\n", inactive_users);
  printf("NUMBER OF TRANSACTION BLOCK WRITTEN INTO THE MASTER BOOK: %d\n\n\n", *book.size);

  printf("Number of transactions left per node:\n");
  i = 0;
  while (i < *nodes.size) {
    printf("NODE %d: %d transactions left\n", nodes.array[i], nof_transactions[i]);
    i++;
  }
}

void wait_siblings(int sem_num) {
  sem_reserve(sem_id, sem_num);
  sem_wait_for_zero(sem_id, sem_num);
}

int* assign_friends(int* array, int size) {
  int i = 0;
  int* friends = init_list(MAX_NODES - 1);
  if (friends == NULL)
    return NULL;
  while (i < SO_NUM_FRIENDS) {
    int random_el = random_element(array, size);
    if (random_el == -1) {
      fprintf(stderr, "%s:%d: something went wrong with the extraction of friends\n", __FILE__, __LINE__);
      free(friends);
      return NULL;
    }
    /* se trova un pid già inserito, estrae un altro pid */
    if (find_element(friends, SO_NUM_FRIENDS, random_el) != -1)
      continue;
    friends[i] = random_el;
    i++;
  }
  return friends;
}

void handler(int signal) {
  switch (signal) {
  case SIGALRM:
    /*alarm per la stampa periodica*/
    ++periodical_flag;
    ++simulation_seconds;
    alarm(1);
    break;
  case SIGINT:
    cleanup();
    exit(EXIT_FAILURE);
    break;
  case SIGUSR2:
    fprintf(stderr, "[%d] master: sono stato notificato da un nodo che la size è stata superata\n", simulation_seconds);
    break;
  case SIGUSR1:
  {
    /* Creazione di un nuovo nodo*/
    msgqnum_t msg_num;
    /* setting errno to 0 as wait was interrupted*/
    errno = 0;
    {
      struct msqid_ds stats;
      bzero(&stats, sizeof(struct sembuf));
      msgctl(queue_id, IPC_STAT, &stats);
      TEST_ERROR_AND_FAIL;

      msg_num = stats.msg_qnum;
    }
    /* per risolvere il merge dei segnali, il master legge tutti i messaggi presenti nella coda */
    while (msg_num-- > 0) {
      struct msg message;
      transaction incoming_t;
      int child;
      int file_descriptors[2];

      pipe(file_descriptors);
      TEST_ERROR_AND_FAIL;

      if (msgrcv(queue_id, &message, sizeof(struct msg) - sizeof(long), 0, IPC_NOWAIT) == -1) {
        if (errno != ENOMSG) {
          TEST_ERROR_AND_FAIL;
        }
        else {
          break;
        }
      }
      incoming_t = message.mtext;

      /* refusing transaction if nodes array is full */
      if (*(nodes.size) == MAX_NODES) {
        int sender = incoming_t.sender;
        if (kill(sender, 0) != -1) {
          int user_q = msgget(getpid() - 1, 0);
          TEST_ERROR_AND_FAIL;
          if (refuse_transaction(incoming_t, user_q) == -1) {
            if (errno != EAGAIN) {
              TEST_ERROR_AND_FAIL;
            }
            else {
              fprintf(stderr, "master: refused transaction queue was full. Check ipcs usage.\n");
            }
          }
        }
        break;
      }

      switch ((child = fork())) {
      case -1:
        TEST_ERROR;
        cleanup();
        exit(EXIT_FAILURE);
        break;
      case 0:
      {
        int* friends;

        close(file_descriptors[1]);
        TEST_ERROR_AND_FAIL;

        {
          struct nodes nodes;
          nodes.array = shmat(shm_nodes_array_id, NULL, SHM_RDONLY);
          TEST_ERROR_AND_FAIL;
          nodes.size = shmat(shm_nodes_size_id, NULL, SHM_RDONLY);
          TEST_ERROR_AND_FAIL;

          friends = assign_friends(nodes.array, *(nodes.size));
          if (friends == NULL) {
            fprintf(stderr, "n%d: something went wrong while trying to create friends list\n" __FILE__, __LINE__);
            CHILD_STOP_SIMULATION;
            exit(EXIT_FAILURE);
          }

          shmdt(nodes.array);
          shmdt(nodes.size);
        }

        /* resetting signal mask to override the parent's */
        {
          sigset_t mask;
          sigfillset(&mask);
          sigprocmask(SIG_UNBLOCK, &mask, NULL);
        }

        init_node(friends, file_descriptors[0], shm_book_id, shm_book_size_id);
        break;
      }
      default:
        break;
      }

      sem_release(sem_id, ID_READY_ALL);
      TEST_ERROR_AND_FAIL;
      sem_wait_for_zero(sem_id, ID_READY_ALL);
      TEST_ERROR_AND_FAIL;

      {
        int i = 0, new_node_msg_id;
        int* extracted_nodes = init_list(SO_NUM_FRIENDS);
        TEST_ERROR_AND_FAIL;
        close(file_descriptors[0]);
        TEST_ERROR_AND_FAIL;

        /* sending through pipe the new generated node pid to SO_NUM_FRIENDS node */
        while (i < SO_NUM_FRIENDS) {
          int node_random = random_element(nodes.array, *(nodes.size));
          if (node_random == -1) {
            fprintf(stderr, "%s:%d: something went wrong with the extraction of the node\n", __FILE__, __LINE__);
            cleanup();
            exit(EXIT_FAILURE);
          }
          /* search if the current node_random wasn't already extracted */
          if (find_element(extracted_nodes, SO_NUM_FRIENDS, node_random) == -1) {
            int index = find_element(nodes.array, *(nodes.size), node_random);
            extracted_nodes[i] = node_random; /* add node_random to the list to avoid duplicates */
            write(nodes_write_fd[index], &child, sizeof(int));
            TEST_ERROR_AND_FAIL;
            kill(node_random, SIGUSR2);
            ++i;
          }
        }
        free(extracted_nodes);
        /* write the write end of the pipe in the same index of the node pid in nodes.array */
        nodes_write_fd[*(nodes.size)] = file_descriptors[1];

        new_node_msg_id = msgget(child, 0);
        TEST_ERROR_AND_FAIL;

        msgsnd(new_node_msg_id, &message, sizeof(struct msg) - sizeof(long), IPC_NOWAIT);
        TEST_ERROR_AND_FAIL;
        kill(child, SIGUSR1);

        nodes.array[*(nodes.size)] = child;
        (*nodes.size)++;
      }
    }
  }
  break;
  }
}

int main() {
  int i = 0;

  load_constants();

  nodes_write_fd = init_list(MAX_NODES);
  TEST_ERROR_AND_FAIL;
  node_budget = init_list(MAX_NODES);
  TEST_ERROR_AND_FAIL;
  user_budget = init_list(SO_USERS_NUM);
  TEST_ERROR_AND_FAIL;
  while (i < SO_USERS_NUM) {
    user_budget[i] = SO_BUDGET_INIT;
    i++;
  }
  printf("[!] MASTER PID: %d\n", getpid());

  /* Inizializzazione semafori start sincronizzato e write del libro mastro */
  sem_id = semget(getpid(), NUM_SEM, IPC_CREATION_FLAGS);
  TEST_ERROR_AND_FAIL;

  semctl(sem_id, ID_MEM, SETVAL, 1);
  TEST_ERROR_AND_FAIL;
  semctl(sem_id, ID_READY_ALL, SETVAL, SO_USERS_NUM + SO_NODES_NUM + 1);
  TEST_ERROR_AND_FAIL;
  semctl(sem_id, ID_READY_NODE, SETVAL, SO_NODES_NUM + 1);
  TEST_ERROR_AND_FAIL;

  /* Inizializzazione libro mastro */
  shm_book_id = shmget(IPC_PRIVATE, REGISTRY_SIZE, IPC_CREATION_FLAGS);
  TEST_ERROR_AND_FAIL;
  book.blocks = shmat(shm_book_id, NULL, 0);
  TEST_ERROR_AND_FAIL;
  shm_book_size_id = shmget(IPC_PRIVATE, sizeof(int), IPC_CREATION_FLAGS);
  TEST_ERROR_AND_FAIL;
  book.size = shmat(shm_book_size_id, NULL, 0);
  TEST_ERROR_AND_FAIL;
  *(book.size) = 0;

  /* Inizializzazione shm users array */
  shm_users_array_id = shmget(IPC_PRIVATE, sizeof(int) * SO_USERS_NUM, IPC_CREATION_FLAGS);
  TEST_ERROR_AND_FAIL;
  users = shmat(shm_users_array_id, NULL, 0);
  TEST_ERROR_AND_FAIL;

  /* Inizializzazione shm per size dei nodi */
  shm_nodes_size_id = shmget(IPC_PRIVATE, sizeof(int), IPC_CREATION_FLAGS);
  TEST_ERROR_AND_FAIL;
  nodes.size = shmat(shm_nodes_size_id, NULL, 0);
  TEST_ERROR_AND_FAIL;
  *(nodes.size) = SO_NODES_NUM;

  /* Inizializzazione shm nodi array */
  shm_nodes_array_id = shmget(IPC_PRIVATE, sizeof(int) * MAX_NODES, IPC_CREATION_FLAGS);
  TEST_ERROR_AND_FAIL;
  nodes.array = shmat(shm_nodes_array_id, NULL, 0);
  TEST_ERROR_AND_FAIL;
  /* Inizializzazione coda di messaggi master-nodi */
  queue_id = msgget(getpid(), IPC_CREATION_FLAGS);
  TEST_ERROR_AND_FAIL;
  /* Inizializzazione coda di messaggi di trasazioni rifiutate master-nodi-user */
  refused_queue_id = msgget(getpid() - 1, IPC_CREATION_FLAGS);
  TEST_ERROR_AND_FAIL;

  /* Handling segnali SIGALRM, SIGINT, SIGUSR1 */
  {
    sigset_t mask;
    struct sigaction act;
    bzero(&mask, sizeof(sigset_t));
    sigemptyset(&mask);
    act.sa_handler = handler;
    act.sa_flags = 0;
    act.sa_mask = mask;
    sigaction(SIGALRM, &act, NULL);
    TEST_ERROR_AND_FAIL;
    sigaction(SIGUSR2, &act, NULL);
    TEST_ERROR_AND_FAIL;

    sigaddset(&mask, SIGALRM);
    sigaddset(&mask, SIGUSR2);
    sigaddset(&mask, SIGUSR1);
    act.sa_mask = mask;
    sigaction(SIGINT, &act, NULL);
    TEST_ERROR_AND_FAIL;

    sigemptyset(&mask);
    sigaddset(&mask, SIGUSR2);
    sigaddset(&mask, SIGALRM);
    act.sa_mask = mask;
    sigaction(SIGUSR1, &act, NULL);
    TEST_ERROR_AND_FAIL;
  }

  i = 0;
  while (i < SO_NODES_NUM) {
    int fd[2], child;
    pipe(fd);
    TEST_ERROR_AND_FAIL;
    switch ((child = fork())) {
    case -1:
      TEST_ERROR;
      cleanup();
      exit(EXIT_FAILURE);
    case 0:
    {
      int* friends;

      close(fd[1]);
      TEST_ERROR_AND_FAIL;

      {
        int* nodes_arr = shmat(shm_nodes_array_id, NULL, SHM_RDONLY);
        TEST_ERROR_AND_FAIL;
        wait_siblings(ID_READY_NODE);
        if ((friends = assign_friends(nodes_arr, SO_NODES_NUM)) == NULL) {
          fprintf(stderr, "%s:%d: n%d: something went wrong with the creation of the friends list\n", __FILE__, __LINE__, getpid());
          CHILD_STOP_SIMULATION;
          exit(EXIT_FAILURE);
        }
        shmdt(nodes_arr);
      }


      init_node(friends, fd[0], shm_book_id, shm_book_size_id);
      break;
    }
    default:
      close(fd[0]);
      TEST_ERROR_AND_FAIL;
      nodes_write_fd[i] = fd[1];
      nodes.array[i] = child;
      i++;
      break;
    }
  }

  sem_reserve(sem_id, ID_READY_NODE);
  sem_wait_for_zero(sem_id, ID_READY_NODE);

  i = 0;
  while (i < SO_USERS_NUM) {
    int child;

    switch ((child = fork())) {
    case -1:
      fprintf(stderr, "master: fork failed for user creation iteration %d/%d.\n", i + 1, SO_USERS_NUM);
      cleanup();
      exit(EXIT_FAILURE);
    case 0:
    {
      int* users = shmat(shm_users_array_id, NULL, SHM_RDONLY);
      TEST_ERROR_AND_FAIL;
      wait_siblings(ID_READY_ALL);
      init_user(users, shm_nodes_array_id, shm_nodes_size_id, shm_book_id, shm_book_size_id);
      break;
    }
    default:
      users[i] = child;
      i++;
      break;
    }
  }

  sem_reserve(sem_id, ID_READY_ALL);
  TEST_ERROR_AND_FAIL;
  sem_wait_for_zero(sem_id, ID_READY_ALL);

  {
    int ending_reason, block_reached = 0;
    int* nof_transactions = init_list(MAX_NODES);
    TEST_ERROR_AND_FAIL;

    alarm(1);
    i = 0;
    while (simulation_seconds < SO_SIM_SEC && inactive_users < SO_USERS_NUM && *book.size < SO_REGISTRY_SIZE) {
      int status;
      int terminated_p;

      if (periodical_flag) {
        periodical_flag = 0;
        block_reached = periodical_update(block_reached);
        periodical_print();
      }

      terminated_p = wait(&status);
      if (terminated_p == -1) {
        if (errno == EINTR) {
          /* a signal interrupted the wait, starting the cicle again*/
          continue;
        }
        TEST_ERROR_AND_FAIL;
      }
      if (WIFEXITED(status)) {
        if (WEXITSTATUS(status) == EXIT_FAILURE) {
          fprintf(stderr, "Child %d encountered an error. Stopping simulation\n", terminated_p);
          free(nof_transactions);
          cleanup();
          exit(EXIT_FAILURE);
        }
        else {
          int size = *(nodes.size);
          int node_index = 0;
          if ((node_index = find_element(nodes.array, size, terminated_p)) != -1) {
            fprintf(stderr, "master: node %d terminated with the unexpected status %d while sim was ongoing. Check the code.\n", terminated_p, WEXITSTATUS(status));
            cleanup();
            exit(EXIT_FAILURE);
          }
          else {
            inactive_users++;
          }
        }
      }

    }
    alarm(0);
    {
      int sblock[2] = { SIGUSR1, SIGUSR2 };
      sig_block(sblock, 2);
    }
    stop_simulation();
    printf("\n\nSIMULATION ENDED IN %dS\n", simulation_seconds);

    /* wait all of the terminated child */
    {
      int terminated_p;
      int status;
      while ((terminated_p = wait(&status)) != -1) {
        int index;
        if ((index = find_element(nodes.array, *(nodes.size), terminated_p)) != -1) {
          if (WIFEXITED(status)) {
            nof_transactions[index] = WEXITSTATUS(status);
          }
        }
      }
    }
    if (errno != ECHILD) {
      TEST_ERROR_AND_FAIL;
    }

    ending_reason = -1;
    if (simulation_seconds == SO_SIM_SEC) {
      ending_reason = SIM_END_SEC;
    }
    else if (inactive_users == SO_USERS_NUM) {
      ending_reason = SIM_END_USR;
    }
    else if (*book.size == SO_REGISTRY_SIZE) {
      ending_reason = SIM_END_SIZ;
    }

    periodical_update(block_reached);
    summary_print(ending_reason, users, user_budget, nodes, node_budget, nof_transactions);
    free(nof_transactions);
  }

  users = NULL;
  nodes.array = NULL;
  cleanup();

  printf("Completed simulation. Exiting...\n");
  exit(EXIT_SUCCESS);
}