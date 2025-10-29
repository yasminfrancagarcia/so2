#include "fila.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>

fila* cria_fila() {
    fila* f = (fila*)malloc(sizeof(fila));
    f->tamanho = 0;
    f->inicio = NULL;
    return f;
}

bool fila_vazia(fila* f) {
    return f->tamanho == 0;
}

void destroi_fila(fila* f) {
    fila_no* atual = f->inicio;
    while (atual != NULL) {
        fila_no* proximo = atual->prox;
        free(atual);
        atual = proximo;
    }
    free(f);
}

static fila_no* criar_no(int pid) {
    fila_no* no = (fila_no*)malloc(sizeof(fila_no));
    no->pid = pid;
    no->prox = NULL;
    return no;
} 


void enfileira(fila* f, int pid) { //cria um nó e insere no final da fila
    fila_no* novo_no = criar_no(pid);

    if (fila_vazia(f)) {
        f->inicio = novo_no; // fila estava vazia, inicio aponta para o novo nó
    } else {
        fila_no* atual = f->inicio;
        while (atual->prox != NULL) { //enquanto não chegar no final da fila
            atual = atual->prox;
        }
        atual->prox = novo_no; //insere o novo nó no final da fila
    }
    f->tamanho++;
}

int desenfileira(fila* f) { //remove o nó do início da fila
    if (fila_vazia(f)) {
        return -1; // fila vazia, nada a desenfileirar
    }
    fila_no* no_a_remover = f->inicio;
    int pid = no_a_remover->pid;
    f->inicio = f->inicio->prox; // atualiza o início da fila
    free(no_a_remover);
    f->tamanho--;
    return pid;
}

