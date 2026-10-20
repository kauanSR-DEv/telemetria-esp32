# Erro não detectado real: SUM, sequência 799

Condição: SUM, modo 2, N = 1000, semente 1, sessão TX 5, em 19/10/2026.

[Registros e resumo da rodada](../resultados/coletas/20261019_175257_105181_SUM_M2_seed1/resumo.md) · [TX](../resultados/coletas/20261019_175257_105181_SUM_M2_seed1/tx.txt) · [RX](../resultados/coletas/20261019_175257_105181_SUM_M2_seed1/rx.txt) · [NOISE](../resultados/coletas/20261019_175257_105181_SUM_M2_seed1/noise.txt) · [Casos não detectados](../resultados/coletas/20261019_175257_105181_SUM_M2_seed1/evidencias_nd.csv)

| Campo | Enviado em hexadecimal | Recebido em hexadecimal |
|---|---|---|
| SOF | A5 5A | A5 5A |
| LEN | 02 | 02 |
| SEQ | 03 1F | 03 1F |
| P0 | F4 | 74 |
| P1 | 16 | 16 |
| CHECK | D2 | 52 |

```text
TX: A5 5A 02 03 1F F4 16 D2
RX: A5 5A 02 03 1F 74 16 52
```

O NOISE registrou os alvos `5.7;7.7`, com bytes e bits numerados a partir de zero. Foram observadas duas alterações efetivas de 1 para 0. A máscara de alvos e o XOR observado foram `0000000000800080`, com `falhas_mask=0` e `qualidade=VALIDO`.

## Cálculo manual

A verificação soma LEN, SEQ, payload e CHECK; SOF fica fora da soma.

```text
Enviado:  2 + 3 + 31 + 244 + 22 + 210 = 512; 512 mod 256 = 0
Recebido: 2 + 3 + 31 + 116 + 22 +  82 = 256; 256 mod 256 = 0
```

A retirada de dois bits de peso 128 reduziu a soma em 256. O resíduo continuou zero, e o RX aceitou o payload `7416`. Só depois da decisão a referência preservada `D3 91 03 1F 02 F4 16` revelou que o payload verdadeiro era `F416`.

Esse é um padrão físico que a soma modular não detecta. A mesma rodada apresentou outro caso na sequência 147. Foram dois não detectados entre 576 corrompidos e dois não detectados: `2 / (576 + 2) × 100 = 0,346021%`.
