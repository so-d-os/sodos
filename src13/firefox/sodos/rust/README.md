# Rust no SODOS

O `rustc` já produz código que roda no SODOS via `link64` — mas ainda com o
alvo embutido **`x86_64-unknown-none`**, não `x86_64-sodos`.

```
RUST64: rust_add(20,22)=42
```

## Como reproduzir

```sh
sh sodos/scripts/fetch-rust.sh
rustup target add x86_64-unknown-none
make -C src13/firefox rust64
# copie build/rust/RUST64.EXE para o D: e rode no QEMU
```

O objeto Rust (`--crate-type=lib --emit=obj`) é ligado pelo `link64` com o
`crt0_64libc` e a LIBCX64.

## Target customizado `x86_64-sodos`

`sodos/rust/custom/` tem:

- `x86_64-sodos.json` (LP64, SSE2, panic=abort, sem PIC);
- `Cargo.toml`/`lib.rs` `#![no_std]`;
- `.cargo/config.toml` com `build-std = ["core", "compiler_builtins"]`.

Com nightly + `rust-src`:

```sh
rustup toolchain install nightly --profile minimal --component rust-src
make -C src13/firefox rust64-custom
```

O cargo compila `core`, `alloc` e `compiler_builtins` para `x86_64-sodos`.
O staticlib inteiro é combinado com `ld -r --whole-archive` e ligado pelo
`link64` (que agora aceita `R_X86_64_PC64`); o teste usa `Vec` com um
`GlobalAlloc` sobre `malloc/free` da LIBCX64:

```
RUST64: add=42 vec=10
```

O `.llvm.*`/`bcmp` que apareciam no link foram resolvidos (`bcmp` na libc e
`ld -r` para os símbolos internos do `compiler_builtins`).

O próximo passo é `std` (syscalls/pthreads/fs do SODOS) e o
`rust.configure` usando o target JSON; hoje ele ainda para em
`Don't know how to translate x86_64-pc-sodos for rustc`.

Tentar `-Z build-std=std,panic_abort` para o target JSON falha nos `cfg` do
`std` (`none of the predicates`, `no_threads`, `sys::io`): o PAL
`unsupported` do Rust ainda não conhece `target_os = "sodos"`.

## `std` no SODOS

O `std` já compila e roda com o PAL `unsupported` depois de adicionar `sodos`
aos `cfg` de `sys/alloc`, `sys/io/error`, `sys/random`, `sys/thread_local`
(incluindo uma `key` de TLS single-thread) e `sys/pal`. O teste
`sodos/rust/stdtest` compila com `std` e imprime na serial:

```
STD64: std main ok
```

(`println!` ainda não tem stdout; o `k_dbg` é usado para confirmar o `main`.)

Wrappers em `sodos/rust/tools/` (`cargo`/`rustc`) mapeiam
`x86_64-sodos` para o JSON e usam `build-std=std,panic_abort`. O `gmake` do
SpiderMonkey compila todo o C++/ICU; o cargo ainda bate em crates de outros
alvos (`hermit-abi`, `wasip*`, `dlmalloc`, `r-efi`) cujos checksums/versões
não batem com o vendor do Gecko. O `x86_64-sodos` já tem `std` próprio; falta
acertar esse vendor.

## O que falta para o Firefox

O `configure` do Firefox para em:

```
ERROR: Don't know how to translate x86_64-pc-sodos for rustc
```

O `find_candidate` do `build/moz.configure/rust.configure` só aceita alvos
que aparecem em `rustc --print target-list`. Para `x86_64-sodos` seria
necessário:

1. um **target JSON** (`x86_64-sodos.json`) com `arch=x86_64`, `os=sodos`,
   `panic-strategy=abort`, `linker` apontando para o toolchain SODOS e SSE2;
2. **`core`/`alloc`/`std`** para esse alvo (`-Z build-std`, nightly +
   `rust-src`), incluindo a camada de syscalls/pthreads do SODOS;
3. ensinar o `rust.configure` a usar o JSON/triple (hoje ele traduz o
   target do configure para um alvo Rust da lista embutida).

Sem `std`, o NSPR/libc++/Gecko não compilam em Rust; o `no_std` acima é só o
primeiro tijolo.
