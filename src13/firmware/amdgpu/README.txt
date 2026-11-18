Firmware da AMD usado pelos drivers de placa do SODOS (docs/GPU.md).

polaris10_k_smc.bin
  Firmware do SMU (SMC) da Polaris 10 "kicker": RX 470/480/570/580
  PCI 1002:67DF revisoes E3, E4, E5, E7 e EF (ASICID_IS_P20 no amdgpu do
  Linux). O AMDSMC.DLL carrega esta imagem no SMC (etapa C2 da RX 570).
  Origem: linux-firmware, amdgpu/polaris10_k_smc.bin
    https://git.kernel.org/pub/scm/linux/kernel/git/firmware/linux-firmware.git
    (conferido igual ao espelho gitlab.com/kernel-firmware/linux-firmware)
  Baixado em 2026-11-17, 130.244 bytes
  sha256 98fa36d74fa933a73419257daae924fda290a7208aa42584a9dd89e1cc44e05d
  Cabecalho: smc_firmware_header_v1_0, ucode_version 0x00171100,
  ucode de 0x1FBC4 bytes em 0x100, carregado a partir de 0x20000.

No disco do SODOS fica em \SODOS\FW\P10KSMC.BIN (nome 8.3), copiado pelo
"make disk-files"/"make sendpc".

polaris10_sdma.bin
  Microcodigo do motor de copia SDMA da Polaris 10 (sdma_firmware_header
  v1.1, ucode_version 0x3A, ucode de 0x3094 bytes em 0x100). O AMDSDMA.DLL
  poe o ucode na VRAM e pede ao SMU que o carregue na SDMA0 (etapa E).
  Origem: linux-firmware, amdgpu/polaris10_sdma.bin (igual nos dois
  servidores acima). Baixado em 2026-11-18, 12.692 bytes
  sha256 e27b3ec976f87e32883b9190e653b20d27f9e77da8dd2d77eeb10c35ee9f1e61
  No disco: \SODOS\FW\P10SDMA.BIN.

Licenca: LICENSE.amdgpu (redistribuivel em forma binaria, sem modificar),
a mesma do linux-firmware (LICENSES/LICENSE.amdgpu).
