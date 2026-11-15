/* SodosClipboard.h - area de transferencia do Gecko sobre a do kernel.
 *
 * A area global (Ctrl+C/Ctrl+V) e a mesma do DOS, do Command e do Bloco de
 * notas do WIN (k_clip_set/k_clip_get), em UTF-8. As outras (selecao, busca)
 * e o text/html ficam so na memoria do Firefox, como no HeadlessClipboard. */
#ifndef mozilla_widget_SodosClipboard_h
#define mozilla_widget_SodosClipboard_h

#include "nsBaseClipboard.h"
#include "nsIClipboard.h"
#include "nsString.h"
#include "mozilla/UniquePtr.h"
#include "HeadlessClipboardData.h"

namespace mozilla::widget {

class SodosClipboard final : public nsBaseClipboard {
 public:
  SodosClipboard();

  NS_DECL_ISUPPORTS_INHERITED
  mozilla::Result<int32_t, nsresult> GetNativeClipboardSequenceNumber(
      ClipboardType aWhichClipboard) override;

 protected:
  ~SodosClipboard() = default;

  NS_IMETHOD SetNativeClipboardData(nsITransferable* aTransferable,
                                    ClipboardType aWhichClipboard) override;
  mozilla::Result<nsCOMPtr<nsISupports>, nsresult> GetNativeClipboardData(
      const nsACString& aFlavor, ClipboardType aWhichClipboard,
      uint64_t aThreshold) override;
  nsresult EmptyNativeClipboardData(ClipboardType aWhichClipboard) override;
  mozilla::Result<bool, nsresult> HasNativeClipboardDataMatchingFlavors(
      const nsTArray<nsCString>& aFlavorList,
      ClipboardType aWhichClipboard) override;

 private:
  // Texto do kernel agora; false se esta vazio.
  bool KernelText(nsACString& aOut) const;
  // A area global ainda e a que o Firefox gravou (senao outro programa copiou).
  bool GlobalIsOurs() const;

  UniquePtr<HeadlessClipboardData>
      mClipboards[nsIClipboard::kClipboardTypeCount];
  nsCString mKernelSet;  // o que o Firefox mandou ao kernel por ultimo
};

}  // namespace mozilla::widget

#endif
