# CRC8 · modo 2

Coleta: **VÁLIDA** · N = 1000 · semente = 1

| Modo | OK | CORROMPIDO | PERDIDO | Não detectado | Taxa (%) |
|---|---:|---:|---:|---:|---:|
| 2 | 399 | 563 | 38 | 0 | 0 |

A tabela descreve as decisões realmente registradas pelo RX. Apenas coletas válidas, inclusive com ressalvas explícitas, e com 1000 classificações entram nas tabelas finais.

| Medida | Média | Mínimo | Máximo | Amostras |
|---|---:|---:|---:|---:|
| calculo_tx_us | 3.002 | 3 | 4 | 1000 |
| verificacao_rx_us | 6.003119 | 6 | 8 | 962 |
| intervalo_tx_us | 50999.997998 | 50998 | 51000 | 999 |
| intervalo_rx_us | 51000.004004 | 50998 | 51004 | 999 |
| recuperacao_rx_ms | 115.225514 | 51.11 | 408.112 | 35 |

Overhead: 6 bytes por quadro, excluindo referência e bits de início/parada.
NA indica ausência de amostra ou denominador zero; não significa zero medido.

## Evidências

[TXT original do TX](tx.txt) · [TXT original do RX](rx.txt) · [TXT original do NOISE](noise.txt)

[Erros não detectados desta coleta](evidencias_nd.csv)
