// so.c
// sistema operacional
// simulador de computador
// so25b

// ---------------------------------------------------------------------
// INCLUDES {{{1
// ---------------------------------------------------------------------

#include "so.h"
#include "cpu.h"
#include "dispositivos.h"
#include "err.h"
#include "irq.h"
#include "memoria.h"
#include "programa.h"
#include "tabpag.h"
#include "processo.h"
#include "fila.h"
#include "metricas.h"
#include "bloco.h"
#include <stdlib.h>
#include <stdbool.h>


// ---------------------------------------------------------------------
// CONSTANTES E TIPOS {{{1
// ---------------------------------------------------------------------

// intervalo entre interrupções do relógio
#define INTERVALO_INTERRUPCAO 50   // em instruções executadas
#define TERMINAIS 4
#define FISICA_TAM 10000 // tamanho da memória física em bytes
// Não tem processos nem memória virtual, mas é preciso usar a paginação,
//   pelo menos para implementar relocação, já que os programas estão sendo
//   todos montados para serem executados no endereço 0 e o endereço 0
//   físico é usado pelo hardware nas interrupções.
// Os programas estão sendo carregados no início de um quadro, e usam quantos
//   quadros forem necessárias. Para isso a variável quadro_livre contém
//   o número do primeiro quadro da memória principal que ainda não foi usado.
//   Na carga do processo, a tabela de páginas (deveria ter uma por processo,
//   mas não tem processo) é alterada para que o endereço virtual 0 resulte
//   no quadro onde o programa foi carregado. Com isso, o programa carregado
//   é acessível, mas o acesso ao anterior é perdido.

// t3: a interface de algumas funções que manipulam memória teve que ser alterada,
//   para incluir o processo ao qual elas se referem. Para isso, é necessário um
//   tipo de dados para identificar um processo. Neste código, não tem processos
//   implementados, e não tem um tipo para isso. Foi usado o tipo int.
//   É necessário também um valor para representar um processo inexistente.
//   Foi usado o valor -1. Altere para o seu tipo, ou substitua os usos de
//   processo_t e NENHUM_PROCESSO para o seu tipo.
//   ALGUM_PROCESSO serve para representar um processo que não é NENHUM. Só tem
//   algum sentido enquanto não tem implementação de processos.
/* typedef int processo_t;
define NENHUM_PROCESSO -1
#define ALGUM_PROCESSO 0 */
#define NENHUM_PROCESSO NULL
struct so_t {
  cpu_t *cpu;
  mem_t *mem;
  mmu_t *mmu;
  es_t *es;
  console_t *console;
  bool erro_interno;

  int regA, regX, regPC, regERRO, regComplemento; // cópia do estado da CPU
  // t2: tabela de processos, processo corrente, pendências, etc
  pcb *tabela_de_processos[MAX_PROCESSES];
  int processo_corrente; // índice na tabela de processos
  // vetor para guardar os pids dos processos que estão usando os terminais
  // idx = 0 -> terminal A
  // idx = 1 -> terminal B...
  int terminais_usados[4];
  fila *fila_prontos;   // fila de processos prontos
  metricas_t *metricas;
  // t3: com memória virtual
  mem_t *mem_fisica; // memória física do sistema
  int bloco_livre; // índice do primeiro bloco livre na memória física
  bloco_t* blocos_memoria; // rastreador de blocos de memória física
  int num_paginas_fisicas; // número de páginas na memória física

};


// função de tratamento de interrupção (entrada no SO)
static int so_trata_interrupcao(void *argC, int reg_A);

// funções auxiliares
// no t3, foi adicionado o 'processo' aos argumentos dessas funções 
// carrega o programa contido no arquivo para memória virtual de um processo
// retorna o endereço virtual inicial de execução
static int so_carrega_programa(so_t *self, pcb* processo,
                               char *nome_do_executavel);
// copia para str da memória do processo, até copiar um 0 (retorna true) ou tam bytes
static bool so_copia_str_do_processo(so_t *self, int tam, char str[tam],
                                     int end_virt, pcb* processo);


// ---------------------------------------------------------------------
// CRIAÇÃO {{{1
// ---------------------------------------------------------------------

so_t *so_cria(cpu_t *cpu, mem_t *mem, mmu_t *mmu,
              es_t *es, console_t *console)
{
  so_t *self = malloc(sizeof(*self));
  if (self == NULL) return NULL;

  self->cpu = cpu;
  self->mem = mem;
  self->mem_fisica = mem_cria(FISICA_TAM);
  self->mmu = mmu;
  self->es = es;
  self->console = console;
  self->erro_interno = false;
  // t3: inicializa controle de memória física
  self->bloco_livre = 0;
  self->num_paginas_fisicas = mem_tam(self->mem_fisica) / TAM_PAGINA;
  self->blocos_memoria = cria_bloco(self->num_paginas_fisicas);
   // processos
  self->processo_corrente = NO_PROCESS;

  // quando a CPU executar uma instrução CHAMAC, deve chamar a função
  //   so_trata_interrupcao, com primeiro argumento um ptr para o SO
  cpu_define_chamaC(self->cpu, so_trata_interrupcao, self);
  self->fila_prontos = cria_fila();
  // inicializar terminais
  for (int i = 0; i < TERMINAIS; i++)
  {
    self->terminais_usados[i] = 0; // nenhum terminal está sendo usado
  }
  self->metricas = metricas_cria();
  return self;

  
  // inicializa a tabela de páginas global, e entrega ela para a MMU
  // t3: com processos, essa tabela não existiria, teria uma por processo, que
  //     deve ser colocada na MMU quando o processo é despachado para execução
  //self->tabpag_global = tabpag_cria();
  //mmu_define_tabpag(self->mmu, self->tabpag_global);

}

void so_destroi(so_t *self)
{
  cpu_define_chamaC(self->cpu, NULL, NULL);
  metricas_destroi(self->metricas);
  free(self);
}
// ---------------------------------------------------------------------
// FUNÇÕES PARA AS MÉTRICAS
// ---------------------------------------------------------------------
metricas_t* so_get_metricas(so_t *self) {
  return self->metricas;
}
es_t* so_get_es(so_t *self) {
  return self->es;
}
pcb** so_get_tabela_de_processos(so_t *self) {
  return self->tabela_de_processos;
}
int so_get_processo_corrente(so_t *self) {
  return self->processo_corrente;
}
int so_get_intervalo_interrupcao(so_t *self) {
  return INTERVALO_INTERRUPCAO;
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
  console_printf("SO: Atualizando tempos antes de tratar IRQ %d", irq);
  so_atualiza_tempos(self); 
  // esse print polui bastante, recomendo tirar quando estiver com mais confiança
  //console_printf("SO: recebi IRQ %d (%s)", irq, irq_nome(irq));
  // métrica 4: Contagem de Interrupções (feita em so_trata_irq
  // salva o estado da cpu no descritor do processo que foi interrompido
  console_printf("SO: salvando estado da CPU antes de tratar IRQ %d", irq);
  so_salva_estado_da_cpu(self);
  // faz o atendimento da interrupção
  console_printf("SO: tratando IRQ %d", irq);
  so_trata_irq(self, irq);
  // faz o processamento independente da interrupção
  console_printf("SO: tratando pendências após IRQ %d", irq);
  so_trata_pendencias(self);
  // escolhe o próximo processo a executar
  console_printf("SO: escalonando após IRQ %d", irq);
  so_escalona(self);
  console_printf("escalonou");
  // recupera o estado do processo escolhido
  return so_despacha(self);
}

static void so_salva_estado_da_cpu(so_t *self)
{
  // t2: salva os registradores que compõem o estado da cpu no descritor do
  //   processo corrente. os valores dos registradores foram colocados pela
  //   CPU na memória, nos endereços CPU_END_PC etc. O registrador X foi salvo
  //   pelo tratador de interrupção (ver trata_irq.asm) no endereço 59
  // se não houver processo corrente, não faz nada
    if (mem_le(self->mem, CPU_END_A, &self->regA) != ERR_OK || mem_le(self->mem, CPU_END_PC, &self->regPC) != ERR_OK || mem_le(self->mem, CPU_END_erro, &self->regERRO) != ERR_OK || mem_le(self->mem, 59, &self->regX))
  {

    if (mem_le(self->mem, CPU_END_A, &self->regA) != ERR_OK
        || mem_le(self->mem, CPU_END_PC, &self->regPC) != ERR_OK
        || mem_le(self->mem, CPU_END_erro, &self->regERRO) != ERR_OK
        || mem_le(self->mem, CPU_END_complemento, &self->regComplemento) != ERR_OK
        || mem_le(self->mem, 59, &self->regX)) {
      console_printf("SO: erro na leitura dos registradores");
      self->erro_interno = true;
    }
  }
    // se tiver algum processo corrente, salva o estado dele
  if (self->processo_corrente != NO_PROCESS)
  {
    cpu_ctx contexto;
    contexto.pc = self->regPC;
    contexto.regA = self->regA;
    contexto.regX = self->regX;
    contexto.erro = self->regERRO;
    contexto.complemento = self->regComplemento;
    self->tabela_de_processos[self->processo_corrente]->ctx_cpu = contexto;
  }

}

static void so_trata_pendencias(so_t *self)
{
    // na função que trata de pendências, o SO deve verificar o estado dos dispositivos
  // que causaram bloqueio e realizar operações pendentes e desbloquear processos se for o caso
  // ver os dispsitivos que podem estar bloqueados, itera por todos os processos
  for (int i = 0; i < MAX_PROCESSES; i++)
  {
    pcb *proc = self->tabela_de_processos[i];
    //só importa os processos bloqueados por E/S
    if (proc == NULL || proc->estado != P_BLOQUEADO || proc->dispositivo_bloqueado == -1) {
      // Também verifica se o bloqueio é por E/S e não por espera de processo (pid_esperando)
      if (proc != NULL && proc->pid_esperando != -1) continue; 
      
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
      if (disp % 4 == 0)
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
      else if (disp % 4 == 2)
      { // Dispositivo de ESCRITA (tela)
        int dado = proc->ctx_cpu.regX;
        if (es_escreve(self->es, disp, dado) != ERR_OK)
        {
          console_printf("SO: erro ao completar escrita pendente para pid %d", proc->pid);
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


//escalonador round robin
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

  //procura por um processo pronto na fila
  while (!fila_vazia(self->fila_prontos))
  {
    //pega o primeiro da fila
    int escolhido_pid = self->fila_prontos->inicio->pid;
    desenfileira(self->fila_prontos, escolhido_pid); // remove da fila

    // acha o PCB desse processo
    pcb *proc_escolhido = NULL;
    int indice_escolhido = -1;
    for (int i = 0; i < MAX_PROCESSES; i++)
    {
      if (self->tabela_de_processos[i] != NULL &&
        self->tabela_de_processos[i]->pid == escolhido_pid)
      {
        proc_escolhido = self->tabela_de_processos[i];
        indice_escolhido = i;
        break;
      }
    }

    // se o processo não existe mais (foi terminado e limpo), ignora
    if (proc_escolhido == NULL) {
      continue; // próximo da fila
    }

    if (proc_escolhido->estado == P_PRONTO) {
        //processo está pronto para rodar, deve ser escolhido
        console_printf("====> processo %d escolhido \n", escolhido_pid);
        imprime_fila(self->fila_prontos);

        self->processo_corrente = indice_escolhido;
        
        // usa a função de métrica
        so_muda_estado(self, proc_escolhido, P_EXECUTANDO);
        
        return;
    }
    
    // NÃO. Este processo estava na fila, mas está BLOQUEADO
    //    (devido ao bug de 'desenfileira' não ser chamado antes).
    //    Apenas ignore-o (ele já foi removido da fila ).
    //    O loop 'while' vai pegar o próximo.
  }

  // se o loop terminou, a fila de prontos está vazia (ou só tinha lixo)
  self->processo_corrente = NO_PROCESS;
  return;
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
  mem_escreve(self->mem, CPU_END_complemento, contexto.complemento); // limpa complemento
  // define a tabela de páginas do processo corrente na MMU
  mmu_define_tabpag(self->mmu, self->tabela_de_processos[self->processo_corrente]->tabela_paginas);

  int q;
  int err = tabpag_traduz(self->tabela_de_processos[self->processo_corrente]->tabela_paginas, contexto.pc/TAM_PAGINA, &q);
  console_printf("Espero ler instrução do quadro físico %d, traduzido da página virtual %d", q, contexto.pc/TAM_PAGINA);
  console_printf("Erro foi: %d. ERR_OK é %d", err, ERR_OK);
  console_printf("Processo = #%d", self->tabela_de_processos[self->processo_corrente]->pid);

  if (self->erro_interno)
    return 1;
  else
    return 0;

}

static int pag_livre(so_t *self) {
    for (size_t i = 0; i < self->num_paginas_fisicas; i++) {
        if (!self->blocos_memoria[i].ocupado) {
            return i;
        }
    }
    return -1; // Nenhuma página livre
}

static void page_fault_tratavel(so_t *self, int end_causador)
{
  // implementar troca de páginas
  pcb *proc_corrente = self->tabela_de_processos[self->processo_corrente];
  int pg_livre = pag_livre(self);
  int ini_end_fisico = proc_corrente->end_disco + end_causador - end_causador % TAM_PAGINA;
  int end_disco = ini_end_fisico;
  console_printf("SO: carregando página do disco, início em %d", end_disco);

  int ini_end_virtual = end_causador;
  int fim_end_virtual = ini_end_virtual + TAM_PAGINA - 1;
  // copia os dados da página do disco para a memória física
  for (int end_virt = ini_end_virtual; end_virt <= fim_end_virtual; end_virt++)
  {
    int dado;
    if (mem_le(self->mem_fisica, end_disco, &dado) != ERR_OK)
    {
      console_printf("SO: erro na leitura da memória física durante troca de página");
      self->erro_interno = true;
      return;
    }
    int quadro_fisico = pg_livre * TAM_PAGINA + (end_virt - ini_end_virtual);
    if (mem_escreve(self->mem, quadro_fisico, dado) != ERR_OK)
    {
      console_printf("SO: erro na escrita da memória principal durante troca de página");
      // self->erro_interno = true;
      return;
    }
    end_disco++;
  }
  // atualiza a tabela de páginas do processo
  self->blocos_memoria[pg_livre].ocupado = true;
  self->blocos_memoria[pg_livre].pid = proc_corrente->pid;
  tabpag_t *tabela = proc_corrente->tabela_paginas;
  tabpag_define_quadro(tabela, end_causador / TAM_PAGINA, pg_livre);
  console_printf("SO: página trocada para o processo %d, página virtual %d mapeada para quadro físico %d", proc_corrente->pid, end_causador / 10, pg_livre);
}

static void so_trata_page_fault(so_t *self)
{
  pcb *proc_corrente = self->tabela_de_processos[self->processo_corrente];
  int end_causador = proc_corrente->ctx_cpu.complemento;
  // 1. Verifica se a página já está mapeada
  int quadro;
  tabpag_t *tabela = proc_corrente->tabela_paginas;
  int pagina_virtual = end_causador / TAM_PAGINA;
  if (tabpag_traduz(tabela, pagina_virtual, &quadro) == ERR_OK) {
    
    

    // Caso não seja end_causador=0, é um erro real de MMU/Estado do processo.
    console_printf("SO: ERRO GRAVE - Falha de página em página %d já mapeada para QF %d (end %d)", pagina_virtual, quadro, end_causador);
    self->erro_interno = true;
    return;
  }
  
  // 2. Se a página não está mapeada (tratamento real de PF)
  console_printf("SO: tratando page fault para endereço %d", end_causador);
  bool existe = false;
  for(size_t i=0; i<self->num_paginas_fisicas; i++) {
    if(!self->blocos_memoria[i].ocupado) {
      //page_fault_tratavel(self, end_causador);
      existe = true;
      break;
    }
  }
  if(existe) {
    page_fault_tratavel(self, end_causador);
  } else {
    console_printf("SO: não há páginas livres para tratar o page fault");
    self->erro_interno = true;
  }

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
    //metrica 4: Contagem de Interrupções
  if (irq >= 0 && irq < N_IRQ){
    self->metricas->contagem_irq[irq]++;
  }

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
  console_printf("SO: carregando programa de tratamento de interrupção");

  int ender = so_carrega_programa(self, NENHUM_PROCESSO, "trata_int.maq");
  if (ender != CPU_END_TRATADOR) {
    console_printf("SO: problema na carga do programa de tratamento de interrupção");
    self->erro_interno = true;
  }

  // programa o relógio para gerar uma interrupção após INTERVALO_INTERRUPCAO
  if (es_escreve(self->es, D_RELOGIO_TIMER, INTERVALO_INTERRUPCAO) != ERR_OK) {
    console_printf("SO: problema na programação do timer");
    self->erro_interno = true;
  }

  // define o primeiro quadro livre de memória como o seguinte àquele que
  //   contém o endereço final da memória protegida (que não podem ser usadas
  //   por programas de usuário)
  // t3: o controle de memória livre deve ser mais aprimorado que isso  
  //self->quadro_livre = CPU_END_FIM_PROT / TAM_PAGINA + 1;

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
  // coloca o endereço do programa init np primeiro processo
  console_printf("SO: criando processo inicial (init)");
  pcb *processo_inicial = criar_processo( D_TERM_A_TECLADO, D_TERM_A_TELA);
  console_printf("SO: processo inicial criado com PID %d ", processo_inicial->pid);
  ender = so_carrega_programa(self, processo_inicial,"init.maq");
  
  if (ender < 0)
  { // Verificação de erro melhorada
    console_printf("SO: problema na carga do programa inicial");
    self->erro_interno = true;
    return;
  }
  
  // marcar o terminal usado
  self->terminais_usados[0] = processo_inicial->pid;
  self->tabela_de_processos[0] = processo_inicial;
  self->processo_corrente = 0; // índice do processo inicial na tabela

  // altera o PC para o endereço de carga
  // self->regPC = ender; // deveria ser no processo
  processo_inicial->ctx_cpu.pc = ender;
  processo_inicial->ctx_cpu.regA = 0; 
  processo_inicial->ctx_cpu.regX = 0; 
  processo_inicial->ctx_cpu.erro = 0; 
  processo_inicial->ctx_cpu.complemento = 0;

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
  self->metricas->num_proc_criados++;

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
  if (self->processo_corrente != NO_PROCESS)
  {
    pcb *proc = self->tabela_de_processos[self->processo_corrente];
    err_t erro = proc->ctx_cpu.erro;
    int complemento = proc->ctx_cpu.complemento;
    console_printf("SO: Erro de CPU detectado: Código %d. Complemento %d. PC %d.", 
                 erro, complemento, proc->ctx_cpu.pc); // ADICIONE ESTE PRINT
    if (erro == ERR_PAG_AUSENTE)
    {
      console_printf("SO: processo %d causou uma falha de página", proc->pid);
      so_trata_page_fault(self);
    }
    else if (erro == ERR_INSTR_INV)
    {
      console_printf("INSTRUCÃO INVÁLIDA executada pelo processo %d", proc->pid);
      int v;
      mmu_le(self->mmu, 0, &v, usuario);
      console_printf("SO: %d", v);
    }
    else
    {
      console_printf("SO: erro na CPU do processo %d: %s", proc->pid, err_nome(erro));
      so_acorda_processos_esperando(self, proc->pid); // acorda processos esperando esse processo
      proc->usando = 0;
      // proc->estado = P_TERMINOU;
      so_muda_estado(self, proc, P_TERMINOU); // usa a função que contabiliza métricas
      self->processo_corrente = NO_PROCESS;   // nenhum processo está executando
      libera_terminal(self, proc->pid);       // libera o terminal usado pelo processo
      console_printf("SO: IRQ TRATADA -- erro na CPU: %s", err_nome(erro));
    }
  }
}

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
  pcb *proc_corrente = self->tabela_de_processos[self->processo_corrente];
  if (proc_corrente == NULL || self->processo_corrente == NO_PROCESS) {
    return;
  }
  proc_corrente->quantum--;
  if (proc_corrente->quantum <= 0 && proc_corrente->estado!= P_BLOQUEADO){
    console_printf("SO: quantum do processo %d expirou, forçando troca de contexto.", proc_corrente->pid);
    // --- Métricas 5 e 7 ---
    proc_corrente->num_preempcoes_proc++;
    self->metricas->num_preemcoes_total++;
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
  // implementação com espera ocupada (retirada no t2 )
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
}

// implementação da chamada se sistema SO_ESCR
// escreve o valor do reg X na saída corrente do processo
static void so_chamada_escr(so_t *self)
{
  // implementação com espera ocupada (retirada no t2)
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
  // t3: identifica direito esses processos
  /* processo_t processo_criador = ALGUM_PROCESSO;
  processo_t processo_criado = ALGUM_PROCESSO; */

  // aponta para o processo corrente na tabela de processos
  // pega o processo corrente para ler o X e pegar o nome do arquivo
  pcb *processo_criador = self->tabela_de_processos[self->processo_corrente];
  //int nome_arquivo = processo_corrente->ctx_cpu.regX;

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
  int ender_proc = self->tabela_de_processos[self->processo_corrente]->ctx_cpu.regX;
  char nome[100];
  if (so_copia_str_do_processo(self, 100, nome, ender_proc, processo_criador))
  {
    // cria o processo , endereço de carga, entrada e saída
    //  Selecionar um terminal disponível, no vetor de terminais
    int terminal_id = so_aloca_terminal(self);
    if (terminal_id == -1)
    {
      console_printf("SO: nenhum terminal disponível para o novo processo\n");
      processo_criador->ctx_cpu.regA = -1; // erro
      return;
    }
    dispositivo_id_t terminal_livre = id_terminal_livre(terminal_id); // achar o terminal correspondente
    pcb *novo_processo = criar_processo( terminal_livre + TERM_TECLADO, terminal_livre + TERM_TELA);
    console_printf("SO: novo processo criado com PID %d usando terminal %d", novo_processo->pid, terminal_id);

    int ender_carga = so_carrega_programa(self, novo_processo, nome);
    // logica contraria
    if (ender_carga < 0)
    { // erro na carga
      console_printf("SO: problema na carga do programa '%s'", nome);
      self->regA = -1; // erro
      return;
      // t2: deveria escrever no PC do descritor do processo criado
      // self->regPC = ender_carga;
    }
   /*  // 1. Encontrar um quadro físico livre na memória principal (RAM)
    int quadro_livre_principal = pag_livre(self); // Sua função pag_livre busca um índice em self->blocos_memoria
    
    if (quadro_livre_principal == -1) {
      console_printf("SO: Sem quadros físicos livres para o novo processo!");
      libera_terminal(self, novo_processo->pid); // Assume que você tem uma função so_libera_terminal
      // Aqui, você pode querer implementar SWAPPING ou simplesmente falhar a criação.
      processo_criador->ctx_cpu.regA = -1; 
      // Não é elegante, mas para este nível de simulação, a falha é aceitável.
      return;
    }
    
    // 2. O endereço de início no "disco" é end_disco (salvo em so_carrega_programa_na_memoria_virtual)
    int end_disco_pg0 = novo_processo->end_disco + (ender_carga / TAM_PAGINA) * TAM_PAGINA; 
    int end_fisico_principal = quadro_livre_principal * TAM_PAGINA;

    // Copiar a primeira página (simulando a primeira carga)
    for (int offset = 0; offset < TAM_PAGINA; offset++) {
      int dado;
      // Leitura do "disco" (self->mem_fisica)
      if (mem_le(self->mem_fisica, end_disco_pg0 + offset, &dado) != ERR_OK) {
         console_printf("SO: Erro na leitura do disco simulado ao criar processo.");
         // ... tratamento de erro e liberação de quadro ...
         return;
      }
      // Escrita na memória principal (self->mem)
      if (mem_escreve(self->mem, end_fisico_principal + offset, dado) != ERR_OK) {
         console_printf("SO: Erro na escrita da RAM ao criar processo.");
         // ... tratamento de erro e liberação de quadro ...
         return;
      }
    }
    
    // 3. Mapear na tabela de páginas
    int pg_virt_inicial = ender_carga / TAM_PAGINA; // Geralmente 0
    tabpag_define_quadro(novo_processo->tabela_paginas, pg_virt_inicial, quadro_livre_principal);
    
    // 4. Marcar o quadro físico como ocupado
    self->blocos_memoria[quadro_livre_principal].ocupado = true;
    self->blocos_memoria[quadro_livre_principal].pid = novo_processo->pid;
 */
   // console_printf("SO: Primeira página do processo %d carregada e mapeada para QF %d.", novo_processo->pid, quadro_livre_principal);
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
    processo_criador->ctx_cpu.regA = novo_processo->pid;
    // inserir na fila de processos prontos
    enfileira(self->fila_prontos, novo_processo->pid);
    self->metricas->num_proc_criados++;

    return;
  }
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
  //destuir tabela de pg do processo 
  tabpag_t* tabela_pg = proc_alvo->tabela_paginas;
  tabpag_destroi(tabela_pg);

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

// funções auxiliares
static int so_carrega_programa_na_memoria_fisica(so_t *self, programa_t *programa);
static int so_carrega_programa_na_memoria_virtual(so_t *self,
                                                  programa_t *programa,
                                                  pcb* processo);

// carrega o programa na memória
// se processo for NENHUM_PROCESSO, carrega o programa na memória física
//   senão, carrega na memória virtual do processo
// retorna o endereço de carga ou -1
static int so_carrega_programa(so_t *self, pcb* processo,
                               char *nome_do_executavel)
{
  console_printf("SO: carga de '%s'", nome_do_executavel);

  programa_t *programa = prog_cria(nome_do_executavel);
  if (programa == NULL) {
    console_printf("Erro na leitura do programa '%s'\n", nome_do_executavel);
    return -1;
  }

  int end_carga;
  if (processo == NENHUM_PROCESSO) {
    end_carga = so_carrega_programa_na_memoria_fisica(self, programa);
  } else {
    end_carga = so_carrega_programa_na_memoria_virtual(self, programa, processo);
    processo->end_disco = end_carga; // salvar o endereço na memória secundaria
  }

  prog_destroi(programa);
  return end_carga;
}

static void so_inicializa_bloco_fisico(so_t *self, int end_ini, int end_fim, int pid){
  for(int end = 0; end < end_fim; end+= TAM_PAGINA){
    self->blocos_memoria[end / TAM_PAGINA].pid = pid;
    self->blocos_memoria[end / TAM_PAGINA].ocupado = true;

  }
}

static int so_carrega_programa_na_memoria_fisica(so_t *self, programa_t *programa)
{
  int end_ini = prog_end_carga(programa);
  int end_fim = end_ini + prog_tamanho(programa);

  for (int end = end_ini; end < end_fim; end++) {
    if (mem_escreve(self->mem, end, prog_dado(programa, end)) != ERR_OK) {
      console_printf("Erro na carga da memória, endereco %d\n", end);
      return -1;
    }
  }
  so_inicializa_bloco_fisico(self, end_ini, end_fim, 0); // pid 0 para SO
  //0 pq é o trata_int.maq que carrega os processos
  console_printf("SO: carga na memória física %d-%d", end_ini, end_fim);
  return end_ini;
}



static int so_carrega_programa_na_memoria_virtual(so_t *self,
                                                  programa_t *programa,
                                                  pcb* processo)
{
  // t3: isto tá furado...
  // está simplesmente lendo para o próximo quadro que nunca foi ocupado,
  //   nem testa se tem memória disponível
  // com memória virtual, a forma mais simples de implementar a carga de um
  //   programa é carregá-lo para a memória secundária, e mapear todas as páginas
  //   da tabela de páginas do processo como inválidas. Assim, as páginas serão
  //   colocadas na memória principal por demanda. Para simplificar ainda mais, a
  //   memória secundária pode ser alocada da forma como a principal está sendo
  //   alocada aqui (sem reuso)
  //int end_virt_ini = prog_end_carga(programa);
  // o código abaixo só funciona se o programa iniciar no início de uma página
  /* if ((end_virt_ini % TAM_PAGINA) != 0) return -1;
  int end_virt_fim = end_virt_ini + prog_tamanho(programa) - 1;
  int pagina_ini = end_virt_ini / TAM_PAGINA;
  int pagina_fim = end_virt_fim / TAM_PAGINA;
  int n_paginas = pagina_fim - pagina_ini + 1;
  int quadro_ini = self->quadro_livre;
  int quadro_fim = quadro_ini + n_paginas - 1;
  // mapeia as páginas nos quadros
  for (int i = 0; i < n_paginas; i++) {
    tabpag_define_quadro(self->tabpag_global, pagina_ini + i, quadro_ini + i);
  }
  self->quadro_livre = quadro_fim + 1; */

  // carrega o programa na memória secundaria
  
  int end_fis_ini = self->bloco_livre;
  int end_fis = end_fis_ini;
  int end_virt_ini = 0;
  int prog_tamanho_bytes = prog_tamanho(programa);
  int end_virt_fim = end_virt_ini + prog_tamanho_bytes - 1;
  self->bloco_livre = end_virt_fim + 1; // atualiza o próximo bloco livre na memória secundaria

  for (int end_virt = end_virt_ini; end_virt <= end_virt_fim; end_virt++) {
    if (mem_escreve(self->mem_fisica, end_fis, prog_dado(programa, end_virt)) != ERR_OK) {
      console_printf("Erro na carga da memória, end virt %d fís %d\n", end_virt,
                     end_fis);
      return -1;
    }
    end_fis++;
  }
  int num_paginas = (end_virt_fim - end_virt_ini ) / TAM_PAGINA; //acho q ta errado
  console_printf("SO: carga na memória secundaria V%d-%d F%d-%d npag=%d",
                 end_virt_ini, end_virt_fim, end_fis_ini, end_fis - 1, num_paginas);
  //processo->end_disco = end_fis_ini; // salvar o endereço na memória secundaria
  return end_virt_ini;
}


// ---------------------------------------------------------------------
// ACESSO À MEMÓRIA DOS PROCESSOS {{{1
// ---------------------------------------------------------------------

// copia uma string da memória do processo para o vetor str.
// retorna false se erro (string maior que vetor, valor não char na memória,
//   erro de acesso à memória)
// O endereço é um endereço virtual de um processo.
// t3: Com memória virtual, cada valor do espaço de endereçamento do processo
//   pode estar em memória principal ou secundária (e tem que achar onde)
static bool so_copia_str_do_processo(so_t *self, int tam, char str[tam],
                                     int end_virt, pcb* processo)
{
  if (processo == NENHUM_PROCESSO) return false;
  for (int indice_str = 0; indice_str < tam; indice_str++) {
    int caractere;
    // não tem memória virtual implementada, posso usar a mmu para traduzir
    //   os endereços e acessar a memória, porque todo o conteúdo do processo
    //   está na memória principal, e só temos uma tabela de páginas
    if (mmu_le(self->mmu, end_virt + indice_str, &caractere, usuario) != ERR_OK) {
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
