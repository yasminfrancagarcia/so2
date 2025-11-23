#include "bloco.h"
#include <stdbool.h>
#include <stdlib.h>
//rastreador de memoria física, cada bloco indica se está ocupado 
//ou livre, e quem esta ocupando
bloco_t* cria_bloco(int tamanho){
    bloco_t* bloco = (bloco_t*) malloc (tamanho * sizeof(bloco ));
    for(int i = 0; i < tamanho; i++){
        if(i< 2){ //reservando os dois primeiros blocos para o SO
            bloco[i].ocupado = true;
            bloco[i].pid = 0; //bloco 0 reservado para o SO
        } else{
            bloco[i].ocupado = false;
            bloco[i].pid = -1;
        }
    }
    return bloco;
}