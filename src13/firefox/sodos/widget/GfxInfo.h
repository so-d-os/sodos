/* GfxInfo.h - o SODOS nao tem GPU: renderizacao por software, sem blocklist. */
#ifndef _mozilla_widget_GfxInfo_h_
#define _mozilla_widget_GfxInfo_h_

#include "GfxDriverInfo.h"
#include "GfxInfoBase.h"
#include "nsString.h"

namespace mozilla::widget {

class GfxInfo : public GfxInfoBase {
 private:
  ~GfxInfo();

 public:
  GfxInfo();

  OperatingSystem GetOperatingSystem() override {
    return OperatingSystem::Linux;
  }

  NS_IMETHOD GetDWriteEnabled(bool* aDWriteEnabled) override;
  NS_IMETHOD GetEmbeddedInFirefoxReality(
      bool* aEmbeddedInFirefoxReality) override;
  NS_IMETHOD GetHasBattery(bool* aHasBattery) override;
  NS_IMETHOD GetDWriteVersion(nsAString& aDwriteVersion) override;
  NS_IMETHOD GetCleartypeParameters(nsAString& aCleartypeParams) override;
  NS_IMETHOD GetWindowProtocol(nsAString& aWindowProtocol) override;
  NS_IMETHOD GetTestType(nsAString& aTestType) override;
  NS_IMETHOD GetAdapterDescription(nsAString& aAdapterDescription) override;
  NS_IMETHOD GetAdapterDriver(nsAString& aAdapterDriver) override;
  NS_IMETHOD GetAdapterVendorID(nsAString& aAdapterVendorID) override;
  NS_IMETHOD GetAdapterDeviceID(nsAString& aAdapterDeviceID) override;
  NS_IMETHOD GetAdapterSubsysID(nsAString& aAdapterSubsysID) override;
  NS_IMETHOD GetAdapterRAM(uint32_t* aAdapterRAM) override;
  NS_IMETHOD GetAdapterDriverVendor(nsAString& aAdapterDriverVendor) override;
  NS_IMETHOD GetAdapterDriverVersion(nsAString& aAdapterDriverVersion) override;
  NS_IMETHOD GetAdapterDriverDate(nsAString& aAdapterDriverDate) override;
  NS_IMETHOD GetAdapterDescription2(nsAString& aAdapterDescription) override;
  NS_IMETHOD GetAdapterDriver2(nsAString& aAdapterDriver) override;
  NS_IMETHOD GetAdapterVendorID2(nsAString& aAdapterVendorID) override;
  NS_IMETHOD GetAdapterDeviceID2(nsAString& aAdapterDeviceID) override;
  NS_IMETHOD GetAdapterSubsysID2(nsAString& aAdapterSubsysID) override;
  NS_IMETHOD GetAdapterRAM2(uint32_t* aAdapterRAM) override;
  NS_IMETHOD GetAdapterDriverVendor2(nsAString& aAdapterDriverVendor) override;
  NS_IMETHOD GetAdapterDriverVersion2(
      nsAString& aAdapterDriverVersion) override;
  NS_IMETHOD GetAdapterDriverDate2(nsAString& aAdapterDriverDate) override;
  NS_IMETHOD GetIsGPU2Active(bool* aIsGPU2Active) override;
  NS_IMETHOD GetDrmRenderDevice(nsACString& aDrmRenderDevice) override;
  using GfxInfoBase::GetFeatureStatus;
  using GfxInfoBase::GetFeatureSuggestedDriverVersion;

 protected:
  const nsTArray<RefPtr<GfxDriverInfo>>& GetGfxDriverInfo() override;
};

}  // namespace mozilla::widget

#endif
