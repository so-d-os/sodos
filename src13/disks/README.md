# Disks – comandos de manutenção de discos

Comandos para discos, partições e sistema de arquivos FAT:

- **mount** – monta partição e sistema FAT
- **dir** – lista arquivos no disco (FAT)
- **cat** – exibe conteúdo de arquivo no disco
- **fdisk** – listar/criar partições (list, create)
- **format** – formata partição FAT16
- **copy** – copia arquivo para o disco (destino `disk:arquivo`)
- **disks** – lista drives/discos
- **df** – espaço total/usado/livre do sistema de arquivos
- **dstat** – debug ATA (último drive, LBA, status, error)

Todos usam a API de `shell.h` e as funções de `fat.c`, `partition.c`, `fat_format.c` e `disk.h`.

### Uso típico (disco 100MB = drive 1)

1. `fdisk list 1` — listar partições (disco vazio mostra "No partitions found")
2. `fdisk create 0 1 204799 0x06 1` — criar partição 0: LBA 1, 204799 setores (~100MB), tipo FAT16, drive 1
3. `mount 0` — montar partição 0
4. `format 0 MYDISK` — formatar partição 0 com rótulo MYDISK
5. `mount 0` — montar de novo após formatar; depois `dir`, `copy`, `df`, etc.
