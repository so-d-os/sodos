#!/bin/bash
# Script para criar e configurar VM no VirtualBox automaticamente

ISO_PATH="$(pwd)/so-grub.iso"
VM_NAME="MeuSO"

echo "Configurando VM no VirtualBox..."
echo "ISO (GRUB): $ISO_PATH"

# Verifica se o VirtualBox está instalado
if ! command -v VBoxManage &> /dev/null; then
    echo "Erro: VirtualBox não está instalado!"
    echo "Execute: ./install_virtualbox.sh"
    exit 1
fi

# Verifica se o ISO existe
if [ ! -f "$ISO_PATH" ]; then
    echo "Erro: ISO não encontrado em $ISO_PATH"
    echo "Execute: make iso-grub"
    exit 1
fi

# Remove VM existente se houver
VBoxManage unregistervm "$VM_NAME" --delete 2>/dev/null

# Cria nova VM
echo "Criando VM: $VM_NAME"
VBoxManage createvm --name "$VM_NAME" --ostype "Other" --register

# Configura memória (64 MB)
VBoxManage modifyvm "$VM_NAME" --memory 64

# Configura processador
VBoxManage modifyvm "$VM_NAME" --cpus 1

# Desabilita EFI (importante para boot via BIOS)
VBoxManage modifyvm "$VM_NAME" --firmware bios

# Desabilita recursos desnecessários
VBoxManage modifyvm "$VM_NAME" --audio none
VBoxManage modifyvm "$VM_NAME" --usb off
VBoxManage modifyvm "$VM_NAME" --clipboard disabled
VBoxManage modifyvm "$VM_NAME" --draganddrop disabled

# Configura boot order (CD primeiro)
VBoxManage modifyvm "$VM_NAME" --boot1 dvd --boot2 disk --boot3 none --boot4 none

# Adiciona ISO como CD/DVD
echo "Adicionando ISO: $ISO_PATH"
VBoxManage storagectl "$VM_NAME" --name "IDE Controller" --add ide --controller PIIX4
VBoxManage storageattach "$VM_NAME" --storagectl "IDE Controller" --port 0 --device 0 --type dvddrive --medium "$ISO_PATH"

echo ""
echo "VM criada com sucesso!"
echo ""
echo "Para iniciar a VM, execute:"
echo "  VBoxManage startvm \"$VM_NAME\" --type gui"
echo ""
echo "Ou abra o VirtualBox e inicie a VM manualmente."
