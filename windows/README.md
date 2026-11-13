# The OpenCore changes on Windows: IntelXe Manager for Windows and IntelXe-EFI.ps1

[Türkçe](#türkçe)

Stages 2 and 4 of the install guide (`README-INSTALL.md`) change the OpenCore EFI, which Windows can do as well as
macOS. There are two tools for it, which make the same changes by the same rules (`efi-rules.json`, also read by
`efi.sh` on macOS) with the same engine, back up the EFI folder in the same place and undo each other's changes:

- **IntelXe Manager for Windows** (`IntelXeManager-VERSION-win-x64.zip` on the release page, new in 0.1.5): the
  recommended way. A window with the plan of each change, a check of the PC and a diagnostics report.
- **`IntelXe-EFI.ps1`** (in `intel-gpu-macos-driver-VERSION-efi.zip`): the scripted way, for the command line,
  for scripting, or when the app cannot run (Windows PowerShell 5.1 is all it needs).

## IntelXe Manager for Windows

1. Download `IntelXeManager-VERSION-win-x64.zip` and `intel-gpu-macos-driver-VERSION-efi.zip` from the release page.
   Check the app's zip before you unpack it: its SHA-256 must be the one listed in `intel-gpu-macos-driver-VERSION.sha256`
   on the release page (`Get-FileHash .\IntelXeManager-VERSION-win-x64.zip -Algorithm SHA256` in PowerShell). That
   list is signed with the project's key (`.sha256.sig`, checked on macOS by IntelXe Manager or
   `swift release/signing.swift verify`).
2. Unpack both into one folder (right-click > Extract All) and start `IntelXeManager-VERSION-win-x64\IntelXeManager.exe`.
   Nothing is installed; the folder (about 205 MB) carries .NET, the Windows App SDK and the EFI builder's Python.
   Windows 10 (version 2004 or later) or 11, x64. The app finds the unpacked `-efi.zip` next to its folder; its EFI page also takes the zip or
   another folder.
3. **Unknown publisher.** The app has no code-signing certificate (the project has none), so SmartScreen may stop its
   first start ("Windows protected your PC": *More info* > *Run anyway*) and the administrator prompt says "Unknown
   publisher". That says only that Windows cannot tell who made it; it would look the same for a changed copy. The
   SHA-256 of step 1 is what tells you the zip is the release's.
4. **Administrator rights only for disks.** The app runs with your rights (`asInvoker`). Windows lets only
   administrators mount an EFI partition, so for each reading, plan or change of one the app starts its own exe again
   as a helper (`IntelXeManager.exe --efi-helper ...`, no window) with administrator rights, and Windows asks: once to
   read, once for a plan, once for the change. The helper mounts the partition at a free letter, does that one thing
   and unmounts it. An EFI folder on a USB stick or a copy needs no prompt. The Overview never asks; the Report page
   asks only with *Boot logs* (to read the EFI partitions). The scan of *Build the EFI* (`--efi-helper sniff`) and
   writing a USB stick (`--stick-helper`, `--probe-helper`) ask too.

Its pages:

- **Overview**: the card and its Windows driver, the CPU and whether `HybridPM.kext` applies, the board, UEFI and
  Secure Boot, Resizable BAR and Above 4G decoding, the Windows version, BitLocker, the EFI partitions. It only reads.
- **EFI (OpenCore)**: choose an EFI partition of the PC or an EFI folder, and the package (the `-efi.zip`); then stage
  2 (with the Safari setting if you want it), the Safari setting alone, stage 4 (`IntelXe.kext`, `SSDT-ARCOFF.aml`
  when you confirm the card's ACPI path, `HybridPM.kext` on a hybrid CPU), boot logging on and off (*Boot logging (for
  troubleshooting)*: OpenCore's log and saved panics on the EFI, for a report when macOS does not start) and undo. *Show the plan...* shows the plan's
  steps, its notes and its SHA-256; *Back up and apply* makes exactly that plan (the default button is *Cancel*),
  after a backup of the whole EFI folder, and refuses when the EFI changed since the plan was shown. Plans and results
  are shown in English or Turkish (the engine's own text stays English in the script).
- **Report**: runs `collect-windows.ps1` (shipped in the app's `tools\` folder; *Boot logs* adds OpenCore's logs from
  the EFI), shows the zip in Explorer and opens the issue forms with the report ID. Nothing is uploaded. It also
  writes the Xe probe stick and reads its reports (`tools/probe-stick/README.md`).
- **Set up** (experimental, the first page): the steps from this PC to an installed macOS, with **BIOS settings** (this
  board maker's menu names; it changes nothing), **Build the EFI** (a stage-1 OpenCore EFI made by OpCore Simplify at
  a pinned commit, stage 2 included) and **USB installer** (macOS's recovery from Apple, written with an EFI to a USB
  stick). Not yet run on Windows.
- **About**: the version, the licence, the language (Windows', English, Turkish) and the appearance.

Tested on Windows 11 on test EFI folders (the helper included, started through the administrator prompt) and by its
xUnit tests, which run the 44 shared vectors of the EFI rules through the same engine on Windows and macOS. **Not tested
yet: changing a real EFI partition** (the mounting runs only with a fake in the tests), and booting from an EFI either
tool changed. Keep a USB stick with a working EFI, and read each plan before you confirm it. The app's sources and
build are in `windows/app/` of the repository (`windows/app/README.md`).

## IntelXe-EFI.ps1

`IntelXe-EFI.ps1` makes the `config.plist` changes and copies the files of stages 2 and 4 of the install guide
(`README-INSTALL.md`) in an OpenCore EFI folder that **you** point it to: the EFI folder on a USB stick, or the EFI
partition of a disk. It does not build an EFI: you keep the one you made with Dortania's guide, and the tool changes
only what the driver needs, the rest of `config.plist` as it is (key order, comments, layout). These are the same
changes as `efi.sh` makes on macOS: both read them from `efi-rules.json`, which comes next to the script; keep it
there (`-RulesFile` names another copy).

It comes in `intel-gpu-macos-driver-VERSION-efi.zip`, next to `efi-rules.json`, `EFI/OC/Kexts/IntelXe.kext`,
`EFI/OC/Kexts/HybridPM.kext` and `EFI/OC/ACPI/SSDT-ARCOFF.aml`. It runs in Windows PowerShell 5.1 (part of Windows 10 and 11) or PowerShell 7; nothing
needs to be installed.

**Tested by its test suite only.** `windows/test/Test-IntelXe-EFI.ps1` runs every step against a throwaway EFI folder
and passes on the test PC in Windows PowerShell 5.1 (November 2026), and its config-editing engine was checked
on macOS against `efi.sh` (see [Testing](#testing)). Its `HybridPM.kext` and adapted `SSDT-ARCOFF.aml` steps ran on
Windows 11 in the shared vectors (12 November, through the app's tests, which compile this script's engine). The
window and the EFI partition mounting have not been used on an EFI that a PC then booted. Keep a USB stick with a
working EFI, and read each plan before you confirm it.

### Starting it

1. Unpack the `-efi.zip` (right-click > Extract All).
2. Right-click `IntelXe-EFI.ps1` > **Run with PowerShell**. A window opens.
   If Windows refuses to run it ("running scripts is disabled"), start it from a PowerShell window instead:

   ```powershell
   cd $HOME\Downloads\intel-gpu-macos-driver-VERSION-efi
   powershell -ExecutionPolicy Bypass -File .\IntelXe-EFI.ps1
   ```

   `-ExecutionPolicy Bypass` applies to that one run only. A file downloaded with a browser may also need
   `Unblock-File .\IntelXe-EFI.ps1` once.

**Administrator rights** are needed only for an EFI partition: Windows lets only administrators mount it and write
to it. A USB stick or a copy of an EFI folder works without them. The window's **Restart as administrator** button
starts it again elevated (Windows asks), with your folders filled in.

### Step by step (the window)

1. **EFI folder.** For a USB stick, *Browse...* to its `EFI` folder (the one with `OC` and `BOOT` in it), e.g.
   `E:\EFI`. For the EFI partition of a disk, press **Mount EFI partition...** (administrator): it lists the EFI
   partitions of the PC with their disk's name and size, mounts the one you choose with `mountvol` (nothing is
   written to it) and fills in `S:\EFI`. **Unmount EFI partition** takes the letter away again.
2. **Package folder** is filled in when the script sits in the unpacked `-efi.zip`.
3. **Backups**: leave it empty. Before each change the whole EFI folder is copied to a numbered, dated folder:
   next to the EFI folder (`E:\IntelXe-EFI-backups\...`), or, for an EFI partition (less than 2 GB, its space is for
   booting), to `%LOCALAPPDATA%\intel-gpu-macos-driver\efi-backups\`. Undo looks in the same place.
4. **Stage 2, any time after macOS is installed:** *Settings: plan* shows what changes; *Settings: apply...* shows the
   plan again and asks, then backs up and writes. It sets `csr-active-config` to at least `03080000` (a wider value
   stays), adds `amfi_get_out_of_my_way=0x1` and `intelxe_surfvram=2` to the boot-args (yours stay), puts
   `csr-active-config` and `boot-args` in `NVRAM > Delete`, and sets `ResizeAppleGpuBars = 10`,
   `DisableIoMapper = true`, `ReconnectGraphicsOnConnect = false`.
5. **Stage 4, after `sudo ./install.sh --apply` on macOS, before the reboot:** *Driver: plan* / *Driver: apply...*
   copy `IntelXe.kext` (when missing or different), add or enable its `Kernel > Add` entry, turn off an earlier
   `B580.kext` entry, and add 1 to `Misc > Security > ExposeSensitiveData` (so that macOS tools find the EFI
   OpenCore booted from).
   **On a 12th-14th gen Intel CPU with efficiency cores** (Alder Lake / Raptor Lake; not Core Ultra, not the desktop
   models with P-cores only) it also copies `HybridPM.kext` and adds or enables its `Kernel > Add` entry **right below
   `Lilu.kext`'s** (it is a Lilu plugin), as `efi.sh` does. The CPU is the one of the PC the tool runs on (its name
   as Windows reports it, `hybridpm_cpu=` in `-Status`): run it on the PC that boots the EFI. Without `Lilu.kext` in
   the EFI (its file and an enabled entry) it adds nothing and says so; an earlier build (`org.hackintosh.HybridPM`)
   is replaced; an entry above Lilu's gets a note.
6. **SSDT-ARCOFF.aml** is added only when you tick its box and give your card's ACPI path. **Not before macOS is
   installed:** the table hides the B580 from the display driver that the macOS installer runs on. Its path depends
   on the board; the shipped table is for `\_SB.PC00.PEG1.PEGP`. In Device Manager > Display adapters > the B580 (or
   "Microsoft Basic Display Adapter") > Properties > Details > **Location paths**, the line
   `ACPI(_SB_)#ACPI(PC00)#ACPI(PEG1)#ACPI(PEGP)#PCI(0100)#PCI(0000)` is `\_SB.PC00.PEG1.PEGP`; you can paste that
   line. *Read from Windows* reads it when Windows runs on the PC with the card. **For another path** the tool
   adapts the shipped table to it, as `efi.sh` does: it writes that path into both places of the table (and, for a
   path with more or fewer parts, the lengths), checks the result and copies it ("the package's table adapted to
   this card's port" in the plan); the bytes are those `efi.sh` writes. Your own `SSDT-ARCOFF.aml` that names the
   card's path stays. Only a table the tool cannot read as the shipped one is left to you (the plan says why).
7. **Undo last change...** puts back, from the newest backup, the files that change touched (`config.plist`,
   `IntelXe.kext`, `HybridPM.kext`, `SSDT-ARCOFF.aml`); each undo goes one change further back. It warns when `config.plist` was edited
   after that change (putting it back loses those edits). The backups stay.

Every change is planned first and carries the plan's SHA-256: if the EFI changed between the plan you read and your
confirmation, nothing is written. `config.plist` is edited in memory, read back and checked before it replaces the
old one; a damaged package (its files differ from its `SHA256SUMS`) is refused.

### Command line

```powershell
.\IntelXe-EFI.ps1 -Status -Efi E:\EFI
.\IntelXe-EFI.ps1 -Plan settings -Efi E:\EFI            # prints the plan and its SHA-256
.\IntelXe-EFI.ps1 -Apply settings -Efi E:\EFI -Expect <SHA-256>
.\IntelXe-EFI.ps1 -Plan driver -Efi E:\EFI [-CardAcpiPath '\_SB.PC00.PEG1.PEGP']
.\IntelXe-EFI.ps1 -Apply driver -Efi E:\EFI [-CardAcpiPath ...] -Expect <SHA-256>
.\IntelXe-EFI.ps1 -Plan safari -Efi E:\EFI              # SecureBootModel = Disabled, for the system-volume copy
.\IntelXe-EFI.ps1 -Apply safari -Efi E:\EFI -Expect <SHA-256>   # (settings-safari: with stage 2, one change)
.\IntelXe-EFI.ps1 -Plan bootlog -Efi E:\EFI             # OpenCore's log file and saved panics on the EFI (slows every boot)
.\IntelXe-EFI.ps1 -Apply bootlog -Efi E:\EFI -Expect <SHA-256>   # (bootlog-off: turn it off again)
.\IntelXe-EFI.ps1 -Plan undo -Efi E:\EFI
.\IntelXe-EFI.ps1 -Undo -Efi E:\EFI -Expect <SHA-256>
.\IntelXe-EFI.ps1 -ListEsp                              # administrator: the EFI partitions
.\IntelXe-EFI.ps1 -MountEsp '\\?\Volume{...}\'          # administrator; -MountEsp system = mountvol S: /S
.\IntelXe-EFI.ps1 -UnmountEsp S:
.\IntelXe-EFI.ps1 -Help
```

`-Package DIR` (default: the script's folder), `-BackupDir DIR` and `-RulesFile FILE` work with all of them. `safari`
sets `Misc > Security > SecureBootModel` to `Disabled`, which only the optional system-volume copy (Safari on the GPU,
`system-volume/` of the macOS package) needs, as `efi.sh --safari-plan` does on macOS; the window does not offer it.
`mountvol S: /S` mounts the EFI partition Windows booted from; when OpenCore is on another disk, use `-ListEsp` and
`-MountEsp` with its volume.

### Testing

`windows/test/` in the repository:

- `Test-IntelXe-EFI.ps1`, on Windows: runs the script, as a user would, against a throwaway EFI folder made from
  `efi/sample-z790/config.plist` as stage 1 leaves it (`stage1/config.plist`), in `%TEMP%`, without administrator
  rights: status, both plans, apply with a wrong hash and with the right one, undo step by step, the confirmed and
  unconfirmed ACPI path, a damaged package, a different kext, an earlier `B580.kext` entry, a wider SIP value, adding
  missing entries, `HybridPM.kext` with a hybrid CPU's name (the sample's disabled entry turned on; a new entry
  inserted below `Lilu.kext`; no Lilu), `SSDT-ARCOFF.aml` adapted to three other ports (byte for byte the tables
  `efi.sh` writes from the 0.1.3 table), the default backup place. Each `config.plist` it writes must hold the same
  keys and values as
  `efi.sh` writes for that step (`expected/efi-sh/`) and the same text as the engine writes on macOS
  (`expected/engine/`, layout kept). Run it with both PowerShells:

  ```powershell
  powershell -ExecutionPolicy Bypass -File windows\test\Test-IntelXe-EFI.ps1
  pwsh -File windows\test\Test-IntelXe-EFI.ps1
  ```

  `-RealPackage DIR` uses an unpacked package's `IntelXe.kext`, `HybridPM.kext` and `SSDT-ARCOFF.aml` instead of
  stand-ins. It also runs in PowerShell 7 on macOS (`dotnet tool install --tool-path DIR PowerShell`, then
  `DIR/pwsh -File windows/test/Test-IntelXe-EFI.ps1`), without the parts that need Windows: administrator rights,
  the card's Location path (`Get-PnpDevice`) and the CPU's name (CIM).
- `Test-EfiVectors.ps1`: the shared test vectors of the EFI rules (`release/efi-rules-vectors.json`; efi.sh runs the
  same ones with `release/test-efi-vectors.sh`) against the script's rules loader and engine: each vector's plan,
  notes, change and undo, the CPU names and the cards. `Test-IntelXe-EFI.ps1` runs it last; it also runs alone, on
  Windows or macOS.
- `make-expected.sh`, on macOS: makes `stage1/` and `expected/` with `efi.sh`, builds the script's engine (C# 5,
  compiled by `Add-Type` on Windows) with `dotnet` and checks it against them (`macos/EngineTest.cs`). `--check` only
  compares.
- `Test-AppInterop.ps1 -App ...\IntelXeManager.exe -Root DIR [-Package DIR]`, on Windows: a change made by the script
  is undone by IntelXe Manager for Windows (`windows/app`) and the other way round, on fake EFI folders made in `DIR`
  from `stage1/config.plist`, through the app's helper (`--efi-helper ... --folder`): the same backups, the same plan
  hashes. The app compiles the script's engine (`// BEGIN ENGINE` ... `// END ENGINE`) into its Core at build time, so
  an edit there changes both; its xUnit tests run the same vectors.

---

## Türkçe

Kurulum rehberinin (`README-INSTALL.tr.md`) 2. ve 4. aşamaları OpenCore EFI'sini değiştirir; Windows bunu macOS kadar
iyi yapabilir. Bunun için iki araç var. İkisi de aynı değişiklikleri aynı kurallarla (`efi-rules.json`, macOS'ta
`efi.sh` de onu okur) ve aynı çekirdekle yapar, EFI klasörünü aynı yere yedekler ve birbirinin değişikliklerini geri
alabilir:

- **Windows için IntelXe Manager** (sürüm sayfasında `IntelXeManager-SÜRÜM-win-x64.zip`, 0.1.5 ile yeni): önerilen yol.
  Her değişikliğin planını gösteren bir pencere, bilgisayarın denetimi ve bir tanılama raporu.
- **`IntelXe-EFI.ps1`** (`intel-gpu-macos-driver-SÜRÜM-efi.zip` içinde): betik yolu; komut satırı, otomasyon ya da
  uygulamanın çalışamadığı durumlar için (yalnızca Windows PowerShell 5.1 ister).

### Windows için IntelXe Manager

1. Sürüm sayfasından `IntelXeManager-SÜRÜM-win-x64.zip` ve `intel-gpu-macos-driver-SÜRÜM-efi.zip` dosyalarını indirin.
   Uygulamanın zip'ini açmadan önce denetleyin: SHA-256'sı sürüm sayfasındaki `intel-gpu-macos-driver-SÜRÜM.sha256`
   listesindekiyle aynı olmalı (PowerShell'de `Get-FileHash .\IntelXeManager-SÜRÜM-win-x64.zip -Algorithm SHA256`).
   Liste projenin anahtarıyla imzalı (`.sha256.sig`; macOS'ta IntelXe Manager ya da `swift release/signing.swift verify`
   denetler).
2. İkisini tek bir klasöre açın (sağ tık > Tümünü Ayıkla) ve `IntelXeManager-SÜRÜM-win-x64\IntelXeManager.exe`'yi
   başlatın. Hiçbir şey kurulmaz; klasör (yaklaşık 205 MB) .NET'i, Windows App SDK'yı ve EFI oluşturucunun Python'unu
   kendi içinde taşır. Windows 10 (2004 ya da sonrası) veya 11, x64. Uygulama açılmış `-efi.zip`'i kendi klasörünün yanında bulur; EFI sayfası zip'in
   kendisini ya da başka bir klasörü de alır.
3. **Bilinmeyen yayımcı.** Uygulamanın kod imzalama sertifikası yok (projenin yok); bu yüzden SmartScreen ilk açılışı
   durdurabilir ("Windows bilgisayarınızı korudu": *Ek bilgi* > *Yine de çalıştır*) ve yönetici izni penceresi
   "Bilinmeyen yayımcı" der. Bu yalnızca Windows'un onu kimin yaptığını bilemediğini söyler; değiştirilmiş bir kopya da
   aynı görünür. Zip'in sürümünki olduğunu 1. adımdaki SHA-256 söyler.
4. **Yönetici hakları yalnızca diskler için.** Uygulama sizin haklarınızla çalışır (`asInvoker`). Windows bir EFI
   bölümünü yalnızca yöneticilerin bağlamasına izin verir; bu yüzden uygulama bir bölümü her okuyuşunda, planlayışında
   ya da değiştirişinde kendi exe'sini yardımcı olarak (`IntelXeManager.exe --efi-helper ...`, penceresiz) yönetici
   haklarıyla yeniden başlatır ve Windows sorar: okumak için bir, plan için bir, değişiklik için bir kez. Yardımcı
   bölümü boş bir harfe bağlar, o tek işi yapar ve bağlantıyı kaldırır. USB bellekteki bir EFI klasörü ya da bir kopya
   için sorulmaz. Genel bakış hiç sormaz; Rapor sayfası yalnızca *Açılış günlükleri* ile sorar (EFI bölümlerini okumak
   için). *EFI'yi oluştur*'un taraması (`--efi-helper sniff`) ve bir USB belleğe yazmak (`--stick-helper`,
   `--probe-helper`) da sorar.

Sayfaları:

- **Genel bakış**: kart ve Windows sürücüsü, işlemci ve `HybridPM.kext` gerekip gerekmediği, anakart, UEFI ve Secure
  Boot, Resizable BAR ve Above 4G decoding, Windows sürümü, BitLocker, EFI bölümleri. Yalnızca okur.
- **EFI (OpenCore)**: bilgisayarın bir EFI bölümünü ya da bir EFI klasörünü ve paketi (`-efi.zip`) seçin; sonra 2. aşama
  (isterseniz Safari ayarıyla birlikte), yalnız Safari ayarı, 4. aşama (`IntelXe.kext`, kartın ACPI yolunu
  onayladığınızda `SSDT-ARCOFF.aml`, hibrit işlemcide `HybridPM.kext`), açılış günlüğünü açıp kapatma (*Açılış günlüğü
  (sorun giderme için)*: macOS açılmadığında bir rapor için OpenCore'un günlüğü ve kaydedilen panikler EFI'de) ve geri
  alma. *Planı göster...* planın
  adımlarını, notlarını ve SHA-256'sını gösterir; *Yedekle ve uygula* tam olarak o planı, EFI klasörünün tamamını
  yedekledikten sonra yapar (varsayılan düğme *İptal*) ve plan gösterildikten sonra EFI değiştiyse reddeder. Planlar
  ve sonuçlar Türkçe ya da İngilizce gösterilir (betikte çekirdeğin kendi metni İngilizce kalır).
- **Rapor**: `collect-windows.ps1`'i (uygulamanın `tools\` klasöründe gelir; *Açılış günlükleri* EFI'deki OpenCore
  günlüklerini de ekler) çalıştırır, zip'i Gezgin'de gösterir ve issue formlarını rapor kimliğiyle açar. Hiçbir şey
  yüklenmez. Xe probe stick'i de yazar ve raporlarını okur (`tools/probe-stick/README.md`).
- **Kurulum** (deneysel, ilk sayfa): bu bilgisayardan kurulu bir macOS'a giden adımlar; **BIOS ayarları** (bu anakart
  üreticisinin menü adları; hiçbir şeyi değiştirmez), **EFI'yi oluştur** (sabitlenmiş bir commit'teki OpCore Simplify
  ile 1. aşamanın OpenCore EFI'si, 2. aşama içinde) ve **USB kurulum belleği** (Apple'dan macOS kurtarma görüntüsü, bir
  EFI ile USB belleğe yazılır). Henüz Windows'ta çalışmadı.
- **Hakkında**: sürüm, lisans, dil (Windows'unki, İngilizce, Türkçe) ve görünüm.

Windows 11'de deneme EFI klasörlerinde (yönetici izni penceresiyle başlatılan yardımcı dahil) ve EFI kurallarının 44
ortak vektörünü aynı çekirdekle Windows'ta ve macOS'ta çalıştıran xUnit testleriyle denendi. **Henüz denenmedi: gerçek
bir EFI bölümünü değiştirmek** (bağlama testlerde yalnızca sahte bir bağlayıcıyla çalışıyor) ve iki aracın birinin
değiştirdiği bir EFI'den açmak. Çalışan bir EFI'si olan USB belleği elinizde tutun ve her planı onaylamadan önce okuyun.
Uygulamanın kaynakları ve derlemesi depodaki `windows/app/` altında (`windows/app/README.md`).

### IntelXe-EFI.ps1

`IntelXe-EFI.ps1`, kurulum rehberinin (`README-INSTALL.tr.md`) 2. ve 4. aşamalarındaki `config.plist` değişikliklerini
ve dosya kopyalarını, **sizin** gösterdiğiniz bir OpenCore EFI klasöründe yapar: bir USB belleğin EFI klasörü ya da bir
diskin EFI bölümü. EFI oluşturmaz: Dortania rehberiyle hazırladığınız EFI sizde kalır, araç yalnızca sürücünün
gerektirdiklerini değiştirir; `config.plist`'in geri kalanı olduğu gibi kalır (anahtar sırası, açıklamalar, düzen).
macOS'ta `efi.sh`'in yaptığı değişikliklerin aynısıdır: ikisi de bunları betiğin yanında gelen `efi-rules.json`'dan
okur; o dosya betiğin yanında kalmalı (`-RulesFile` başka bir kopyasını gösterir).

`intel-gpu-macos-driver-SÜRÜM-efi.zip` içinde, `efi-rules.json`, `EFI/OC/Kexts/IntelXe.kext`, `EFI/OC/Kexts/HybridPM.kext` ve
`EFI/OC/ACPI/SSDT-ARCOFF.aml` ile birlikte gelir. Windows PowerShell 5.1'de (Windows 10 ve 11'de hazır gelir) ya da PowerShell 7'de çalışır; hiçbir şey kurmak
gerekmez.

**Yalnızca deneme takımıyla denendi.** `windows/test/Test-IntelXe-EFI.ps1` her adımı geçici bir EFI klasöründe
çalıştırıyor ve test bilgisayarında Windows PowerShell 5.1'de hatasız geçti (Kasım 2026); `config.plist`
düzenleyen çekirdeği de macOS'ta `efi.sh`'e karşı denendi (bkz. [Deneme](#deneme)). `HybridPM.kext` ve uyarlanan
`SSDT-ARCOFF.aml` adımları Windows 11'de ortak vektörlerde çalıştı (12 Kasım, uygulamanın testleriyle; uygulama bu
betiğin çekirdeğini derler). Pencere ve EFI bölümünü bağlama,
ardından bilgisayarın açıldığı bir EFI üzerinde henüz kullanılmadı. Çalışan bir EFI'si olan USB belleği elinizde tutun
ve her planı onaylamadan önce okuyun.

#### Başlatmak

1. `-efi.zip`'i açın (sağ tık > Tümünü Ayıkla).
2. `IntelXe-EFI.ps1`'e sağ tık > **PowerShell ile Çalıştır**. Bir pencere açılır.
   Windows çalıştırmayı reddederse ("betik çalıştırma devre dışı") bir PowerShell penceresinden başlatın:

   ```powershell
   cd $HOME\Downloads\intel-gpu-macos-driver-SÜRÜM-efi
   powershell -ExecutionPolicy Bypass -File .\IntelXe-EFI.ps1
   ```

   `-ExecutionPolicy Bypass` yalnızca o çalıştırma için geçerlidir. Tarayıcıyla indirilen dosya için bir kez
   `Unblock-File .\IntelXe-EFI.ps1` de gerekebilir.

**Yönetici hakları** yalnızca EFI bölümü için gerekir: Windows bu bölümü yalnızca yöneticilerin bağlamasına ve yazmasına
izin verir. USB bellek ya da bir EFI klasörünün kopyası için gerekmez. Penceredeki **Restart as administrator** düğmesi
aracı klasörleriniz doldurulmuş olarak yönetici olarak yeniden başlatır (Windows izin ister).

#### Adım adım (pencere)

1. **EFI folder.** USB bellek için *Browse...* ile `EFI` klasörünü seçin (içinde `OC` ve `BOOT` olan), örneğin `E:\EFI`.
   Bir diskin EFI bölümü için **Mount EFI partition...** (yönetici): bilgisayardaki EFI bölümlerini diskin adı ve
   boyutuyla listeler, seçtiğinizi `mountvol` ile bağlar (bölüme hiçbir şey yazılmaz) ve `S:\EFI`'yi doldurur.
   **Unmount EFI partition** sürücü harfini geri kaldırır.
2. **Package folder**, betik açılmış `-efi.zip`'in içindeyse kendiliğinden dolar.
3. **Backups**: boş bırakın. Her değişiklikten önce EFI klasörünün tamamı numaralı ve tarihli bir klasöre kopyalanır:
   EFI klasörünün yanına (`E:\IntelXe-EFI-backups\...`) ya da EFI bölümü için (2 GB'tan küçük; alanı açılış içindir)
   `%LOCALAPPDATA%\intel-gpu-macos-driver\efi-backups\` altına. Geri alma da aynı yere bakar.
4. **2. aşama, macOS kurulduktan sonra istediğiniz zaman:** *Settings: plan* nelerin değişeceğini gösterir;
   *Settings: apply...* planı yeniden gösterip sorar, sonra yedekleyip yazar. `csr-active-config`'i en az `03080000`
   yapar (daha geniş bir değer kalır), boot-args'a `amfi_get_out_of_my_way=0x1` ve `intelxe_surfvram=2` ekler (sizinkiler
   kalır), `csr-active-config` ve `boot-args`'ı `NVRAM > Delete`'e koyar; `ResizeAppleGpuBars = 10`,
   `DisableIoMapper = true`, `ReconnectGraphicsOnConnect = false` yapar.
5. **4. aşama, macOS'ta `sudo ./install.sh --apply`'dan sonra, yeniden başlatmadan önce:** *Driver: plan* /
   *Driver: apply...* `IntelXe.kext`'i kopyalar (yoksa ya da farklıysa), `Kernel > Add` kaydını ekler ya da açar,
   eski bir `B580.kext` kaydını kapatır ve `Misc > Security > ExposeSensitiveData`'ya 1 ekler (macOS araçları
   OpenCore'un açıldığı EFI'yi bulabilsin diye).
   **Verimlilik çekirdekli 12.-14. nesil Intel işlemcide** (Alder Lake / Raptor Lake; Core Ultra ve yalnızca P
   çekirdekli masaüstü modelleri değil) `efi.sh` gibi `HybridPM.kext`'i de kopyalar ve `Kernel > Add` kaydını
   **`Lilu.kext`'in kaydının hemen altına** ekler ya da açar (bir Lilu eklentisidir). İşlemci, aracın çalıştığı
   bilgisayarınkidir (Windows'un bildirdiği adıyla; `-Status`'ta `hybridpm_cpu=`): aracı EFI'yi açacak bilgisayarda
   çalıştırın. EFI'de `Lilu.kext` yoksa (dosyası ve açık bir kaydı) hiçbir şey eklemez ve bunu söyler; eski bir
   derleme (`org.hackintosh.HybridPM`) değiştirilir; Lilu'nun üstündeki bir kayıt için not düşer.
6. **SSDT-ARCOFF.aml** yalnızca kutusunu işaretleyip kartınızın ACPI yolunu yazarsanız eklenir. **macOS kurulmadan
   önce değil:** bu tablo B580'i macOS kurulum programının çalıştığı ekran sürücüsünden gizler. Yolu anakarta göre
   değişir; paketteki tablo `\_SB.PC00.PEG1.PEGP` içindir. Aygıt Yöneticisi > Görüntü bağdaştırıcıları > B580 (ya da
   "Microsoft Temel Görüntü Bağdaştırıcısı") > Özellikler > Ayrıntılar > **Konum yolları**'ndaki
   `ACPI(_SB_)#ACPI(PC00)#ACPI(PEG1)#ACPI(PEGP)#PCI(0100)#PCI(0000)` satırı `\_SB.PC00.PEG1.PEGP` demektir; satırı olduğu
   gibi yapıştırabilirsiniz. *Read from Windows*, Windows kartın takılı olduğu bilgisayarda çalışıyorsa yolu okur.
   **Başka bir yolda** araç, `efi.sh` gibi paketteki tabloyu o yola uyarlar: yolu tablodaki iki yere yazar (daha
   çok ya da daha az parçalı bir yolda uzunlukları da), sonucu denetler ve kopyalar (planda "the package's table
   adapted to this card's port"); baytlar `efi.sh`'in yazdıklarıyla aynıdır. Kartın yolunu adlandıran kendi
   `SSDT-ARCOFF.aml`'niz kalır. Yalnızca paketteki tablo gibi okuyamadığı bir tablo size kalır (plan nedenini söyler).
7. **Undo last change...** en yeni yedekten, o değişikliğin dokunduğu dosyaları (`config.plist`, `IntelXe.kext`,
   `HybridPM.kext`, `SSDT-ARCOFF.aml`) geri koyar; her geri alma bir değişiklik daha geriye gider. O değişiklikten sonra `config.plist`
   düzenlendiyse uyarır (geri koymak o düzenlemeleri siler). Yedekler kalır.

Her değişiklik önce planlanır ve planın SHA-256'sını taşır: okuduğunuz planla onayınız arasında EFI değiştiyse hiçbir
şey yazılmaz. `config.plist` bellekte düzenlenir, yerine konmadan önce yeniden okunup denetlenir; dosyaları
`SHA256SUMS` ile uyuşmayan (bozuk) bir paket reddedilir.

#### Komut satırı

Komutlar yukarıdaki İngilizce bölümdekilerin aynısıdır (`-Status`, `-Plan settings|driver|safari|settings-safari|bootlog|bootlog-off|undo`,
`-Apply settings|driver|safari|settings-safari|bootlog|bootlog-off -Expect <SHA-256>`, `-Undo -Expect <SHA-256>`, `-ListEsp`, `-MountEsp`, `-UnmountEsp`,
`-Help`). `mountvol S: /S` Windows'un açıldığı EFI bölümünü bağlar; OpenCore başka bir diskteyse `-ListEsp` ve o
bölümün birimiyle `-MountEsp` kullanın.

#### Deneme

Depodaki `windows/test/`: `Test-IntelXe-EFI.ps1` Windows'ta betiği, `efi/sample-z790/config.plist`'ten 1. aşama
hâline getirilmiş geçici bir EFI klasörüne karşı çalıştırır (yönetici hakkı gerekmez) ve yazdığı her `config.plist`'i
`efi.sh`'in sonucuyla (aynı anahtarlar ve değerler) ve çekirdeğin macOS'taki çıktısıyla (aynı metin) karşılaştırır;
hibrit bir işlemci adıyla `HybridPM.kext`'i ve üç başka yola uyarlanan `SSDT-ARCOFF.aml`'yi de dener (tablolar,
`efi.sh`'in yazdıklarıyla bayt bayt aynı olmalı):

```powershell
powershell -ExecutionPolicy Bypass -File windows\test\Test-IntelXe-EFI.ps1
pwsh -File windows\test\Test-IntelXe-EFI.ps1
```

`make-expected.sh` macOS'ta bu karşılaştırma dosyalarını `efi.sh` ile üretir ve betiğin çekirdeğini `dotnet` ile
derleyip onlara karşı dener. `Test-EfiVectors.ps1`, EFI kurallarının ortak deneme vektörlerini
(`release/efi-rules-vectors.json`; macOS'ta `efi.sh` aynılarını `release/test-efi-vectors.sh` ile çalıştırır) betiğin
kural okuyucusu ve çekirdeğiyle dener; `Test-IntelXe-EFI.ps1` onu en sonda çalıştırır. `Test-AppInterop.ps1`
Windows'ta betiğin yaptığı bir değişikliği Windows için IntelXe Manager'a (`windows/app`) geri aldırır, uygulamanınkini
de betiğe (sahte EFI klasörlerinde, uygulamanın `--efi-helper ... --folder` yardımcısıyla); uygulama betiğin çekirdeğini
derleme sırasında kendi Core'una alır. `Test-IntelXe-EFI.ps1` macOS'ta PowerShell 7 ile de çalışır (`dotnet tool install
--tool-path DİZİN PowerShell`); Windows gerektiren kısımlar (yönetici hakkı, kartın Location path'i, işlemci adı)
atlanır.
