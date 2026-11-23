#!/bin/sh
# build-ffmpeg.sh - FFmpeg estatico para o FIREFOX.EXE (video H.264/HEVC/AAC).
#
# O ffvpx que vem no Firefox so tem VP8/VP9/AV1/MP3/FLAC; Instagram e TikTok
# (e parte do YouTube) usam H.264 + AAC em MP4. Este script compila o FFmpeg
# da MESMA revisao do ffvpx (media/ffvpx/README_MOZILLA, libavcodec 62.29),
# entao os headers com que o Gecko foi compilado batem com a biblioteca.
# link-firefox64.sh liga ~/ffsrc/ffmpeg/build/lib*.a no lugar das listas do
# ffvpx, e o dlopen("libmozavcodec.so") do port (sodos_extra.c) devolve as
# funcoes dela (tabela gerada em ~/ffsrc/ffmpeg/build/sodos_av_syms.c).
#
# Saida: ~/ffsrc/ffmpeg/build/libavcodec/libavcodec.a, libavutil/libavutil.a,
#        sodos_av_syms.o
set -e
REV=9917308cc209a885c6870f0345905104c6ea8799
S=$(cd "$(dirname "$0")/../.." && pwd)        # src13/firefox
FF=${FFSRC:-$HOME/ffsrc}
W=$FF/ffmpeg
TB=$FF/dl/FFmpeg-${REV%${REV#???????}}.tar.gz
export PATH="$S/toolchain/bin:$PATH"

mkdir -p "$FF/dl" "$W"
[ -f "$TB" ] || curl -sL -o "$TB" "https://github.com/FFmpeg/FFmpeg/archive/$REV.tar.gz"
if [ ! -f "$W/src/configure" ]; then
  rm -rf "$W/src"; mkdir -p "$W/src"
  tar xzf "$TB" -C "$W/src" --strip-components=1
  # A libc do SODOS declara erf() mas nao tem: usa a versao do proprio FFmpeg
  # com outro nome (senao "static declaration of erf follows non-static").
  sed -i 's/^static inline double erf(double z)/#define erf ff_sodos_erf\nstatic inline double erf(double z)/' \
    "$W/src/libavutil/libm.h"
fi

mkdir -p "$W/build"
cd "$W/build"
if [ ! -f ffbuild/config.mak ] || [ "$0" -nt ffbuild/config.mak ]; then
  ../src/configure --enable-cross-compile --target-os=none --arch=x86_64 \
    --cc=x86_64-sodos-gcc64 --ar=ar --ranlib=ranlib --nm=nm --strip=strip \
    --x86asmexe=nasm --extra-cflags="-O2" \
    --disable-everything --disable-programs --disable-doc --disable-avformat \
    --disable-avdevice --disable-avfilter --disable-swscale --disable-swresample \
    --disable-network --disable-autodetect --disable-iconv --disable-hwaccels \
    --disable-debug --enable-pic --enable-pthreads --enable-static --disable-shared \
    --enable-decoder=h264,hevc,aac,mp3,mp3float,vp8,vp9,flac,vorbis,pcm_alaw,pcm_mulaw,pcm_s16le,pcm_s24le,pcm_f32le \
    --enable-parser=h264,hevc,aac,vp8,vp9,mpegaudio,opus,vorbis,flac >/dev/null
fi
make -j"$(nproc)" >make.log 2>&1 || { tail -30 make.log; exit 1; }

# Tabela nome -> endereco para o dlsym: todas as funcoes publicas av_*/avcodec_*
# que as duas bibliotecas definem (o Gecko procura por nome, FFmpegLibWrapper).
{
  echo "/* gerado por build-ffmpeg.sh */"
  echo "#include <string.h>"
  nm -g --defined-only libavcodec/libavcodec.a libavutil/libavutil.a 2>/dev/null |
    awk '$2 ~ /^[TDBR]$/ && $3 ~ /^(av_|avcodec_|avutil_|avpriv_)/ {print $3}' | sort -u > syms.txt
  while read -r n; do echo "extern char $n[];"; done < syms.txt
  echo "static const struct { const char* n; void* p; } tab[] = {"
  while read -r n; do echo "  {\"$n\", (void*)$n},"; done < syms.txt
  echo "};"
  echo "void* sodos_av_dlsym(const char* n) {"
  echo "  unsigned i;"
  echo "  for (i = 0; i < sizeof tab / sizeof tab[0]; i++)"
  echo "    if (!strcmp(tab[i].n, n)) return tab[i].p;"
  echo "  return 0;"
  echo "}"
} > sodos_av_syms.c
x86_64-sodos-gcc64 -O2 -c sodos_av_syms.c -o sodos_av_syms.o
echo "ffmpeg: $(wc -l < syms.txt) simbolos"
ls -la libavcodec/libavcodec.a libavutil/libavutil.a sodos_av_syms.o
