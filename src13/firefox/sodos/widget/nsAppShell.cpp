/* nsAppShell.cpp - laco de eventos do SODOS. */
#include "nsAppShell.h"

#include "HeadlessScreenHelper.h"
#include "mozilla/TimeStamp.h"
#include "mozilla/UniquePtr.h"
#include "mozilla/widget/ScreenManager.h"
#include "nsXULAppAPI.h"

using namespace mozilla;
using mozilla::widget::HeadlessScreenHelper;
using mozilla::widget::ScreenManager;

nsAppShell::nsAppShell() : mMonitor("nsAppShell"), mPending(false) {}

nsAppShell::~nsAppShell() = default;

nsresult nsAppShell::Init() {
  if (XRE_IsParentProcess()) {
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
  {
    MonitorAutoLock lock(mMonitor);
    if (!mPending && aMayWait) {
      // Prazo curto: quando houver eventos de janela, eles serao lidos aqui.
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
