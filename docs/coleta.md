# Coleta e reanálise dos registros

## Preparação

Grave os três sketches com a Arduino IDE e confira as conexões descritas em [montagem.md](montagem.md). O coletor usa Python 3.10 ou superior e pySerial. As três placas devem estar conectadas por USB.

Feche os Monitores Seriais antes de abrir o coletor: cada porta deve ser usada por um único programa.

## Coleta no Windows

1. Abra `INICIAR_COLETA.bat`. A primeira execução cria o ambiente Python e instala a dependência.
2. Selecione as portas de TX, RX e NOISE, a técnica, o modo, N e a semente.
3. Clique em **Iniciar coleta** e aguarde a conclusão.
4. Consulte `resumo.md` na nova pasta em `resultados/coletas/`.

O coletor confirma a identificação das placas, configura a rodada, inicia RX → NOISE → TX e encerra o RX depois de receber DONE do TX. Uma interrupção é registrada como tal.

## Linux e macOS

Com Python, Tk e permissão para usar as portas seriais:

```bash
python3 -m venv .venv
source .venv/bin/activate
python -m pip install -r requirements.txt
python ferramentas/coletar.py
```

## Coleta pelo Monitor Serial

Use 115200 baud e final de linha Nova linha. Para SUM, envie `SUM` ao TX e ao RX; para CRC-8, envie `CRC` aos dois. Configure `MODE 0`, `MODE 1`, `MODE 2` ou `MODE 3` e `SEED 1` no NOISE, fora de sessão.

Inicie com `START 1000` no RX, no NOISE e por último no TX. Espere DONE do TX antes de enviar END ao RX e ao NOISE, se este ainda estiver ativo. STOP interrompe a rodada.

Salve as saídas integrais em uma pasta por rodada, como `tx.txt`, `rx.txt` e `noise.txt`, incluindo cabeçalhos e encerramento. Não acrescente timestamps da IDE aos registros.

## Reanalisar arquivos existentes

No Windows, abra `REANALISAR_COLETAS.bat`. Pela linha de comando:

```bash
python ferramentas/organizar_logs.py --todas
```

Para uma única pasta:

```bash
python ferramentas/organizar_logs.py resultados/coletas/NOME_DA_RODADA
```

A reanálise gera CSVs, resumos e tabelas a partir dos TXT, sem transmitir quadros ou modificar os registros originais. A coluna `seq` ou equivalente permite localizar o mesmo envio nas fontes. Os CSVs usam vírgula e UTF-8 com BOM; no Excel, use Dados → De Texto/CSV se necessário.

## Validação

O organizador confere configuração, encerramento, sequências, classificações, referências e observações do injetor. Cada envio recebe uma única classe. INCONCLUSIVO, PENDENTE, erro de UART ou inconsistência nos bytes impede validar a rodada.

Uma ressalva documental é aceita pelo organizador somente quando as classificações do RX, os registros do NOISE e os quadros originais do TX continuam preservados e coerentes. O resumo conserva os avisos e descreve a conferência cruzada. As medidas usam apenas as linhas completas; tempos ausentes não são estimados.

Nas sete rodadas publicadas, CRC-8 modo 1 apresenta essa ressalva. As demais seis condições passaram pela validação estrita. Consulte [o índice das coletas](../resultados/coletas/README.md) e os resumos individuais.

## Repetição dos ensaios

Uma rodada usa N = 1000. SUM cobre os modos 0 a 3; CRC-8, os modos 1 a 3. A semente usada nos ensaios publicados é 1. Cada repetição deve criar uma nova pasta datada. O consolidado seleciona a rodada elegível mais recente de cada técnica/modo.

O TX reinicia a sequência de payloads a cada START. A mesma semente reproduz os alvos planejados do NOISE; como CHECK muda entre técnicas, o número de alterações efetivas pode diferir. Um alvo originalmente zero pode não mudar.
