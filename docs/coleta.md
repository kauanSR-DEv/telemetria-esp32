# Coleta e organização dos registros

## Caminho mais simples: coletor com janela

Os programas continuam sendo gravados pela Arduino IDE. Durante os testes, o coletor assume as três portas USB para salvar a saída das placas e enviar os comandos. A geração e a verificação dos quadros continuam nas ESP32.

### Primeira abertura no Windows

1. Instale **Python 3.10 ou superior**, se ainda não estiver instalado: [python.org](https://www.python.org/downloads/). Habilite a opção de adicionar Python ao PATH no instalador.
2. Extraia o pacote para uma pasta normal. Não execute diretamente de dentro do ZIP.
3. Grave os sketches nas três placas e feche todos os Monitores Seriais da Arduino IDE.
4. Abra `INICIAR_COLETA.bat`. Na primeira vez ele cria um ambiente Python local e instala pySerial; é necessário acesso à internet nessa preparação.
5. Escolha a porta COM de cada placa. A IDE mostra essa informação em Ferramentas → Porta. O coletor confirma o papel de cada placa antes de iniciar.

### Em cada ensaio

- Escolha SUM ou CRC8, modo 0–3, semente e quantidade.
- Primeiro teste **SUM / modo 0 / 10 quadros**. Essa é uma conferência curta, não uma rodada oficial.
- Clique em **Iniciar coleta**. Não abra o Monitor Serial enquanto o coletor estiver usando a porta.
- Aguarde a conclusão. O botão **Abrir pasta dos resultados** leva aos arquivos gerados.
- Consulte `resumo.md`: ele mostra as contagens e eventuais pendências. **REVISAR** significa que a rodada não entra na tabela final.

O coletor faz a configuração, inicia RX e NOISE antes do TX, espera `DONE` do TX e só então solicita `END`. Cada rodada cria uma pasta nova; os TXT das rodadas anteriores são preservados. Interromper gera uma coleta parcial identificada como tal.

Se uma porta estiver ocupada, feche o Monitor Serial ou outro terminal. Se houver erro de identificação, confira as portas escolhidas e os firmwares gravados. Não é preciso regravar as placas entre modos: o modo e a técnica são configurados por comandos.

### Linux e macOS

Com Python 3.10+, Tk e permissão de acesso às portas seriais:

```bash
python3 -m venv .venv
source .venv/bin/activate
python -m pip install -r requirements.txt
python ferramentas/coletar.py
```

## Os sete ensaios

| Ordem sugerida | Técnica | Modo | N | Semente sugerida |
|---:|---|---:|---:|---:|
| 1 | SUM | 0 | 1000 | 1 |
| 2 | SUM | 1 | 1000 | 1 |
| 3 | SUM | 2 | 1000 | 1 |
| 4 | SUM | 3 | 1000 | 1 |
| 5 | CRC8 | 1 | 1000 | 1 |
| 6 | CRC8 | 2 | 1000 | 1 |
| 7 | CRC8 | 3 | 1000 | 1 |

Usar a mesma semente nos mesmos modos reproduz os alvos planejados. O TX reinicia sua sequência de payloads a cada START. O conteúdo de CHECK muda entre técnicas, portanto a mesma máscara de ruído não garante o mesmo número de bits efetivamente alterados quando CHECK é atingido. O efeito físico deve ser conferido nos logs.

CRC modo 0 é opcional e pode ser coletado como controle adicional; as tabelas principais seguem as sete condições solicitadas. Repita os testes necessários para obter um erro não detectado real, preservando cada rodada.

## Se preferir o Monitor Serial

Use 115200 baud e final de linha Nova linha. Selecione a mesma técnica no TX e no RX. Abaixo há um exemplo SUM, modo 1, 1000 quadros:

| Momento | Placa | Comando |
|---|---|---|
| Configuração | TX | `SUM` |
| Configuração | RX | `SUM` |
| Configuração | NOISE | `MODE 1` |
| Configuração | NOISE | `SEED 1` |
| Início 1 | RX | `START 1000` |
| Início 2 | NOISE | `START 1000` |
| Início 3 | TX | `START 1000` |
| Depois de `DONE` no TX | RX | `END` |
| Depois de `DONE` no TX | NOISE | `END`, se ainda estiver ativo |

Para CRC, envie `CRC` no TX e no RX. Para mudar o modo, use `MODE 0`, `MODE 2` ou `MODE 3` no NOISE, fora de sessão. `STOP` aborta, portanto não o use no encerramento de uma rodada completa.

Salve a saída integral de cada placa na mesma pasta, com os nomes **`tx.txt`**, **`rx.txt`** e **`noise.txt`**. Inclua o cabeçalho `# START`, o cabeçalho CSV, todas as linhas e o resumo final. Desative timestamps adicionados pela IDE ao copiar; os próprios registros já incluem seus tempos. Um recorte mostrando apenas o fim da tela não substitui os registros individuais.

Depois organize:

```bash
python ferramentas/organizar_logs.py resultados/coletas/NOME_DA_RODADA
```

O organizador não altera os TXT. Se o conteúdo estiver incompleto, ele gera o resumo com as pendências e deixa a rodada fora da tabela oficial. TXT antigos ou de outra revisão podem continuar guardados como histórico, mas não serão promovidos automaticamente a resultados finais do NOISE 1.12.

## Como os arquivos são separados

| Categoria | Arquivos | Uso |
|---|---|---|
| Evidência original | `tx.txt`, `rx.txt`, `noise.txt` | Conferir tudo o que as placas imprimiram |
| Dados por quadro | `tx.csv`, `rx.csv`, `noise.csv` | Filtrar e analisar no Excel, LibreOffice ou Python |
| Resultado da rodada | `resumo.md`, `resumo.json`, `tabela_professor.csv` | Preencher e conferir o relatório |
| Erros não detectados | `evidencias_nd.csv` | Localizar os bytes e o XOR observado de cada caso |
| Rastreabilidade | `sessao.json`, `comandos.csv` | Registrar data, portas, semente e comandos |

O `sessao.json` registra os hashes dos arquivos locais de firmware. Isso identifica os arquivos distribuídos; não lê a memória das placas nem prova qual binário estava gravado. O cabeçalho do NOISE confirma a versão declarada em execução.

Os CSVs usam vírgula e UTF-8 com BOM. Se o Excel não separar as colunas ao abrir, use **Dados → De Texto/CSV** e escolha o delimitador **vírgula**.

## Critérios de validade

O organizador verifica uma sessão por arquivo, mesma técnica e N, sequências completas, ausência de duplicações, `DONE` do TX, resumo válido do RX, quatro classes coerentes e resumo válido do NOISE. Também confere os quadros do TX contra a observação limpa do injetor e as referências disponíveis do RX. Uma falha em qualquer placa impede a inclusão automática.

O controle não substitui a inspeção física nem a análise dos tempos. A identificação de falhas é conservadora: arquivo truncado, cabeçalho ausente ou informação insuficiente gera pendência, em vez de estimar dados faltantes.

Nas tabelas consolidadas, entram somente coletas válidas de **1000 quadros**. Se houver mais de uma por técnica/modo, a pasta com nome datado mais recente é selecionada. Os links em `resultados/tabelas/README.md` mostram exatamente quais rodadas foram usadas. Conserve esse padrão de nomes ao adicionar rodadas manualmente.

## Tempos e erro não detectado

O resumo calcula média, mínimo, máximo e número de amostras de cálculo do TX, verificação do RX, intervalos e recuperação. `NA` significa ausência de amostra, não tempo zero. A média de verificação inclui checagens concluídas, inclusive quadros rejeitados; quadros perdidos sem checagem não entram nessa média.

Para o caso real exigido no relatório, abra `evidencias_nd.csv`, escolha uma sequência e consulte as linhas correspondentes dos três arquivos. O XOR indica o que mudou; a lista de posições indica os alvos planejados. Depois apresente o cálculo manual sobre os bytes recebidos. O CSV ajuda a localizar a evidência; o cálculo e a explicação ainda devem ser redigidos pela equipe.

## Limites da ferramenta

O coletor e o organizador foram verificados por testes no computador e simulação das respostas seriais. A primeira utilização nas três placas precisa ser confirmada com a coleta curta em modo 0. O coletor não executa CAL nem SELFTEST automaticamente; esses diagnósticos permanecem disponíveis no firmware para quando forem necessários.
