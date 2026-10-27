#!/bin/bash
# Script para instalar VirtualBox no Debian 12

echo "Instalando VirtualBox no Debian 12..."
echo "Este script requer privilégios de root (sudo)"

# Atualiza lista de pacotes
sudo apt-get update

# Instala dependências
sudo apt-get install -y wget gnupg2

# Adiciona chave GPG do repositório VirtualBox
wget -q https://www.virtualbox.org/download/oracle_vbox_2016.asc -O- | sudo gpg --dearmor --yes -o /usr/share/keyrings/oracle-virtualbox-2016.gpg

# Adiciona repositório VirtualBox
echo "deb [arch=amd64 signed-by=/usr/share/keyrings/oracle-virtualbox-2016.gpg] https://download.virtualbox.org/virtualbox/debian bookworm contrib" | sudo tee /etc/apt/sources.list.d/virtualbox.list

# Atualiza lista de pacotes novamente
sudo apt-get update

# Instala VirtualBox (versão mais recente disponível)
sudo apt-get install -y virtualbox-7.0

# Verifica instalação
if command -v VBoxManage &> /dev/null; then
    echo "VirtualBox instalado com sucesso!"
    VBoxManage --version
else
    echo "Erro na instalação do VirtualBox"
    exit 1
fi

echo ""
echo "Para usar o VirtualBox, você também pode precisar instalar:"
echo "  sudo apt-get install -y virtualbox-ext-pack"
echo ""
echo "E adicionar seu usuário ao grupo vboxusers:"
echo "  sudo usermod -aG vboxusers $USER"
echo ""
echo "Depois faça logout e login novamente."
