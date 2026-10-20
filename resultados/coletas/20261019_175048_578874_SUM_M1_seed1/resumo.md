# SUM · modo 1

Coleta: **VÁLIDA** · N = 1000 · semente = 1

| Modo | OK | CORROMPIDO | PERDIDO | Não detectado | Taxa (%) |
|---|---:|---:|---:|---:|---:|
| 1 | 616 | 368 | 16 | 0 | 0 |

A tabela descreve as decisões realmente registradas pelo RX. Apenas coletas válidas, inclusive com ressalvas explícitas, e com 1000 classificações entram nas tabelas finais.

| Medida | Média | Mínimo | Máximo | Amostras |
|---|---:|---:|---:|---:|
| calculo_tx_us | 1.001 | 1 | 2 | 1000 |
| verificacao_rx_us | 3.002033 | 3 | 4 | 984 |
| intervalo_tx_us | 50999.998999 | 50999 | 51000 | 999 |
| intervalo_rx_us | 51000.004004 | 50998 | 51003 | 999 |
| recuperacao_rx_ms | 89.36175 | 51.11 | 204.112 | 16 |

Overhead: 6 bytes por quadro, excluindo referência e bits de início/parada.
NA indica ausência de amostra ou denominador zero; não significa zero medido.

## Evidências

[TXT original do TX](tx.txt) · [TXT original do RX](rx.txt) · [TXT original do NOISE](noise.txt)

[Erros não detectados desta coleta](evidencias_nd.csv)
