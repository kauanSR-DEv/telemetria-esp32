# CRC8 · modo 3

Coleta: **VÁLIDA** · N = 1000 · semente = 1

| Modo | OK | CORROMPIDO | PERDIDO | Não detectado | Taxa (%) |
|---|---:|---:|---:|---:|---:|
| 3 | 167 | 747 | 86 | 0 | 0 |

A tabela descreve as decisões realmente registradas pelo RX. Apenas coletas válidas, inclusive com ressalvas explícitas, e com 1000 classificações entram nas tabelas finais.

| Medida | Média | Mínimo | Máximo | Amostras |
|---|---:|---:|---:|---:|
| calculo_tx_us | 3.002 | 3 | 4 | 1000 |
| verificacao_rx_us | 6.308534 | 6 | 8 | 914 |
| intervalo_tx_us | 50999.997998 | 50998 | 51000 | 999 |
| intervalo_rx_us | 50999.98999 | 50916 | 51011 | 999 |
| recuperacao_rx_ms | 350.868694 | 51.112 | 2040.115 | 49 |

Overhead: 6 bytes por quadro, excluindo referência e bits de início/parada.
NA indica ausência de amostra ou denominador zero; não significa zero medido.

## Evidências

[TXT original do TX](tx.txt) · [TXT original do RX](rx.txt) · [TXT original do NOISE](noise.txt)

[Erros não detectados desta coleta](evidencias_nd.csv)
