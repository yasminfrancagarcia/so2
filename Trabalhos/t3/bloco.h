#ifndef BLOCO_H
#define BLOCO_H
#include <stdbool.h>
#include "cpu.h" //pegar CPU_END_FIM_PROT
#include "mmu.h" //pegar TAM_PAGINA


#define BLOCOS_RESERVADOS ((CPU_END_FIM_PROT + TAM_PAGINA - 1) / TAM_PAGINA )//número de blocos reservados para o SO
typedef struct bloco{
    bool ocupado;
    int pid; //pid do processo que está usando este bloco
    int pg; //
    int ciclos; //quantos ciclos a página está na memória
} bloco_t;

bloco_t* cria_bloco(int tamanho);
#endif