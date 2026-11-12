/* nsAppShell.cpp - laco de eventos do SODOS. */
#include "nsAppShell.h"

#include "HeadlessScreenHelper.h"
#include "mozilla/TimeStamp.h"
#include "mozilla/UniquePtr.h"
#include "mozilla/widget/ScreenManager.h"
#include "SodosWindow.h"
#include "nsPrintfCString.h"
#include "nsXULAppAPI.h"
#include "prenv.h"

using namespace mozilla;
using mozilla::widget::HeadlessScreenHelper;
using mozilla::widget::ScreenManager;

nsAppShell::nsAppShell() : mMonitor("nsAppShell"), mPending(false) {}

nsAppShell::~nsAppShell() = default;

nsresult nsAppShell::Init() {
  if (XRE_IsParentProcess()) {
    // A tela do Gecko e a area da janela que o WIN deu ao programa.
    if (widget::sodos::Open()) {
      LayoutDeviceIntSize size = widget::sodos::Size();
      PR_SetEnv(ToNewCString(nsPrintfCString("MOZ_HEADLESS_WIDTH=%d", size.width)));
      PR_SetEnv(ToNewCString(nsPrintfCString("MOZ_HEADLESS_HEIGHT=%d", size.height)));
    }
    ScreenManager::GetSingleton().SetHelper(MakeUnique<HeadlessScreenHelper>());
  }
  return nsBaseAppShell::Init();
}

void nsAppShell::ScheduleNativeEventCallback() {
  MonitorAutoLock lock(mMonitor);
  mPending = true;
  lock.Notify();
}

bool nsAppShell::ProcessNextNativeEvent(bool aMayWait) {
  widget::sodos::PumpEvents();
  {
    MonitorAutoLock lock(mMonitor);
    if (!mPending && aMayWait) {
      // Prazo curto: os eventos da janela sao lidos a cada volta.
      lock.Wait(TimeDuration::FromMilliseconds(10));
    }
    if (!mPending) {
      return false;
    }
    mPending = false;
  }
  NativeEventCallback();
  return true;
}
