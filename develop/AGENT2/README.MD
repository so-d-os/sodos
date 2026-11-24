# AGENT2

Assistente de IA que roda nativamente dentro do SODOS, com interface de
tela de texto no padrao do TOP. Fala com a API no formato OpenAI (via
`ai_client.c`), executa acoes no sistema (comandos, gravacao de arquivos,
abertura de programas) e devolve o resultado ao modelo ate ele terminar.

## Referencias de projeto

- **Modelo de conexao / token / enderecos:** o mesmo esquema do `AGENT`
  (le `AGENT.CFG`: key/url/model, anthropic=, deepseek=).
- **Referencia de tela:** `\DEVELOP\sodos\src13\top\`
  (janela com sombra, barra de menu em cima, barra de teclas embaixo).
- **Base de mudancas:** `\DEVELOP\sodos\src13\c_compiler\agent\`
  (`agent.c` + `ai_client.c`).

## Arquivos

| Arquivo        | Papel                                                         |
|----------------|---------------------------------------------------------------|
| `AGENT2.C`     | Fonte principal: tela, conversa, ferramentas, loop de entrada |
| `AI_CLIENT.C`  | Cliente da API (fonte unica, compartilhada com o AIPANEL)     |
| `AGENT.CFG`    | Configuracao: key, url, model (mesmo do AGENT.EXE)            |
| `AGENT2.EXE`   | Executavel compilado                                          |

## Compilacao

No SODOS, dentro da pasta dos fontes:

    cd \develop\agent2
    make            (MAKEFILE: so recompila o que mudou)
    build           (BUILD.BAT: compila tudo, sem o MAKE.EXE)

Ou direto: `cc -I D:\ -L D:\ -o AGENT2.EXE AGENT2.C`.
</parameter>
</invoke>
</??DSML?? calls>
## Como funciona

1. O usuario escreve o pedido e tecla Enter.
2. O AGENT2 envia a conversa ao modelo (formato OpenAI) com um system
   prompt que descreve o SODOS e as acoes permitidas.
3. O modelo responde com texto e, opcionalmente, **um** bloco de acao:
   - bloco `cmd` - uma linha de comando DOS (roda via `k_shell_capture`)
   - bloco `write CAMINHO` - grava um arquivo (`k_fsave`)
   - bloco `open CAMINHO` - abre um programa (`k_win_launch`)
   - ou ferramentas nativas (bloco DSML `invoke`)
4. O AGENT2 executa a acao, captura a saida e devolve ao modelo, que
   continua ate responder sem acao (limite de `STEPS_MAX` = 8 passos).

## Tela

Segue o padrao do TOP/SPECIAL: janela com sombra, barra de menu no topo,
barra de teclas no rodape e bolhas de conversa (voce / SODOS / sistema).
A conversa fica encostada embaixo, como no AIPANEL, e rola com as setas.

## Teclas

| Tecla               | Acao                                                  |
|---------------------|-------------------------------------------------------|
| Enter               | envia a mensagem                                      |
| Backspace           | apaga o ultimo caractere                              |
| Seta cima / baixo   | rola a conversa uma linha (entra no modo navegando)   |
| PgUp / PgDn         | rola uma pagina                                       |
| Home / End          | vai ao comeco / ao fim da conversa                    |
| Espaco (navegando)  | volta para a area de digitacao (ESC e Enter tambem)   |
| F1 / ?              | ajuda (`?` so com o campo de texto vazio)             |
| F2                  | sessoes salvas: lista a esquerda, previa a direita    |
| F3                  | conversa nova (a atual fica salva)                    |
| F4                  | anexa um arquivo ou uma pasta                         |
| F5                  | tira os anexos da conversa                            |
| F10 / ESC           | sai (ESC so sai com o campo de texto vazio)           |

### Modo navegando

Qualquer seta, PgUp/PgDn ou Home/End leva o foco para a conversa: o titulo
vira "Conversa - navegando", o cursor do campo some e a linha de separacao
mostra quantas linhas faltam ate o fim (`g_scroll`). **Espaco** volta para a
digitacao e desce ao fim. Digitar uma letra tambem volta e ja escreve a letra.
Ao enviar uma mensagem o scroll volta ao fim.

## Sessoes

Cada conversa nova ganha um nome com a data e a hora em que comecou:

    SESSOES\dd-mm-aa-hhmmss.SES        ex.: SESSOES\23-11-26-191530.SES

- A pasta `SESSOES` fica dentro da pasta de onde o AGENT2 foi aberto.
  Para usar outra, ponha `sessoes=D:\CAMINHO` no `AGENT.CFG`.
- O arquivo so e criado quando a conversa tem a primeira pergunta, e e
  regravado depois de cada resposta, ao apertar F3, ao carregar outra sessao
  e ao sair.
- A hora e a do relogio do SODOS (UTC, a mesma da barra de cima).
- Duas sessoes no mesmo segundo ganham `-2`, `-3`... no fim do nome.

**F2** abre a tela de sessoes: a lista fica a esquerda (mais nova primeiro,
`*` marca a atual) e a direita aparece um trecho da sessao marcada (anexos e
primeiras mensagens). Enter carrega a sessao e a conversa continua nela;
Del (ou F8) apaga, com confirmacao; ESC volta.

Formato do arquivo (texto, mensagens em UTF-8):

    AGENT2 SESSAO 1
    data=23/11/26 19:15:30
    modelo=deepseek-chat
    arquivo=D:\DEVELOP\AGENT2\AGENT2.C
    pasta=D:\DEVELOP\AGENT2\SRC
    #M 1 18
    texto da pergunta
    #M 0 42
    texto da resposta...

`#M <papel> <bytes>`: papel 1 = voce, 0 = SODOS, 2 = sistema, 3 = saida de
comando (vai para o modelo, mas nao aparece na tela).

## Anexos

**F4** abre um navegador de pastas: Enter entra na pasta ou anexa o arquivo,
Espaco anexa o item marcado (arquivo ou pasta), a linha `.` anexa a pasta
aberta, Backspace sobe (da raiz vai para a lista de unidades), ESC fecha.

Os anexos ficam listados na linha de separacao e sao gravados na sessao.
A cada pergunta o conteudo e lido de novo e vai no system prompt:

- arquivo: ate 12 KB do texto;
- pasta: a listagem e o texto dos arquivos dela (ate 4 KB cada; EXE, OBJ,
  DLL, imagens e outros binarios ficam de fora);
- no total ate 32 KB por pergunta; o que nao couber e avisado ao modelo.

Ate 8 anexos por conversa. **F5** tira todos.

## Codigos de tecla (kernel)

Os de `\DEVELOP\sodos\src13\shell.h` (os mesmos do TOP). Atencao: o
`D:\CURSES.H` tem cima/baixo trocados; o AGENT2 usa os do kernel.

    KEY_UP    0x100     KEY_DOWN  0x101
    KEY_LEFT  0x102     KEY_RIGHT 0x103
    KEY_PGUP  0x104     KEY_PGDN  0x105
    KEY_HOME  0x106     KEY_END   0x107
    KEY_DEL   0x108     KEY_F(n)  0x140 + n

## Notas

- Precisa da tela de texto do DOS (nao roda no WIN).
- Precisa de rede pronta (`net`) e de uma key valida no `AGENT.CFG`.
- Nunca executa programas interativos (edit, mc, win, etc.).
