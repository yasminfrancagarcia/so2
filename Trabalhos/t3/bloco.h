#ifndef BLOCO_H
#define BLOCO_H
#include <stdbool.h>
#include "cpu.h" //pegar CPU_END_FIM_PROT
#include "mmu.h" //pegar TAM_PAGINA

// CPU_END_FIM_PROT é o numero do último endereço da memória protegida
//ou seja, o numero de bytes reservados para o SO é CPU_END_FIM_PROT
// TAM_PAGINA é o tamanho de cada página em bytes
//se N for múltiplo exato de TAM_PAGINA, o resultado é N / TAM_PAGINA.
//Se não for múltiplo, precisamos arredondar para cima: ceil(N / TAM_PAGINA)
//Em aritmética inteira em C, a divisão trunca (arredonda para baixo)
//Então, para simular o “arredondamento para cima” sem usar float,
// somamos TAM_PAGINA-1 ao numerador antes de dividir:
//N = 99 -> (99 + 9) / 10 = 108 / 10 = 10 (arredonda pra cima)
//N = 100 -> (100 + 9) / 10 = 109 / 10 = 10 (exato)
//N = 101 -> (101 + 9) / 10 = 110 / 10 = 11 (arredonda pra cima)
#define BLOCOS_RESERVADOS ((CPU_END_FIM_PROT + TAM_PAGINA - 1) / TAM_PAGINA )//número de blocos reservados para o SO
typedef struct bloco{
    bool ocupado;
    int pid; //pid do processo que está usando este bloco
    int pg; //
    int ciclos; //quantos ciclos a página está na memória
} bloco_t;

bloco_t* cria_bloco(int tamanho);
#endif