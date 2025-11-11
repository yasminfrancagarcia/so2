#include "processo.h"
#include <stdio.h>
#include <stdlib.h>

int pid_inicial = 1;

pcb* criar_processo(int pc, dispositivo_id_t entrada, dispositivo_id_t saida) {
    pcb* novo_processo = (pcb*)malloc(sizeof(pcb));
    if (novo_processo == NULL) {
        printf("Erro ao alocar memória para o novo processo.\n");
        return NULL;
    }
    novo_processo->usando = 1; // Marcado como ocupado
    novo_processo->pid = pid_inicial++; //processo começa com pid 1 
    novo_processo->estado = P_PRONTO; // Estado inicial como pronto
    novo_processo->ctx_cpu.pc = pc; //salva o antigo valor de pc 
    novo_processo->ctx_cpu.regA = 0;
    novo_processo->ctx_cpu.regX = 0;
    novo_processo->ctx_cpu.erro = 0;
    novo_processo->entrada = entrada;
    novo_processo->saida = saida;
    novo_processo->dispositivo_bloqueado = -1; // Nenhum dispositivo bloqueado inicialmente
    novo_processo->pid_esperando = -1; // Nenhum processo esperando inicialmente
    novo_processo->quantum = QUANTUM; // Inicializa o quantum
    novo_processo->prioridade = 0.5; // Inicializa a prioridade com 0.5

    return novo_processo;
}

void mata_processo(pcb* processo) {
    if (processo != NULL) {
        processo->usando = 0; // marca como livre
        processo->estado = P_TERMINOU; // Atualiza estado para terminado
        free(processo); // Libera a memória alocada
    }
}

void atualiza_prioridade(pcb * proc){
    // prio = (prio + t_exec/t_quantum) / 2 
    //onde t_exec é o tempo desde que ele foi escolhido para executar e t_quantum é o
    // tempo do quantum. O t_exec é o quantum menos o valor da variável que o 
    //escalonador decrementa a cada interrupção. 
    proc->prioridade = (proc->prioridade + proc->quantum/ QUANTUM) / 2.0;
}