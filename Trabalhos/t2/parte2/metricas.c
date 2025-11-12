// metricas.c
#include "metricas.h"
#include "so.h" // Inclua para ter acesso aos GETTERS que vamos criar
#include "err.h"
#include "console.h" // Para console_printf
#include <stdlib.h>

// --- Funções de gerenciamento ---

metricas_t* metricas_cria(void) {
  metricas_t *self = malloc(sizeof(*self));
  if (self == NULL) return NULL;

  self->num_proc_criados = 0;
  self->tempo_ocioso = 0;
  self->num_preemcoes_total = 0;
  self->tempo_ultima_atualizacao_metricas = 0;
  
  for (int i = 0; i < N_IRQ; i++) {
    self->contagem_irq[i] = 0;
  }
  for (int i = 0; i < MAX_PROCESSES; i++) {
    self->historico_metricas[i].utilizado = false;
    self->historico_metricas[i].pid = -1;
  }
  return self;
}

void metricas_destroi(metricas_t *self) {
  free(self);
}

// --- Funções de Métricas  ---
/*
 Retorna o tempo total de ciclos/instruções executadas no sistema.
 Faz isso lendo o registrador 0 (D_RELOGIO_INSTRUCOES) do dispositivo de relógio.
 */
int so_tempo_total(struct so_t *self)
{
    int tempo_atual;
    // Use o getter para pegar o 'es' (que é privado do so_t)
    es_t *es = so_get_es(self); 

    if (es_le(es, D_RELOGIO_INSTRUCOES, &tempo_atual) != ERR_OK) {
        // Não podemos acessar 'self->erro_interno' diretamente
        // Mas podemos logar
        console_printf("MÉTRICA: ERRO FATAL AO LER O RELOGIO!");
        return 0; 
    }
    return tempo_atual;
}
//função auxiliar para inicializar métricas da pcb
void inicializa_metricas_pcb(pcb *proc, int tempo_atual)
{
  // métricas 6, 7, 8, 9, 10
  proc->tempo_criacao = tempo_atual;
  proc->tempo_termino = -1;
  proc->num_preempcoes_proc = 0;
  proc->tempo_desbloqueou = -1;
  proc->tempo_total_resposta_pos_bloqueio = 0;
  proc->num_respostas_pos_bloqueio = 0;
  proc->tempo_ultima_mudanca_estado = tempo_atual;

  for (int i = 0; i < P_N_ESTADOS; i++)
  {
    proc->contagem_estados[i] = 0;
    proc->tempo_em_estado[i] = 0;
  }
}
//Função para centralizar a mudança de estado e contabilizar (Métricas 8 e 9)
void so_muda_estado(struct so_t *self, pcb *proc, estado_processo novo_estado)
{
  if (proc == NULL || proc->estado == novo_estado)
    return;

  int tempo_atual = so_tempo_total(self);
  // contabiliza tempo gasto no estado anterior
  int delta_t = tempo_atual - proc->tempo_ultima_mudanca_estado;
  proc->tempo_em_estado[proc->estado] += delta_t; // metríca 9
  // atualiza para o novo estado
  proc->estado = novo_estado;
  proc->contagem_estados[novo_estado]++; // metríca 8
  proc->tempo_ultima_mudanca_estado = tempo_atual;

  //lógica específica para Métrica 10 (tempo de Resposta)
  if (novo_estado == P_PRONTO && proc->dispositivo_bloqueado != -1)
  {
    //acabou de ser desbloqueado (vinha de P_BLOQUEADO)
    proc->tempo_desbloqueou = tempo_atual;
    proc->num_respostas_pos_bloqueio++;
  }
  else if (novo_estado == P_EXECUTANDO && proc->tempo_desbloqueou != -1)
  {
    //foi escalonado após um desbloqueio
    int tempo_espera = tempo_atual - proc->tempo_desbloqueou; //tempo de espera entre desbloqueio e escalonamento
    proc->tempo_total_resposta_pos_bloqueio += tempo_espera;
    proc->tempo_desbloqueou = -1; // Reseta flag
  }
}

//atualiza o tempo ocioso e o tempo em estado dos processos (Métricas 3 e 9)
//deve ser chamada NO INICIO de so trata interrupcao
void so_atualiza_tempos(struct so_t *self)
{
    int tempo_atual = so_tempo_total(self);
    // usar GETTERS para acessar os dados
    metricas_t *m = so_get_metricas(self); 
    pcb **tabela_processos = so_get_tabela_de_processos(self);
    int processo_corrente = so_get_processo_corrente(self);
    int delta_t = tempo_atual - m->tempo_ultima_atualizacao_metricas;

    //int delta_t = tempo_atual - self->tempo_ultima_atualizacao_metricas;
 
    if (delta_t == 0)
        return;

    if (processo_corrente == NO_PROCESS){
        // METRICA 3: Sistema estava ocioso
        //self->tempo_ocioso += delta_t;
        m->tempo_ocioso += delta_t;
    }
    else{
        // metrica 9: Atualiza tempo do processo que estava executando
        pcb *proc = tabela_processos[processo_corrente];
        if (proc != NULL && proc->estado == P_EXECUTANDO){
        proc->tempo_em_estado[P_EXECUTANDO] += delta_t;
        proc->tempo_ultima_mudanca_estado = tempo_atual;
        }
    }

    // Métrica 9: Atualiza tempo de processos prontos ou bloqueados
    for (int i = 0; i < MAX_PROCESSES; i++){
        pcb *p = tabela_processos[i];
        if (p != NULL && i != processo_corrente){
        if (p->estado == P_PRONTO || p->estado == P_BLOQUEADO){
            p->tempo_em_estado[p->estado] += delta_t;
            p->tempo_ultima_mudanca_estado = tempo_atual;
        }
        }
    }
    m->tempo_ultima_atualizacao_metricas = tempo_atual;
}
//função chamada IMEDIATAMENTE ANTES de dar free() em um PCB, para salvar as métricas finais
// no histórico, salcva as métricas do processo que está sendo finalizado
void so_salva_metricas_finais(struct so_t *self, pcb *proc)
{
  if (proc == NULL)
    return;
  // acha um slot no histórico.
  // usaremos o (pid - 1) como índice (assume que PID começa em 1)
  int idx = proc->pid - 1;

  if (idx < 0 || idx >= MAX_PROCESSES)
  {
    console_printf("SO: ERRO DE MÉTRICA: PID %d fora do limite do histórico!", proc->pid);
    return;
  }

  //atualiza o tempo final no estado TERMINOU
  int tempo_atual = so_tempo_total(self);
  metricas_t *m = so_get_metricas(self);
  
  if (proc->tempo_termino == -1)
  { //garante que o tempo de término foi setado
    proc->tempo_termino = tempo_atual;
  }

  //contabiliza o último delta de tempo
  int delta_t = proc->tempo_termino - proc->tempo_ultima_mudanca_estado;
  proc->tempo_em_estado[proc->estado] += delta_t;

  // Copia os dados para o histórico
  metricas_processo_final_t *hist = &m->historico_metricas[idx];

  hist->utilizado = true;
  hist->pid = proc->pid;
  hist->tempo_criacao = proc->tempo_criacao;
  hist->tempo_termino = proc->tempo_termino;
  hist->num_preempcoes_proc = proc->num_preempcoes_proc;
  hist->tempo_total_resposta_pos_bloqueio = proc->tempo_total_resposta_pos_bloqueio;
  hist->num_respostas_pos_bloqueio = proc->num_respostas_pos_bloqueio;

  for (int i = 0; i < P_N_ESTADOS; i++)
  {
    hist->contagem_estados[i] = proc->contagem_estados[i];
    hist->tempo_em_estado[i] = proc->tempo_em_estado[i];
  }

  console_printf("SO: Métricas finais do PID %d salvas no histórico.", proc->pid);
}

const char *estado_nome(estado_processo estado)
{
  switch (estado){
  case P_PRONTO:
    return "Pronto";
  case P_EXECUTANDO:
    return "Executando";
  case P_BLOQUEADO:
    return "Bloqueado";
  case P_TERMINOU:
    return "Terminou";
  default:
    return "Desconhecido";
  }
}

void imprimir_dados(struct so_t *self)
{
  // Atualiza uma última vez os tempos antes de imprimir
  so_atualiza_tempos(self);
  metricas_t *m = so_get_metricas(self);
  pcb **tabela_processos = so_get_tabela_de_processos(self);
  // garante que o tempo do último processo a terminar seja contabilizado
  for (int i = 0; i < MAX_PROCESSES; i++)
  {
    pcb *p = tabela_processos[i];
    if (p != NULL && p->estado != P_TERMINOU)
    {
      int tempo_atual = so_tempo_total(self);
      int delta_t = tempo_atual - p->tempo_ultima_mudanca_estado;
      p->tempo_em_estado[p->estado] += delta_t;
    }
  }

  console_printf("\nMÉTRICAS GLOBAIS DO SISTEMA ");

  // Métrica 1: Número de processos criados
  console_printf("1. Número total de processos criados: %d", m->num_proc_criados);

  // Métrica 2: Tempo total de execução
  int tempo_total = so_tempo_total(self);
  console_printf("2. Tempo total de execução do sistema: %d ciclos", tempo_total);

  // Métrica 3: Tempo total ocioso
  console_printf("3. Tempo total em que o sistema ficou ocioso: %d ciclos (%.2f%%)",
  m->tempo_ocioso,
  tempo_total > 0 ? (double)m->tempo_ocioso / tempo_total * 100.0 : 0.0);

  // Métrica 4: Número de interrupções por tipo
  console_printf("4. Número de interrupções recebidas:");
  for (int i = 0; i < N_IRQ; i++)
  {
    if (m->contagem_irq[i] > 0)
    {
      console_printf("   - IRQ %d (%s): %d", i, irq_nome(i), m->contagem_irq[i]);
    }
  }

  // Métrica 5: Número de preempções
  console_printf("5. Número total de preempções (troca por quantum): %d", m->num_preemcoes_total );

  console_printf("\n--- MÉTRICAS POR PROCESSO ---");
  // Antes de imprimir, faz uma última varredura na tabela de processos
  // para salvar as métricas de quem ainda não foi liberado (ex: o próprio init
  // ou outros processos que sobraram)
  int tempo_final = so_tempo_total(self);
  for (int i = 0; i < MAX_PROCESSES; i++)
  {
    pcb *p = tabela_processos[i];
    if (p != NULL)
    {
      //se o processo estava rodando ou pronto, seu "término" é agora
      if (p->estado != P_TERMINOU)
      {
        so_muda_estado(self, p, P_TERMINOU); //mude para terminado
        p->tempo_termino = tempo_final;      // setar o tempo final
      }
      //salva as métricas de quem sobrou
      so_salva_metricas_finais(self, p);
    }
  }

  // imprime TUDO o que está no histórico
  // self->num_proc_criados tem o número total de processos que já existiram
  for (int i = 0; i < m->num_proc_criados; i++){
    //pega a métrica salva do histórico (índice i == pid i+1)
    metricas_processo_final_t *p = &m->historico_metricas[i];

    // Verifica se há dados (se o pid foi salvo)
    if (!p->utilizado)
      continue;

    console_printf("\n>> Processo PID: %d", p->pid);

    //métrica 6: Tempo de retorno
    if (p->tempo_termino != -1)
    {
      int turnaround = p->tempo_termino - p->tempo_criacao;
      console_printf("6. Tempo de Retorno (Turnaround): %d ciclos (Criado: %d, Terminado: %d)",
      turnaround, p->tempo_criacao, p->tempo_termino);
    }
    else
    {
      console_printf("6. Tempo de Retorno: Processo NÃO terminou (Criado: %d)", p->tempo_criacao);
    }

    // Métrica 7: Preempções
    console_printf("7. Número de preempções sofridas: %d", p->num_preempcoes_proc);

    // Métrica 8: Vezes em cada estado
    console_printf("8. Entradas em cada estado:");
    for (int j = 0; j < P_N_ESTADOS; j++)
    {
      console_printf("   - %s: %d vez(es)", estado_nome(j), p->contagem_estados[j]);
    }

    // Métrica 9: Tempo em cada estado
    console_printf("9. Tempo total em cada estado:");
    for (int j = 0; j < P_N_ESTADOS; j++)
    {
      console_printf("   - %s: %d ciclos", estado_nome(j), p->tempo_em_estado[j]);
    }

    // Métrica 10: Tempo médio de resposta
    if (p->num_respostas_pos_bloqueio > 0)
    {
      double tempo_medio_resp = (double)p->tempo_total_resposta_pos_bloqueio / p->num_respostas_pos_bloqueio;
      console_printf("10. Tempo médio de resposta (pós-bloqueio): %.2f ciclos (Total: %d / %d eventos)",
      tempo_medio_resp, p->tempo_total_resposta_pos_bloqueio, p->num_respostas_pos_bloqueio);
    }
    else
    {
      console_printf("10. Tempo médio de resposta (pós-bloqueio): N/A (nunca foi desbloqueado)");
    }
  }
}