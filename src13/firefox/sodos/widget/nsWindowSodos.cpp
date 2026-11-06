/* nsWindowSodos.cpp - toda janela e um HeadlessWidget, por enquanto. */
#include "nsIWidget.h"

already_AddRefed<nsIWidget> nsIWidget::CreateTopLevelWindow() {
  return CreateHeadlessWidget();
}

already_AddRefed<nsIWidget> nsIWidget::CreateChildWindow() {
  return CreateHeadlessWidget();
}
