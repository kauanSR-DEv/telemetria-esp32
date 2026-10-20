# Coletas experimentais

Cada pasta contém uma rodada de 1000 quadros coletada em 19/10/2026, com semente 1.

| Técnica | Modo | Sessão TX | Pasta e resumo | Situação |
|---|---:|---:|---|---|
| SUM | 0 | 3 | [20261019_174927_927081_SUM_M0_seed1](20261019_174927_927081_SUM_M0_seed1/resumo.md) | Válida |
| SUM | 1 | 4 | [20261019_175048_578874_SUM_M1_seed1](20261019_175048_578874_SUM_M1_seed1/resumo.md) | Válida |
| SUM | 2 | 5 | [20261019_175257_105181_SUM_M2_seed1](20261019_175257_105181_SUM_M2_seed1/resumo.md) | Válida |
| SUM | 3 | 6 | [20261019_175441_404069_SUM_M3_seed1](20261019_175441_404069_SUM_M3_seed1/resumo.md) | Válida |
| CRC8 | 1 | 8 | [20261019_180738_095814_CRC8_M1_seed1](20261019_180738_095814_CRC8_M1_seed1/resumo.md) | Com ressalva documental |
| CRC8 | 2 | 9 | [20261019_181355_979307_CRC8_M2_seed1](20261019_181355_979307_CRC8_M2_seed1/resumo.md) | Válida |
| CRC8 | 3 | 10 | [20261019_181546_219907_CRC8_M3_seed1](20261019_181546_219907_CRC8_M3_seed1/resumo.md) | Válida |

| Arquivo | Conteúdo |
|---|---|
| `tx.txt`, `rx.txt`, `noise.txt` | Registros originais das três placas |
| `tx.csv`, `rx.csv`, `noise.csv` | Registros por quadro para análise |
| `resumo.md`, `resumo.json` | Contagens, medidas, validação e hashes |
| `sessao.json`, `comandos.csv` | Configuração e comandos da rodada |
| `tabela_professor.csv` | Resultado da rodada no formato solicitado |
| `evidencias_nd.csv` | Casos não detectados encontrados |
| `tx_fragmentos.csv` | Campos recuperáveis de linhas parciais, quando presentes |

Os TXT são a evidência original. Os demais arquivos podem ser regenerados com as ferramentas do repositório. Um CSV sem linhas de dados pode indicar ausência de eventos dessa categoria, como erros não detectados ou fragmentos do TX.
