# Como Testar o ISO com GRUB

## 1. Gerar o ISO

```bash
cd /home/usuario/so/src_dos
make clean
make iso-grub
```

## 2. Configurar VM no VirtualBox

### Opção A: Script Automático

```bash
./setup_vbox.sh
```

### Opção B: Manual (GUI)

1. **Criar Nova VM:**
   - Nome: `MeuSO`
   - Tipo: `Other`
   - Versão: `Other/Unknown (32-bit)`
   - Memória: `64 MB`

2. **Configurações → Sistema:**
   - **Aba Principal:**
     - Desmarque **"Habilitar EFI"** (CRÍTICO!)
     - Ordem de Boot:
       - 1º: **Óptico**
       - 2º: Disco Rígido
     - Desmarque Floppy
   
   - **Aba Processador:**
     - Processadores: 1
     - Desmarque "Habilitar PAE/NX"

3. **Configurações → Armazenamento:**
   - Se não houver controladora, clique em **"Adicionar controladora"** → **IDE**
   - Clique no ícone de **CD/DVD** (vazio)
   - À direita, clique no ícone de **disco**
   - Selecione **"Escolher um arquivo de disco..."**
   - Navegue até `/home/usuario/so/src_dos/so-grub.iso`
   - Selecione o arquivo

4. **Desabilitar (não necessário):**
   - Rede
   - Áudio
   - USB

## 3. Iniciar a VM

- Clique em **"Iniciar"**
- Deve aparecer o **menu do GRUB** com "Meu SO"
- Selecione ou aguarde 5 segundos
- O kernel deve iniciar

## 4. Solução de Problemas

### Erro: "Could not read from the boot medium"

**Causas possíveis:**
1. ISO não está anexado no CD/DVD
2. EFI está habilitado (desmarque!)
3. Ordem de boot errada (Óptico deve ser 1º)

**Solução:**
- Verifique se `so-grub.iso` aparece na aba **Armazenamento**
- Desmarque **EFI** na aba **Sistema**
- Verifique ordem de boot

### Erro: Tela preta / Nada acontece

**Solução:**
- Verifique se o ISO foi gerado corretamente: `ls -lh so-grub.iso`
- Regenere: `make clean && make iso-grub`
- Teste no QEMU primeiro: `qemu-system-i386 -cdrom so-grub.iso -m 64`

### Testar no QEMU (para debug)

```bash
qemu-system-i386 -cdrom so-grub.iso -m 64 -boot d
```

Se funcionar no QEMU mas não no VirtualBox, o problema é configuração da VM.
