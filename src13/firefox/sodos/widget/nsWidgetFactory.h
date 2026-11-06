/* nsWidgetFactory.h - modulo XPCOM do backend do SODOS. */
#ifndef widget_sodos_nsWidgetFactory_h
#define widget_sodos_nsWidgetFactory_h

#include "nscore.h"
#include "nsID.h"

nsresult nsAppShellConstructor(const nsIID& iid, void** result);

void nsWidgetSodosModuleCtor();
void nsWidgetSodosModuleDtor();

#endif
