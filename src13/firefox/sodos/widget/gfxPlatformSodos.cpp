/* gfxPlatformSodos.cpp - plataforma grafica do SODOS. */
#include "gfxPlatformSodos.h"

#include "SoftwareVsyncSource.h"
#include "ft2build.h"
#include "gfxFT2FontList.h"
#include "gfxImageSurface.h"
#include "mozilla/gfx/2D.h"
#include FT_FREETYPE_H

using namespace mozilla;
using namespace mozilla::gfx;

static FT_Library gPlatformFTLibrary = nullptr;

gfxPlatformSodos::gfxPlatformSodos() {
  FT_Init_FreeType(&gPlatformFTLibrary);
  Factory::SetFTLibrary(gPlatformFTLibrary);
}

gfxPlatformSodos::~gfxPlatformSodos() {
  FT_Done_FreeType(gPlatformFTLibrary);
  gPlatformFTLibrary = nullptr;
}

already_AddRefed<gfxASurface> gfxPlatformSodos::CreateOffscreenSurface(
    const IntSize& aSize, gfxImageFormat aFormat) {
  if (!Factory::AllowedSurfaceSize(aSize)) {
    return nullptr;
  }
  RefPtr<gfxASurface> surface = new gfxImageSurface(aSize, aFormat);
  return surface.forget();
}

bool gfxPlatformSodos::CreatePlatformFontList() {
  return gfxPlatformFontList::Initialize(new gfxFT2FontList);
}

void gfxPlatformSodos::ReadSystemFontList(
    mozilla::dom::SystemFontList* aFontList) {
  gfxFT2FontList::PlatformFontList()->ReadSystemFontList(aFontList);
}

void gfxPlatformSodos::GetCommonFallbackFonts(
    uint32_t aCh, Script aRunScript, FontPresentation aPresentation,
    nsTArray<const char*>& aFontList) {
  // Familias livres que o SODOS instala em fonts/.
  aFontList.AppendElement("Roboto");
  aFontList.AppendElement("Inter");
}

already_AddRefed<mozilla::gfx::VsyncSource>
gfxPlatformSodos::CreateGlobalHardwareVsyncSource() {
  // Sem vsync de hardware: um temporizador a 60 Hz.
  RefPtr<VsyncSource> source =
      new SoftwareVsyncSource(TimeDuration::FromMilliseconds(1000.0 / 60.0));
  return source.forget();
}
