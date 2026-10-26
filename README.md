# Cartucho

Emulador para Android que joga **Game Boy Advance**, **Game Boy / Color**,
**Nintendo DS** e **Nintendo 3DS** num app só, com biblioteca de jogos, controle na tela
configurável e suporte a controles físicos.

Por dentro, o Cartucho é um *frontend* próprio que carrega os emuladores como
módulos (cores no formato libretro):

| Console | Emulador | Licença |
|---|---|---|
| Game Boy Advance, Game Boy, Game Boy Color | [VisualBoyAdvance-M](https://github.com/visualboyadvance-m/visualboyadvance-m) | GPL-2.0 |
| Nintendo DS | [melonDS DS](https://github.com/JesseTG/melonds-ds) (melonDS) | GPL-3.0 |
| Nintendo 3DS | [Azahar](https://github.com/azahar-emu/azahar) | GPL-2.0+ |

Os dois são compilados direto do código oficial pelo workflow **Build cores**
(`.github/workflows/cores.yml`, versões em `VBAM_SHA`, `MELONDS_SHA` e `AZAHAR_SHA`), que
publica o resultado na release `cores`. O workflow **Build APK** roda a cada
envio e monta o app em poucos minutos usando esses cores.

## Baixar o APK

Cada envio para `main` gera um APK novo. Ele fica em **Releases → latest** e
também como artefato da execução em **Actions**. Todos os APKs são assinados
com a mesma chave, então cada um instala por cima do anterior sem perder
saves.

## O que já tem

- Biblioteca com capas, filtros por console, favoritos, recentes e busca.
  Capas, em ordem: a que você escolheu; as que o Skraper/EmulationStation
  deixaram na pasta dos jogos (`media/...`, `gamelist.xml`); seu repositório
  de capas (GitHub ou qualquer site); ScreenScraper (com sua conta); o
  acervo libretro (por semelhança de nome); e o TheGamesDB (com sua chave).
- Abre jogos de qualquer pasta do celular (inclusive cartão SD) sem copiar,
  e também jogos dentro de `.zip` e `.7z` (extraídos uma vez, na primeira vez
  que você joga).
- Controle na tela com multitoque, vibração, transparência e tamanho
  ajustáveis, e **modo de edição** para arrastar e redimensionar cada botão
  (layout salvo por console e por orientação).
- Controles Bluetooth/USB e teclado; os botões na tela somem quando você usa
  um controle físico.
- 6 slots de save state por jogo, mais salvar/carregar rápido na tela; saves
  de bateria automáticos.
- Voltar no tempo (segure «) no GB, GBA e DS.
- Trapaças (GameShark, Action Replay, CodeBreaker) por jogo.
- Captura de tela para a galeria (Imagens/Cartucho).
- Acelerar (2x a 6x).
- Filtros de imagem: nítido, suave, nítido suave, tela LCD, linhas de TV e
  bordas arredondadas Scale2x/Scale3x; no 3DS, filtros de textura do Azahar
  (xBRZ, ScaleForce, MMPX, Anime4K) e cache de shaders.
- **Modo TV:** com o celular ligado a uma TV (HDMI, Chromecast/Smart View),
  o jogo vai para a TV e o celular vira o controle; no DS e 3DS a tela de cima
  fica na TV e a de toque no celular.
- Sensor de movimento (jogos de inclinar), vibração (celular ou controle) e
  microfone (DS: botão MIC; 3DS: quando o jogo pede).
- Run-ahead (1 ou 2 quadros) para menos atraso nos botões no GB e GBA.
- Backup dos saves num .zip (inclusive direto no Google Drive) e restauração.
- Relatório de erro pronto para copiar ou compartilhar.
- 3DS: duas telas com toque na tela de baixo, analógico, ZL/ZR, layouts
  (empilhado, lado a lado, tela grande + pequena, uma tela), trocar telas,
  resolução interna até 4x, New 3DS.

- **Link cable pela rede** (GBA e Game Boy): hospede ou entre numa partida
  no mesmo Wi-Fi; as partidas aparecem sozinhas, sem digitar IP. Cabo de
  GBA (2 a 4 jogadores) e adaptador sem fio (Union Room de Pokémon). Usa o
  mesmo protocolo do VBA-M de PC na mesma versão, então PC e celular também
  se conectam.

## Ainda não tem

- BIOS: nenhuma vem no app (são da Nintendo). Nenhuma é obrigatória; em
  Configurações → BIOS dá para importar as do seu próprio console.
- Instalar arquivos `.cia` no 3DS (use `.3ds`/`.cci`/`.cxi` descriptografados).

## Jogos e arquivos de sistema

O Cartucho não vem com jogos, BIOS ou chaves. Use cópias dos seus próprios
cartuchos. Jogos de 3DS precisam estar **descriptografados**.

## Licença

O código do Cartucho é GPL-2.0 ou posterior. Os emuladores mantêm as licenças
de cada projeto (tabela acima) e são distribuídos como bibliotecas separadas.

## Estrutura

- `app/src/main/cpp/` — o motor nativo: carrega o core (`core_host`),
  desenha com OpenGL ES 3 (`gl_presenter`), áudio AAudio com reamostragem
  (`audio_output`, `resampler`), ritmo dos quadros (`session`) e a ponte JNI.
- `app/src/main/java/com/cartucho/emu/` — o app: biblioteca, tela do jogo,
  controle na tela, controles físicos, configurações.
- `tools/host-test/` — testes do motor que rodam no Linux, inclusive com o
  core real do VBA-M e uma ROM de teste.
