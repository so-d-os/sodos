/* SodosWindow.cpp - janela, pintura e entrada do Gecko sobre k_gui_*. */
#include "SodosWindow.h"

#include "mozilla/MouseEvents.h"
#include "mozilla/TextEvents.h"
#include "mozilla/UniquePtr.h"
#include "mozilla/gfx/2D.h"
#include "nsIWidget.h"
#include "nsIWidgetListener.h"
#include "nsIRollupListener.h"
#include "nsCOMPtr.h"
#include "mozilla/widget/InitData.h"
#include "nsThreadUtils.h"
#include "prenv.h"
#include <stdio.h>

extern "C" {
int k_gui_open(const char* title);
void k_gui_size(int* w, int* h);
void k_gui_pixels(int x, int y, int w, int h, const void* rgba, int stride);
void k_gui_present(void);
int k_gui_event(int* ev, int timeout_ms);
}

/* gui.h do SODOS */
#define GUI_EV_DOWN 1
#define GUI_EV_KEY 2
#define GUI_EV_CLOSE 3
#define GUI_EV_UP 4
#define GUI_EV_DRAG 5
#define GUI_EV_WHEEL 6
#define GUI_KEY_UP 0x101
#define GUI_KEY_DOWN 0x102
#define GUI_KEY_LEFT 0x103
#define GUI_KEY_RIGHT 0x104
#define GUI_KEY_PGUP 0x105
#define GUI_KEY_PGDN 0x106
#define GUI_KEY_HOME 0x107
#define GUI_KEY_END 0x108
#define GUI_KEY_DEL 0x109

using namespace mozilla::gfx;
using mozilla::widget::WindowType;

namespace mozilla {
namespace widget {
namespace sodos {

static bool sOpen;
static int sWidth, sHeight;
static nsIWidget* sMain;  // a primeira janela de topo; nao segura referencia
static UniquePtr<uint8_t[]> sRow;  // faixa RGBA mandada ao WIN
static int sRowW;
static LayoutDeviceIntPoint sMouse;
static bool sDirty;  // ha pixels novos esperando o k_gui_present

/* Camadas: o WIN da uma so janela por processo, entao cada widget do Gecko
 * que aparece (janela principal, outra janela de topo, dialogo, menu, lista
 * do <select>, dica) desenha no proprio buffer e a tela e a composicao delas,
 * de baixo para cima: janelas de topo pela ordem de ativacao, popups por
 * cima. As coordenadas de tela do Gecko sao as do canvas do WIN (a janela
 * principal fica em 0,0). */
struct Layer {
  nsIWidget* w;  // sem referencia: WindowDestroyed tira daqui
  UniquePtr<uint8_t[]> buf;  // BGRA (popups e dialogos) ou BGRX (topo)
  IntSize size;
  bool shown;
  bool painted;
  bool popup;
  uint32_t seq;  // ordem de empilhamento
};
static const int kMaxLayers = 24;
static Layer sLayers[kMaxLayers];
static uint32_t sSeq;
static nsIWidget* sCapture;  // quem recebeu o botao (arrasto e soltura)

// Marcos na serial (COM1): poucos, para acompanhar a partida da janela.
static void Mark(const char* aText, long aA = 0, long aB = 0) {
  fprintf(stderr, "[sodos-win] %s %ld %ld\n", aText, aA, aB);
}

bool Open() {
  if (sOpen) {
    return true;
  }
  if (PR_GetEnv("MOZ_HEADLESS") || k_gui_open("Firefox") != 0) {
    return false;
  }
  k_gui_size(&sWidth, &sHeight);
  if (sWidth < 64 || sHeight < 64) {
    return false;
  }
  sOpen = true;
  Mark("janela aberta", sWidth, sHeight);
  return true;
}

bool IsOpen() { return sOpen; }

LayoutDeviceIntSize Size() { return LayoutDeviceIntSize(sWidth, sHeight); }

static Layer* Find(nsIWidget* aWidget) {
  for (Layer& l : sLayers) {
    if (l.w && l.w == aWidget) {
      return &l;
    }
  }
  return nullptr;
}

static Layer* Add(nsIWidget* aWidget) {
  if (Layer* l = Find(aWidget)) {
    return l;
  }
  for (Layer& l : sLayers) {
    if (!l.w) {
      l.w = aWidget;
      l.shown = false;
      l.painted = false;
      l.popup = aWidget->GetWindowType() == WindowType::Popup;
      l.seq = ++sSeq;
      return &l;
    }
  }
  return nullptr;
}

/* Janelas de topo (nao dialogos) ocupam o canvas todo. */
static bool FullWindow(nsIWidget* aWidget) {
  return aWidget->GetWindowType() == WindowType::TopLevel;
}

/* Retangulo da camada na tela. */
static IntRect LayerRect(const Layer& l) {
  if (FullWindow(l.w)) {
    return IntRect(0, 0, sWidth, sHeight);
  }
  LayoutDeviceIntRect b = l.w->GetBounds();
  return IntRect(b.x, b.y, l.size.width, l.size.height);
}

/* Camadas visiveis de baixo para cima. */
static int Stack(Layer** aOut) {
  int n = 0;
  for (Layer& l : sLayers) {
    if (l.w && l.shown && l.painted && l.buf) {
      aOut[n++] = &l;
    }
  }
  // poucas camadas: insercao. Popups sempre acima das janelas.
  for (int i = 1; i < n; i++) {
    Layer* x = aOut[i];
    int j = i - 1;
    while (j >= 0 && (aOut[j]->popup > x->popup ||
                      (aOut[j]->popup == x->popup && aOut[j]->seq > x->seq))) {
      aOut[j + 1] = aOut[j];
      j--;
    }
    aOut[j + 1] = x;
  }
  return n;
}

/* Janela de topo (ou dialogo) mais alta: recebe teclado e o fechar do WIN. */
static nsIWidget* Top() {
  Layer* best = nullptr;
  for (Layer& l : sLayers) {
    if (l.w && l.shown && !l.popup && (!best || l.seq > best->seq)) {
      best = &l;
    }
  }
  return best ? best->w : sMain;
}

/* Recompoe um retangulo da tela e manda ao WIN. */
static void Compose(IntRect aRect) {
  aRect = aRect.Intersect(IntRect(0, 0, sWidth, sHeight));
  if (aRect.IsEmpty()) {
    return;
  }
  Layer* st[kMaxLayers];
  int n = Stack(st);
  if (!sRow || sRowW < sWidth) {
    sRow = MakeUnique<uint8_t[]>(size_t(sWidth) * 64 * 4);
    sRowW = sWidth;
  }
  for (int y = aRect.y; y < aRect.YMost(); y += 64) {
    int rows = std::min(64, aRect.YMost() - y);
    IntRect band(aRect.x, y, aRect.width, rows);
    // fundo cinza onde nenhuma camada cobre
    for (int j = 0; j < rows; j++) {
      uint32_t* d = (uint32_t*)(sRow.get() + size_t(j) * aRect.width * 4);
      for (int i = 0; i < aRect.width; i++) {
        d[i] = 0xff606060u;
      }
    }
    for (int k = 0; k < n; k++) {
      Layer* l = st[k];
      IntRect lr = LayerRect(*l);
      IntRect r = band.Intersect(lr).Intersect(
          IntRect(lr.x, lr.y, l->size.width, l->size.height));
      if (r.IsEmpty()) {
        continue;
      }
      bool blend = l->popup || !FullWindow(l->w);
      for (int j = r.y; j < r.YMost(); j++) {
        const uint8_t* s =
            l->buf.get() +
            (size_t(j - lr.y) * l->size.width + (r.x - lr.x)) * 4;
        uint8_t* d =
            sRow.get() + (size_t(j - y) * aRect.width + (r.x - aRect.x)) * 4;
        for (int i = 0; i < r.width; i++, s += 4, d += 4) {
          if (!blend || s[3] == 255) {
            d[0] = s[2];
            d[1] = s[1];
            d[2] = s[0];
          } else if (s[3]) {
            // BGRA pre-multiplicado sobre o que ja esta na faixa
            unsigned ia = 255 - s[3];
            d[0] = uint8_t(s[2] + (d[0] * ia + 127) / 255);
            d[1] = uint8_t(s[1] + (d[1] * ia + 127) / 255);
            d[2] = uint8_t(s[0] + (d[2] * ia + 127) / 255);
          }
          d[3] = 255;
        }
      }
    }
    k_gui_pixels(aRect.x, y, aRect.width, rows, sRow.get(), aRect.width * 4);
  }
  sDirty = true;
}

static void FitMain() {
  for (Layer& l : sLayers) {
    if (l.w && l.shown && FullWindow(l.w) &&
        !l.w->GetBounds().IsEqualEdges(LayoutDeviceIntRect(0, 0, sWidth, sHeight))) {
      l.w->Resize(DesktopRect(0, 0, sWidth, sHeight), true);
    }
  }
}

/* Dialogo fora da tela (o Gecko centraliza pela tela que acha que tem): traz
 * para dentro do canvas. */
static void KeepInside(nsIWidget* aWidget) {
  LayoutDeviceIntRect b = aWidget->GetBounds();
  int x = std::max(0, std::min(b.x, sWidth - b.width));
  int y = std::max(0, std::min(b.y, sHeight - b.height));
  if (x != b.x || y != b.y) {
    aWidget->Move(DesktopPoint(x, y));
  }
}

void WindowShown(nsIWidget* aWidget, bool aVisible, bool aTopLevel) {
  (void)aTopLevel;
  if (!sOpen || aWidget->GetWindowType() == WindowType::Invisible) {
    return;
  }
  Layer* l = aVisible ? Add(aWidget) : Find(aWidget);
  if (!l) {
    return;
  }
  if (l->shown == aVisible) {
    return;
  }
  IntRect before = l->shown && l->painted ? LayerRect(*l) : IntRect();
  l->shown = aVisible;
  if (aVisible) {
    l->seq = ++sSeq;  // aparece por cima
    if (!sMain && aWidget->GetWindowType() == WindowType::TopLevel) {
      sMain = aWidget;
      Mark("widget principal");
    } else {
      Mark(l->popup ? "popup" : "outra janela", int(aWidget->GetWindowType()),
           aWidget->GetBounds().width);
    }
    if (FullWindow(aWidget)) {
      // Fora do Show(): o redimensionamento avisa o layout.
      NS_DispatchToMainThread(
          NS_NewRunnableFunction("sodos::FitMain", [] { FitMain(); }));
    } else if (!l->popup) {
      NS_DispatchToMainThread(NS_NewRunnableFunction(
          "sodos::KeepInside", [w = RefPtr<nsIWidget>(aWidget)] {
            if (Find(w) && w->IsVisible()) {
              KeepInside(w);
            }
          }));
    }
    if (l->painted) {
      Compose(LayerRect(*l));
    }
  } else {
    if (sCapture == aWidget) {
      sCapture = nullptr;
    }
    // o que estava embaixo ja esta nos buffers das outras camadas
    Compose(before);
  }
}

void WindowDestroyed(nsIWidget* aWidget) {
  Layer* l = Find(aWidget);
  if (sCapture == aWidget) {
    sCapture = nullptr;
  }
  if (l) {
    IntRect before = l->shown && l->painted ? LayerRect(*l) : IntRect();
    l->w = nullptr;
    l->buf = nullptr;
    l->shown = false;
    Compose(before);
  }
  if (sMain == aWidget) {
    sMain = nullptr;
    // outra janela de topo continua aberta: ela vira a principal
    for (Layer& o : sLayers) {
      if (o.w && o.shown && o.w->GetWindowType() == WindowType::TopLevel) {
        sMain = o.w;
        break;
      }
    }
  }
}

already_AddRefed<DrawTarget> StartDrawing(nsIWidget* aWidget) {
  if (!sOpen || aWidget->GetWindowType() == WindowType::Invisible) {
    return nullptr;
  }
  Layer* l = Add(aWidget);
  if (!l) {
    return nullptr;
  }
  IntSize size;
  if (FullWindow(aWidget)) {
    size = IntSize(sWidth, sHeight);
  } else {
    LayoutDeviceIntSize b = aWidget->GetBounds().Size();
    size = IntSize(std::max(1, std::min(b.width, sWidth)),
                   std::max(1, std::min(b.height, sHeight)));
  }
  if (!l->buf || l->size != size) {
    l->buf = MakeUnique<uint8_t[]>(size_t(size.width) * size.height * 4);
    l->size = size;
    l->painted = false;
  }
  bool full = FullWindow(aWidget);
  return Factory::CreateDrawTargetForData(
      BackendType::SKIA, l->buf.get(), size, size.width * 4,
      full ? SurfaceFormat::B8G8R8X8 : SurfaceFormat::B8G8R8A8);
}

void EndDrawing(nsIWidget* aWidget, const LayoutDeviceIntRegion& aRegion) {
  Layer* l = sOpen ? Find(aWidget) : nullptr;
  if (!l || !l->buf) {
    return;
  }
  static int sFrames;
  if (sFrames++ < 3) {
    Mark("quadro", aRegion.GetBounds().width, aRegion.GetBounds().height);
  }
  bool first = !l->painted;
  l->painted = true;
  if (!l->shown) {
    return;
  }
  IntRect lr = LayerRect(*l);
  if (first) {
    Compose(lr);
    return;
  }
  for (auto iter = aRegion.RectIter(); !iter.Done(); iter.Next()) {
    IntRect r = iter.Get().ToUnknownRect();
    Compose(IntRect(r.x + lr.x, r.y + lr.y, r.width, r.height));
  }
}

/* Popup visivel mais alto sob o ponto (coordenadas de tela). */
static nsIWidget* PopupAt(LayoutDeviceIntPoint aPt) {
  Layer* st[kMaxLayers];
  int n = Stack(st);
  for (int k = n - 1; k >= 0; k--) {
    if (st[k]->popup && LayerRect(*st[k]).Contains(aPt.x, aPt.y)) {
      return st[k]->w;
    }
  }
  return nullptr;
}

/* Janela (de topo ou dialogo) mais alta sob o ponto. */
static nsIWidget* WindowAt(LayoutDeviceIntPoint aPt) {
  Layer* st[kMaxLayers];
  int n = Stack(st);
  for (int k = n - 1; k >= 0; k--) {
    if (!st[k]->popup && LayerRect(*st[k]).Contains(aPt.x, aPt.y)) {
      return st[k]->w;
    }
  }
  return Top();
}

static bool InWidget(nsIWidget* aWidget, LayoutDeviceIntPoint aPt) {
  Layer* l = Find(aWidget);
  return l && l->shown && LayerRect(*l).Contains(aPt.x, aPt.y);
}

/* Clique (ou roda) fora do menu aberto fecha o menu, como o GTK faz em
 * CheckForRollup. true: o evento foi consumido pelo fechamento. */
static bool CheckRollup(LayoutDeviceIntPoint aPt, bool aWheel) {
  nsIRollupListener* rl = nsIWidget::GetActiveRollupListener();
  nsCOMPtr<nsIWidget> rw = rl ? rl->GetRollupWidget() : nullptr;
  if (!rw || InWidget(rw, aPt)) {
    return false;
  }
  bool consume = false;
  if (aWheel) {
    consume = rl->ShouldConsumeOnMouseWheelEvent();
    if (!rl->ShouldRollupOnMouseWheelEvent()) {
      return consume;
    }
  }
  nsIRollupListener::RollupOptions opts;
  AutoTArray<nsIWidget*, 5> chain;
  uint32_t same = rl->GetSubmenuWidgetChain(&chain);
  for (uint32_t i = 0; i < chain.Length(); i++) {
    if (InWidget(chain[i], aPt)) {
      if (i < same) {
        return consume;
      }
      opts.mCount = same;
      break;
    }
  }
  if (!aWheel) {
    opts.mPoint = &aPt;
  }
  if (rl->Rollup(opts)) {
    consume = true;
  }
  return consume;
}

/* Widget que recebe o evento de rato neste ponto. */
static nsIWidget* MouseTarget(LayoutDeviceIntPoint aPt) {
  if (nsIWidget* p = PopupAt(aPt)) {
    return p;
  }
  return WindowAt(aPt);
}

static void Mouse(nsIWidget* aWidget, nsIWidget::NativeMouseMessage aMsg,
                  int aButton) {
  if (!aWidget) {
    return;
  }
  nsCOMPtr<nsIWidget> grip(aWidget);
  aWidget->SynthesizeNativeMouseEvent(
      sMouse, aMsg, aButton == 2 ? MouseButton::eSecondary : MouseButton::ePrimary,
      nsIWidget::NativeModifiers::NO_MODIFIERS, nullptr);
}

/* Ativa (traz para cima) a janela de topo dona do widget. */
static void Raise(nsIWidget* aWidget) {
  if (!aWidget) {
    return;
  }
  nsIWidget* top = aWidget->GetWindowType() == WindowType::Popup
                       ? nullptr
                       : aWidget->GetTopLevelWidget();
  if (!top) {
    return;
  }
  if (Layer* l = Find(top)) {
    if (l->seq != sSeq) {
      l->seq = ++sSeq;
      if (l->painted && l->shown) {
        Compose(LayerRect(*l));
      }
    }
  }
  nsCOMPtr<nsIWidget> grip(top);
  top->SetFocus(nsIWidget::Raise::Yes, mozilla::dom::CallerType::System);
}

static void Key(int aKey) {
  uint32_t keyCode = 0;
  char16_t ch = 0;
  KeyNameIndex name = KEY_NAME_INDEX_USE_STRING;
  Modifiers mods = MODIFIER_NONE;
  switch (aKey) {
    case '\r':
    case '\n':
      keyCode = NS_VK_RETURN;
      name = KEY_NAME_INDEX_Enter;
      break;
    case 8:
      keyCode = NS_VK_BACK;
      name = KEY_NAME_INDEX_Backspace;
      break;
    case 9:
      keyCode = NS_VK_TAB;
      name = KEY_NAME_INDEX_Tab;
      break;
    case 27:
      keyCode = NS_VK_ESCAPE;
      name = KEY_NAME_INDEX_Escape;
      break;
    case GUI_KEY_UP:
      keyCode = NS_VK_UP;
      name = KEY_NAME_INDEX_ArrowUp;
      break;
    case GUI_KEY_DOWN:
      keyCode = NS_VK_DOWN;
      name = KEY_NAME_INDEX_ArrowDown;
      break;
    case GUI_KEY_LEFT:
      keyCode = NS_VK_LEFT;
      name = KEY_NAME_INDEX_ArrowLeft;
      break;
    case GUI_KEY_RIGHT:
      keyCode = NS_VK_RIGHT;
      name = KEY_NAME_INDEX_ArrowRight;
      break;
    case GUI_KEY_PGUP:
      keyCode = NS_VK_PAGE_UP;
      name = KEY_NAME_INDEX_PageUp;
      break;
    case GUI_KEY_PGDN:
      keyCode = NS_VK_PAGE_DOWN;
      name = KEY_NAME_INDEX_PageDown;
      break;
    case GUI_KEY_HOME:
      keyCode = NS_VK_HOME;
      name = KEY_NAME_INDEX_Home;
      break;
    case GUI_KEY_END:
      keyCode = NS_VK_END;
      name = KEY_NAME_INDEX_End;
      break;
    case GUI_KEY_DEL:
      keyCode = NS_VK_DELETE;
      name = KEY_NAME_INDEX_Delete;
      break;
    default:
      if (aKey >= 1 && aKey <= 26) {
        // Ctrl+letra chega como caractere de controle.
        ch = char16_t('a' + aKey - 1);
        mods = MODIFIER_CONTROL;
      } else if (aKey >= 32 && aKey < 256) {
        ch = char16_t(aKey);
      } else {
        return;
      }
      if (ch >= 'a' && ch <= 'z') {
        keyCode = ch - 'a' + 'A';
      } else if ((ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') ||
                 ch == ' ') {
        keyCode = ch;
      }
      break;
  }
  static const EventMessage kMsgs[] = {eKeyDown, eKeyPress, eKeyUp};
  for (EventMessage msg : kMsgs) {
    nsCOMPtr<nsIWidget> top = Top();
    if (!top) {
      return;
    }
    WidgetKeyboardEvent event(true, msg, top);
    event.mModifiers = mods;
    event.mKeyNameIndex = name;
    if (name == KEY_NAME_INDEX_USE_STRING) {
      event.mKeyValue.Assign(ch);
    }
    event.mCodeNameIndex = CODE_NAME_INDEX_UNKNOWN;
    event.mLocation = eKeyLocationStandard;
    if (msg == eKeyPress && ch) {
      event.mCharCode = ch;
    } else {
      event.mKeyCode = keyCode;
    }
    event.AssignEventTime(WidgetEventTime());
    top->DispatchInputEvent(&event);
  }
}

void PumpEvents() {
  if (!sOpen) {
    return;
  }
  // Antes do present: ele repinta a tela toda e os eventos ficariam para tras.
  int ev[4];
  for (int n = 0; n < 32; n++) {
    int type = k_gui_event(ev, 0);
    if (type <= 0) {
      break;
    }
    static int sEvents;
    if (sEvents++ < 12) {
      Mark(sMain ? "evento" : "evento sem janela", type, ev[3]);
    }
    if (!sMain) {
      continue;
    }
    switch (type) {
      case GUI_EV_DOWN: {
        sMouse = LayoutDeviceIntPoint(ev[1], ev[2]);
        // clique fora do menu aberto: fecha o menu (e pode consumir o clique)
        if (CheckRollup(sMouse, false)) {
          sCapture = nullptr;
          break;
        }
        nsCOMPtr<nsIWidget> target = MouseTarget(sMouse);
        // O WIN so entrega eventos a janela que o usuario esta usando: ela tem
        // de ser a ativa do Gecko, ou o foco (e o teclado) fica com a ultima
        // janela de topo mostrada. Popups nao mudam a janela ativa.
        if (target && target->GetWindowType() != WindowType::Popup) {
          Raise(target);
          target = MouseTarget(sMouse);
        }
        sCapture = target;
        Mouse(target, nsIWidget::NativeMouseMessage::Move, 0);
        if (sCapture == target) {
          Mouse(target, nsIWidget::NativeMouseMessage::ButtonDown, ev[3]);
        }
        // Botao direito: menu de contexto ao apertar, como no GTK.
        if (ev[3] == 2 && target && sCapture == target && Find(target)) {
          WidgetPointerEvent cm(true, eContextMenu, target);
          cm.mRefPoint = sMouse - target->WidgetToScreenOffset();
          cm.AssignEventTime(WidgetEventTime());
          target->DispatchInputEvent(&cm);
        }
        break;
      }
      case GUI_EV_UP: {
        sMouse = LayoutDeviceIntPoint(ev[1], ev[2]);
        nsCOMPtr<nsIWidget> target = sCapture ? sCapture : MouseTarget(sMouse);
        sCapture = nullptr;
        Mouse(target, nsIWidget::NativeMouseMessage::ButtonUp, ev[3]);
        break;
      }
      case GUI_EV_DRAG: {
        sMouse = LayoutDeviceIntPoint(ev[1], ev[2]);
        nsCOMPtr<nsIWidget> target = sCapture ? sCapture : MouseTarget(sMouse);
        Mouse(target, nsIWidget::NativeMouseMessage::Move, 0);
        break;
      }
      case GUI_EV_WHEEL: {
        // O WIN manda a posicao do ponteiro: rola o que esta embaixo dele, e
        // nao o ultimo lugar clicado (o WIN nao manda movimento sem botao).
        sMouse = LayoutDeviceIntPoint(ev[1], ev[2]);
        if (CheckRollup(sMouse, true)) {
          break;
        }
        nsCOMPtr<nsIWidget> target = MouseTarget(sMouse);
        Mouse(target, nsIWidget::NativeMouseMessage::Move, 0);
        if (!target || !Find(target)) {
          break;
        }
        // ev[3] > 0 desce; o HeadlessWidget inverte o sinal do delta
        // (patch 0012: linhas, 3 por passo, como no GTK).
        target->SynthesizeNativeMouseScrollEvent(
            sMouse, 0, 0, -double(ev[3]), 0,
            nsIWidget::NativeModifiers::NO_MODIFIERS, 0, nullptr);
        break;
      }
      case GUI_EV_KEY: {
        nsCOMPtr<nsIWidget> top = Top();
        if (top) {
          top->SetFocus(nsIWidget::Raise::Yes, mozilla::dom::CallerType::System);
        }
        Key(ev[3]);
        break;
      }
      case GUI_EV_CLOSE: {
        // O X do WIN fecha a janela de cima; na principal, fecha o Firefox.
        nsCOMPtr<nsIWidget> top = Top();
        if (top) {
          if (nsIWidgetListener* l = top->GetWidgetListener()) {
            l->RequestWindowClose(top);
          }
        }
        break;
      }
      default:
        break;
    }
  }
  int w = 0, h = 0;
  k_gui_size(&w, &h);
  if (w >= 64 && h >= 64 && (w != sWidth || h != sHeight)) {
    sWidth = w;
    sHeight = h;
  }
  // O tamanho e o do WIN: o XUL restaura o tamanho que ele mesmo guardou.
  FitMain();
  if (sDirty) {
    sDirty = false;
    k_gui_present();
  }
}

}  // namespace sodos
}  // namespace widget
}  // namespace mozilla
