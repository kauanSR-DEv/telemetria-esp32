# Publicar no GitHub

## Nome e descrição sugeridos

**Nome:** `telemetria-esp32`  
**Descrição:** `Enlace UART com três ESP32: comparação experimental entre soma modular e CRC-8 sob ruído físico controlado. Projeto IFMT.`

Tópicos sugeridos: `esp32`, `uart`, `crc8`, `checksum`, `telemetry`, `arduino`, `ifmt`.

## Pelo navegador

1. Crie o repositório na sua conta. Escolha a visibilidade conforme a entrega combinada com o professor.
2. Extraia o ZIP recebido. Abra a pasta `telemetria-esp32`.
3. No GitHub, abra **Add file → Upload files**; em um repositório vazio, use o link para enviar arquivos existentes.
4. Envie **o conteúdo de dentro da pasta**, para que `README.md` fique na raiz do repositório. Não envie somente o ZIP e não crie uma pasta externa duplicada.
5. Confira a lista de arquivos e confirme o commit com a mensagem `Organiza projeto de telemetria com ESP32`.

Pastas que começam por ponto podem não aparecer no seletor do Windows. O Git Bash, abaixo, inclui `.github`, `.gitignore` e `.gitattributes` corretamente. A interface web limita uploads; para o pacote completo, o envio por Git é a opção mais confiável.

## Pelo Git Bash — recomendado para o pacote completo

Abra a pasta extraída, clique com o botão direito e escolha **Open Git Bash here**. Substitua a URL pela do seu repositório vazio:

```bash
git init
git add .
git commit -m "Organiza projeto de telemetria com ESP32"
git branch -M main
git remote add origin https://github.com/SEU_USUARIO/telemetria-esp32.git
git push -u origin main
```

O Git pode pedir autenticação pelo navegador. Se o repositório já tiver commits, clone-o primeiro e copie o conteúdo deste pacote para dentro da pasta clonada; depois faça `git add`, `git commit` e `git push` nessa cópia.

## Depois de cada coleta

O coletor grava em `resultados/coletas/` e atualiza `resultados/tabelas/`. Para publicar os novos resultados:

```bash
git add resultados
git commit -m "Adiciona logs e resultados dos ensaios"
git push
```

Abra as tabelas pelo GitHub e confira se os links de cada rodada levam a `resumo.md` e aos TXT completos. Abaixo da tabela do relatório, pode constar: “Registros completos disponíveis no repositório do projeto”, com o link real.

## Acabamento da página

- Na área **About**, preencha descrição e tópicos.
- O README já contém capa, navegação, fotos e links relativos; não depende do nome da sua conta para funcionar.
- Coloque a versão revisada do relatório em `docs/relatorio/` e atualize o link dessa pasta.
- Envie os resultados somente depois de conferir a validade e a correspondência com o relatório.

A pasta `.venv/`, caches e arquivos de compilação estão no `.gitignore`. Os logs experimentais ficam versionados de propósito. O pacote não impõe uma licença pública: a equipe pode escolher e incluir uma depois.

Referência: [GitHub Docs — adicionar arquivos](https://docs.github.com/en/repositories/working-with-files/managing-files/adding-a-file-to-a-repository).
