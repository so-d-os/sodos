/* GfxInfo.cpp - o SODOS nao tem GPU: renderizacao por software, sem blocklist. */
#include "GfxInfo.h"

namespace mozilla::widget {

GfxInfo::GfxInfo() = default;
GfxInfo::~GfxInfo() = default;

#define GFXINFO_FAIL_STR(name)                        \
  NS_IMETHODIMP GfxInfo::name(nsAString& aValue) {    \
    return NS_ERROR_FAILURE;                          \
  }
#define GFXINFO_EMPTY_STR(name)                       \
  NS_IMETHODIMP GfxInfo::name(nsAString& aValue) {    \
    aValue.Truncate();                                \
    return NS_OK;                                     \
  }

GFXINFO_FAIL_STR(GetDWriteVersion)
GFXINFO_FAIL_STR(GetCleartypeParameters)
GFXINFO_FAIL_STR(GetAdapterDriver)
GFXINFO_FAIL_STR(GetAdapterDriver2)
GFXINFO_FAIL_STR(GetAdapterDriverDate)
GFXINFO_FAIL_STR(GetAdapterDriverDate2)
GFXINFO_FAIL_STR(GetAdapterSubsysID)
GFXINFO_FAIL_STR(GetAdapterSubsysID2)
GFXINFO_FAIL_STR(GetAdapterDescription2)
GFXINFO_FAIL_STR(GetAdapterDriverVendor2)
GFXINFO_EMPTY_STR(GetAdapterDriverVendor)
GFXINFO_EMPTY_STR(GetAdapterDriverVersion)
GFXINFO_EMPTY_STR(GetAdapterDriverVersion2)
GFXINFO_EMPTY_STR(GetAdapterVendorID)
GFXINFO_EMPTY_STR(GetAdapterVendorID2)
GFXINFO_EMPTY_STR(GetAdapterDeviceID)
GFXINFO_EMPTY_STR(GetAdapterDeviceID2)

NS_IMETHODIMP GfxInfo::GetAdapterDescription(nsAString& aValue) {
  aValue.AssignLiteral("SODOS software");
  return NS_OK;
}

NS_IMETHODIMP GfxInfo::GetDWriteEnabled(bool* aEnabled) {
  return NS_ERROR_FAILURE;
}

NS_IMETHODIMP GfxInfo::GetEmbeddedInFirefoxReality(bool* aValue) {
  *aValue = false;
  return NS_OK;
}

NS_IMETHODIMP GfxInfo::GetHasBattery(bool* aHasBattery) {
  return NS_ERROR_NOT_IMPLEMENTED;
}

NS_IMETHODIMP GfxInfo::GetWindowProtocol(nsAString& aValue) {
  return NS_ERROR_NOT_IMPLEMENTED;
}

NS_IMETHODIMP GfxInfo::GetTestType(nsAString& aValue) {
  return NS_ERROR_NOT_IMPLEMENTED;
}

NS_IMETHODIMP GfxInfo::GetAdapterRAM(uint32_t* aValue) {
  return NS_ERROR_FAILURE;
}

NS_IMETHODIMP GfxInfo::GetAdapterRAM2(uint32_t* aValue) {
  return NS_ERROR_FAILURE;
}

NS_IMETHODIMP GfxInfo::GetIsGPU2Active(bool* aValue) {
  return NS_ERROR_FAILURE;
}

NS_IMETHODIMP GfxInfo::GetDrmRenderDevice(nsACString& aValue) {
  return NS_ERROR_NOT_IMPLEMENTED;
}

const nsTArray<RefPtr<GfxDriverInfo>>& GfxInfo::GetGfxDriverInfo() {
  return *sDriverInfo;
}

}  // namespace mozilla::widget
