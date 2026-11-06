/* gfxPlatformSodos.h - plataforma grafica do SODOS: software, fontes via FreeType. */
#ifndef GFX_PLATFORM_SODOS_H
#define GFX_PLATFORM_SODOS_H

#include "gfxPlatform.h"

class gfxPlatformSodos final : public gfxPlatform {
 public:
  gfxPlatformSodos();
  virtual ~gfxPlatformSodos();

  static gfxPlatformSodos* GetPlatform() {
    return (gfxPlatformSodos*)gfxPlatform::GetPlatform();
  }

  already_AddRefed<gfxASurface> CreateOffscreenSurface(
      const IntSize& aSize, gfxImageFormat aFormat) override;

  gfxImageFormat GetOffscreenFormat() override {
    return mozilla::gfx::SurfaceFormat::X8R8G8B8_UINT32;
  }

  bool CreatePlatformFontList() override;
  void ReadSystemFontList(mozilla::dom::SystemFontList*) override;
  void GetCommonFallbackFonts(uint32_t aCh, Script aRunScript,
                              FontPresentation aPresentation,
                              nsTArray<const char*>& aFontList) override;

  already_AddRefed<mozilla::gfx::VsyncSource> CreateGlobalHardwareVsyncSource()
      override;

  static bool CheckVariationFontSupport() { return true; }
};

#endif
