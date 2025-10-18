#ifndef PROCESSO_H
#define PROCESSO_H

#include <stdio.h>

#include "dispositivos.h"

typedef enum {
    P_FREE = 0,    // entrada livre
    P_PRONTO,       // pronto para executar
    P_EXECUTANDO,     // em execução
    P_BLOQUEADO,     // esperando (por E/S ou recurso)
    P_TERMINOU       // processo terminou
} estado_processo;

typedef struct {
    int pc;
    int regA;
    int regX;
    err_t erro;
} cpu_ctx;

typedef struct {
    int usando;         // 1 se ocupado, 0 se livre
    int pid;          // identificador único do processo
    estado_processo estado;    // estado atual
    cpu_ctx ctx_cpu;         // registradores salvos
    dispositivo_id_t entrada;
    dispositivo_id_t saida;

    int dispositivo_bloqueado; // dispositivo que causou o bloqueio (se houver)
    int pid_esperando;       // PID do processo que está esperando este (se houver)
} pcb;


pcb* criar_processo(int pc, dispositivo_id_t entrada, dispositivo_id_t saida);

void mata_processo(pcb* processo);


#endif


