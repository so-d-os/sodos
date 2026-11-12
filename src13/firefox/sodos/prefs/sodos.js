// sodos.js - preferencias padrao do Firefox no SODOS.
// Fica em D:\FIREFOX\browser\defaults\preferences: essa pasta e lida depois de
// defaults/pref, entao sobrepoe o firefox.js do upstream (que fixa, por
// exemplo, browser.startup.homepage=about:home). Os arquivos entram em ordem
// alfabetica e "sodos.js" vem depois de "firefox.js".
// A rede do SODOS e IPv4 e TCP (sem UDP para o programa): sem HTTP/3 nem IPv6.
pref("network.http.http3.enable", false);
pref("network.dns.disableIPv6", true);
pref("network.trr.mode", 5);
// Detector de portal cativo e de conectividade: pedidos em segundo plano.
pref("network.captive-portal-service.enabled", false);
pref("network.connectivity-service.enabled", false);
// Primeira execucao: sem a aba de privacidade e sem as boas-vindas.
pref("datareporting.policy.firstRunURL", "");
pref("datareporting.policy.dataSubmissionPolicyBypassNotification", true);
pref("browser.aboutwelcome.enabled", false);
pref("startup.homepage_welcome_url", "");
pref("startup.homepage_welcome_url.additional", "");
pref("browser.startup.homepage_override.mstone", "ignore");
pref("toolkit.telemetry.reportingpolicy.firstRun", false);
pref("app.normandy.enabled", false);
pref("app.update.enabled", false);
// Alvo do teste de rede do port (trocar por uma pagina inicial propria depois).
pref("browser.startup.page", 1);
pref("browser.startup.homepage", "https://www.google.com/");
pref("browser.newtabpage.enabled", false);
// O QEMU quase sempre e fechado a força: sem isto o Firefox restaura a sessao
// anterior (uma aba "Nova aba") em vez de abrir a pagina inicial.
pref("browser.sessionstore.resume_from_crash", false);
