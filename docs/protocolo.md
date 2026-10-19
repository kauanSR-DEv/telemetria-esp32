# Especificação do protocolo

Este documento descreve o protocolo implementado nos sketches entregues. A numeração interna 1.2, presente nos comentários do código original, é uma revisão do projeto, não um padrão da UART.

## Transporte e formato

UART a 115200 bit/s, 8 bits de dados, sem paridade e 1 bit de parada (8N1). Enlace de telemetria unidirecional TX → RX, com GND comum e sem retransmissão. Um quadro tem oito bytes:

| Posição, a partir de zero | Campo | Tamanho | Valor / interpretação |
|---:|---|---:|---|
| 0–1 | SOF | 2 bytes | `A5 5A` |
| 2 | LEN | 1 byte | `02`, tamanho do payload |
| 3–4 | SEQ | 2 bytes | Inteiro sem sinal, byte mais significativo primeiro |
| 5–6 | Payload | 2 bytes | `P0 P1` |
| 7 | CHECK | 1 byte | SUM ou CRC-8, conforme a rodada |

LEN, SEQ e payload são protegidos: cinco bytes no cálculo do TX. O RX verifica esses cinco bytes mais CHECK, totalizando seis. O marcador SOF não entra na checagem. O comprimento delimita o quadro, sem marcador de fim.

## Detecção de erro

**SUM.** Some os cinco bytes protegidos módulo 256. O TX envia `CHECK = −soma mod 256`. O RX soma os seis bytes: resultado zero permite a aceitação.

**CRC-8.** Polinômio `0x07`, equivalente a `x⁸ + x² + x + 1`; valor inicial `0x00`; sem reflexão de entrada/saída; XOR final `0x00`. O TX anexa o resto calculado sobre os cinco bytes. O RX processa os seis bytes e aceita resíduo `0x00`. A rotina CRC processa o bit mais significativo primeiro; a serialização física da UART envia os bits de cada byte na ordem da UART.

As técnicas nunca são aplicadas simultaneamente ao mesmo quadro. TX e RX precisam selecionar a mesma técnica antes de START.

## Referência para auditoria

Depois de transmitir fisicamente o quadro, o TX aguarda uma guarda de 2 ms e envia sete bytes:

`D3 91 SEQ_H SEQ_L 02 P0 P1`

São marcador, sequência, tamanho e payload original. O RX fixa a decisão SUM/CRC antes de consultar essa referência. Ela permite classificar erros não detectados e não entra no overhead solicitado. O NOISE deve preservá-la.

## Recepção, descarte e ressincronização

O RX procura SOF, lê o candidato a quadro e decide pelo resíduo. Quadro aceito tem o payload entregue à aplicação; quadro reprovado é descartado sem entrega e registrado como CORROMPIDO. Não há retransmissão.

Após uma checagem concluída, a busca continua depois de CHECK. LEN diferente de `02` ou timeout antes da conclusão abandona o candidato. Nessa recuperação, a busca é retomada depois de LEN, reaproveitando bytes posteriores já recebidos. Um quadro classificado como CORROMPIDO não é contado novamente como PERDIDO.

## Sequências e sessões

Cada sessão começa na sequência zero, com N entre 1 e 1000. A sequência cabe em 16 bits e admite o retorno de 65535 para zero, embora uma sessão de 1000 quadros não alcance essa volta.

O rastreador usa diferença modular entre sequência aceita e esperada: zero significa ordem; de 1 a 32767, lacuna com atualização da expectativa; a partir de 32768, repetido ou fora de ordem. A sequência de um candidato reprovado não é considerada confiável para inferir lacunas. A auditoria reconcilia cada envio uma única vez com a referência posterior.

## Tempos e overhead

| Parâmetro implementado | Valor | Ponto de partida |
|---|---:|---|
| Período nominal do TX | 50 ms | Início do quadro anterior |
| Timeout do quadro | 2 ms | SOF reconhecido |
| Timeout entre quadros | 75 ms | Último SOF reconhecido |
| Timeout da referência | 5 ms | Início da referência |
| Guarda anterior à referência | 2 ms | Fim físico do quadro |

Os intervalos reais são registrados em `intervalo_us`; use a distribuição observada para justificar os timeouts no relatório. O valor nominal de 50 ms não substitui essa medição. O tempo de recuperação do RX vai do início da condição de recuperação até a próxima decisão aceita; não equivale simplesmente ao intervalo entre quadros.

O overhead é de seis bytes: SOF (2), LEN (1), SEQ (2), CHECK (1). Excluem-se a referência e os bits de início/parada. Eficiência em bytes do quadro: 2/8 = 25%.

## Classificação

OK: aceito e payload correto. CORROMPIDO: checagem reprovada. PERDIDO: checagem não concluída. NAO_DETECTADO: aceito e payload incorreto. A soma das quatro classes deve ser N. INCONCLUSIVO ou PENDENTE ao final impedem o uso como rodada oficial.

Erros de UART e inconsistências na auditoria invalidam a coleta do RX. A validade do RX não substitui a validação independente do injetor. `END` confirma que o TX concluiu N envios; `STOP` aborta a coleta.
