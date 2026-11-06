/* nsAppShell.h - laco de eventos do SODOS.
 *
 * Por enquanto so entrega os eventos do Gecko. Os eventos de janela (k_gui_event)
 * entram em ProcessNextNativeEvent quando as janelas forem ligadas ao k_gui_*.
 */
#ifndef nsAppShell_h_
#define nsAppShell_h_

#include "mozilla/Monitor.h"
#include "nsBaseAppShell.h"

class nsAppShell final : public nsBaseAppShell {
 public:
  nsAppShell();

  nsresult Init();

 protected:
  virtual ~nsAppShell();

  void ScheduleNativeEventCallback() override;
  bool ProcessNextNativeEvent(bool aMayWait) override;

 private:
  mozilla::Monitor mMonitor;
  bool mPending MOZ_GUARDED_BY(mMonitor);
};

#endif
