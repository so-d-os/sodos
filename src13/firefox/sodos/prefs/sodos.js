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
// O kernel tem 64 soquetes TCP para tudo. Sem limite o pool HTTP guarda
// conexoes ociosas para cada host de uma pagina do Google (e as especulativas)
// e a proxima aba fica sem vaga: "Unable to connect".
pref("network.http.max-connections", 40);
pref("network.http.max-persistent-connections-per-server", 4);
pref("network.http.max-urgent-start-excessive-connections-per-host", 1);
pref("network.http.keep-alive.timeout", 30);
pref("network.http.speculative-parallel-limit", 0);
pref("network.predictor.enabled", false);
pref("network.prefetch-next", false);
pref("network.dns.disablePrefetch", true);
pref("browser.urlbar.speculativeConnect.enabled", false);
pref("browser.places.speculativeConnect.enabled", false);
// Primeira execucao: sem a aba de privacidade e sem as boas-vindas.
pref("datareporting.policy.firstRunURL", "");
pref("datareporting.policy.dataSubmissionPolicyBypassNotification", true);
pref("browser.aboutwelcome.enabled", false);
pref("startup.homepage_welcome_url", "");
pref("startup.homepage_welcome_url.additional", "");
pref("browser.startup.homepage_override.mstone", "ignore");
pref("toolkit.telemetry.reportingpolicy.firstRun", false);
// Telemetria desligada: com o envio ligado o Glean grava o banco dele
// (datareporting/glean/db/data.safe.bin, rkv em modo "safe") inteiro a cada
// metrica registrada, mais o rename. Medido no QEMU/KVM (tools/qprof): a partir
// de ~20 s da abertura, 94% da CPU ia para essas gravacoes em PIO e a pagina
// inicial nao chegava a carregar. Sem envio o Glean nao registra metricas.
pref("datareporting.healthreport.uploadEnabled", false);
pref("datareporting.policy.dataSubmissionEnabled", false);
pref("toolkit.telemetry.enabled", false);
pref("toolkit.telemetry.unified", false);
pref("toolkit.telemetry.archive.enabled", false);
pref("toolkit.telemetry.newProfilePing.enabled", false);
pref("toolkit.telemetry.firstShutdownPing.enabled", false);
pref("toolkit.telemetry.shutdownPingSender.enabled", false);
pref("toolkit.telemetry.updatePing.enabled", false);
pref("toolkit.telemetry.bhrPing.enabled", false);
pref("toolkit.telemetry.coverage.opt-out", true);
pref("toolkit.coverage.opt-out", true);
pref("browser.newtabpage.activity-stream.feeds.telemetry", false);
pref("browser.newtabpage.activity-stream.telemetry", false);
pref("app.shield.optoutstudies.enabled", false);
pref("app.normandy.enabled", false);
pref("app.update.enabled", false);
// Alvo do teste de rede do port (trocar por uma pagina inicial propria depois).
pref("browser.startup.page", 1);
pref("browser.startup.homepage", "https://www.google.com/");
pref("browser.newtabpage.enabled", false);
// window.open (mesmo com largura/altura) e target=_blank abrem em aba: o WIN
// mostra uma janela por programa e outra janela de topo cobriria a principal.
pref("browser.link.open_newwindow", 3);
pref("browser.link.open_newwindow.restriction", 0);
// O QEMU quase sempre e fechado a força: sem isto o Firefox restaura a sessao
// anterior (uma aba "Nova aba") em vez de abrir a pagina inicial.
pref("browser.sessionstore.resume_from_crash", false);
// Menos escrita em disco: o fcntl do port nao implementa travas de arquivo e o
// fsync reescreve o arquivo inteiro no FAT. Historico/favicons/thumbnails sao
// os que mais gravam durante a navegacao e ja apareceram corrompidos
// ("database disk image is malformed" no places.sqlite/favicons.sqlite).
pref("places.history.enabled", false);
pref("browser.chrome.site_icons", false);
pref("browser.pagethumbnails.capturing_disabled", true);
pref("browser.safebrowsing.malware.enabled", false);
pref("browser.safebrowsing.phishing.enabled", false);
// Perfil e cache ficam na pasta do SET FIREFOX_WORKFOLDER (AUTOEXEC.BAT) ou,
// sem ele, no Z: (ramdisk, some no reboot). Downloads vao para o
// disco (/D/ e a unidade do .EXE quando nao ha D:) para nao se perderem.
// O cache HTTP (cache2) vai sempre para o Z: (ramdisk), mesmo com o perfil
// no disco: era o que mais gravava (78 MB em 6 min com o WhatsApp no QEMU) e
// nao precisa sobreviver ao boot. Login, cookies e IndexedDB ficam no perfil.
// Para o cache no disco: user_pref("browser.cache.disk.parent_directory", "")
// no user.js do perfil.
pref("browser.cache.disk.parent_directory", "/Z/TEMP/FFCACHE");
// Sessao (sessionstore.jsonlz4) a cada 60 s em vez de 15 s.
pref("browser.sessionstore.interval", 60000);
pref("browser.download.folderList", 2);
pref("browser.download.dir", "/D/USER/DEFAULT/DOWNLOADS");
// Sem fork(): o Gecko tenta lancar "utility processes" (validador de JS do ORB
// em toda resposta opaca, decodificacao de audio, RDD) e cada tentativa sai no
// log como "fork() failed" + "Failed to launch utility subprocess". Tudo roda
// no processo principal.
pref("browser.opaqueResponseBlocking.javascriptValidator", false);
pref("media.utility-process.enabled", false);
pref("media.rdd-process.enabled", false);
// BackupService procura a pasta "Docs" (XDG), que nao existe no SODOS:
// "PathUtils does not support empty paths" a cada inicio.
pref("browser.backup.enabled", false);
pref("browser.backup.scheduled.enabled", false);
pref("browser.backup.archive.enabled", false);
// Audio: backend do SODOS (cubeb_sodos.c) pelo k_audio_* do kernel (SET AUDIO=1);
// sem placa ele so faz o papel de relogio.
pref("media.cubeb.backend", "sodos");
// O SODOS compila como XP_LINUX, onde o padrao e true: o cubeb ia pelo AudioIPC
// (servidor de audio em outro processo, que nao existe aqui) e o contexto vinha
// nulo ("Can't get cubeb context!"), sem nunca chamar o backend sodos.
pref("media.cubeb.sandbox", false);
// Video: o FFmpeg completo esta dentro do FIREFOX.EXE no lugar do ffvpx
// (sodos/scripts/build-ffmpeg.sh), com H.264/HEVC/AAC alem de VP8/VP9/MP3.
// So o caminho "FFVPX" e usado; o de biblioteca do sistema (libavcodec.so)
// nao existe e so geraria o aviso de "FFmpeg nao encontrado".
pref("media.ffmpeg.enabled", false);
// Decodificacao so em CPU: H.264 e o formato mais leve. Sem AV1 e sem VP9 no
// MSE o YouTube manda H.264 + AAC (MP4), como o Instagram e o TikTok.
pref("media.av1.enabled", false);
pref("media.mediasource.vp9.enabled", false);
// WebGL em GL de software (Mesa ligado no FIREFOX.EXE, docs/07-webgl.md). Sem
// GPU o gfxInfo marcaria FORBID_HARDWARE e o caminho EGL so tenta o display
// de software do GTK; force-enabled usa o display EGL padrao (surfaceless).
pref("webgl.force-enabled", true);

// Memoria do wasm: sem "huge memory" o SpiderMonkey reserva so o maximo da
// memoria (ate 4 GB + guarda) em vez de ~6 GB por instancia. O SODOS nao tem
// paginas sob demanda: toda reserva vira RAM de verdade (acima de 4 GB, do
// k_hmalloc, teto SET HIMEM).
pref("javascript.options.wasm_disable_huge_memory", true);
