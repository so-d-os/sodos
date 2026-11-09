# Dispositivos do qemu-system-i386 no SODOS (com --without-default-devices).
# O configure-qemu.sh copia este arquivo para
# vendor/qemu-*/configs/devices/i386-softmmu/sodos.mak (o QEMU so procura la).
#
# isapc: PC sem PCI de verdade; o Kconfig puxa i8259, PIT, RTC, teclado,
# serial 16550 e IDE ISA. Ele depende do I440FX (mesmo arquivo pc_piix.c).
CONFIG_ISAPC=y
CONFIG_I440FX=y
# isa-debug-exit: o guest de teste sai do QEMU com um outb.
CONFIG_ISA_DEBUG=y
CONFIG_TEST_DEVICES=y
# VGA ISA para o Q4 (tela); no Q3 roda com -display none.
CONFIG_VGA_ISA=y
