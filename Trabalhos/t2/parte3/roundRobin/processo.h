#ifndef PROCESSO_H
#define PROCESSO_H

#define QUANTUM 10

#include <stdio.h>

#include "dispositivos.h"

typedef enum {
    P_PRONTO,       // pronto para executar
    P_EXECUTANDO,     // em execução
    P_BLOQUEADO,     // esperando (por E/S ou recurso)
    P_TERMINOU,       // processo terminou
    //serve para ser o tamanho do arr que guarda quantas vezes o processo esteve em cada estado
    P_N_ESTADOS // número de estados, atualmente 4
} estado_processo;

typedef struct {
    int pc;
    int regA;
    int regX;
    err_t erro;
} cpu_ctx;

/* 1- número de processos criados
2- tempo total de execução
3 tempo total em que o sistema ficou ocioso (todos os processos bloqueados)
4- número de interrupções recebidas de cada tipo
5- número de preempções
PROCESSO INDIVIDUAL
6- tempo de retorno de cada processo (diferença entre data do término e da criação)
7- número de preempções de cada processo
8- número de vezes que cada processo entrou em cada estado (pronto, bloqueado, executando)
9- tempo total de cada processo em cada estado (pronto, bloqueado, executando)
10- tempo médio de resposta de cada processo (tempo entre desbloquear e ser escalonado)
 */
typedef struct {
    int usando;         // 1 se ocupado, 0 se livre
    int pid;          // identificador único do processo
    estado_processo estado;    // estado atual
    cpu_ctx ctx_cpu;         // registradores salvos
    dispositivo_id_t entrada;
    dispositivo_id_t saida;

    int dispositivo_bloqueado; // dispositivo que causou o bloqueio (se houver)
    int pid_esperando;       // PID do processo que está esperando este (se houver)
    int quantum;              // tempo restante no quantum
    //métricas
    int tempo_criacao;      // 6 - tempo de criação do processo
    int tempo_termino;      // 6- tempo de término do processo
    int num_preempcoes_proc;     // 7 - número de preempções do processo
    int contagem_estados[P_N_ESTADOS]; // 8 - número de vezes em cada estado
    int tempo_em_estado[P_N_ESTADOS]; // 9 - tempo total em cada
    int tempo_ultima_mudanca_estado; // Timestamp da última mudança
    // 10- tempo médio de resposta (desbloqueio -> escalonamento)
    //ver se nao precisa de uma variavel que diga quando o processo foi bloqueado
    int tempo_desbloqueou; // Timestamp de quando saiu de BLOQUEADO
    int tempo_total_resposta_pos_bloqueio;// soma dos tempos de resposta pós bloqueio
    int num_respostas_pos_bloqueio; // N. de vezes que foi de BLOQUEADO -> PRONTO 
} pcb;

//a struct que guardará as métricas finais, é um histórico de processos finalizados
typedef struct {
    int pid;
    bool utilizado; // Para sabermos se esse slot do histórico foi usado

    //métrica 6
    int tempo_criacao;
    int tempo_termino;

    //métrica 7
    int num_preempcoes_proc;

    //métrica 8
    int contagem_estados[P_N_ESTADOS];

    //métrica 9
    int tempo_em_estado[P_N_ESTADOS];

    //métrica 10
    int tempo_total_resposta_pos_bloqueio;
    int num_respostas_pos_bloqueio;
} metricas_processo_final_t;

pcb* criar_processo(int pc, dispositivo_id_t entrada, dispositivo_id_t saida);

void mata_processo(pcb* processo);


#endif


