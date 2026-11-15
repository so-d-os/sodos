/* SodosClipboard.cpp - area de transferencia do Gecko sobre a do kernel. */
#include "SodosClipboard.h"

#include "nsCOMPtr.h"
#include "nsComponentManagerUtils.h"
#include "nsISupportsPrimitives.h"
#include "nsITransferable.h"

extern "C" {
void k_clip_set(const char* s, int n);
int k_clip_get(char* out, int max);
}

namespace mozilla::widget {

// CLIP_MAX do kernel (kernel/clip.h): o texto e cortado em 4095 bytes.
static const int kKernelClipMax = 4096;

NS_IMPL_ISUPPORTS_INHERITED0(SodosClipboard, nsBaseClipboard)

SodosClipboard::SodosClipboard()
    : nsBaseClipboard(mozilla::dom::ClipboardCapabilities(
          true /* supportsSelectionClipboard */,
          true /* supportsFindClipboard */,
          true /* supportsSelectionCache */)) {
  for (auto& clipboard : mClipboards) {
    clipboard = MakeUnique<HeadlessClipboardData>();
  }
  mKernelSet.SetIsVoid(true);
}

bool SodosClipboard::KernelText(nsACString& aOut) const {
  static char buf[kKernelClipMax];
  int n = k_clip_get(buf, kKernelClipMax);
  if (n <= 0) {
    aOut.Truncate();
    return false;
  }
  aOut.Assign(buf, n);
  return true;
}

bool SodosClipboard::GlobalIsOurs() const {
  if (mKernelSet.IsVoid()) {
    return false;
  }
  nsAutoCString now;
  KernelText(now);
  return now.Equals(mKernelSet);
}

NS_IMETHODIMP
SodosClipboard::SetNativeClipboardData(nsITransferable* aTransferable,
                                       ClipboardType aWhichClipboard) {
  MOZ_DIAGNOSTIC_ASSERT(aTransferable);
  EmptyNativeClipboardData(aWhichClipboard);

  nsTArray<nsCString> flavors;
  nsresult rv = aTransferable->FlavorsTransferableCanExport(flavors);
  if (NS_FAILED(rv)) {
    return rv;
  }
  auto& clipboard = mClipboards[aWhichClipboard];
  for (const auto& flavor : flavors) {
    if (!flavor.EqualsLiteral(kTextMime) && !flavor.EqualsLiteral(kHTMLMime)) {
      continue;
    }
    nsCOMPtr<nsISupports> data;
    rv = aTransferable->GetTransferData(flavor.get(), getter_AddRefs(data));
    if (NS_FAILED(rv)) {
      continue;
    }
    nsCOMPtr<nsISupportsString> wideString = do_QueryInterface(data);
    if (!wideString) {
      continue;
    }
    nsAutoString utf16string;
    wideString->GetData(utf16string);
    if (flavor.EqualsLiteral(kTextMime)) {
      clipboard->SetText(utf16string);
      if (aWhichClipboard == kGlobalClipboard) {
        NS_ConvertUTF16toUTF8 utf8(utf16string);
        int n = int(utf8.Length());
        if (n > kKernelClipMax - 1) {
          n = kKernelClipMax - 1;
        }
        k_clip_set(utf8.get(), n);
        KernelText(mKernelSet);  // como o kernel guardou (cortado)
      }
    } else {
      clipboard->SetHTML(utf16string);
    }
  }
  return NS_OK;
}

mozilla::Result<nsCOMPtr<nsISupports>, nsresult>
SodosClipboard::GetNativeClipboardData(const nsACString& aFlavor,
                                       ClipboardType aWhichClipboard,
                                       uint64_t aThreshold) {
  if (!aFlavor.EqualsLiteral(kTextMime) && !aFlavor.EqualsLiteral(kHTMLMime)) {
    return nsCOMPtr<nsISupports>{};
  }
  bool isText = aFlavor.EqualsLiteral(kTextMime);
  nsAutoString value;
  if (aWhichClipboard == kGlobalClipboard && !GlobalIsOurs()) {
    // Outro programa copiou: so ha texto puro.
    nsAutoCString kernel;
    if (!isText || !KernelText(kernel)) {
      return nsCOMPtr<nsISupports>{};
    }
    CopyUTF8toUTF16(kernel, value);
  } else {
    auto& clipboard = mClipboards[aWhichClipboard];
    if (!(isText ? clipboard->HasText() : clipboard->HasHTML())) {
      return nsCOMPtr<nsISupports>{};
    }
    value = isText ? clipboard->GetText() : clipboard->GetHTML();
  }
  if (aThreshold && isText && value.Length() > aThreshold) {
    return mozilla::Err(NS_ERROR_CLIPBOARD_TOO_BIG);
  }
  nsresult rv;
  nsCOMPtr<nsISupportsString> dataWrapper =
      do_CreateInstance(NS_SUPPORTS_STRING_CONTRACTID, &rv);
  if (NS_FAILED(rv) || !dataWrapper) {
    return nsCOMPtr<nsISupports>{};
  }
  rv = dataWrapper->SetData(value);
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return nsCOMPtr<nsISupports>{};
  }
  return nsCOMPtr<nsISupports>(std::move(dataWrapper));
}

nsresult SodosClipboard::EmptyNativeClipboardData(
    ClipboardType aWhichClipboard) {
  mClipboards[aWhichClipboard]->Clear();
  if (aWhichClipboard == kGlobalClipboard) {
    mKernelSet.SetIsVoid(true);
  }
  return NS_OK;
}

mozilla::Result<int32_t, nsresult>
SodosClipboard::GetNativeClipboardSequenceNumber(
    ClipboardType aWhichClipboard) {
  int32_t seq = mClipboards[aWhichClipboard]->GetChangeCount();
  if (aWhichClipboard == kGlobalClipboard) {
    // Muda quando qualquer programa troca o texto do kernel.
    nsAutoCString kernel;
    KernelText(kernel);
    uint32_t h = 2166136261u;
    const char* p = kernel.BeginReading();
    for (uint32_t i = 0; i < kernel.Length(); i++) {
      h = (h ^ uint8_t(p[i])) * 16777619u;
    }
    seq = int32_t(h ^ (uint32_t(seq) * 2654435761u));
  }
  return seq;
}

mozilla::Result<bool, nsresult>
SodosClipboard::HasNativeClipboardDataMatchingFlavors(
    const nsTArray<nsCString>& aFlavorList, ClipboardType aWhichClipboard) {
  if (aWhichClipboard == kGlobalClipboard && !GlobalIsOurs()) {
    nsAutoCString kernel;
    if (!KernelText(kernel)) {
      return false;
    }
    for (auto& flavor : aFlavorList) {
      if (flavor.EqualsLiteral(kTextMime)) {
        return true;
      }
    }
    return false;
  }
  auto& clipboard = mClipboards[aWhichClipboard];
  for (auto& flavor : aFlavorList) {
    if ((flavor.EqualsLiteral(kTextMime) && clipboard->HasText()) ||
        (flavor.EqualsLiteral(kHTMLMime) && clipboard->HasHTML())) {
      return true;
    }
  }
  return false;
}

}  // namespace mozilla::widget
