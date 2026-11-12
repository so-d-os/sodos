/* SodosWindow.cpp - janela, pintura e entrada do Gecko sobre k_gui_*. */
#include "SodosWindow.h"

#include "mozilla/MouseEvents.h"
#include "mozilla/TextEvents.h"
#include "mozilla/UniquePtr.h"
#include "mozilla/gfx/2D.h"
#include "nsIWidget.h"
#include "nsIWidgetListener.h"
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

namespace mozilla {
namespace widget {
namespace sodos {

static bool sOpen;
static int sWidth, sHeight;
static nsIWidget* sMain;  // o widget que e a janela; nao segura referencia
static UniquePtr<uint8_t[]> sBack;  // BGRX, sWidth x sHeight
static UniquePtr<uint8_t[]> sRow;   // faixa RGBA mandada ao WIN
static IntSize sBackSize;
static LayoutDeviceIntPoint sMouse;
static bool sDirty;  // ha pixels novos esperando o k_gui_present

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

static void FitMain() {
  if (sMain) {
    sMain->Resize(DesktopRect(0, 0, sWidth, sHeight), true);
  }
}

void WindowShown(nsIWidget* aWidget, bool aVisible, bool aTopLevel) {
  if (!sOpen || !aVisible || !aTopLevel || sMain) {
    return;
  }
  sMain = aWidget;
  Mark("widget principal");
  // Fora do Show(): o redimensionamento avisa o layout.
  NS_DispatchToMainThread(NS_NewRunnableFunction("sodos::FitMain", [] { FitMain(); }));
}

void WindowDestroyed(nsIWidget* aWidget) {
  if (sMain == aWidget) {
    sMain = nullptr;
  }
}

already_AddRefed<DrawTarget> StartDrawing(nsIWidget* aWidget) {
  static int sOther;
  if (sOpen && aWidget != sMain && sOther++ < 3) {
    Mark("compositor de outro widget");
  }
  if (!sOpen || aWidget != sMain) {
    return nullptr;
  }
  IntSize size(sWidth, sHeight);
  if (!sBack || sBackSize != size) {
    sBack = MakeUnique<uint8_t[]>(size_t(size.width) * size.height * 4);
    sRow = MakeUnique<uint8_t[]>(size_t(size.width) * 64 * 4);
    sBackSize = size;
  }
  return Factory::CreateDrawTargetForData(BackendType::SKIA, sBack.get(), size,
                                          size.width * 4,
                                          SurfaceFormat::B8G8R8X8);
}

void EndDrawing(nsIWidget* aWidget, const LayoutDeviceIntRegion& aRegion) {
  if (!sOpen || aWidget != sMain || !sBack) {
    return;
  }
  static int sFrames;
  if (sFrames++ < 3) {
    Mark("quadro", aRegion.GetBounds().width, aRegion.GetBounds().height);
  }
  IntRect all(IntPoint(0, 0), sBackSize);
  for (auto iter = aRegion.RectIter(); !iter.Done(); iter.Next()) {
    IntRect r = iter.Get().ToUnknownRect().Intersect(all);
    if (r.IsEmpty()) {
      continue;
    }
    // Em faixas de 64 linhas: BGRX -> RGBA opaco.
    for (int y = r.y; y < r.YMost(); y += 64) {
      int rows = std::min(64, r.YMost() - y);
      for (int j = 0; j < rows; j++) {
        const uint8_t* s =
            sBack.get() + (size_t(y + j) * sBackSize.width + r.x) * 4;
        uint8_t* d = sRow.get() + size_t(j) * r.width * 4;
        for (int i = 0; i < r.width; i++, s += 4, d += 4) {
          d[0] = s[2];
          d[1] = s[1];
          d[2] = s[0];
          d[3] = 255;
        }
      }
      k_gui_pixels(r.x, y, r.width, rows, sRow.get(), r.width * 4);
    }
    sDirty = true;
  }
}

static void Mouse(nsIWidget::NativeMouseMessage aMsg, int aButton) {
  sMain->SynthesizeNativeMouseEvent(
      sMouse, aMsg, aButton == 2 ? MouseButton::eSecondary : MouseButton::ePrimary,
      nsIWidget::NativeModifiers::NO_MODIFIERS, nullptr);
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
    if (!sMain) {
      return;
    }
    WidgetKeyboardEvent event(true, msg, sMain);
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
    RefPtr<nsIWidget> kungFuDeathGrip(sMain);
    sMain->DispatchInputEvent(&event);
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
    RefPtr<nsIWidget> kungFuDeathGrip(sMain);
    switch (type) {
      case GUI_EV_DOWN:
        sMouse = LayoutDeviceIntPoint(ev[1], ev[2]);
        Mouse(nsIWidget::NativeMouseMessage::Move, 0);
        if (sMain) {
          Mouse(nsIWidget::NativeMouseMessage::ButtonDown, ev[3]);
        }
        break;
      case GUI_EV_UP:
        sMouse = LayoutDeviceIntPoint(ev[1], ev[2]);
        Mouse(nsIWidget::NativeMouseMessage::ButtonUp, ev[3]);
        break;
      case GUI_EV_DRAG:
        sMouse = LayoutDeviceIntPoint(ev[1], ev[2]);
        Mouse(nsIWidget::NativeMouseMessage::Move, 0);
        break;
      case GUI_EV_WHEEL:
        // ev[3] > 0 desce; o HeadlessWidget inverte o sinal do delta.
        sMain->SynthesizeNativeMouseScrollEvent(
            sMouse, 0, 0, -double(ev[3]), 0,
            nsIWidget::NativeModifiers::NO_MODIFIERS, 0, nullptr);
        break;
      case GUI_EV_KEY:
        Key(ev[3]);
        break;
      case GUI_EV_CLOSE:
        if (nsIWidgetListener* l = sMain->GetWidgetListener()) {
          l->RequestWindowClose(sMain);
        }
        break;
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
  if (sMain && sMain->GetBounds().Size() != LayoutDeviceIntSize(sWidth, sHeight)) {
    FitMain();
  }
  if (sDirty) {
    sDirty = false;
    k_gui_present();
  }
}

}  // namespace sodos
}  // namespace widget
}  // namespace mozilla
