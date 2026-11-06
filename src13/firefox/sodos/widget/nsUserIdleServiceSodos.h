/* nsUserIdleServiceSodos.h - o SODOS ainda nao informa inatividade. */
#ifndef nsUserIdleServiceSodos_h_
#define nsUserIdleServiceSodos_h_

#include "mozilla/AppShutdown.h"
#include "nsUserIdleService.h"

class nsUserIdleServiceSodos : public nsUserIdleService {
 public:
  NS_INLINE_DECL_REFCOUNTING_INHERITED(nsUserIdleServiceSodos,
                                       nsUserIdleService)

  bool PollIdleTime(uint32_t* aIdleTime) override;

  static already_AddRefed<nsUserIdleServiceSodos> GetInstance() {
    RefPtr<nsUserIdleService> idleService = nsUserIdleService::GetInstance();
    if (!idleService) {
      if (mozilla::AppShutdown::IsInOrBeyond(
              mozilla::ShutdownPhase::AppShutdownConfirmed)) {
        return nullptr;
      }
      idleService = new nsUserIdleServiceSodos();
    }
    return idleService.forget().downcast<nsUserIdleServiceSodos>();
  }

 protected:
  nsUserIdleServiceSodos() = default;
  virtual ~nsUserIdleServiceSodos() = default;
};

#endif
