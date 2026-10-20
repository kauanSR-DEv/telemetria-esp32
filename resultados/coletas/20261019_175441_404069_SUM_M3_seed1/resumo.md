# SUM · modo 3

Coleta: **VÁLIDA** · N = 1000 · semente = 1

| Modo | OK | CORROMPIDO | PERDIDO | Não detectado | Taxa (%) |
|---|---:|---:|---:|---:|---:|
| 3 | 174 | 740 | 86 | 0 | 0 |

A tabela descreve as decisões realmente registradas pelo RX. Apenas coletas válidas, inclusive com ressalvas explícitas, e com 1000 classificações entram nas tabelas finais.

| Medida | Média | Mínimo | Máximo | Amostras |
|---|---:|---:|---:|---:|
| calculo_tx_us | 1.001 | 1 | 2 | 1000 |
| verificacao_rx_us | 3.291028 | 3 | 4 | 914 |
| intervalo_tx_us | 50999.998999 | 50999 | 51000 | 999 |
| intervalo_rx_us | 50999.985986 | 50920 | 51006 | 999 |
| recuperacao_rx_ms | 305.168423 | 51.142 | 1326.187 | 52 |

Overhead: 6 bytes por quadro, excluindo referência e bits de início/parada.
NA indica ausência de amostra ou denominador zero; não significa zero medido.

## Evidências

[TXT original do TX](tx.txt) · [TXT original do RX](rx.txt) · [TXT original do NOISE](noise.txt)

[Erros não detectados desta coleta](evidencias_nd.csv)
