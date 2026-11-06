/* nsLookAndFeel.cpp - aparencia padrao do SODOS. */
#include "nsLookAndFeel.h"

#include "gfxFont.h"
#include "gfxFontConstants.h"
#include "mozilla/FontPropertyTypes.h"
#include "nsStyleConsts.h"

using namespace mozilla;

nsLookAndFeel::nsLookAndFeel() = default;
nsLookAndFeel::~nsLookAndFeel() = default;

nsresult nsLookAndFeel::NativeGetColor(ColorID aID, ColorScheme aColorScheme,
                                       nscolor& aResult) {
  aResult = GetStandinForNativeColor(aID, aColorScheme);
  return NS_OK;
}

nsresult nsLookAndFeel::NativeGetInt(IntID aID, int32_t& aResult) {
  switch (aID) {
    case IntID::ScrollButtonLeftMouseButtonAction:
      aResult = 0;
      return NS_OK;
    case IntID::ScrollButtonMiddleMouseButtonAction:
    case IntID::ScrollButtonRightMouseButtonAction:
      aResult = 3;
      return NS_OK;
    case IntID::CaretBlinkTime:
      aResult = 567;
      return NS_OK;
    case IntID::CaretWidth:
      aResult = 1;
      return NS_OK;
    case IntID::SelectTextfieldsOnKeyFocus:
      aResult = 1;
      return NS_OK;
    case IntID::SubmenuDelay:
      aResult = 200;
      return NS_OK;
    case IntID::SkipNavigatingDisabledMenuItem:
      aResult = 1;
      return NS_OK;
    case IntID::DragThresholdX:
    case IntID::DragThresholdY:
      aResult = 4;
      return NS_OK;
    case IntID::ScrollArrowStyle:
      aResult = eScrollArrow_StartBackward | eScrollArrow_EndForward;
      return NS_OK;
    case IntID::TreeOpenDelay:
    case IntID::TreeCloseDelay:
      aResult = 1000;
      return NS_OK;
    case IntID::TreeLazyScrollDelay:
      aResult = 150;
      return NS_OK;
    case IntID::TreeScrollDelay:
      aResult = 100;
      return NS_OK;
    case IntID::TreeScrollLinesMax:
      aResult = 3;
      return NS_OK;
    case IntID::IMERawInputUnderlineStyle:
    case IntID::IMEConvertedTextUnderlineStyle:
    case IntID::IMESelectedRawTextUnderlineStyle:
    case IntID::IMESelectedConvertedTextUnderline:
      aResult = static_cast<int32_t>(StyleTextDecorationStyle::Solid);
      return NS_OK;
    case IntID::SpellCheckerUnderlineStyle:
      aResult = static_cast<int32_t>(StyleTextDecorationStyle::Dotted);
      return NS_OK;
    case IntID::ContextMenuOffsetVertical:
    case IntID::ContextMenuOffsetHorizontal:
      aResult = 2;
      return NS_OK;
    default:
      aResult = 0;
      return NS_ERROR_FAILURE;
  }
}

nsresult nsLookAndFeel::NativeGetFloat(FloatID aID, float& aResult) {
  switch (aID) {
    case FloatID::IMEUnderlineRelativeSize:
    case FloatID::SpellCheckerUnderlineRelativeSize:
      aResult = 1.0f;
      return NS_OK;
    default:
      aResult = -1.0f;
      return NS_ERROR_FAILURE;
  }
}

bool nsLookAndFeel::NativeGetFont(FontID aID, nsString& aFontName,
                                  gfxFontStyle& aFontStyle) {
  aFontName.AssignLiteral("sans-serif");
  aFontStyle.style = FontSlantStyle::NORMAL;
  aFontStyle.weight = FontWeight::NORMAL;
  aFontStyle.stretch = FontStretch::NORMAL;
  aFontStyle.size = 14;
  aFontStyle.systemFont = true;
  return true;
}
