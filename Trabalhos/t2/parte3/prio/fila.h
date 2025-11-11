#ifndef FILA_H
#define FILA_H
#include <stdbool.h>

typedef struct fila_no {
    int pid;
    struct fila_no* prox;
} fila_no;


typedef struct fila {
    int tamanho;
    fila_no* inicio;
} fila;

fila* cria_fila();
bool fila_vazia(fila* f);
void destroi_fila(fila* f);
void enfileira(fila* f, int pid);
int desenfileira(fila* f, int pid);
void imprime_fila(fila* f);

#endif