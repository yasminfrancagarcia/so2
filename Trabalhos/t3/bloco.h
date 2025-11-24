#ifndef BLOCO_H
#define BLOCO_H
#include <stdbool.h>
#include "cpu.h" //pegar CPU_END_FIM_PROT
#include "mmu.h" //pegar TAM_PAGINA
#include <stdint.h>
//Queremos saber quantas blocos de memória o SO precisa reservar para si

// N é a quantidade de espaços de memória que o SO reserva para si

// CPU_END_FIM_PROT é o numero do último endereço da memória protegida
// entao o espaço total reservado  é CPU_END_FIM_PROT + 1

// TAM_PAGINA é o tamanho de cada página


#define BLOCOS_RESERVADOS ((CPU_END_FIM_PROT +1) / TAM_PAGINA )//número de blocos reservados para o SO
typedef struct bloco{
    bool ocupado;
    int pid; //pid do processo que está usando este bloco
    int pg; //
    int ciclos; //guardar momento em que a página entrou na memória
    uint32_t acesso; // para algoritmo de substituição LRU

} bloco_t;

bloco_t* cria_bloco(int tamanho);
#endif