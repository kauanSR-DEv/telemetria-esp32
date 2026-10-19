# Seu repositório está pronto para começar

## Para colocar no GitHub

1. Extraia este ZIP.
2. Abra a pasta `telemetria-esp32`.
3. Publique o conteúdo dessa pasta no repositório que você criou. O arquivo `README.md` deve ficar na raiz.

O passo a passo pelo navegador e pelo Git Bash está em [docs/publicar-github.md](docs/publicar-github.md).

## Para salvar os testes sem copiar o Monitor Serial

1. Grave os códigos nas placas pela Arduino IDE e feche os Monitores Seriais.
2. Abra `INICIAR_COLETA.bat` no Windows. Tenha Python 3.10+ instalado; na primeira abertura, a ferramenta prepara a dependência.
3. Escolha as portas de TX, RX e NOISE. Selecione **SUM / modo 0 / 10 quadros** e inicie a coleta.
4. Confira `resumo.md`. Se o teste curto estiver válido, passe às rodadas de **1000 quadros**.

Os arquivos ficam em `resultados/coletas/`. As tabelas de SUM e CRC são atualizadas em `resultados/tabelas/`. Publique essas pastas depois dos testes.

O coletor foi verificado no computador com respostas seriais simuladas; confirme o primeiro teste curto nas suas placas. Os três firmwares originais foram preservados.

## O que falta preencher

- As sete rodadas experimentais com os registros completos.
- O relatório revisado em `docs/relatorio/`.
- O endereço real do repositório no relatório.

Não há resultados experimentais inventados no pacote. As tabelas começam com “A coletar”.
