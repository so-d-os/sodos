#!/bin/bash
# Script para instalar dependências do GRUB para gerar ISO

echo "Instalando dependências para gerar ISO com GRUB..."
echo "Este script requer privilégios de root (sudo)"

sudo apt-get update
sudo apt-get install -y grub-pc-bin grub-common xorriso mtools

echo ""
echo "Dependências instaladas!"
echo ""
echo "Agora você pode gerar o ISO com:"
echo "  make iso-grub"
