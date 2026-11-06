/* nsWidgetFactory.cpp - construtores do backend do SODOS. */
#include "nsWidgetFactory.h"

#include "HeadlessClipboard.h"
#include "mozilla/Components.h"
#include "mozilla/ModuleUtils.h"
#include "mozilla/WidgetUtils.h"
#include "nsAppShell.h"
#include "nsAppShellSingleton.h"
#include "nsISupports.h"
#include "nsLookAndFeel.h"

using namespace mozilla;
using namespace mozilla::widget;

NS_IMPL_COMPONENT_FACTORY(nsIClipboard) {
  nsCOMPtr<nsIClipboard> inst = new HeadlessClipboard();
  return inst.forget();
}

void nsWidgetSodosModuleCtor() { nsAppShellInit(); }

void nsWidgetSodosModuleDtor() {
  WidgetUtils::Shutdown();
  nsLookAndFeel::Shutdown();
  nsAppShellShutdown();
}
