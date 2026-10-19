# Roteiro da demonstração — 20 minutos

| Tempo sugerido | Conteúdo |
|---|---|
| 0–3 min | Objetivo, três papéis e montagem física |
| 3–6 min | Formato do quadro, SUM/CRC e decisão independente da referência |
| 6–10 min | Rodada curta, logs e explicação das quatro classes |
| 10–14 min | Tabelas comparativas, tempos e justificativa dos timeouts |
| 14–17 min | Erro não detectado real, bytes e cálculo manual |
| 17–20 min | Predição de 10 quadros e padrão que quebra a técnica |

Antes da demonstração, confirme os cabos, as portas e os firmwares. Use uma pasta de coleta por rodada e mantenha o relatório acessível. Todos os integrantes precisam explicar o sistema inteiro.

## Predição

O professor escolhe o modo sem avisar. Antes dos envios, cada integrante registra sua previsão dos próximos 10 quadros e a justifica a partir das tabelas. Depois compare a previsão com as classificações realmente registradas. A previsão é uma estimativa apoiada nos experimentos, não uma garantia para cada quadro.

## Erro não detectado

Use um caso real preservado em `evidencias_nd.csv`, com os TXT das três placas. Mostre quadro enviado, quadro recebido, XOR observado e cálculo manual. Um exemplo matemático criado para explicar a técnica não substitui a evidência real.

## Quebrar o protocolo

Para SUM, discuta por que reduções que somam 256 podem preservar a soma módulo 256. Por exemplo, retirar o bit de valor 128 em dois bytes reduz o total em 256. Só apresente isso como resultado físico se estiver comprovado nos logs da bancada.

Para CRC, relacione padrões não detectados aos múltiplos do polinômio gerador e explique as garantias do polinômio para os erros analisados. A ausência de erros não detectados numa rodada não prova detecção de todo padrão possível.
