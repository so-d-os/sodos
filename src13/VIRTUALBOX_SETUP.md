# Configuração do VirtualBox para o SO

> **Método atual (Windows/WSL):** rode `wsl make vbox` na pasta `src13` (ou `make vbox` dentro do WSL/MSYS2). O alvo recompila as imagens, cria/atualiza a VM `SODOS` com BIOS, 256 MB, VBoxSVGA e os discos `os.vdi`, `disk.vdi` e `disk2.vdi`, e inicia a VM.
>
> Use `make vbox-update` para atualizar sem iniciar e `make vbox VBOX_DISKS=1` se também quiser recriar `D:`/`disk2` a partir de `disk.img`/`disk2.img`. As instruções de ISO abaixo são **legadas** (o caminho atual usa a imagem de disco, não o CD).

## Configurações Recomendadas da VM

### 1. Criar Nova Máquina Virtual

1. Abra o VirtualBox
2. Clique em **Novo** (ou Ctrl+N)
3. Configure:
   - **Nome**: `Meu SO` (ou qualquer nome)
   - **Tipo**: `Other`
   - **Versão**: `Other/Unknown (32-bit)`
   - **Memória**: `64 MB` (mínimo) ou `128 MB` (recomendado)

### 2. Configurações do Sistema

Na aba **Sistema**:

- **Aba Principal**:
  - Desmarque **Floppy** (não precisa)
  - Marque **CD/DVD** como primeira opção de boot
  - Marque **Hard Disk** como segunda opção

- **Aba Processador**:
  - **Processadores**: 1
  - **Limite de execução**: 100%
  - **Recursos de virtualização**: Desmarque **Habilitar PAE/NX** (não necessário para 32-bit)

- **Aba Aceleração**:
  - **Interface de Paravirtualização**: `None`
  - **Aceleração de Hardware**: Marque **VT-x/AMD-V** se disponível

### 3. Configurações de Armazenamento

Na aba **Armazenamento**:

1. **Controlador IDE**:
   - Clique no ícone de **CD/DVD** vazio
   - Clique no ícone de **disco** à direita
   - Selecione **Escolher arquivo de disco...**
   - Navegue até `/home/usuario/so/src_dos/so.iso`
   - Selecione o arquivo `so.iso`

2. **Controlador SATA** (opcional):
   - Se quiser adicionar um disco secundário (para testes de FAT):
   - Clique em **Adicionar disco rígido**
   - Crie um novo disco de 2GB (para testes)

### 4. Configurações de Rede

Na aba **Rede**:
- **Adaptador 1**: Desmarque **Habilitar Adaptador de Rede** (não necessário para este SO)

### 5. Configurações de Áudio

Na aba **Áudio**:
- Desmarque **Habilitar Áudio** (não necessário)

### 6. Configurações de USB

Na aba **USB**:
- Desmarque **Habilitar Controlador USB** (não necessário)

### 7. Configurações de Tela

Na aba **Tela**:
- **Memória de Vídeo**: `8 MB` (mínimo)
- **Monitor**: 1
- **Aceleração**: Desmarque todas as opções de aceleração

## Iniciar a VM

1. Selecione a VM criada
2. Clique em **Iniciar**
3. O SO deve bootar e mostrar "Boot OK. Loading..."

## Solução de Problemas

### Se não bootar:

1. **Verifique se o ISO está correto**:
   ```bash
   cd /home/usuario/so/src_dos
   file so.iso
   ls -lh so.iso
   ```

2. **Regenere o ISO**:
   ```bash
   cd /home/usuario/so/src_dos
   make clean
   make
   make iso
   ```

3. **Verifique a ordem de boot**:
   - CD/DVD deve ser o primeiro
   - Hard Disk deve ser o segundo

4. **Tente modo de compatibilidade**:
   - Na aba **Sistema** > **Aba Principal**
   - Selecione **Modo de compatibilidade**: `PC BIOS`

5. **Desabilite EFI**:
   - Na aba **Sistema** > **Aba Principal**
   - Desmarque **Habilitar EFI** (se estiver marcado)

### Se travar após "KER":

Isso pode ser um problema no kernel, não no VirtualBox. Verifique:
- Se o kernel está sendo carregado corretamente
- Se há erros na compilação

## Configuração Mínima Recomendada

- **RAM**: 64 MB
- **CPU**: 1 core
- **Disco**: Não necessário (usa ISO)
- **Rede**: Desabilitada
- **Áudio**: Desabilitado
- **USB**: Desabilitado
