# CRC8 · modo 1

Coleta: **VÁLIDA COM RESSALVAS** · N = 1000 · semente = 1

| Modo | OK | CORROMPIDO | PERDIDO | Não detectado | Taxa (%) |
|---|---:|---:|---:|---:|---:|
| 1 | 621 | 363 | 16 | 0 | 0 |

A tabela descreve as decisões realmente registradas pelo RX. Apenas coletas válidas, inclusive com ressalvas explícitas, e com 1000 classificações entram nas tabelas finais.

| Medida | Média | Mínimo | Máximo | Amostras |
|---|---:|---:|---:|---:|
| calculo_tx_us | 3.002002 | 3 | 4 | 999 |
| verificacao_rx_us | 6.941057 | 6 | 8 | 984 |
| intervalo_tx_us | 50999.997996 | 50998 | 51000 | 998 |
| intervalo_rx_us | 51000.07007 | 50990 | 51007 | 999 |
| recuperacao_rx_ms | 80.731437 | 52.011 | 154.041 | 16 |

Overhead: 6 bytes por quadro, excluindo referência e bits de início/parada.
NA indica ausência de amostra ou denominador zero; não significa zero medido.

## Evidências

[TXT original do TX](tx.txt) · [TXT original do RX](rx.txt) · [TXT original do NOISE](noise.txt)

[Erros não detectados desta coleta](evidencias_nd.csv)

## Ressalvas de registro

- TX: sequência 695 com campos iniciais danificados; quadro preservado e conferido no NOISE. Tempos dessa linha excluídos das médias.
- RX: bytes inválidos antes do START atual, na linha 1.
- RX: mensagem auxiliar de lacuna danificada na linha 891; as classificações são conferidas separadamente.

Auditoria cruzada: 1000 quadros conferidos entre TX, RX e NOISE.
Os TXT foram preservados. O CSV do TX e suas médias usam somente linhas completas; os fragmentos ficam separados em tx_fragmentos.csv, com timestamps ausentes marcados como NA.
