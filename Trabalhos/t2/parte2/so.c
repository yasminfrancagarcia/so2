
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

#include <stdlib.h>
#include <stdbool.h>


// ---------------------------------------------------------------------
// CONSTANTES E TIPOS {{{1
// ---------------------------------------------------------------------

// intervalo entre interrupções do relógio
#define INTERVALO_INTERRUPCAO 50   // em instruções executadas

#define TERMINAIS 4

//constantes de processos
#define MAX_PROCESSES 4
#define NO_PROCESS -1
struct so_t {
  cpu_t *cpu;
  mem_t *mem;
  es_t *es;
  console_t *console;
  bool erro_interno;

  int regA, regX, regPC, regERRO; // cópia do estado da CPU
  // t2: tabela de processos, processo corrente, pendências, etc
  pcb* tabela_de_processos[MAX_PROCESSES];
  int processo_corrente; // índice na tabela de processos
  //vetor para guardar os pids dos processos que estão usando os terminais
  //idx = 0 -> terminal A
  //idx = 1 -> terminal B...
  int terminais_usados[4];
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
  if (self == NULL) return NULL;

  self->cpu = cpu;
  self->mem = mem;
  self->es = es;
  self->console = console;
  self->erro_interno = false;
  //processos
  self->processo_corrente = NO_PROCESS;

  // quando a CPU executar uma instrução CHAMAC, deve chamar a função
  //   so_trata_interrupcao, com primeiro argumento um ptr para o SO
  cpu_define_chamaC(self->cpu, so_trata_interrupcao, self);
  //inicializar terminais
  for(int i = 0; i < TERMINAIS; i++) {
      self->terminais_usados[i] = 0; // nenhum terminal está sendo usado
  }
  return self;
}

void so_destroi(so_t *self)
{
  cpu_define_chamaC(self->cpu, NULL, NULL);
  free(self);
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
  // esse print polui bastante, recomendo tirar quando estiver com mais confiança
  console_printf("SO: recebi IRQ %d (%s)", irq, irq_nome(irq));
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

//salva o valor antigo da cpu? antes do processo? nao entendi
static void so_salva_estado_da_cpu(so_t *self)
{
  // t2: salva os registradores que compõem o estado da cpu no descritor do
  //   processo corrente. os valores dos registradores foram colocados pela
  //   CPU na memória, nos endereços CPU_END_PC etc. O registrador X foi salvo
  //   pelo tratador de interrupção (ver trata_irq.asm) no endereço 59
  // se não houver processo corrente, não faz nada
  if (mem_le(self->mem, CPU_END_A, &self->regA) != ERR_OK
      || mem_le(self->mem, CPU_END_PC, &self->regPC) != ERR_OK
      || mem_le(self->mem, CPU_END_erro, &self->regERRO) != ERR_OK
      || mem_le(self->mem, 59, &self->regX)) {
    console_printf("SO: erro na leitura dos registradores");
    self->erro_interno = true;
  }
  //se tiver algum processo corrente, salva o estado dele
  if(self->processo_corrente != NO_PROCESS){
    cpu_ctx contexto;
    contexto.pc = self->regPC;
    contexto.regA = self->regA;
    contexto.regX = self->regX;
    contexto.erro = self->regERRO;
    self->tabela_de_processos[self->processo_corrente]->ctx_cpu = contexto;
  }
}

// so.c (so_trata_pendencias - A PARTIR DA LINHA ~140)

static void so_trata_pendencias(so_t *self)
{
  for (int i = 0; i < MAX_PROCESSES; i++) {
    pcb* proc = self->tabela_de_processos[i];
    
    //só importa os processos bloqueados por E/S
    if (proc == NULL || proc->estado != P_BLOQUEADO || proc->dispositivo_bloqueado == -1) {
      // Também verifica se o bloqueio é por E/S e não por espera de processo (pid_esperando)
      if (proc != NULL && proc->pid_esperando != -1) continue; 
      
      continue; 
    }
    
    // checar o estado do dispositivo
    dispositivo_id_t disp = proc->dispositivo_bloqueado;
    dispositivo_id_t disp_ok = disp + 1; //dispositivo de status (OK)
    int estado;
    if (es_le(self->es, disp_ok, &estado) != ERR_OK) {
      console_printf("SO: erro ao checar E/S pendente para pid %d", proc->pid);
      self->erro_interno = true; 
      continue;
    }

    if (estado != 0) {
      //dispositivo está PRONTO
      console_printf("SO: E/S pronta para pid %d (disp %d), desbloqueando.", proc->pid, disp);
      //operações pendentes 
      if (disp % 4 == 0) { //dispositivo de LEITURA (Teclado - D_TERM_X_TECLADO)
        int dado;
        if (es_le(self->es, disp, &dado) != ERR_OK) {
           console_printf("SO: erro ao completar leitura pendente para pid %d", proc->pid);
           proc->ctx_cpu.regA = -1; // sinaliza erro no processo
        } else {
           proc->ctx_cpu.regA = dado; //coloca o dado no regA
        }
      } else if (disp % 4 == 2) { // Dispositivo de ESCRITA (Tela - D_TERM_X_TELA)
        int dado = proc->ctx_cpu.regX; // PEGA O DADO SALVO NO REGX DA PCB
        if (es_escreve(self->es, disp, dado) != ERR_OK) {
          console_printf("SO: erro ao completar escrita pendente for pid %d", proc->pid);
          proc->ctx_cpu.regA = -1;
        } else {
          proc->ctx_cpu.regA = 0; // Sucesso
        }
      }
      
      // Desbloqueia o processo
      proc->estado = P_PRONTO;
      proc->dispositivo_bloqueado = -1; //marca que não está mais esperando E/S
    }
  }
}


static void so_escalona(so_t *self)
{
  // escolhe o próximo processo a executar, que passa a ser o processo
  //   corrente; pode continuar sendo o mesmo de antes ou não
  // t2: na primeira versão, escolhe um processo pronto caso o processo
  //   corrente não possa continuar executando, senão deixa o mesmo processo.
  //   depois, implementa um escalonador melhor

    // limpa processos terminados
  for (int i = 0; i < MAX_PROCESSES; i++) {
    if (self->tabela_de_processos[i] != NULL &&
      self->tabela_de_processos[i]->estado == P_TERMINOU) {
      libera_terminal(self, self->tabela_de_processos[i]->pid);
      free(self->tabela_de_processos[i]);
      self->tabela_de_processos[i] = NULL;
    }
  }
  //verifica se o processo corrente pode continuar executando
  if(self->processo_corrente != NO_PROCESS &&
   self->tabela_de_processos[self->processo_corrente]->estado == P_EXECUTANDO) {
    // processo atual ainda está executando
    return;
  }

  //procura um processo pronto para executar
  for(int i = 0; i < MAX_PROCESSES; i++){
    if(self->tabela_de_processos[i] != NULL && self->tabela_de_processos[i]->estado == P_PRONTO){
      //encontrou um processo pronto
      self->processo_corrente = i;
      self->tabela_de_processos[i]->estado = P_EXECUTANDO;
      return;
    }
    
  }
}

// coloca o estado do processo corrente na CPU, para que ela execute
//O escalonador define quem vai rodar.
//O despachante (dispatcher) coloca ele para rodar.
static int so_despacha(so_t *self)
{
  // t2: se houver processo corrente, coloca o estado desse processo onde ele
  //   será recuperado pela CPU (em CPU_END_PC etc e 59) e retorna 0,
  //   senão retorna 1
  // o valor retornado será o valor de retorno de CHAMAC, e será colocado no 
  //   registrador A para o tratador de interrupção (ver trata_irq.asm).
  if (self->processo_corrente == NO_PROCESS) {
    //nenhum processo para rodar
    return 1;
  }
  cpu_ctx contexto = self->tabela_de_processos[self->processo_corrente]->ctx_cpu;
  mem_escreve(self->mem, CPU_END_PC, contexto.pc);
  mem_escreve(self->mem, CPU_END_A, contexto.regA);
  mem_escreve(self->mem, 59, contexto.regX);
  mem_escreve(self->mem, CPU_END_erro, contexto.erro); 

  if (self->erro_interno) return 1;
  else return 0;
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
  // verifica o tipo de interrupção que está acontecendo, e atende de acordo
  switch (irq) {
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
  if (ender != CPU_END_TRATADOR) {
    console_printf("SO: problema na carga do programa de tratamento de interrupção");
    self->erro_interno = true;
  }

  // programa o relógio para gerar uma interrupção após INTERVALO_INTERRUPCAO
  if (es_escreve(self->es, D_RELOGIO_TIMER, INTERVALO_INTERRUPCAO) != ERR_OK) {
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

  ///processos
   //inicializa a tabela de processos 
  // tabela_de_processos já é um array fixo, não precisa de malloc
  for (int i = 0; i < MAX_PROCESSES; i++) {
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
  if (ender < 0) { // Verificação de erro melhorada
    console_printf("SO: problema na carga do programa inicial");
    self->erro_interno = true;
    return;
  }
  //coloca o endereço do programa init np primeiro processo
  pcb* processo_inicial = criar_processo(ender, D_TERM_A_TECLADO, D_TERM_A_TELA);
  //marcar o terminal usado
  self->terminais_usados[0] = processo_inicial->pid;
  self->tabela_de_processos[0] = processo_inicial;
  self->processo_corrente = 0; //índice do processo inicial na tabela

  // altera o PC para o endereço de carga
  //self->regPC = ender; // deveria ser no processo
  processo_inicial->ctx_cpu.pc = ender;
  processo_inicial->ctx_cpu.regA = 0;   // <-- ESSENCIAL
  processo_inicial->ctx_cpu.regX = 0;   // <-- ESSENCIAL
  processo_inicial->ctx_cpu.erro = 0; // <-- ESSENCIAL


  self->regPC = processo_inicial->ctx_cpu.pc;
  // Inicializa campos de bloqueio
  processo_inicial->dispositivo_bloqueado = -1;
  processo_inicial->pid_esperando = -1;
  processo_inicial->estado = P_EXECUTANDO;
  
}

//acorda qualquer processo que estava bloqueado esperando 'pid_que_morreu'
static void so_acorda_processos_esperando(so_t *self, int pid_que_morreu)
{
  if (pid_que_morreu <= 0) return; // PID inválido

  for (int i = 0; i < MAX_PROCESSES; i++) {
    pcb* proc = self->tabela_de_processos[i];
    
    //se o processo [i] estava bloqueado esperando o PID que acabou de morrer...
    if (proc != NULL && 
        proc->estado == P_BLOQUEADO && 
        proc->pid_esperando == pid_que_morreu) 
    {
      console_printf("SO: processo %d (que morreu) estava sendo esperado por %d. Acordando.",
                     pid_que_morreu, proc->pid);
      proc->estado = P_PRONTO;
      proc->pid_esperando = -1; // não está mais esperando
      proc->ctx_cpu.regA = 0; // retorna sucesso para a chamada SO_ESPERA_PROC
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
  if(self->processo_corrente != NO_PROCESS){
    pcb* proc = self->tabela_de_processos[self->processo_corrente];
    err_t erro = proc->ctx_cpu.erro;
    console_printf("SO: erro na CPU do processo %d: %s", proc->pid, err_nome(erro));
    so_acorda_processos_esperando(self, proc->pid); //acorda processos esperando esse processo
    proc->usando = 0;
    proc->estado = P_TERMINOU;
    self->processo_corrente = NO_PROCESS; //nenhum processo está executando
    libera_terminal(self, proc->pid); //libera o terminal usado pelo processo
    console_printf("SO: IRQ TRATADA -- erro na CPU: %s", err_nome(erro));
    self->erro_interno = true;
  }
    return;
  }
  //err_t err = self->regERRO;
  

// interrupção gerada quando o timer expira
static void so_trata_irq_relogio(so_t *self)
{
  // rearma o interruptor do relógio e reinicializa o timer para a próxima interrupção
  err_t e1, e2;
  e1 = es_escreve(self->es, D_RELOGIO_INTERRUPCAO, 0); // desliga o sinalizador de interrupção
  e2 = es_escreve(self->es, D_RELOGIO_TIMER, INTERVALO_INTERRUPCAO);
  if (e1 != ERR_OK || e2 != ERR_OK) {
    console_printf("SO: problema da reinicialização do timer");
    self->erro_interno = true;
  }
  // t2: deveria tratar a interrupção
  //   por exemplo, decrementa o quantum do processo corrente, quando se tem
  //   um escalonador com quantum
  console_printf("SO: interrupção do relógio (não tratada)");
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
  //int id_chamada = self->regA;
  // so.c (so_trata_irq_chamada_sistema - CORREÇÃO)
  pcb *proc = self->tabela_de_processos[self->processo_corrente];
  int id_chamada = proc->ctx_cpu.regA;
  console_printf("SO: chamada de sistema %d", id_chamada);
  switch (id_chamada) {
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
  if (es_le(self->es, entrada_ok, &estado) != ERR_OK) {
    console_printf("SO: problema no acesso ao estado do teclado (pid %d)", proc->pid);
    proc->ctx_cpu.regA = -1; //retorna erro
    self->erro_interno = true;
    return;
  }
    
   /*  if (estado != 0) break;
    // como não está saindo do SO, a unidade de controle não está executando seu laço.
    // esta gambiarra faz pelo menos a console ser atualizada
    // t2: com a implementação de bloqueio de processo, esta gambiarra não
    //   deve mais existir.
    console_tictac(self->console); */
 if (estado != 0) {
    // Dispositivo PRONTO: lê imediatamente
    int dado;
    if (es_le(self->es, entrada, &dado) != ERR_OK) {
      console_printf("SO: problema no acesso ao teclado (pid %d)", proc->pid);
      proc->ctx_cpu.regA = -1; // Retorna erro
      self->erro_interno = true;
      return;
    }
    proc->ctx_cpu.regA = dado; //Coloca o dado no registrador A do PROCESSO
  } else {
    //dspositivo NÃO PRONTO: bloqueia o processo
    console_printf("SO: processo %d bloqueado esperando E/S (leitura)", proc->pid);
    proc->estado = P_BLOQUEADO;
    proc->dispositivo_bloqueado = entrada; //salva qual dispositivo está esperando
    self->processo_corrente = NO_PROCESS; //força o escalonador a rodar
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
  dispositivo_id_t saida = proc->saida;        // Ex: D_TERM_B_TELA
  dispositivo_id_t saida_ok = saida + 1;       // Ex: D_TERM_B_TELA_OK

  
  int estado;
  if (es_le(self->es, saida_ok, &estado) != ERR_OK) {
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
  if (estado != 0) {//dispositivo pronto, escreve o dado na tela
    int dado = proc->ctx_cpu.regX;//pega o dado do reg X do processo
    if (es_escreve(self->es, saida, dado) != ERR_OK) {
      console_printf("SO: problema no acesso à tela (pid %d)", proc->pid);
      proc->ctx_cpu.regA = -1; // Retorna erro
      self->erro_interno = true;
      return;
    }
    proc->ctx_cpu.regA = 0; // Sucesso
  } else {
    // Dispositivo NÃO PRONTO: bloqueia o processo
    console_printf("SO: processo %d bloqueado esperando E/S (escrita)", proc->pid);
    proc->estado = P_BLOQUEADO;
    proc->dispositivo_bloqueado = saida; // Salva qual dispositivo está esperando
    self->processo_corrente = NO_PROCESS; // Força o escalonador a rodar
  }
}
// retorna o índice do primeiro terminal livre ou -1 se todos estiverem ocupados
static int so_aloca_terminal(so_t *self) {
    for (int i = 0; i < TERMINAIS; i++) {
        if (self->terminais_usados[i]==0) { // terminal livre
            //self->terminais_usados[i] = 0;
            return i;// retorna o índice do terminal alocado
        }
    }
    return -1; // nenhum terminal disponível
}

// retorna o id do terminal correspondente ao índice
static dispositivo_id_t id_terminal_livre(int idx) {
   
    switch (idx) {
        case 0: return D_TERM_A;
        case 1: return D_TERM_B;
        case 2: return D_TERM_C;
        case 3: return D_TERM_D;
        default: return -1; // índice inválido
    }
}

static void libera_terminal(so_t *self, int pid) {
    for (int i = 0; i < TERMINAIS; i++) {
        if (self->terminais_usados[i] == pid) {
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

  //aponta para o processo corrente na tabela de processos
  //pega o processo corrente para ler o X e pegar o nome do arquivo 
  pcb* processo_corrente = self->tabela_de_processos[self->processo_corrente];
  int nome_arquivo = processo_corrente->ctx_cpu.regX;

  // em X está o endereço onde está o nome do arquivo
  //int ender_proc;

  //achar uma posição vazia na tabela de processos, para colocar o processo novo
  int possivel_indice = -1;
  for (int i = 0; i < MAX_PROCESSES; i++) {
    if (self->tabela_de_processos[i] == NULL) {
      possivel_indice = i;
      break;
    }
  }
  if (possivel_indice == -1) {
    //não tem mais espaço na tabela de processos
    console_printf("sem espaço na tabela de processos");
    self->regA = -1; //erro
    return;
  }
  // t2: deveria ler o X do descritor do processo criador
  //ender_proc = self->regX;
  char nome[100];
  if (copia_str_da_mem(100, nome, self->mem, nome_arquivo)) {
    int ender_carga = so_carrega_programa(self, nome);
    //logica contraria
    if (ender_carga < 0) { //erro na carga
      console_printf("SO: problema na carga do programa '%s'", nome);
      self->regA = -1; //erro
      return;
      // t2: deveria escrever no PC do descritor do processo criado
      //self->regPC = ender_carga;
    }
    //cria o processo , endereço de carga, entrada e saída
    // Selecionar um terminal disponível, no vetor de terminais
    int terminal_id = so_aloca_terminal(self);
    if (terminal_id == -1) {
        console_printf("SO: nenhum terminal disponível para o novo processo\n");
        processo_corrente->ctx_cpu.regA = -1; // erro
        return;
    }
    dispositivo_id_t terminal_livre = id_terminal_livre(terminal_id); //achar o terminal correspondente
    pcb* novo_processo = criar_processo(ender_carga, terminal_livre + TERM_TECLADO, terminal_livre + TERM_TELA);
    //marca o terminal como usado com o pid do processo que está usando
    self->terminais_usados[terminal_id] = novo_processo->pid;
    self->tabela_de_processos[possivel_indice] = novo_processo;//colocar o processo na tabela
    novo_processo->estado = P_PRONTO;
    // t2: deveria escrever no PC do descritor do processo criado (antes: //self->regPC = ender_carga;)
    novo_processo->ctx_cpu.pc = ender_carga;
    //escrever o PID do processo criado no reg A do processo que pediu a criação
    processo_corrente->ctx_cpu.regA = novo_processo->pid;
    
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

    if (pid_a_matar == 0 || pid_a_matar == proc_corrente->pid) {
        // mata a si mesmo
        proc_alvo = proc_corrente;
    } else {
        // mata outro processo
        proc_alvo = achar_processo(self, pid_a_matar);
    }

    if (proc_alvo == NULL) {
        proc_corrente->ctx_cpu.regA = -1; // erro: PID não encontrado
        return;
    }
    so_acorda_processos_esperando(self, proc_alvo->pid);
    // marca como terminado
    proc_alvo->estado = P_TERMINOU;
    proc_alvo->usando = 0;

    // libera terminal
    libera_terminal(self, proc_alvo->pid);

    // remove da tabela
    for (int i = 0; i < MAX_PROCESSES; i++) {
        if (self->tabela_de_processos[i] == proc_alvo) {
            free(proc_alvo);
            self->tabela_de_processos[i] = NULL;
            break;
        }
    }

    // se matou a si mesmo, não há processo corrente
    if (pid_a_matar == 0 || pid_a_matar == proc_corrente->pid) {
        self->processo_corrente = NO_PROCESS;
    }

    proc_corrente->ctx_cpu.regA = 0; // sucesso
}



// implementação da chamada se sistema SO_ESPERA_PROC
// espera o fim do processo com pid X
//bloqueia o processo corrente(init) se o processo com pid X não tiver terminado
static void so_chamada_espera_proc(so_t *self)
{
  // t2: deveria bloquear o processo se for o caso (e desbloquear na morte do esperado)
  //bloquei o processo corrente
  //verificar se o processo a se esperar é valido 
  pcb *proc_corrente = self->tabela_de_processos[self->processo_corrente];
  int pid_esperado = proc_corrente->ctx_cpu.regX; //ver o que tem em X, que é o pid do processo esperado
  pcb *proc_esperado = achar_processo(self, pid_esperado);
  if(pid_esperado == proc_corrente->pid || !proc_esperado) {
    //não pode esperar ele mesmo
    self->erro_interno = true;
    return;
  }
  console_printf("SO: processo %d esperando o processo %d", proc_corrente->pid, pid_esperado);
  //bloqueia o processo chamador 
  if(proc_esperado->estado != P_TERMINOU){
    proc_corrente->estado = P_BLOQUEADO;
    proc_corrente->pid_esperando = pid_esperado;
    //self->processo_corrente = NO_PROCESS; // Força escalonamento
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
  if (prog == NULL) {
    console_printf("Erro na leitura do programa '%s'\n", nome_do_executavel);
    return -1;
  }

  int end_ini = prog_end_carga(prog);
  int end_fim = end_ini + prog_tamanho(prog);

  for (int end = end_ini; end < end_fim; end++) {
    if (mem_escreve(self->mem, end, prog_dado(prog, end)) != ERR_OK) {
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
  for (int indice_str = 0; indice_str < tam; indice_str++) {
    int caractere;
    if (mem_le(mem, ender + indice_str, &caractere) != ERR_OK) {
      return false;
    }
    if (caractere < 0 || caractere > 255) {
      return false;
    }
    str[indice_str] = caractere;
    if (caractere == 0) {
      return true;
    }
  }
  // estourou o tamanho de str
  return false;
}

pcb *achar_processo(so_t *self, int pid) {
  for (int i = 0; i < MAX_PROCESSES; i++) {
    if (self->tabela_de_processos[i] != NULL && self->tabela_de_processos[i]->pid == pid) {
      return self->tabela_de_processos[i];
    }
  }
  return NULL;
}

// vim: foldmethod=marker