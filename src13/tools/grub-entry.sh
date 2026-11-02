#!/bin/sh
# grub-entry.sh - poe (ou tira) o SODOS no menu do GRUB desta maquina Linux.
#
#   sudo tools/grub-entry.sh install   # copia kernel_multiboot.elf e cria a entrada
#   sudo tools/grub-entry.sh remove    # apaga a entrada e o kernel copiado
#
# Rode de dentro de src13/ depois de "make kernel_multiboot.elf",
# ou use "make grub", que recompila e chama este script se preciso.
# A entrada padrao do GRUB nao muda: o SODOS so sobe se for escolhido no menu.
# Atalho F6: o tema OEM da Positivo esconde a lista do menu e so mostra as
# teclas F5/F7, entao a entrada e escolhida pelo atalho, nao pelas setas.
set -e

DEST=/boot/sodos
ENTRY=/etc/grub.d/41_sodos

[ "$(id -u)" = 0 ] || { echo "Rode com sudo."; exit 1; }

case "$1" in
  install)
    [ -f kernel_multiboot.elf ] || { echo "kernel_multiboot.elf nao encontrado (rode em src13/)."; exit 1; }
    mkdir -p "$DEST"
    cp kernel_multiboot.elf "$DEST/kernel.elf"
    cat > "$ENTRY" <<'EOF'
#!/bin/sh
exec tail -n +3 $0
menuentry 'SODOS (experimental)' --hotkey=f6 {
    insmod all_video
    insmod multiboot2
    search --no-floppy --set=root --file /boot/sodos/kernel.elf
    multiboot2 /boot/sodos/kernel.elf
    boot
}
EOF
    chmod +x "$ENTRY"
    ;;
  remove)
    rm -f "$ENTRY"
    rm -rf "$DEST"
    ;;
  *)
    echo "uso: sudo $0 install|remove"; exit 1 ;;
esac

update-grub
echo "Pronto: $1"
