# Pint-OS — Alarm Clock

Este repositório contém a implementação da atividade **Alarm Clock** da
disciplina de Infraestrutura de Software, sobre o sistema operacional
didático **Pint-OS** (Pintos).

Autor: Arthur (arthurdelimavonsohsten@gmail.com)

## O que foi feito

A função `timer_sleep()`, em [`src/devices/timer.c`](src/devices/timer.c),
originalmente fazia a thread chamadora ficar num laço de **busy wait**
(ficava chamando `thread_yield()` repetidamente até o tempo passar,
gastando CPU à toa). Ela foi reescrita para que a thread **bloqueie de
verdade** (sem consumir CPU) até o momento de acordar:

- Cada chamada de `timer_sleep()` registra um pequeno registro local
  (tick de despertar, um semáforo e um elemento de lista) numa lista
  estática `sleep_list`, ordenada por tick de despertar (e, em caso de
  empate, por prioridade decrescente), e bloqueia a thread com
  `sema_down()`.
- A cada interrupção do timer (`timer_interrupt`), o kernel percorre o
  início da `sleep_list` e "acorda" (via `sema_up()`) toda thread cujo
  horário já chegou.
- `sleep_list` é compartilhada entre threads e o handler de interrupção,
  então só é manipulada com interrupções desabilitadas pelo tempo mínimo
  necessário — nenhuma outra parte do código depende de interrupções
  desativadas para sincronização.

Mais detalhes de implementação, dificuldades e evidências de teste estão
no relatório entregue e em [`evidencias.log`](evidencias.log).

## Como compilar e rodar

Pré-requisitos: Linux (ou WSL) com `gcc` com suporte a `-m32`,
`qemu-system-x86` e `perl`.

```bash
cd src/threads
make                                    # compila o kernel
cd build
pintos -- -q run alarm-multiple         # roda um teste específico
cd .. && make check                     # roda a suíte de testes de threads
```

O utilitário `pintos` (e os demais scripts) ficam em `src/utils/` — é
preciso compilá-los uma vez (`cd src/utils && make`) e colocar essa pasta
no `PATH`.

## Testes

Os 6 testes oficiais de alarm clock passam:
`alarm-single`, `alarm-multiple`, `alarm-simultaneous`, `alarm-priority`,
`alarm-zero` e `alarm-negative`. Os logs completos (antes e depois da
correção) estão em [`evidencias.log`](evidencias.log).

Testes de outras categorias presentes na mesma árvore (`priority-*`,
`priority-donate-*`, `mlfqs-*`) pertencem a projetos futuros (escalonamento
por prioridade e escalonador 4.4BSD) e **não** fazem parte desta
atividade — por isso falham, como esperado.

## Origem do código

O código-base do Pint-OS foi desenvolvido por Ben Pfaff e outros
colaboradores, a partir de Stanford (ver [`src/LICENSE`](src/LICENSE)).
Este fork parte da versão adaptada para a disciplina 600.318 da Johns
Hopkins University (professor Ryan Huang), disponível em
[jhu-cs318/pintos](https://github.com/jhu-cs318/pintos). As mudanças
específicas desta atividade (implementação do alarm clock e ajustes de
toolchain) estão no histórico de commits deste repositório.
