#include "fila.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include "console.h"
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

int desenfileira(fila* f, int pid) { //remove o nó  especificado pelo pid
    fila_no* atual = f->inicio;
    if (fila_vazia(f)) {
        return -1; // fila vazia, nada a desenfileirar
    }
    fila_no* anterior = NULL;
    while (atual != NULL) {
        if (atual->pid == pid) {
            if (anterior == NULL) {
                f->inicio = atual->prox; // removendo o primeiro nó da fila
            } else {
                anterior->prox = atual->prox; // removendo nó do meio ou fim
            }
            free(atual);
            f->tamanho--;
            return pid; // retorna o PID removido
        }
        anterior = atual;
        atual = atual->prox;
    }
    console_printf("PID %d não encontrado na fila.\n", pid);
    return -1; // PID não encontrado na fila
}

void imprime_fila(fila* f) {
    fila_no* atual = f->inicio;
    console_printf("Fila: ");
    while (atual != NULL) {
        console_printf("%d ", atual->pid);
        atual = atual->prox;
    }
    console_printf("\n");
}

