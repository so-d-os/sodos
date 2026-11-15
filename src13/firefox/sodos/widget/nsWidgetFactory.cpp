/* nsWidgetFactory.cpp - construtores do backend do SODOS. */
#include "nsWidgetFactory.h"

// SodosClipboard.cpp entra aqui: um .cpp novo no moz.build so compila depois
// de reconfigurar o objdir (o backend.mk nao se refaz sozinho).
#include "SodosClipboard.cpp"
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
  nsCOMPtr<nsIClipboard> inst = new SodosClipboard();
  return inst.forget();
}

void nsWidgetSodosModuleCtor() { nsAppShellInit(); }

void nsWidgetSodosModuleDtor() {
  WidgetUtils::Shutdown();
  nsLookAndFeel::Shutdown();
  nsAppShellShutdown();
}
