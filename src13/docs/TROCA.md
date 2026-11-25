# TROCA — kernel e DLLs do sistema sem reiniciar

`TROCA.EXE` troca o kernel e os DLLs que o kernel mantém carregados (WINE.DLL, FAT32.DLL, LINKEXT.DLL) com o sistema rodando. Programas, janelas, arquivos abertos e threads continuam como estavam.

Fontes: `utils/troca.c` (o programa) e `kernel/troca.c` (a parte do kernel).

## Uso

```
TROCA                              o que está rodando: kernel, cópias do WINE.DLL, drivers
TROCA KERNEL [novo.elf] [atual.elf] [/T] [/F]
TROCA WINE [caminho\WINE.DLL]
TROCA DLL FAT32.DLL | LINKEXT.DLL
TROCA arquivo.ELF | arquivo.DLL    o mesmo, pelo tipo do arquivo
```

| Opção | Efeito |
|---|---|
| `novo.elf` | Kernel novo. Padrão: `D:\BOOT\KERNEL.ELF`. Precisa ter sido ligado com `ld -q` e passado pelo `tools/relaoculta` (o `make kernel_multiboot.elf` do `src13` faz os dois). |
| `atual.elf` | ELF do kernel que o GRUB carregou. Só é preciso na primeira troca depois do boot, e o TROCA procura sozinho em `D:\BOOT`, `D:\TEMP`, `D:\` e `Z:\` o ELF cujo `.text` é igual ao da memória. |
| `/T` | Só testa: monta tudo, mostra o que faria e não troca nada. |
| `/F` | Aceita variáveis que mudaram de tamanho (veja Limites). |

## Primeira vez

O kernel que está rodando precisa ter a parte do kernel da troca (`kernel/troca.c`, exports `k_troca_*`). Num kernel mais antigo, o TROCA avisa e não faz nada. Reinicie uma vez com um kernel que tenha a troca; daí em diante as trocas são a quente.

## Kernel

Fluxo típico, com o kernel novo já compilado na VM:

```
copy D:\TEMP\KRN55.ELF D:\BOOT\KERNEL.ELF      (o próximo boot também usa o novo)
TROCA KERNEL /T                                 (confere)
TROCA KERNEL
```

**Atenção, boot:** o GRUB (multiboot2) recusa um ELF que tenha seções `SHT_RELA`: aparece "ELF files with relocs are not supported yet" e "você precisa carregar o kernel primeiro", e o PC não sobe (aconteceu com o build 54). O `tools/relaoculta` troca só o tipo das `.rela.*` (para `0x80000004`); o TROCA acha essas seções pelo nome. Um ELF ligado à mão com `ld -q` precisa passar por `tools/relaoculta arquivo.elf` antes de ir para `D:\BOOT\KERNEL.ELF`. Para conferir: `readelf -S` não pode listar nenhuma seção do tipo `RELA`.

Na primeira troca depois do boot, o `KERNEL.ELF` que acabou de ser sobrescrito era o do boot. O TROCA então procura outro `.ELF` igual à memória: guarde o anterior, por exemplo `D:\BOOT\KERNEL54.ELF`, ou passe o caminho como segundo argumento. Da segunda troca em diante o kernel guarda o mapa de símbolos e não precisa mais do ELF antigo.

`ver` passa a mostrar o build novo. `TROCA` sem argumentos lista as trocas feitas desde o boot.

### Como funciona

1. O `.text` e o `.rodata` do ELF novo vão inteiros para um bloco de memória do kernel abaixo de 2 GB (`k_troca_alloc`; o kernel é `-mcmodel=small`).
2. As relocações do ELF (`.rela.text`, `.rela.rodata`, `.rela.data`) são refeitas. Referências a código e a constantes apontam para o bloco. Referências a variáveis apontam para as que já estão na memória, com a mesma chave: o nome, ou `arquivo#n:nome` para as `static`. Variáveis que não existiam ganham memória nova, com o valor inicial do ELF.
3. O kernel (`k_troca_apply`, com o BKL) para as outras CPUs com a IPI `0xF6` e confere onde cada CPU parou. Também confere as threads tiradas da CPU no meio do código do kernel (`task.irq_rip`). Então escreve um `jmp` no começo de cada função antiga para a nova e solta as CPUs. Se alguém estiver no meio dos 5 bytes de um `jmp`, a troca é adiada e o TROCA tenta de novo, até 50 vezes.
4. O kernel guarda o mapa de símbolos novo para a próxima troca e chama `troca_depois` no kernel novo (linha `[troca]` na serial).

Os thunks do BKL que os programas abertos já receberam continuam valendo. O alvo antigo de cada um tem o `jmp` para a função nova, e exports novos ou em outra ordem ganham thunks novos (`kexports.c`, `thunk_for`).

### Referências ambíguas

O `.rela` de uma variável `static` aponta para "`.bss` + deslocamento", não para a variável. Uma referência RIP-relativa ainda traz no deslocamento o −4..−8 do fim da instrução. Perto de onde o layout das variáveis mudou, o mesmo endereço no ELF novo pode ser o fim de uma variável ou o começo da seguinte, e os dois ficam em lugares diferentes na memória. O TROCA resolve esses casos, nesta ordem:

1. pelo tamanho da instrução (um decodificador x86-64): um `mov` ou `cmp` sempre acessa o começo da variável seguinte;
2. pela mesma instrução no código antigo, quando a função não mudou;
3. pelas outras referências da mesma função.

Sem uma decisão segura, a troca é recusada (com `/F`, ele usa o começo da variável e avisa).

## Limites: quando reiniciar

- **Variável global ou `static` que mudou de tamanho** (um array maior, um `#define` de capacidade): a troca recusa e lista as variáveis. `/F` dá memória nova a elas, com o que cabia do conteúdo antigo, por sua conta: código antigo que ainda esteja rodando continua usando a variável velha.
- **`struct` que mudou de layout** (campo novo no meio, campo trocado): o TROCA não tem como perceber. Código novo lendo dados no formato antigo corrompe a memória. Para trocar a quente, só acrescente campos no fim de structs que vivem no heap, ou reinicie.
- **Laços que nunca voltam** (o laço do WIN, o shell, a tarefa ociosa das CPUs, threads do kernel em laço) continuam no código antigo. As funções que eles chamam mudam. Mudança dentro do próprio laço só entra no próximo boot.
- **Assinatura de função** (parâmetros) chamada por um desses laços: o laço antigo passa os argumentos antigos. Crie uma função nova em vez de mudar a antiga.
- **Clones do gcc** (`foo.part.0`, `foo.isra.0`, `foo.cold`) e funções com menos de 5 bytes não recebem `jmp`. Só o código antigo em andamento ainda os chama.
- **Variáveis `static` dentro de funções**: o gcc numera (`buf.3`). Mudar a ordem delas num arquivo pode trocar o estado entre elas.
- **Código de boot** (`kernel_entry`, `multiboot`, trampolim dos APs, `stage2`): só vale no próximo boot.
- **Driver de vídeo**: escolhido no boot, a tela depende dele. Reinicie.

Cada troca deixa o código anterior na memória, cerca de 1 MB, porque alguma CPU pode estar no meio dele. Os blocos voltam no próximo boot.

Endereços de falha no código novo aparecem como `trocaN+0x...` no CRASH.LOG. Esse é o deslocamento a partir do `.text` do ELF aplicado na N-ésima troca: some ao endereço do `.text` do ELF para usar com o `addr2line`.

## WINE.DLL

```
copy D:\TEMP\WINE.DLL D:\SODOS\DRV\WINE.DLL
TROCA WINE
```

O WINE.DLL novo é carregado numa cópia nova, e os EXEs do Windows abertos daí em diante usam essa cópia. Os que já rodam continuam na cópia deles, e os filhos deles também. Os filhos aninhados, em que o pai está na cadeia do `mod_exec`, e os filhos em paralelo (`k_win_launch_arg` com `--wine-spawn=N`) nascem na cópia do pai. Isso é necessário porque processos, handles e pedidos de spawn ficam nas variáveis de cada cópia.

Uma cópia que deixou de ser a atual sai da memória quando o último EXE dela fecha. Assim dá para testar um WINE.DLL novo no Chrome com o Claude aberto, sem reiniciar.

Programas de cópias diferentes não se enxergam pelo WINE (pipes, eventos e seções com nome são de cada cópia). O registro e os arquivos do disco são os mesmos para as duas.

`TROCA WINE caminho\WINE.DLL` carrega de outro lugar (um teste) sem mexer no `D:\SODOS\DRV`.

## FAT32.DLL e LINKEXT.DLL

- `TROCA DLL FAT32.DLL`: a próxima montagem FAT32 lê o DLL do disco de novo. Os volumes já montados continuam com o driver antigo até serem remontados.
- `TROCA DLL LINKEXT.DLL`: descarrega e carrega de novo (o mesmo que `SET LINKEXT=0` e `SET LINKEXT=1`).

Os DLLs dos programas (`LIBC.DLL`, `GUI.DLL`, `IMG.DLL`...) não precisam de troca: cada programa carrega a cópia dele ao abrir, já do arquivo novo.
