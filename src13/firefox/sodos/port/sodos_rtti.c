/* sodos_rtti.c - vtables RTTI minimas (libc++abi nao esta no vendor). */
__asm__(
  ".section .data\n"
  ".globl _ZTVN10__cxxabiv120__si_class_type_infoE\n"
  "_ZTVN10__cxxabiv120__si_class_type_infoE:\n .quad 0\n .quad 0\n"
  ".globl _ZTVN10__cxxabiv117__class_type_infoE\n"
  "_ZTVN10__cxxabiv117__class_type_infoE:\n .quad 0\n .quad 0\n"
  ".globl _ZTVN10__cxxabiv121__vmi_class_type_infoE\n"
  "_ZTVN10__cxxabiv121__vmi_class_type_infoE:\n .quad 0\n .quad 0\n"
  ".globl _ZTVN10__cxxabiv123__fundamental_type_infoE\n"
  "_ZTVN10__cxxabiv123__fundamental_type_infoE:\n .quad 0\n .quad 0\n"
  ".globl _ZTVN10__cxxabiv119__pointer_type_infoE\n"
  "_ZTVN10__cxxabiv119__pointer_type_infoE:\n .quad 0\n .quad 0\n"
  ".globl _ZTVN10__cxxabiv129__pointer_to_member_type_infoE\n"
  "_ZTVN10__cxxabiv129__pointer_to_member_type_infoE:\n .quad 0\n .quad 0\n"
);
