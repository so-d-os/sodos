/* SodosWindow.h - a janela do Gecko no desktop (WIN) do SODOS.
 *
 * O SODOS da uma janela canvas por processo (k_gui_*). O primeiro
 * HeadlessWidget de topo que aparece vira essa janela: o compositor (WebRender
 * por software) desenha num buffer e o resultado vai para a tela com
 * k_gui_pixels; rato e teclado do WIN voltam como eventos do widget. Os demais
 * widgets (menus, dialogos) continuam so em memoria.
 */
#ifndef widget_sodos_SodosWindow_h
#define widget_sodos_SodosWindow_h

#include "Units.h"
#include "mozilla/AlreadyAddRefed.h"

class nsIWidget;

namespace mozilla {
namespace gfx {
class DrawTarget;
}
namespace widget {
namespace sodos {

/* Abre a janela do WIN. false: modo headless ou programa fora do WIN. */
bool Open();
bool IsOpen();
LayoutDeviceIntSize Size();

/* Avisos do HeadlessWidget. */
void WindowShown(nsIWidget* aWidget, bool aVisible, bool aTopLevel);
void WindowDestroyed(nsIWidget* aWidget);

/* Do compositor (pelo HeadlessWidget): buffer BGRX do tamanho da janela, ou nullptr se o widget nao
 * e o da janela do SODOS. */
already_AddRefed<gfx::DrawTarget> StartDrawing(nsIWidget* aWidget);
void EndDrawing(nsIWidget* aWidget, const LayoutDeviceIntRegion& aRegion);

/* Do laco de eventos, na thread principal: le o WIN e entrega ao Gecko. */
void PumpEvents();

}  // namespace sodos
}  // namespace widget
}  // namespace mozilla

#endif
