# Riuso e analisi di un TV box "MXQ PRO" (Rockchip RK3229)

Reverse engineering e riuso di un TV box Android cinese economico, venduto come
"MXQ PRO". Dal box fermo in un cassetto a: identificazione reale dell'hardware,
analisi della ROM (con una backdoor pubblicitaria preinstallata), pulizia del
sistema, uso come console retro con un driver custom per il pad, e infine
sostituzione completa di Android con Linux (Armbian/Ubuntu) sulla eMMC.

> Scritto per chi vuole rifare qualcosa di simile su un box RK3228/RK3229. Adatta
> tutti i comandi al tuo dispositivo. **Lavori a tuo rischio**: puoi rendere il box
> temporaneamente non avviabile (recuperabile, vedi la sezione backup).

<p align="center">
  <img src="docs/img/retroarch-menu.png" width="32%" alt="RetroArch">
  <img src="docs/img/mario64-50fps.png" width="32%" alt="Super Mario 64 ~50fps">
  <img src="docs/img/projectivy.png" width="32%" alt="Projectivy Launcher">
</p>

---

## 1. Cos'è davvero il box

"MXQ PRO" è solo un nome commerciale riciclato da decine di produttori. L'hardware
reale, letto da ADB:

| Voce | Valore |
|---|---|
| SoC | **Rockchip RK3229** (4× ARM Cortex-A7, 32 bit, `armeabi-v7a`) |
| GPU | Mali-400 MP2 (solo OpenGL ES 2.0) |
| RAM | 1 GB |
| Storage | eMMC 8 GB (`/dev/block/mmcblk0`, 7634944 KB) |
| Wi-Fi | SSV6051 (`ssv6xxx`) — spesso senza monitor mode/injection |
| Display frontale | driver `FD628_Demo` |
| OS di fabbrica | Android 6.0.1 `userdebug`, **test-keys**, build `MV3.20170626` (2017) |
| Kernel | Linux 3.10 (vendor Rockchip) |
| SELinux | **permissive** |
| Root | `su` setuid già presente in `/system/xbin` + app "Superuser" |

Verifica (le etichette mentono spesso su RAM/Android/"4K"):

```sh
getprop | grep -E 'ro.board.platform|ro.product.(model|device)|ro.build.version|ro.rk.(cpu|soc)'
cat /proc/cpuinfo        # Hardware : Rockchip RK3229
cat /proc/meminfo        # MemTotal reale
```

---

## 2. Accesso: abilitare ADB via rete

Questi box non hanno "Debug wireless" (è di Android 11+). Con root preinstallato si
abilita ADB TCP a mano. Da un emulatore di terminale sul box (es. *Terminal Emulator
for Android* di jackpal, `jackpal.androidterm`; **non** Termux, che richiede Android 7+):

```sh
su
setprop persist.adb.tcp.port 5555   # persiste al riavvio
setprop service.adb.tcp.port 5555
stop adbd; start adbd
```

Dal PC: `adb connect <ip-box>:5555` e accettare il popup RSA sulla TV. Attenzione a non
confondere il box con altri dispositivi con ADB in rete (es. una smart TV): `adb
devices -l` mostra `model:` e `device:`.

Note (Windows + Git Bash): usare `MSYS_NO_PATHCONV=1` e path `C:/…` per `adb push`,
altrimenti i path assoluti `/data/...` vengono riscritti.

---

## 3. Analisi della ROM: la backdoor Adups FOTA

Il risultato più interessante. La ROM include **Adups FOTA** (`com.adups.fota` +
`com.adups.fota.sysoper`, quest'ultimo con UID `system`), un "servizio di
aggiornamento" che è in realtà una piattaforma di installazione silenziosa di app.

Decompilando `FotaUpdate.apk` (JADX) si trova (`com/adups/fota/a/b.java`) che gli URL
sono costruiti carattere per carattere per non comparire tra le stringhe:

```
https://fota5.adups.com   (e .cn)
/otainter-5.0/fota5/apkDownlod.do  apkUpgrade.do  apkGroup.do  submitReport.do ...
```

Comportamento verificato nel codice:

- Scarica APK indicate dal server e le **installa in silenzio** con `pm install -r`
  (`com/adups/fota/update/download/g.java` → `Command.with(context).sysRun("pm install -r " + path)`).
  Il modello `AdditionalModel` ha campi `advUserId`, `advertiseName`, `advApkId`:
  sono **app pubblicitarie di inserzionisti**.
- Esegue comandi arbitrari con privilegi `system` (`Command.sysRun`, bind a
  `com.adups.fota.sysoper`).
- **Disattiva la verifica app di Google**: `settings put global package_verifier_enable 0`.
- Invia al server **IMEI e MAC** (`ro.fota.id=mac`), versioni firmware, esiti
  installazioni; ha receiver su `PACKAGE_ADDED/REMOVED/REPLACED` e `BOOT_COMPLETED`.

Questa famiglia (Adups/FOTA) è nota per incidenti di raccolta dati di massa su device
economici. Qui è attiva di fabbrica.

### Riprodurre l'analisi

```sh
adb pull /system/app/FotaUpdate/FotaUpdate.apk
jadx -d out_fota FotaUpdate.apk
grep -rnE "adups|pm install|package_verifier|getDeviceId|sysRun" out_fota/sources
```

---

## 4. Pulizia del sistema (debloat)

Reversibile: `pm disable` si riattiva con `pm enable`.

```sh
su
for p in com.adups.fota com.adups.fota.sysoper android.rockchip.update.service \
         com.rockchip.devicetest com.ftest com.cghs.stresstest; do
  am force-stop $p; pm disable $p
done
settings put global package_verifier_enable 1          # ri-abilita la verifica app
mount -o remount,rw /system
printf '0.0.0.0 fota5.adups.com\n0.0.0.0 fota5.adups.cn\n0.0.0.0 adups.com\n' >> /system/etc/hosts
```

Alcune app "persistenti" (es. `StressTest`) ripartono anche da disabilitate: vanno
rimosse da `/system/priv-app/…` (tenere una copia).

---

## 5. Uso come console retro

### 5.1 RetroArch 32 bit

Il SoC è a 32 bit: serve la build **`RetroArch_ra32.apk`** (non aarch64) da
`buildbot.libretro.com/stable/<ver>/android/`.

Resa attesa (≈ Raspberry Pi 2/3): fluidi fino a NES/SNES/GBA/Mega Drive/PS1 e arcade
d'epoca. **N64/PSP/DC/Saturn** no. Super Mario 64 con Mupen64Plus-Next (impostazioni
per hardware debole, renderer **non** threaded) gira ~50 fps — ed è il limite massimo.

I core armv7 moderni possono richiedere Android 7+ (simboli `LIBC_N`): su Android 6
alcuni core non si caricano (`dlopen ... __aeabi_memclr4`). Se un core va in `SIGSEGV`
all'avvio video è il driver Mali-400 legacy: abbassare/disabilitare le opzioni GPU.

### 5.2 Driver custom per pad Xbox One / GIP (NACON Pro Compact)

Il kernel 3.10 **non ha** `xpad` per i pad GIP (Xbox One, incluso il NACON): il pad si
accende a metà, i LED restano spenti, Android non lo legge come gamepad. Soluzione: un
driver userspace ([`gipd/gipd.c`](gipd/gipd.c)) che:

1. reclama l'interfaccia USB via `usbdevfs` (serve root);
2. invia la sequenza di init GIP (power-on, LED on, … gli stessi pacchetti del driver
   `xpad` mainline). **Solo dopo questo i LED/logo del pad si accendono**;
3. ri-espone il pad via `uinput` come Xbox 360 (`045e:028e`), così Android usa il suo
   keylayout standard.

Build (cross, senza NDK, con [Zig](https://ziglang.org)): [`gipd/build.sh`](gipd/build.sh).
Avvio automatico: aggiunto a `/system/bin/preinstall.sh` (init, a ogni boot come root).

---

## 6. Backup completo (SEMPRE prima di toccare le partizioni)

Partizioni (`by-name`): `uboot, trust, misc, baseparamer, resource, kernel, boot,
recovery, backup, metadata, kpanic, system, userdata`. Dump di tutto tranne `userdata`
(rapido su una chiavetta USB nel box):

```sh
su
B=/dev/block/platform/30020000.rksdmmc/by-name
O=/mnt/media_rw/<UUID-chiavetta>/backup; mkdir -p $O
for p in uboot trust misc baseparamer resource kernel boot recovery backup metadata kpanic system; do
  dd if=$B/$p of=$O/$p.img bs=1M
done
cd $O && md5sum *.img > md5.txt
```

Verifica incrociata: `md5sum` del file == `md5sum` della partizione sul device.

---

## 7. Sostituire Android con Linux (Armbian) sulla eMMC

### 7.1 Vincoli importanti

- **"Ubuntu Server" ufficiale NON gira**: è solo arm64, il box è armv7 (32 bit).
  L'equivalente reale è **Armbian per `rk322x-box`** (userland Ubuntu/Debian a 32 bit),
  da `armbian.com/boards/rk322x-box` (immagini "current" = kernel mainline).
- **Il mainline può non partire da SD** su alcune schede (schermo nero, niente rete).
  Le immagini **legacy** (kernel 4.4 vendor) sarebbero più compatibili, ma **gli host
  storici sono morti** (users.armbian.com/jock, imola, hosthatch): reperibilità scarsa.
- Questi box **avviano da microSD prima della eMMC**.

### 7.2 Il Multitool

Ambiente di servizio legacy (di Paolo Sabatino / "jock") che su questi box parte di
sicuro **e mostra un menu su HDMI**. Modo supportato per backup/restore/scrittura
eMMC. Sorgente: `github.com/paolosabatino/multitool` (da compilare); immagine
precompilata dell'autore: `https://apt.undo.it:7243/multitool-rk322x.xz`.
Menu: `backup flash`, `restore flash`, `erase flash`, `burn image to flash`,
`drop to bash shell`, `reboot`, `shutdown`.

### 7.3 Scrivere un'immagine su microSD (Windows, senza software extra)

Decomprimere `.img.xz` con Python (`lzma`) e scrivere sul disco fisico della SD.
**Pericoloso: sbagliare disco cancella altri dati.** Script con controlli di sicurezza
(verifica bus USB, dimensione 2–128 GB, non boot/sistema):
[`scripts/flash_sd.ps1`](scripts/flash_sd.ps1). Punto chiave su Windows: i supporti
rimovibili non si mettono "offline"; va disabilitato l'automount con `diskpart`
(`automount disable` + `clean`) **prima** di scrivere, altrimenti Windows rimonta la
partizione a metà scrittura → "Accesso negato".

### 7.4 Flash della eMMC con un Multitool minimale (niente wget/curl)

Questo Multitool aveva **rootfs squashfs read-only** e **niente** `wget`, `curl`,
`busybox`, `telnetd`; `passwd`/`chpasswd` falliscono ("authentication token
manipulation error", `/etc/shadow` non scrivibile) → niente SSH classico.

Trucco: la shell è **bash**, che apre socket TCP via `/dev/tcp`. Niente programmi di
download: si streamma l'immagine grezza dal PC direttamente sul device eMMC. Sul PC un
server TCP "raw" ([`scripts/rawserve.py`](scripts/rawserve.py)) che manda i byte del
file **già decompresso**:

```sh
python rawserve.py 9000 Armbian_rk322x_resolute_minimal.img     # su questo PC (LAN)
```

Sul box (bash del Multitool), device da `cat /proc/partitions`:
- `mmcblk2` (con `mmcblk2boot0/boot1/rpmb`) = **eMMC interna** → target;
- `mmcblk0` = la microSD.

Poi, una riga, senza pipe:

```sh
cat < /dev/tcp/<ip-pc>/9000 > /dev/mmcblk2
sync
```

Poi `poweroff`, **togliere la microSD**, riaccendere → avvio dalla eMMC. L'immagine
Armbian è un'immagine disco completa (idbloader + u-boot + boot + root), quindi
scriverla su tutto `mmcblk2` installa anche il bootloader. Al primo avvio ridimensiona
il rootfs.

### 7.5 Accesso headless

Se parte con Ethernet (cavo + DHCP), si entra via SSH. Si ritrova il box cercando il
suo MAC in ARP e la porta 22 aperta. Se non compare mai in rete e lo schermo resta
nero → mainline senza driver per questa scheda: ripiego con `restore flash` (Android),
oppure Linux in chroot Debian/Ubuntu **sopra** Android (funziona sempre; costo: RAM).

### 7.6 Esito su questo esemplare

La stessa immagine mainline (Armbian "resolute", kernel 6.18) che **da microSD non
dava né video né rete**, scritta sulla **eMMC** (col bootloader incluso) è partita:
U-Boot a schermo, login su HDMI, Ethernet funzionante, Internet OK. Due dettagli:

- l'interfaccia di rete non è `eth0` ma **`end0`** (naming predicibile dei kernel
  recenti): cercare il box per IP/porta 22, non per `eth0`;
- il Wi-Fi SSV (`wlan0`) resta `DOWN` col mainline — si usa il cavo.

Morale: per questi box **non fermarsi al fallito boot da SD**, provare comunque la
scrittura su eMMC. Accesso finale headless via SSH a chiave (la rootfs Armbian è
scrivibile, quindi `authorized_keys` si installa con un `wget` dalla console).

Risultato: box da cassetto → mini server Linux headless (4 core, ~970 MB RAM, ~5,8 GB
liberi, ~2–3 W). Usi sensati: Pi-hole/AdGuard, WireGuard, Syncthing, piccoli servizi.

---

## 8. Contenuto del repo

```
.
├─ README.md                questo documento
├─ gipd/
│  ├─ gipd.c                driver userspace pad Xbox One/GIP → uinput 360
│  └─ build.sh              build cross con Zig
├─ scripts/
│  ├─ flash_sd.ps1          scrittura immagine su SD (Windows, con safety check)
│  └─ rawserve.py           server TCP raw per streammare un file sul box via /dev/tcp
└─ docs/img/                screenshot
```

Non inclusi nel repo (grandi o proprietari): immagini Armbian/Multitool, APK di
sistema, sorgenti decompilati, dump delle partizioni.

## 9. Strumenti e crediti

- Reverse engineering guidato con **REA — reverse-engineer-anything**
  (`github.com/morluto/rea`): inventario del device via ADB e il provider Android
  (jadx-headless-mcp) per la decompilazione.
- **JADX** per la decompilazione degli APK; **Zig** per la cross-compilazione del
  driver; **Armbian** (`rk322x-box`) e il **Multitool** di Paolo Sabatino per il
  passaggio a Linux.

## 10. Lezioni rapide

- Il nome commerciale non dice niente: identifica il SoC da `/proc/cpuinfo`.
- Diffida delle ROM di fabbrica: qui c'era Adups che installa app in silenzio e
  disattiva le verifiche di sicurezza.
- 1 GB di RAM + Mali-400 GLES2 = retro fino a PS1, non oltre; media center e piccolo
  server Linux sono gli usi più sensati.
- Ecosistema rk322x decaduto: il Multitool è l'ingresso affidabile, e `/dev/tcp` di
  bash salva quando mancano i tool di download.
- Backup completo **sempre** prima: il box è quasi impossibile da brickare (BootROM +
  maskrom via USB), ma recuperare senza backup è noioso.
