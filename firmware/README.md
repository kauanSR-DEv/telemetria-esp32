# Firmwares

| Sketch | Placa | Papel |
|---|---|---|
| [tx/tx.ino](tx/tx.ino) | Transmissor | Monta os quadros, calcula CHECK e envia a referência |
| [rx/rx.ino](rx/rx.ino) | Receptor | Verifica, decide e audita as classificações |
| [noise/noise.ino](noise/noise.ino) | Injetor | NOISE v1.12-sincronizado; gera e audita o ruído físico |

Os três arquivos foram copiados sem alterar bytes do pacote final NOISE v1.12 fornecido pela equipe. `SHA256.json` registra a identidade dessas cópias. Os comentários sobre revisões fazem parte dos originais.

A coleta estruturada funciona no computador, portanto não modifica temporização, checagem ou pulsos dos firmwares. Veja [montagem e configuração](../docs/montagem.md) e [protocolo](../docs/protocolo.md).
