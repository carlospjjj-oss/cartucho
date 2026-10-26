# Cartucho

Emulador para Android que joga **Game Boy Advance**, **Game Boy / Color** e
**Nintendo 3DS** num app só, com biblioteca de jogos, controle na tela
configurável e suporte a controles físicos.

Por dentro, o Cartucho é um *frontend* próprio que carrega os emuladores como
módulos (cores no formato libretro):

| Console | Emulador | Licença |
|---|---|---|
| Game Boy Advance, Game Boy, Game Boy Color | [VisualBoyAdvance-M](https://github.com/visualboyadvance-m/visualboyadvance-m) | GPL-2.0 |
| Nintendo 3DS | [Azahar](https://github.com/azahar-emu/azahar) | GPL-2.0+ |

Os dois são compilados direto do código oficial pelo GitHub Actions, na versão
fixada em `.github/workflows/build.yml` (`VBAM_SHA`, `AZAHAR_SHA`).

## Baixar o APK

Cada envio para `main` gera um APK novo. Ele fica em **Releases → latest** e
também como artefato da execução em **Actions**. Todos os APKs são assinados
com a mesma chave, então cada um instala por cima do anterior sem perder
saves.

## O que já tem

- Biblioteca com capas, filtros por console, favoritos, recentes e busca.
- Abre jogos de qualquer pasta do celular (inclusive cartão SD) sem copiar.
- Controle na tela com multitoque, vibração, transparência e tamanho
  ajustáveis, e **modo de edição** para arrastar e redimensionar cada botão
  (layout salvo por console e por orientação).
- Controles Bluetooth/USB e teclado; os botões na tela somem quando você usa
  um controle físico.
- 6 slots de save state por jogo, saves de bateria automáticos.
- Acelerar (2x a 6x).
- Filtros de imagem: nítido, suave, nítido suave, tela LCD e linhas de TV.
- 3DS: duas telas com toque na tela de baixo, analógico, ZL/ZR, layouts
  (empilhado, lado a lado, tela grande + pequena, uma tela), trocar telas,
  resolução interna até 4x, New 3DS.

## Ainda não tem

- Link cable entre aparelhos na rede (próxima etapa).
- Instalar arquivos `.cia` no 3DS (use `.3ds`/`.cci`/`.cxi` descriptografados).

## Jogos e arquivos de sistema

O Cartucho não vem com jogos, BIOS ou chaves. Use cópias dos seus próprios
cartuchos. Jogos de 3DS precisam estar **descriptografados**.

## Estrutura

- `app/src/main/cpp/` — o motor nativo: carrega o core (`core_host`),
  desenha com OpenGL ES 3 (`gl_presenter`), áudio AAudio com reamostragem
  (`audio_output`, `resampler`), ritmo dos quadros (`session`) e a ponte JNI.
- `app/src/main/java/com/cartucho/emu/` — o app: biblioteca, tela do jogo,
  controle na tela, controles físicos, configurações.
- `tools/host-test/` — testes do motor que rodam no Linux, inclusive com o
  core real do VBA-M e uma ROM de teste.
