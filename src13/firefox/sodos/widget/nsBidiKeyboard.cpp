/* nsBidiKeyboard.cpp - o SODOS nao tem teclado RTL. */
#include "nsBidiKeyboard.h"

#include "nsIWidget.h"

NS_IMPL_ISUPPORTS(nsBidiKeyboard, nsIBidiKeyboard)

nsBidiKeyboard::nsBidiKeyboard() = default;
nsBidiKeyboard::~nsBidiKeyboard() = default;

NS_IMETHODIMP nsBidiKeyboard::Reset() { return NS_OK; }

NS_IMETHODIMP nsBidiKeyboard::IsLangRTL(bool* aIsRTL) {
  *aIsRTL = false;
  return NS_OK;
}

NS_IMETHODIMP nsBidiKeyboard::GetHaveBidiKeyboards(bool* aResult) {
  *aResult = false;
  return NS_OK;
}

// static
already_AddRefed<nsIBidiKeyboard> nsIWidget::CreateBidiKeyboardInner() {
  return do_AddRef(new nsBidiKeyboard());
}
