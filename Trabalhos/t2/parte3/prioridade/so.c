

// ---------------------------------------------------------------------
// INCLUDES {{{1
// ---------------------------------------------------------------------

#include "so.h"
#include "dispositivos.h"
#include "err.h"
#include "irq.h"
#include "memoria.h"
#include "programa.h"
#include "processo.h"
#include "fila.h"

#include <stdlib.h>
#include <stdbool.h>

// ---------------------------------------------------------------------
// CONSTANTES E TIPOS {{{1
// ---------------------------------------------------------------------

// intervalo entre interrupções do relógio
#define INTERVALO_INTERRUPCAO 50 // em instruções executadas

#define TERMINAIS 4

// constantes de processos
#define MAX_PROCESSES 4
#define NO_PROCESS -1

//métricas
/* 1- número de processos criados
2- tempo total de execução
3 tempo total em que o sistema ficou ocioso (todos os processos bloqueados)
4- número de interrupções recebidas de cada tipo
5- número de preempções */
struct so_t
{
  cpu_t *cpu;
  mem_t *mem;
  es_t *es;
  console_t *console;
  bool erro_interno;

  int regA, regX, regPC, regERRO; // cópia do estado da CPU
  // t2: tabela de processos, processo corrente, pendências, etc
  pcb *tabela_de_processos[MAX_PROCESSES];
  int processo_corrente; // índice na tabela de processos
  // vetor para guardar os pids dos processos que estão usando os terminais
  // idx = 0 -> terminal A
  // idx = 1 -> terminal B...
  int terminais_usados[4];
  fila *fila_prontos;   // fila de processos prontos
  //métricas
  int num_proc_criados; // 1- número de processos ativos (métrica 2 nao precisa ser guardada aqui)
  int tempo_ocioso;    // 3- tempo total em que o sistema ficou ocioso
  int contagem_irq[N_IRQ]; // 4- número de interrupções recebidas de cada tipo
  int num_preemcoes_total;      // 5- número de preempções
  //auxiliar metricas 3 e 9
  int tempo_ultima_atualizacao_metricas; // Timestamp da última atualização de métricas
  //Novo Campo para Histórico, guardará as métricas de TODOS os processos que já existiram
  metricas_processo_final_t historico_metricas[MAX_PROCESSES];
};

// função de tratamento de interrupção (entrada no SO)
static int so_trata_interrupcao(void *argC, int reg_A);

// funções auxiliares
// carrega o programa contido no arquivo na memória do processador; retorna end. inicial
static int so_carrega_programa(so_t *self, char *nome_do_executavel);
// copia para str da memória do processador, até copiar um 0 (retorna true) ou tam bytes
static bool copia_str_da_mem(int tam, char str[tam], mem_t *mem, int ender);

// ---------------------------------------------------------------------
// CRIAÇÃO {{{1
// ---------------------------------------------------------------------

so_t *so_cria(cpu_t *cpu, mem_t *mem, es_t *es, console_t *console)
{
  so_t *self = malloc(sizeof(*self));
  if (self == NULL)
    return NULL;

  self->cpu = cpu;
  self->mem = mem;
  self->es = es;
  self->console = console;
  self->erro_interno = false;
  // processos
  self->processo_corrente = NO_PROCESS;

  // quando a CPU executar uma instrução CHAMAC, deve chamar a função
  //   so_trata_interrupcao, com primeiro argumento um ptr para o SO
  cpu_define_chamaC(self->cpu, so_trata_interrupcao, self);
  self->fila_prontos = cria_fila();
  self->num_proc_criados = 0;
  // inicializar terminais
  for (int i = 0; i < TERMINAIS; i++)
  {
    self->terminais_usados[i] = 0; // nenhum terminal está sendo usado
  }
  // inicializar histórico de métricas
  for (int i = 0; i < MAX_PROCESSES; i++)
  {
    self->historico_metricas[i].utilizado = false; //nenhum slot usado ainda
    self->historico_metricas[i].pid = -1; //nenhum pid ainda 
  }
  //inicialização das mtricas 
  self->tempo_ocioso = 0;
  self->num_preemcoes_total = 0;
  self->tempo_ultima_atualizacao_metricas = 0; // Tempo inicial é 0
  return self;
}

void so_destroi(so_t *self)
{
  cpu_define_chamaC(self->cpu, NULL, NULL);
  free(self);
}

// ---------------------------------------------------------------------
// FUNÇÕES PARA AS MÉTRICAS
// ---------------------------------------------------------------------

/**
 * Retorna o tempo total de ciclos/instruções executadas no sistema.
 * Faz isso lendo o registrador 0 (D_RELOGIO_INSTRUCOES) do dispositivo de relógio.
 */
static int so_tempo_total(so_t *self)
{
    int tempo_atual;

    // Esta é a constante correta
    if (es_le(self->es, D_RELOGIO_INSTRUCOES, &tempo_atual) != ERR_OK) {
        console_printf("SO: ERRO FATAL AO LER O RELOGIO (INSTRUCOES)!");
        self->erro_interno = true;
        return 0; // Retorna 0 em caso de erro
    }
    
    return tempo_atual;
}
//função auxiliar para inicializar métricas da pcb
static void inicializa_metricas_pcb(pcb *proc, int tempo_atual)
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
static void so_muda_estado(so_t *self, pcb *proc, estado_processo novo_estado)
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
static void so_atualiza_tempos(so_t *self)
{
  int tempo_atual = so_tempo_total(self);
  int delta_t = tempo_atual - self->tempo_ultima_atualizacao_metricas;

  if (delta_t == 0)
    return;

  if (self->processo_corrente == NO_PROCESS){
    // METRICA 3: Sistema estava ocioso
    self->tempo_ocioso += delta_t;
  }
  else{
    // metrica 9: Atualiza tempo do processo que estava executando
    pcb *proc = self->tabela_de_processos[self->processo_corrente];
    if (proc != NULL && proc->estado == P_EXECUTANDO){
      proc->tempo_em_estado[P_EXECUTANDO] += delta_t;
      proc->tempo_ultima_mudanca_estado = tempo_atual;
    }
  }

  // Métrica 9: Atualiza tempo de processos prontos ou bloqueados
  for (int i = 0; i < MAX_PROCESSES; i++){
    pcb *p = self->tabela_de_processos[i];
    if (p != NULL && i != self->processo_corrente){
      if (p->estado == P_PRONTO || p->estado == P_BLOQUEADO){
        p->tempo_em_estado[p->estado] += delta_t;
        p->tempo_ultima_mudanca_estado = tempo_atual;
      }
    }
  }

  self->tempo_ultima_atualizacao_metricas = tempo_atual;
}
//função chamada IMEDIATAMENTE ANTES de dar free() em um PCB, para salvar as métricas finais
// no histórico, salcva as métricas do processo que está sendo finalizado
static void so_salva_metricas_finais(so_t *self, pcb *proc)
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
  if (proc->tempo_termino == -1)
  { //garante que o tempo de término foi setado
    proc->tempo_termino = tempo_atual;
  }

  //contabiliza o último delta de tempo
  int delta_t = proc->tempo_termino - proc->tempo_ultima_mudanca_estado;
  proc->tempo_em_estado[proc->estado] += delta_t;

  // Copia os dados para o histórico
  metricas_processo_final_t *m = &self->historico_metricas[idx];

  m->utilizado = true;
  m->pid = proc->pid;
  m->tempo_criacao = proc->tempo_criacao;
  m->tempo_termino = proc->tempo_termino;
  m->num_preempcoes_proc = proc->num_preempcoes_proc;
  m->tempo_total_resposta_pos_bloqueio = proc->tempo_total_resposta_pos_bloqueio;
  m->num_respostas_pos_bloqueio = proc->num_respostas_pos_bloqueio;

  for (int i = 0; i < P_N_ESTADOS; i++)
  {
    m->contagem_estados[i] = proc->contagem_estados[i];
    m->tempo_em_estado[i] = proc->tempo_em_estado[i];
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

void imprimir_dados(so_t *self)
{
  // Atualiza uma última vez os tempos antes de imprimir
  so_atualiza_tempos(self);
  // garante que o tempo do último processo a terminar seja contabilizado
  for (int i = 0; i < MAX_PROCESSES; i++)
  {
    pcb *p = self->tabela_de_processos[i];
    if (p != NULL && p->estado != P_TERMINOU)
    {
      int tempo_atual = so_tempo_total(self);
      int delta_t = tempo_atual - p->tempo_ultima_mudanca_estado;
      p->tempo_em_estado[p->estado] += delta_t;
    }
  }

  console_printf("\nMÉTRICAS GLOBAIS DO SISTEMA ");

  // Métrica 1: Número de processos criados
  console_printf("1. Número total de processos criados: %d", self->num_proc_criados);

  // Métrica 2: Tempo total de execução
  int tempo_total = so_tempo_total(self);
  console_printf("2. Tempo total de execução do sistema: %d ciclos", tempo_total);

  // Métrica 3: Tempo total ocioso
  console_printf("3. Tempo total em que o sistema ficou ocioso: %d ciclos (%.2f%%)",
  self->tempo_ocioso,
  tempo_total > 0 ? (double)self->tempo_ocioso / tempo_total * 100.0 : 0.0);

  // Métrica 4: Número de interrupções por tipo
  console_printf("4. Número de interrupções recebidas:");
  for (int i = 0; i < N_IRQ; i++)
  {
    if (self->contagem_irq[i] > 0)
    {
      console_printf("   - IRQ %d (%s): %d", i, irq_nome(i), self->contagem_irq[i]);
    }
  }

  // Métrica 5: Número de preempções
  console_printf("5. Número total de preempções (troca por quantum): %d", self->num_preemcoes_total );

  console_printf("\n--- MÉTRICAS POR PROCESSO ---");
  // Antes de imprimir, faz uma última varredura na tabela de processos
  // para salvar as métricas de quem ainda não foi liberado (ex: o próprio init
  // ou outros processos que sobraram)
  int tempo_final = so_tempo_total(self);
  for (int i = 0; i < MAX_PROCESSES; i++)
  {
    pcb *p = self->tabela_de_processos[i];
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
  for (int i = 0; i < self->num_proc_criados; i++){
    //pega a métrica salva do histórico (índice i == pid i+1)
    metricas_processo_final_t *p = &self->historico_metricas[i];

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
// ---------------------------------------------------------------------
// TRATAMENTO DE INTERRUPÇÃO {{{1
// ---------------------------------------------------------------------

// funções auxiliares para o tratamento de interrupção
static void so_salva_estado_da_cpu(so_t *self);
static void so_trata_irq(so_t *self, int irq);
static void so_trata_pendencias(so_t *self);
static void so_escalona(so_t *self);
static int so_despacha(so_t *self);
static void libera_terminal(so_t *self, int pid);
// função a ser chamada pela CPU quando executa a instrução CHAMAC, no tratador de
//   interrupção em assembly
// essa é a única forma de entrada no SO depois da inicialização
// na inicialização do SO, a CPU foi programada para chamar esta função para executar
//   a instrução CHAMAC
// a instrução CHAMAC só deve ser executada pelo tratador de interrupção
//
// o primeiro argumento é um ponteiro para o SO, o segundo é a identificação
//   da interrupção
// o valor retornado por esta função é colocado no registrador A, e pode ser
//   testado pelo código que está após o CHAMAC. No tratador de interrupção em
//   assembly esse valor é usado para decidir se a CPU deve retornar da interrupção
//   (e executar o código de usuário) ou executar PARA e ficar suspensa até receber
//   outra interrupção
static int so_trata_interrupcao(void *argC, int reg_A)
{
  so_t *self = argC;
  irq_t irq = reg_A;
  // ATUALIZAÇÃO DE TEMPOS (Métricas 3 e 9)
  so_atualiza_tempos(self); 
  
  // esse print polui bastante, recomendo tirar quando estiver com mais confiança
  console_printf("SO: recebi IRQ %d (%s)", irq, irq_nome(irq));
  // métrica 4: Contagem de Interrupções (feita em so_trata_irq)
  // salva o estado da cpu no descritor do processo que foi interrompido
  so_salva_estado_da_cpu(self);
  // faz o atendimento da interrupção
  so_trata_irq(self, irq);
  // faz o processamento independente da interrupção
  so_trata_pendencias(self);
  // escolhe o próximo processo a executar
  so_escalona(self);
  // recupera o estado do processo escolhido
  return so_despacha(self);
}

// salva o valor antigo da cpu? antes do processo? nao entendi
static void so_salva_estado_da_cpu(so_t *self)
{
  // t2: salva os registradores que compõem o estado da cpu no descritor do
  //   processo corrente. os valores dos registradores foram colocados pela
  //   CPU na memória, nos endereços CPU_END_PC etc. O registrador X foi salvo
  //   pelo tratador de interrupção (ver trata_irq.asm) no endereço 59
  // se não houver processo corrente, não faz nada
  if (mem_le(self->mem, CPU_END_A, &self->regA) != ERR_OK || mem_le(self->mem, CPU_END_PC, &self->regPC) != ERR_OK || mem_le(self->mem, CPU_END_erro, &self->regERRO) != ERR_OK || mem_le(self->mem, 59, &self->regX))
  {
    console_printf("SO: erro na leitura dos registradores");
    self->erro_interno = true;
  }
  // se tiver algum processo corrente, salva o estado dele
  if (self->processo_corrente != NO_PROCESS)
  {
    cpu_ctx contexto;
    contexto.pc = self->regPC;
    contexto.regA = self->regA;
    contexto.regX = self->regX;
    contexto.erro = self->regERRO;
    self->tabela_de_processos[self->processo_corrente]->ctx_cpu = contexto;
  }
}

static void so_trata_pendencias(so_t *self)
{ // t2: realiza ações que não são diretamente ligadas com a interrupção que
  //   está sendo atendida:
  // - E/S pendente
  // - desbloqueio de processos
  // - contabilidades
  // - etc
  // na função que trata de pendências, o SO deve verificar o estado dos dispositivos
  // que causaram bloqueio e realizar operações pendentes e desbloquear processos se for o caso
  // ver os dispsitivos que podem estar bloqueados, itera por todos os processos
  for (int i = 0; i < MAX_PROCESSES; i++)
  {
    pcb *proc = self->tabela_de_processos[i];
    // só importa os processos bloqueados por E/S
    if (proc == NULL || proc->estado != P_BLOQUEADO || proc->dispositivo_bloqueado == -1)
    {
      continue;
    }
    // processo [i] está bloqueado em um dispositivo, checar
    dispositivo_id_t disp = proc->dispositivo_bloqueado;
    dispositivo_id_t disp_ok = disp + 1;
    int estado;
    if (es_le(self->es, disp_ok, &estado) != ERR_OK)
    {
      console_printf("SO: erro ao checar E/S pendente para pid %d", proc->pid);
      self->erro_interno = true;
      continue;
    }

    if (estado != 0)
    {
      // dispositivo está PRONTO
      console_printf("SO: E/S pronta para pid %d (disp %d), desbloqueando.", proc->pid, disp);

      // realiza a operação pendente
      if (disp % 2 == 0)
      { // dispositivo de LEITURA (teclado)
        int dado;
        if (es_le(self->es, disp, &dado) != ERR_OK)
        {
          console_printf("SO: erro ao completar leitura pendente para pid %d", proc->pid);
          proc->ctx_cpu.regA = -1; // Sinaliza erro no processo
        }
        else
        {
          proc->ctx_cpu.regA = dado; // Coloca o dado no regA
        }
      }
      else
      { // Dispositivo de ESCRITA (tela)
        int dado = proc->ctx_cpu.regX;
        if (es_escreve(self->es, disp, dado) != ERR_OK)
        {
          console_printf("SO: erro ao completar escrita pendente for pid %d", proc->pid);
          proc->ctx_cpu.regA = -1;
        }
        else
        {
          proc->ctx_cpu.regA = 0; // Sucesso
        }
      }

      // Desbloqueia o processo
      //proc->estado = P_PRONTO;
      so_muda_estado(self, proc, P_PRONTO); // usa a função que contabiliza métricas
      proc->dispositivo_bloqueado = -1; // marca que não está mais esperando E/S
      enfileira(self->fila_prontos, proc->pid); // coloca na fila de prontos
    }
  }
}

static void so_escalona(so_t *self)
{
  //limpa processos terminados
  for (int i = 0; i < MAX_PROCESSES; i++)
  {
    pcb *proc = self->tabela_de_processos[i];
    if (proc != NULL && proc->estado == P_TERMINOU)
    {
      libera_terminal(self, proc->pid);
      
      //salva métricas 
      so_salva_metricas_finais(self, proc); 

      free(proc);
      self->tabela_de_processos[i] = NULL;
    }
  }

  // verifica se o processo corrente pode continuar
  pcb *proc_atual = (self->processo_corrente == NO_PROCESS) ? 
    NULL : self->tabela_de_processos[self->processo_corrente];

  if (proc_atual != NULL && proc_atual->estado == P_EXECUTANDO)
  {
    return; //deixa ele continuar
  }

  //procura por um processo pronto na fila, com maior prioridade
  int idx_escolhido = -1; //INDICE na tabela de processos do processo com maior prioridade
  float  maior_prioridade = QUANTUM; 
  pcb* proc_candidato = NULL;
  while(!fila_vazia(self->fila_prontos)){
    //acha o índice do processo candidato
    for (int i = 0; i < MAX_PROCESSES; i++){
      if (self->tabela_de_processos[i] != NULL && self->tabela_de_processos[i]->prioridade < maior_prioridade){
        idx_escolhido = i;
        maior_prioridade = self->tabela_de_processos[i]->prioridade;
        proc_candidato = self->tabela_de_processos[i];
        break;
      }
    }
    //escolher o processo de maior prioridade
    if (idx_escolhido != -1 && proc_candidato->estado == P_PRONTO){
      self->processo_corrente =  idx_escolhido;
      so_muda_estado(self, proc_candidato, P_EXECUTANDO);
      
    }else{
      //só tem 1 procesos na fila
      self->processo_corrente =  self->fila_prontos->inicio->pid;
      //desenfileira(self->fila_prontos, self->processo_corrente);
    }
  }
 
  

}

// coloca o estado do processo corrente na CPU, para que ela execute
// O escalonador define quem vai rodar.
// O despachante (dispatcher) coloca ele para rodar.
static int so_despacha(so_t *self)
{
  // t2: se houver processo corrente, coloca o estado desse processo onde ele
  //   será recuperado pela CPU (em CPU_END_PC etc e 59) e retorna 0,
  //   senão retorna 1
  // o valor retornado será o valor de retorno de CHAMAC, e será colocado no
  //   registrador A para o tratador de interrupção (ver trata_irq.asm).
  if (self->processo_corrente == NO_PROCESS)
  {
    // nenhum processo para rodar
    return 1;
  }
  cpu_ctx contexto = self->tabela_de_processos[self->processo_corrente]->ctx_cpu;
  mem_escreve(self->mem, CPU_END_PC, contexto.pc);
  mem_escreve(self->mem, CPU_END_A, contexto.regA);
  mem_escreve(self->mem, 59, contexto.regX);
  mem_escreve(self->mem, CPU_END_erro, contexto.erro);

  if (self->erro_interno)
    return 1;
  else
    return 0;
}

// ---------------------------------------------------------------------
// TRATAMENTO DE UMA IRQ {{{1
// ---------------------------------------------------------------------

// funções auxiliares para tratar cada tipo de interrupção
static void so_trata_reset(so_t *self);
static void so_trata_irq_chamada_sistema(so_t *self);
static void so_trata_irq_err_cpu(so_t *self);
static void so_trata_irq_relogio(so_t *self);
static void so_trata_irq_desconhecida(so_t *self, int irq);

static void so_trata_irq(so_t *self, int irq)
{
  //metrica 4: Contagem de Interrupções
  if (irq >= 0 && irq < N_IRQ){
    self->contagem_irq[irq]++;
  }
  // verifica o tipo de interrupção que está acontecendo, e atende de acordo
  switch (irq)
  {
  case IRQ_RESET:
    so_trata_reset(self);
    break;
  case IRQ_SISTEMA:
    so_trata_irq_chamada_sistema(self);
    break;
  case IRQ_ERR_CPU:
    so_trata_irq_err_cpu(self);
    break;
  case IRQ_RELOGIO:
    so_trata_irq_relogio(self);
    break;
  default:
    so_trata_irq_desconhecida(self, irq);
  }
}

// chamada uma única vez, quando a CPU inicializa
static void so_trata_reset(so_t *self)
{
  // coloca o tratador de interrupção na memória
  // quando a CPU aceita uma interrupção, passa para modo supervisor,
  //   salva seu estado à partir do endereço CPU_END_PC, e desvia para o
  //   endereço CPU_END_TRATADOR
  // colocamos no endereço CPU_END_TRATADOR o programa de tratamento
  //   de interrupção (escrito em asm). esse programa deve conter a
  //   instrução CHAMAC, que vai chamar so_trata_interrupcao (como
  //   foi definido na inicialização do SO)
  int ender = so_carrega_programa(self, "trata_int.maq");
  if (ender != CPU_END_TRATADOR)
  {
    console_printf("SO: problema na carga do programa de tratamento de interrupção");
    self->erro_interno = true;
  }

  // programa o relógio para gerar uma interrupção após INTERVALO_INTERRUPCAO
  if (es_escreve(self->es, D_RELOGIO_TIMER, INTERVALO_INTERRUPCAO) != ERR_OK)
  {
    console_printf("SO: problema na programação do timer");
    self->erro_interno = true;
  }

  // t2: deveria criar um processo para o init, e inicializar o estado do
  //   processador para esse processo com os registradores zerados, exceto
  //   o PC e o modo.
  // como não tem suporte a processos, está carregando os valores dos
  //   registradores diretamente no estado da CPU mantido pelo SO; daí vai
  //   copiar para o início da memória pelo despachante, de onde a CPU vai
  //   carregar para os seus registradores quando executar a instrução RETI
  //   em bios.asm (que é onde está a instrução CHAMAC que causou a execução
  //   deste código

  /// processos
  // inicializa a tabela de processos
  // tabela_de_processos já é um array fixo, não precisa de malloc
  for (int i = 0; i < MAX_PROCESSES; i++)
  {
    self->tabela_de_processos[i] = NULL;
  }

  // coloca o programa init na memória
  ender = so_carrega_programa(self, "init.maq");
  /* if (ender != 100) {
    console_printf("SO: problema na carga do programa inicial");
    self->erro_interno = true;
    return;
  }
 */
  if (ender < 0)
  { // Verificação de erro melhorada
    console_printf("SO: problema na carga do programa inicial");
    self->erro_interno = true;
    return;
  }
  // coloca o endereço do programa init np primeiro processo
  pcb *processo_inicial = criar_processo(ender, D_TERM_A_TECLADO, D_TERM_A_TELA);
  // marcar o terminal usado
  self->terminais_usados[0] = processo_inicial->pid;
  self->tabela_de_processos[0] = processo_inicial;
  self->processo_corrente = NO_PROCESS; // índice do processo inicial na tabela

  // altera o PC para o endereço de carga
  // self->regPC = ender; // deveria ser no processo
  processo_inicial->ctx_cpu.pc = ender;
  processo_inicial->ctx_cpu.regA = 0; // <-- ESSENCIAL
  processo_inicial->ctx_cpu.regX = 0; // <-- ESSENCIAL
  processo_inicial->ctx_cpu.erro = 0; // <-- ESSENCIAL

  //self->regPC = processo_inicial->ctx_cpu.pc;
  // Inicializa campos de bloqueio
  processo_inicial->dispositivo_bloqueado = -1;
  processo_inicial->pid_esperando = -1;
  // processo_inicial->estado = P_PRONTO;
  //inicializa métricas 
  int tempo_atual = so_tempo_total(self); // Tempo é 0
  inicializa_metricas_pcb(processo_inicial, tempo_atual);
  so_muda_estado(self, processo_inicial, P_PRONTO); //substitui processo_inicial->estado = P_PRONTO
  // coloca init na fila de prontos
  enfileira(self->fila_prontos, processo_inicial->pid);
  self->num_proc_criados++;
}

//acorda qualquer processo que estava bloqueado esperando 'pid_que_morreu'
static void so_acorda_processos_esperando(so_t *self, int pid_que_morreu)
{
  if (pid_que_morreu <= 0)
    return; // PID inválido

  for (int i = 0; i < MAX_PROCESSES; i++)
  {
    pcb *proc = self->tabela_de_processos[i];

    //se o processo [i] estava bloqueado esperando o PID que acabou de morrer
    if (proc != NULL && proc->estado == P_BLOQUEADO && proc->pid_esperando == pid_que_morreu)
    {
      console_printf("SO: processo %d (que morreu) estava sendo esperado por %d. Acordando.",
      pid_que_morreu, proc->pid);
      //proc->estado = P_PRONTO;
      so_muda_estado(self, proc, P_PRONTO); // usa a função que contabiliza métricas
      proc->pid_esperando = -1; //nao está mais esperando
      proc->ctx_cpu.regA = 0;   //retorna sucesso para a chamada SO_ESPERA_PROC
      // colocar na fila de prontos
      enfileira(self->fila_prontos, proc->pid);
    }
  }
}
// interrupção gerada quando a CPU identifica um erro
static void so_trata_irq_err_cpu(so_t *self)
{
  // Ocorreu um erro interno na CPU
  // O erro está codificado em CPU_END_erro
  // Em geral, causa a morte do processo que causou o erro
  // Ainda não temos processos, causa a parada da CPU
  // t2: com suporte a processos, deveria pegar o valor do registrador erro
  //   no descritor do processo corrente, e reagir de acordo com esse erro
  //   (em geral, matando o processo)
  //  Obtém código do erro do descritor e mata o processo corrente.
  if (self->processo_corrente != NO_PROCESS)
  {
    pcb *proc = self->tabela_de_processos[self->processo_corrente];
    err_t erro = proc->ctx_cpu.erro;
    console_printf("SO: erro na CPU do processo %d: %s", proc->pid, err_nome(erro));
    so_acorda_processos_esperando(self, proc->pid); // acorda processos esperando esse processo
    proc->usando = 0;
    //proc->estado = P_TERMINOU;
    so_muda_estado(self, proc, P_TERMINOU); // usa a função que contabiliza métricas
    self->processo_corrente = NO_PROCESS; // nenhum processo está executando
    libera_terminal(self, proc->pid);     // libera o terminal usado pelo processo
    console_printf("SO: IRQ TRATADA -- erro na CPU: %s", err_nome(erro));
    self->erro_interno = true;
  }
  return;
}
// err_t err = self->regERRO;

// interrupção gerada quando o timer expira
static void so_trata_irq_relogio(so_t *self)
{
  // rearma o interruptor do relógio e reinicializa o timer para a próxima interrupção
  err_t e1, e2;
  e1 = es_escreve(self->es, D_RELOGIO_INTERRUPCAO, 0); // desliga o sinalizador de interrupção
  e2 = es_escreve(self->es, D_RELOGIO_TIMER, INTERVALO_INTERRUPCAO);
  if (e1 != ERR_OK || e2 != ERR_OK)
  {
    console_printf("SO: problema da reinicialização do timer");
    self->erro_interno = true;
  }
  // t2: deveria tratar a interrupção
  //   por exemplo, decrementa o quantum do processo corrente, quando se tem
  //   um escalonador com quantum
  pcb *proc_corrente = self->tabela_de_processos[self->processo_corrente];
  if (proc_corrente == NULL || self->processo_corrente == NO_PROCESS) {
    return;
  }
  proc_corrente->quantum--;
  if (proc_corrente->quantum <= 0 && proc_corrente->estado!= P_BLOQUEADO){
    console_printf("SO: quantum do processo %d expirou, forçando troca de contexto.", proc_corrente->pid);
    // --- Métricas 5 e 7 ---
    proc_corrente->num_preempcoes_proc++;
    self->num_preemcoes_total++;
    // --- Fim Métricas ---
    //proc_corrente->estado = P_PRONTO;
    so_muda_estado(self, proc_corrente, P_PRONTO); // usa a função que contabiliza métricas
    proc_corrente->quantum = QUANTUM; // reseta o quantum
    self->processo_corrente = NO_PROCESS; // força o escalonador a escolher outro processo
    enfileira(self->fila_prontos, proc_corrente->pid);
  }
}

// foi gerada uma interrupção para a qual o SO não está preparado
static void so_trata_irq_desconhecida(so_t *self, int irq)
{
  console_printf("SO: não sei tratar IRQ %d (%s)", irq, irq_nome(irq));
  self->erro_interno = true;
}

// ---------------------------------------------------------------------
// CHAMADAS DE SISTEMA {{{1
// ---------------------------------------------------------------------

// funções auxiliares para cada chamada de sistema
static void so_chamada_le(so_t *self);
static void so_chamada_escr(so_t *self);
static void so_chamada_cria_proc(so_t *self);
static void so_chamada_mata_proc(so_t *self);
static void so_chamada_espera_proc(so_t *self);
static pcb *achar_processo(so_t *self, int pid);

static void so_trata_irq_chamada_sistema(so_t *self)
{
  // a identificação da chamada está no registrador A
  // t2: com processos, o reg A deve estar no descritor do processo corrente
  int id_chamada = self->regA;
  console_printf("SO: chamada de sistema %d", id_chamada);
  switch (id_chamada)
  {
  case SO_LE:
    so_chamada_le(self);
    break;
  case SO_ESCR:
    so_chamada_escr(self);
    break;
  case SO_CRIA_PROC:
    so_chamada_cria_proc(self);
    break;
  case SO_MATA_PROC:
    so_chamada_mata_proc(self);
    break;
  case SO_ESPERA_PROC:
    so_chamada_espera_proc(self);
    break;
  default:
    console_printf("SO: chamada de sistema desconhecida (%d)", id_chamada);
    // t2: deveria matar o processo
    so_chamada_mata_proc(self);
    self->erro_interno = true;
  }
}

// implementação da chamada se sistema SO_LE
// faz a leitura de um dado da entrada corrente do processo, coloca o dado no reg A
static void so_chamada_le(so_t *self)
{
  // implementação com espera ocupada (retirada no t2)
  //   t2: deveria realizar a leitura somente se a entrada estiver disponível,
  //     senão, deveria bloquear o processo.
  //   no caso de bloqueio do processo, a leitura (e desbloqueio) deverá
  //     ser feita mais tarde, em tratamentos pendentes em outra interrupção,
  //     ou diretamente em uma interrupção específica do dispositivo, se for
  //     o caso
  // implementação lendo direto do terminal A
  //   t2: deveria usar dispositivo de entrada corrente do processo
  pcb *proc = self->tabela_de_processos[self->processo_corrente];
  dispositivo_id_t entrada = proc->entrada;  // Ex: D_TERM_B_TECLADO
  dispositivo_id_t entrada_ok = entrada + 1; // Ex: D_TERM_B_TECLADO_OK

  int estado;
  if (es_le(self->es, entrada_ok, &estado) != ERR_OK)
  {
    console_printf("SO: problema no acesso ao estado do teclado (pid %d)", proc->pid);
    proc->ctx_cpu.regA = -1; // retorna erro
    self->erro_interno = true;
    return;
  }

  /*  if (estado != 0) break;
   // como não está saindo do SO, a unidade de controle não está executando seu laço.
   // esta gambiarra faz pelo menos a console ser atualizada
   // t2: com a implementação de bloqueio de processo, esta gambiarra não
   //   deve mais existir.
   console_tictac(self->console); */
  if (estado != 0)
  {
    // Dispositivo PRONTO: lê imediatamente
    int dado;
    if (es_le(self->es, entrada, &dado) != ERR_OK)
    {
      console_printf("SO: problema no acesso ao teclado (pid %d)", proc->pid);
      proc->ctx_cpu.regA = -1; // Retorna erro
      self->erro_interno = true;
      return;
    }
    proc->ctx_cpu.regA = dado; // Coloca o dado no registrador A do PROCESSO
  }
  else
  {
    // dspositivo NÃO PRONTO: bloqueia o processo
    console_printf("SO: processo %d bloqueado esperando E/S (leitura)", proc->pid);
    //proc->estado = P_BLOQUEADO;
    so_muda_estado(self, proc, P_BLOQUEADO); // usa a função que contabiliza métricas
    proc->dispositivo_bloqueado = entrada; // salva qual dispositivo está esperando
    self->processo_corrente = NO_PROCESS;  // força o escalonador a rodar
    //desenfileira(self->fila_prontos, proc->pid);    // retira o processo corrente da fila de prontos
  }
  // escreve no reg A do processador
  // (na verdade, na posição onde o processador vai pegar o A quando retornar da int)
  // t2: se houvesse processo, deveria escrever no reg A do processo
  // t2: o acesso só deve ser feito nesse momento se for possível; se não, o processo
  //   é bloqueado, e o acesso só deve ser feito mais tarde (e o processo desbloqueado)
  /* self->regA = dado;
  self->tabela_de_processos[self->processo_corrente]->ctx_cpu.regA = dado; */
}

// implementação da chamada se sistema SO_ESCR
// escreve o valor do reg X na saída corrente do processo
static void so_chamada_escr(so_t *self)
{
  // implementação com espera ocupada
  //   t2: deveria bloquear o processo se dispositivo ocupado
  // implementação escrevendo direto do terminal A
  //   t2: deveria usar o dispositivo de saída corrente do processo
  pcb *proc = self->tabela_de_processos[self->processo_corrente];
  dispositivo_id_t saida = proc->saida;  // Ex: D_TERM_B_TELA
  dispositivo_id_t saida_ok = saida + 1; // Ex: D_TERM_B_TELA_OK

  int estado;
  if (es_le(self->es, saida_ok, &estado) != ERR_OK)
  {
    console_printf("SO: problema no acesso ao estado da tela (pid %d)", proc->pid);
    proc->ctx_cpu.regA = -1; // Retorna erro
    self->erro_interno = true;
    return;
  }

  /* if (estado != 0) break;
  // como não está saindo do SO, a unidade de controle não está executando seu laço.
  // esta gambiarra faz pelo menos a console ser atualizada
  // t2: não deve mais existir quando houver suporte a processos, porque o SO não poderá
  //   executar por muito tempo, permitindo a execução do laço da unidade de controle
  console_tictac(self->console); */

  // está lendo o valor de X e escrevendo o de A direto onde o processador colocou/vai pegar
  // t2: deveria usar os registradores do processo que está realizando a E/S
  // t2: caso o processo tenha sido bloqueado, esse acesso deve ser realizado em outra execução
  //   do SO, quando ele verificar que esse acesso já pode ser feito.
  if (estado != 0)
  {                                // dispositivo pronto, escreve o dado na tela
    int dado = proc->ctx_cpu.regX; // pega o dado do reg X do processo
    if (es_escreve(self->es, saida, dado) != ERR_OK)
    {
      console_printf("SO: problema no acesso à tela (pid %d)", proc->pid);
      proc->ctx_cpu.regA = -1; // Retorna erro
      self->erro_interno = true;
      return;
    }
    proc->ctx_cpu.regA = 0; // Sucesso
  }
  else
  {
    // Dispositivo NÃO PRONTO: bloqueia o processo
    console_printf("SO: processo %d bloqueado esperando E/S (escrita)", proc->pid);
    //proc->estado = P_BLOQUEADO;
    so_muda_estado(self, proc, P_BLOQUEADO); // usa a função que contabiliza métricas
    proc->dispositivo_bloqueado = saida;  // Salva qual dispositivo está esperando
    self->processo_corrente = NO_PROCESS; // Força o escalonador a rodar
    //desenfileira(self->fila_prontos, proc->pid);    // retira o processo corrente da fila de prontos
  }
}
// retorna o índice do primeiro terminal livre ou -1 se todos estiverem ocupados
static int so_aloca_terminal(so_t *self)
{
  for (int i = 0; i < TERMINAIS; i++)
  {
    if (self->terminais_usados[i] == 0)
    { // terminal livre
      // self->terminais_usados[i] = 0;
      return i; // retorna o índice do terminal alocado
    }
  }
  return -1; // nenhum terminal disponível
}

// retorna o id do terminal correspondente ao índice
static dispositivo_id_t id_terminal_livre(int idx)
{

  switch (idx)
  {
  case 0:
    return D_TERM_A;
  case 1:
    return D_TERM_B;
  case 2:
    return D_TERM_C;
  case 3:
    return D_TERM_D;
  default:
    return -1; // índice inválido
  }
}

static void libera_terminal(so_t *self, int pid)
{
  for (int i = 0; i < TERMINAIS; i++)
  {
    if (self->terminais_usados[i] == pid)
    {
      self->terminais_usados[i] = 0;
      return;
    }
  }
}

// implementação da chamada se sistema SO_CRIA_PROC
// cria um processo
static void so_chamada_cria_proc(so_t *self)
{
  // ainda sem suporte a processos, carrega programa e passa a executar ele
  // quem chamou o sistema não vai mais ser executado, coitado!
  // t2: deveria criar um novo processo

  // aponta para o processo corrente na tabela de processos
  // pega o processo corrente para ler o X e pegar o nome do arquivo
  pcb *processo_corrente = self->tabela_de_processos[self->processo_corrente];
  int nome_arquivo = processo_corrente->ctx_cpu.regX;

  // em X está o endereço onde está o nome do arquivo
  // int ender_proc;

  // achar uma posição vazia na tabela de processos, para colocar o processo novo
  int possivel_indice = -1;
  for (int i = 0; i < MAX_PROCESSES; i++)
  {
    if (self->tabela_de_processos[i] == NULL)
    {
      possivel_indice = i;
      break;
    }
  }
  if (possivel_indice == -1)
  {
    // não tem mais espaço na tabela de processos
    console_printf("sem espaço na tabela de processos");
    self->regA = -1; // erro
    return;
  }
  // t2: deveria ler o X do descritor do processo criador
  // ender_proc = self->regX;
  char nome[100];
  if (copia_str_da_mem(100, nome, self->mem, nome_arquivo))
  {
    int ender_carga = so_carrega_programa(self, nome);
    // logica contraria
    if (ender_carga < 0)
    { // erro na carga
      console_printf("SO: problema na carga do programa '%s'", nome);
      self->regA = -1; // erro
      return;
      // t2: deveria escrever no PC do descritor do processo criado
      // self->regPC = ender_carga;
    }
    // cria o processo , endereço de carga, entrada e saída
    //  Selecionar um terminal disponível, no vetor de terminais
    int terminal_id = so_aloca_terminal(self);
    if (terminal_id == -1)
    {
      console_printf("SO: nenhum terminal disponível para o novo processo\n");
      processo_corrente->ctx_cpu.regA = -1; // erro
      return;
    }
    dispositivo_id_t terminal_livre = id_terminal_livre(terminal_id); // achar o terminal correspondente
    pcb *novo_processo = criar_processo(ender_carga, terminal_livre + TERM_TECLADO, terminal_livre + TERM_TELA);
    // marca o terminal como usado com o pid do processo que está usando
    self->terminais_usados[terminal_id] = novo_processo->pid;
    self->tabela_de_processos[possivel_indice] = novo_processo; // colocar o processo na tabela
    //novo_processo->estado = P_PRONTO;
    //inicializa Métricas (Novo Processo)
    int tempo_atual = so_tempo_total(self);
    inicializa_metricas_pcb(novo_processo, tempo_atual);
    so_muda_estado(self, novo_processo, P_PRONTO); // substitui novo_processo->estado = P_PRONTO
    // t2: deveria escrever no PC do descritor do processo criado (antes: //self->regPC = ender_carga;)
    novo_processo->ctx_cpu.pc = ender_carga;
    
    // escrever o PID do processo criado no reg A do processo que pediu a criação
    processo_corrente->ctx_cpu.regA = novo_processo->pid;
    // inserir na fila de processos prontos
    enfileira(self->fila_prontos, novo_processo->pid);
    self->num_proc_criados++;

    return;
  }
  // deveria escrever -1 (se erro) ou o PID do processo criado (se OK) no reg A
  //   do processo que pediu a criação
  // self->regA = -1;A
}

// implementação da chamada se sistema SO_MATA_PROC
// mata o processo com pid X (ou o processo corrente se X é 0)
static void so_chamada_mata_proc(so_t *self)
{
  pcb *proc_corrente = self->tabela_de_processos[self->processo_corrente];
  int pid_a_matar = proc_corrente->ctx_cpu.regX;
  pcb *proc_alvo;

  // Variável para rastrear se o processo está se matando
  bool matando_a_si_mesmo = false; 

  if (pid_a_matar == 0 || pid_a_matar == proc_corrente->pid)
  {
    proc_alvo = proc_corrente;
    matando_a_si_mesmo = true; //marcar que está se matando
  }
  else
  {
    proc_alvo = achar_processo(self, pid_a_matar);
  }

  if (proc_alvo == NULL)
  {
    proc_corrente->ctx_cpu.regA = -1; // erro: pid não encontrado
    return;
  }

  so_acorda_processos_esperando(self, proc_alvo->pid);

  //proc_alvo->estado = P_TERMINOU;
  so_muda_estado(self, proc_alvo, P_TERMINOU); // usa a função que contabiliza métricas
  proc_alvo->usando = 0;
  libera_terminal(self, proc_alvo->pid);
  //desenfileira(self->fila_prontos, proc_alvo->pid);
  if (proc_alvo->estado == P_PRONTO) {
    desenfileira(self->fila_prontos, proc_alvo->pid);
  }

  /* // remove da tabela e libera a memória
  for (int i = 0; i < MAX_PROCESSES; i++){
    if (self->tabela_de_processos[i] == proc_alvo){
      //SALVA MÉTRICAS ANTES DO FREE 
      so_salva_metricas_finais(self, proc_alvo);
      free(proc_alvo);
      self->tabela_de_processos[i] = NULL;
      break;
    }
  } */

  // se matou a si mesmo, não há processo corrente
  if (matando_a_si_mesmo){
    self->processo_corrente = NO_PROCESS;
  } else {
    // SÓ definir o valor de retorno se o processo chamador NÃO morreu
    proc_corrente->ctx_cpu.regA = 0; // sucesso
  }
  //se ele se matou NADA é escrito no regA.
}

// implementação da chamada se sistema SO_ESPERA_PROC
// espera o fim do processo com pid X
// bloqueia o processo corrente(init) se o processo com pid X não tiver terminado
static void so_chamada_espera_proc(so_t *self)
{
  pcb *proc_corrente = self->tabela_de_processos[self->processo_corrente];
  int pid_esperado = proc_corrente->ctx_cpu.regX;

  // checar se o pid é inválido (0 ou ele mesmo)
  if (pid_esperado <= 0 || pid_esperado == proc_corrente->pid)
  {
    proc_corrente->ctx_cpu.regA = -1; // Retorna erro no RegA
    return;
  }

  pcb *proc_esperado = achar_processo(self, pid_esperado);

  //checar se o processo existe na tabela
  if (proc_esperado == NULL)
  {
    //o processo não existe (provavelmente JÁ TERMINOU)
    //não há porque esperar
    proc_corrente->ctx_cpu.regA = 0; // retorna SUCESSO imediatamente
    return;                        
  }

  // O processo existe. Se ele NÃO terminou, bloqueia.
  if (proc_esperado->estado != P_TERMINOU)
  {
    console_printf("SO: processo %d esperando o processo %d", proc_corrente->pid, pid_esperado);
    //proc_corrente->estado = P_BLOQUEADO;
    so_muda_estado(self, proc_corrente, P_BLOQUEADO); // usa a função que contabiliza métricas
    proc_corrente->pid_esperando = pid_esperado;
    self->processo_corrente = NO_PROCESS;
    //desenfileira(self->fila_prontos, proc_corrente->pid);
  }
  else
  {
    // O processo existe mas seu estado é P_TERMINOU (ainda não foi limpo)
    proc_corrente->ctx_cpu.regA = 0; // Retorna SUCESSO.
  }
}

// ---------------------------------------------------------------------
// CARGA DE PROGRAMA {{{1
// ---------------------------------------------------------------------

// carrega o programa na memória
// retorna o endereço de carga ou -1
static int so_carrega_programa(so_t *self, char *nome_do_executavel)
{
  // programa para executar na nossa CPU
  programa_t *prog = prog_cria(nome_do_executavel);
  if (prog == NULL)
  {
    console_printf("Erro na leitura do programa '%s'\n", nome_do_executavel);
    return -1;
  }

  int end_ini = prog_end_carga(prog);
  int end_fim = end_ini + prog_tamanho(prog);

  for (int end = end_ini; end < end_fim; end++)
  {
    if (mem_escreve(self->mem, end, prog_dado(prog, end)) != ERR_OK)
    {
      console_printf("Erro na carga da memória, endereco %d\n", end);
      return -1;
    }
  }

  prog_destroi(prog);
  console_printf("SO: carga de '%s' em %d-%d", nome_do_executavel, end_ini, end_fim);
  return end_ini;
}

// ---------------------------------------------------------------------
// ACESSO À MEMÓRIA DOS PROCESSOS {{{1
// ---------------------------------------------------------------------

// copia uma string da memória do simulador para o vetor str.
// retorna false se erro (string maior que vetor, valor não char na memória,
//   erro de acesso à memória)
// t2: deveria verificar se a memória pertence ao processo
static bool copia_str_da_mem(int tam, char str[tam], mem_t *mem, int ender)
{
  for (int indice_str = 0; indice_str < tam; indice_str++)
  {
    int caractere;
    if (mem_le(mem, ender + indice_str, &caractere) != ERR_OK)
    {
      return false;
    }
    if (caractere < 0 || caractere > 255)
    {
      return false;
    }
    str[indice_str] = caractere;
    if (caractere == 0)
    {
      return true;
    }
  }
  // estourou o tamanho de str
  return false;
}

pcb *achar_processo(so_t *self, int pid)
{
  for (int i = 0; i < MAX_PROCESSES; i++)
  {
    if (self->tabela_de_processos[i] != NULL && self->tabela_de_processos[i]->pid == pid)
    {
      return self->tabela_de_processos[i];
    }
  }
  return NULL;
}

// vim: foldmethod=marker