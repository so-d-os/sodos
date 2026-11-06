/* OSPreferences_sodos.cpp - o SODOS nao tem preferencias de idioma do sistema. */
#include "OSPreferences.h"

using namespace mozilla::intl;

OSPreferences::OSPreferences() = default;

void OSPreferences::RemoveObservers() {}

bool OSPreferences::ReadSystemLocales(nsTArray<nsCString>& aLocaleList) {
  aLocaleList.AppendElement("en-US"_ns);
  return true;
}

bool OSPreferences::ReadRegionalPrefsLocales(nsTArray<nsCString>& aLocaleList) {
  return ReadSystemLocales(aLocaleList);
}

bool OSPreferences::ReadDateTimePattern(DateTimeFormatStyle aDateStyle,
                                        DateTimeFormatStyle aTimeStyle,
                                        const nsACString& aLocale,
                                        nsACString& aRetVal) {
  return false;  // o chamador usa os padroes do ICU
}
