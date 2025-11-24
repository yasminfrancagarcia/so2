#include "bloco.h"
#include <stdbool.h>
#include <stdlib.h>

// rastreador de memoria física, cada bloco indica se está ocupado 
// ou livre, e quem esta ocupando
bloco_t* cria_bloco(int tamanho){
    if (tamanho <= 0) return NULL;
    // aloca 'tamanho' structs bloco_t
    bloco_t* bloco = malloc(tamanho * sizeof(*bloco));
    if (bloco == NULL) return NULL;

    for(int i = 0; i < tamanho; i++){
        if(i < BLOCOS_RESERVADOS){ // reservando os dois primeiros blocos para o SO
            bloco[i].ocupado = true;
            bloco[i].pid = 0; // bloco reservado para o SO (pid 0)
            bloco[i].pg = -1;
            bloco[i].ciclos = 0;
            bloco[i].acesso = 0;
        } else{
            bloco[i].ocupado = false;
            bloco[i].pid = -1; // -1 indica livre
            bloco[i].pg = -1;
            bloco[i].ciclos = 0;
            bloco[i].acesso = 0;
        }
    }
    return bloco;
}