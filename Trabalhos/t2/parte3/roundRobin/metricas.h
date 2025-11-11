// metricas.h
#pragma once

#include "irq.h"       // Para N_IRQ
#include "processo.h"  // Para metricas_processo_final_t e MAX_PROCESSES

// AGRUPAR todos os campos de métrica
typedef struct metricas_t {
  int num_proc_criados;
  int tempo_ocioso;
  int contagem_irq[N_IRQ];
  int num_preemcoes_total;
  int tempo_ultima_atualizacao_metricas;
  metricas_processo_final_t historico_metricas[MAX_PROCESSES];
} metricas_t;

// --- PROTÓTIPOS DAS FUNÇÕES DE MÉTRICAS ---
// (Note que elas ainda precisam do so_t* para acessar dados
//  como a tabela de processos, E/S, etc.)

// Precisamos declarar 'so_t' como um tipo incompleto (opaco)
struct so_t;
struct pcb;
enum estado_processo; // ou inclua o header que define isso

// Funções de gerenciamento da struct metricas_t
metricas_t* metricas_cria(void);
void metricas_destroi(metricas_t *self);

// Funções de métricas que você quer mover
int so_tempo_total(struct so_t *self);
void inicializa_metricas_pcb(pcb *proc, int tempo_atual);
void so_muda_estado(struct so_t *self, pcb *proc, estado_processo novo_estado);
void so_atualiza_tempos(struct so_t *self);
void so_salva_metricas_finais(struct so_t *self, pcb *proc);
const char *estado_nome(estado_processo estado);
void imprimir_dados(struct so_t *self);