<p align="center">
  <img src="docs/assets/capa.svg" alt="Telemetria UART com ESP32: SUM e CRC-8 sob ruído físico" width="100%">
</p>

<p align="center">
  <strong>Enlace UART com três ESP32: comparação entre soma modular e CRC-8 sob ruído físico controlado.</strong><br>
  Transmissão e Comunicação de Dados · Engenharia da Computação · IFMT
</p>

<p align="center">
  <a href="docs/protocolo.md">Protocolo</a> ·
  <a href="docs/montagem.md">Montagem</a> ·
  <a href="resultados/tabelas/README.md">Resultados</a> ·
  <a href="docs/relatorio/README.md">Relatório</a> ·
  <a href="docs/demonstracao.md">Demonstração</a>
</p>

## O projeto

O transmissor envia quadros de telemetria pela UART. O receptor aceita ou descarta cada quadro usando **SUM** ou **CRC-8**, em rodadas distintas. Uma terceira ESP32 injeta ruído físico na linha por meio de um transistor NPN, forçando os bits selecionados ao nível baixo.

Depois da decisão, o RX consulta uma cópia do payload original para classificar o resultado. Essa referência não participa da aceitação do quadro.

| TX | RX | NOISE |
|---|---|---|
| Gera payload e sequência | Reconhece o início do quadro | Detecta o marcador de início |
| Calcula e anexa CHECK | Executa SUM ou CRC-8 e decide | Aplica o plano de ruído |
| Envia quadro e referência | Audita o payload e registra a classe | Registra alvos e alterações efetivas |

## Resultados experimentais

Foram coletadas sete condições em **19/10/2026**, com **1000 quadros por condição**, semente 1 e NOISE v1.12. Os arquivos integrais e os CSVs estão em [resultados/coletas](resultados/coletas/).

| Técnica | Modo | OK | CORROMPIDO | PERDIDO | Não detectado | Taxa (%) |
|---|---:|---:|---:|---:|---:|---:|---:|
| SUM | 0 | 1000 | 0 | 0 | 0 | N/A |
| SUM | 1 | 616 | 368 | 16 | 0 | 0,000000 |
| SUM | 2 | 384 | 576 | 38 | 2 | 0,346021 |
| SUM | 3 | 174 | 740 | 86 | 0 | 0,000000 |
| CRC-8 | 1 | 621 | 363 | 16 | 0 | 0,000000 |
| CRC-8 | 2 | 399 | 563 | 38 | 0 | 0,000000 |
| CRC-8 | 3 | 167 | 747 | 86 | 0 | 0,000000 |

Taxa = Não detectados / (CORROMPIDO + Não detectados) × 100. N/A indica denominador zero.

**SUM modo 2 apresentou dois erros não detectados reais**, nas sequências 147 e 799. O [caso byte a byte](docs/erro-nao-detectado.md) mostra os registros e o cálculo que explica a aceitação do payload incorreto.

**CRC-8 modo 1 possui uma ressalva documental:** alguns campos de uma linha do TX e mensagens auxiliares do RX foram danificados na captura. As 1000 classificações foram conferidas com os registros das três placas. As medidas do TX usam somente as 999 linhas completas. Os TXT originais e a [ressalva detalhada](resultados/coletas/20261019_180738_095814_CRC8_M1_seed1/resumo.md) estão preservados.

[Tabelas completas e identificação das rodadas →](resultados/tabelas/README.md)

## Protocolo

`A5 5A 02 SEQ_H SEQ_L P0 P1 CHECK`

| Propriedade | Configuração |
|---|---|
| Transporte | UART2, 115200 bit/s, 8N1 |
| Quadro | 8 bytes; payload de 2 bytes |
| Campos protegidos | LEN, sequência e payload |
| SUM | CHECK fecha a soma módulo 256 em zero |
| CRC-8 | Polinômio 0x07, inicial 0x00, sem reflexão, XOR final 0x00 |
| Decisão | Resíduo zero: aceita; diferente de zero: descarta |
| Sequência | 16 bits; byte mais significativo primeiro |
| Overhead | 6 bytes, excluindo a referência |
| Intervalo | Nominal de 50 ms; aproximadamente 51 ms nas coletas |

A referência `D3 91 SEQ_H SEQ_L 02 P0 P1` é enviada após uma guarda de 2 ms. [Especificação completa →](docs/protocolo.md)

## Montagem e execução

<p align="center">
  <img src="docs/fotos/detalhe-injetor.jpg" alt="Transistor e resistores do injetor de ruído na protoboard" width="600">
</p>

1. Consulte as [conexões e a configuração da Arduino IDE](docs/montagem.md).
2. Grave `firmware/tx/tx.ino`, `firmware/rx/rx.ino` e `firmware/noise/noise.ino` nas placas correspondentes.
3. Feche os Monitores Seriais e abra `INICIAR_COLETA.bat` no Windows, com Python 3.10 ou superior instalado.
4. Selecione as portas, a técnica, o modo, N e a semente. O coletor inicia RX → NOISE → TX e salva os registros automaticamente.

O modo 0 desliga o ruído; os modos 1 e 2 selecionam um e dois bits; o modo 3 seleciona uma janela de 3 a 8 bits dentro de um byte. Um alvo que já está em zero pode permanecer inalterado.

Para reanalisar registros existentes, execute `REANALISAR_COLETAS.bat`. [Instruções de coleta →](docs/coleta.md)

## Organização

| Pasta | Conteúdo |
|---|---|
| `firmware/` | Códigos de TX, RX e NOISE; hashes de integridade |
| `docs/` | Especificação, montagem, fotos, caso não detectado e demonstração |
| `docs/relatorio/` | Relatório do trabalho |
| `resultados/coletas/` | Sete rodadas com TXT, CSVs, metadados e resumos |
| `resultados/tabelas/` | Tabelas consolidadas de SUM e CRC-8 |
| `ferramentas/` | Coletor e organizador de registros |
| `tests/` | Verificações das ferramentas e da lógica no computador |

## Equipe

| Integrante | Responsabilidade principal |
|---|---|
| Emily Weber de Arruda | TX |
| Felipe Enzo Fukase | RX |
| Kauan Santos Rondon | Ruído |

Instituto Federal de Mato Grosso · Engenharia da Computação · Cuiabá, 2026.  
Disciplina: Transmissão e Comunicação de Dados · Professor: Ruy Oliveira.

## Verificação das ferramentas

```bash
python -m unittest discover -s tests -v
g++ -std=c++17 -O2 -Wall -Wextra tests/test_noise.cpp -o test_noise
./test_noise
```

Essas verificações executam no computador. A compilação dos sketches na Arduino IDE e a avaliação física do enlace são etapas distintas.

## Referências técnicas

- [Arduino-ESP32: UART](https://docs.espressif.com/projects/arduino-esp32/en/latest/api/serial.html)
- [Espressif: ESP Timer](https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-reference/system/esp_timer.html)
- [CRC Catalogue: CRC-8/SMBUS](https://reveng.sourceforge.io/crc-catalogue/all.htm#crc.cat.crc-8-smbus)
- [pySerial](https://pyserial.readthedocs.io/en/latest/pyserial_api.html)
