/* dwtest.c - DirectWrite do WINE.DLL: fabrica, colecao do sistema, nome do
 * Windows mapeado (Segoe UI), face, metricas, glifos, tabela OpenType e a
 * palavra "SODOS" rasterizada (impressa em ASCII). */
#define COBJMACROS
#include <windows.h>
#include <initguid.h>
#include <dwrite.h>
#include <stdio.h>

static int ok = 1;
static void check(int c, const char* what) {
  printf("%s %s\n", c ? "ok  " : "FALHOU", what);
  if (!c) ok = 0;
}

int main(void) {
  IDWriteFactory* f = NULL;
  IDWriteFontCollection* col = NULL;
  IDWriteFontFamily* fam = NULL;
  IDWriteFont* font = NULL;
  IDWriteFontFace* face = NULL;
  DWRITE_FONT_METRICS fm;
  UINT32 idx = 0, i, cps[5] = {'S', 'O', 'D', 'O', 'S'};
  UINT16 gl[5];
  BOOL exists = FALSE;
  HRESULT hr;
  setvbuf(stdout, NULL, _IONBF, 0);

  hr = DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, &IID_IDWriteFactory, (IUnknown**)&f);
  check(SUCCEEDED(hr) && f, "DWriteCreateFactory");
  if (!f) return 1;
  hr = IDWriteFactory_GetSystemFontCollection(f, &col, FALSE);
  check(SUCCEEDED(hr) && col, "GetSystemFontCollection");
  printf("familias: %u\n", IDWriteFontCollection_GetFontFamilyCount(col));
  for (i = 0; i < IDWriteFontCollection_GetFontFamilyCount(col); i++) {
    IDWriteFontFamily* ff;
    IDWriteLocalizedStrings* names;
    WCHAR buf[64];
    if (FAILED(IDWriteFontCollection_GetFontFamily(col, i, &ff))) continue;
    if (SUCCEEDED(IDWriteFontFamily_GetFamilyNames(ff, &names))) {
      IDWriteLocalizedStrings_GetString(names, 0, buf, 64);
      printf("  %ls (%u fontes)\n", buf, IDWriteFontFamily_GetFontCount(ff));
      IDWriteLocalizedStrings_Release(names);
    }
    IDWriteFontFamily_Release(ff);
  }
  hr = IDWriteFontCollection_FindFamilyName(col, L"Segoe UI", &idx, &exists);
  check(SUCCEEDED(hr) && exists, "FindFamilyName(Segoe UI) -> familia do SODOS");
  IDWriteFontCollection_GetFontFamily(col, idx, &fam);
  hr = IDWriteFontFamily_GetFirstMatchingFont(fam, DWRITE_FONT_WEIGHT_BOLD, DWRITE_FONT_STRETCH_NORMAL,
                                              DWRITE_FONT_STYLE_NORMAL, &font);
  check(SUCCEEDED(hr) && font, "GetFirstMatchingFont(bold)");
  printf("peso escolhido: %d\n", IDWriteFont_GetWeight(font));
  hr = IDWriteFont_CreateFontFace(font, &face);
  check(SUCCEEDED(hr) && face, "CreateFontFace");
  IDWriteFontFace_GetMetrics(face, &fm);
  printf("upem %u asc %u desc %u capH %u\n", fm.designUnitsPerEm, fm.ascent, fm.descent, fm.capHeight);
  check(fm.designUnitsPerEm > 0 && fm.ascent > 0, "GetMetrics");
  IDWriteFontFace_GetGlyphIndices(face, cps, 5, gl);
  check(gl[0] && gl[1] && gl[0] == gl[4], "GetGlyphIndices");
  {
    const void* data;
    UINT32 size;
    void* ctx;
    BOOL ex;
    IDWriteFontFace_TryGetFontTable(face, DWRITE_MAKE_OPENTYPE_TAG('c', 'm', 'a', 'p'), &data, &size, &ctx, &ex);
    check(ex && size > 0, "TryGetFontTable(cmap)");
  }
  {
    DWRITE_GLYPH_METRICS gm[5];
    IDWriteFontFace_GetDesignGlyphMetrics(face, gl, 5, gm, FALSE);
    check(gm[0].advanceWidth > 0, "GetDesignGlyphMetrics");
  }
  {
    DWRITE_GLYPH_RUN run;
    IDWriteGlyphRunAnalysis* a = NULL;
    RECT rc;
    BYTE* tex;
    int x, y, w, h;
    memset(&run, 0, sizeof(run));
    run.fontFace = face;
    run.fontEmSize = 20.0f;
    run.glyphCount = 5;
    run.glyphIndices = gl;
    hr = IDWriteFactory_CreateGlyphRunAnalysis(f, &run, 1.0f, NULL, DWRITE_RENDERING_MODE_NATURAL,
                                               DWRITE_MEASURING_MODE_NATURAL, 0.0f, 0.0f, &a);
    check(SUCCEEDED(hr) && a, "CreateGlyphRunAnalysis");
    IDWriteGlyphRunAnalysis_GetAlphaTextureBounds(a, DWRITE_TEXTURE_CLEARTYPE_3x1, &rc);
    w = rc.right - rc.left;
    h = rc.bottom - rc.top;
    printf("textura %d x %d em (%ld,%ld)\n", w, h, rc.left, rc.top);
    check(w > 20 && h > 8, "GetAlphaTextureBounds");
    tex = (BYTE*)malloc((size_t)(w * h * 3));
    IDWriteGlyphRunAnalysis_CreateAlphaTexture(a, DWRITE_TEXTURE_CLEARTYPE_3x1, &rc, tex, (UINT32)(w * h * 3));
    for (y = 0; y < h; y++) {
      for (x = 0; x < w; x++) {
        int v = tex[(y * w + x) * 3];
        putchar(v > 170 ? '#' : v > 85 ? '+' : v > 20 ? '.' : ' ');
      }
      putchar('\n');
    }
    free(tex);
    IDWriteGlyphRunAnalysis_Release(a);
  }
  IDWriteFontFace_Release(face);
  IDWriteFont_Release(font);
  IDWriteFontFamily_Release(fam);
  printf("%s\n", ok ? "DWTEST OK" : "DWTEST FALHOU");
  return ok ? 0 : 1;
}
