# develop — ferramentas que rodam em qualquer versão do SODOS

Esta pasta fica fora do `src13/` de propósito. O `src13/` é uma versão do sistema (kernel, drivers, libc, compilador, desktop). O que está aqui não pertence a nenhuma versão: são programas que compilam e rodam no SODOS de hoje, no de amanhã e, sempre que der, nos anteriores.

Cabem aqui:

- **Ferramentas de desenvolvimento**, como o `make` (port do pdpmake) e outras do mesmo tipo.
- **Sistemas experimentais**: programas e protótipos que ainda não sabemos se vão para o sistema, e que é bom testar em várias versões sem mexer no `src13/`.

Fica de fora tudo o que depende de uma versão específica (kernel, drivers, DLLs do sistema, o próprio `cc`). Isso continua em `src13/`.

## O que tem hoje

| Pasta | O que é | No disco |
|---|---|---|
| `make/` | `MAKE.EXE`, port do [pdpmake](https://github.com/rmyorston/pdpmake) (domínio público). As receitas rodam no prompt do SODOS. Veja `make/LEIA.TXT`. | fontes em `D:\DEVELOP\MAKE`, `MAKE.EXE` na raiz do `D:` |

## Regras para um programa ficar aqui

1. **Só a interface pública.** Use apenas os headers do `cc` (`src13/c_compiler/include`, que vão para `D:\`), a `LIBC.DLL`, a `POSIX.DLL` e o que o `KERNEL.IMP` exporta. Nada de incluir headers do kernel nem chamar funções internas: elas mudam de uma versão para outra.
2. **Compila dentro do SODOS.** Cada pasta tem um `BUILD.BAT` que compila só com o `cc`, para a primeira vez ou para quando ainda não existe `MAKE.EXE`, e um `MAKEFILE` para o `make` recompilar só o que mudou. Os dois acham headers e DLLs em `D:\` e, sem `D:` (no notebook), em `U:\INCLUDE` e `U:\`, como o `make/BUILD.BAT` faz.
3. **Nomes 8.3.** Nomes longos na FAT só existem nas versões mais novas. Para a pasta funcionar em qualquer uma, use nomes como `MAIN.C` e `LEIA.TXT`. No disco os nomes ficam em maiúsculas.
4. **Texto sem acento nos arquivos que vão para o disco** (`LEIA.TXT`, `.BAT`, comentários), como já fazem os do `make`. Assim o texto não depende de como cada versão mostra o que não é ASCII. Este README fica só no repositório, por isso pode ter acento.
5. **Falta de recurso não pode derrubar o programa.** Se ele usar algo que só as versões novas têm, teste antes de usar e mostre uma mensagem clara quando não tiver.
6. **Um `LEIA.TXT` por pasta**, dizendo de onde veio o código (se for port), a licença, como compilar e as diferenças para o original.

## Como uma ferramenta chega ao disco

Os Makefiles do `src13/` buscam os fontes aqui com caminho relativo:

- `src13/Makefile`: a lista `<NOME>_SRC` (por exemplo `MAKE_SRC = ../develop/make/...`) e um bloco no alvo `disk-files` que cria `D:\DEVELOP\<NOME>` e copia os fontes em maiúsculas.
- `src13/c_compiler/Makefile`: a regra que compila o `.EXE` no Linux (por exemplo `MAKE_DIR = ../../develop/make`), que vai para `c_compiler/out/` e de lá para a raiz do `D:`.

Para uma ferramenta nova, crie `develop/<nome>/` e acrescente essas duas entradas, seguindo o modelo do `make`.

Compilar e testar o `make` no Linux, a partir de `src13/`:

```bash
make -C c_compiler out/MAKE.EXE
```

```bash
make -C c_compiler maketest
```

Dentro do SODOS:

```
cd \develop\make
build
```

Depois da primeira vez, `make` basta.
