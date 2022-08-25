#ifndef _TRANSACTION_H_
#define _TRANSACTION_H_

#define SO_BLOCK_SIZE 10

typedef struct
{
  long timestamp;
  int sender;
  int receiver;
  int quantita;
  int reward;
} transaction;

struct master_book {
  /* dimensione codificata in numero di blocchi di transazioni */
  int* size;
  transaction* blocks;
};

struct msg {
  long mtype;
  transaction mtext;
};


void new_transaction(transaction* new, int sender, int reciever, int quantita, int reward);
void print_transaction(transaction t);
int find_element_in_book(struct master_book book, int limit, transaction x);

#endif