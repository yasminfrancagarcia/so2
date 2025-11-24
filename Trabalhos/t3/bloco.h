#ifndef BLOCO_H
#define BLOCO_H
#include <stdbool.h>
typedef struct bloco{
    bool ocupado;
    int pid; //pid do processo que está usando este bloco
    int pg; //
    int ciclos; //quantos ciclos a página está na memória
} bloco_t;

bloco_t* cria_bloco(int tamanho);
#endif