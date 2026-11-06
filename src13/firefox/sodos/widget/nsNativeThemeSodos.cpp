/* nsNativeThemeSodos.cpp - o tema nao nativo do Gecko desenha todos os controles. */
#include "Theme.h"

using mozilla::widget::Theme;

already_AddRefed<Theme> do_CreateNativeThemeDoNotUseDirectly() {
  return do_AddRef(new Theme(Theme::ScrollbarStyle()));
}
