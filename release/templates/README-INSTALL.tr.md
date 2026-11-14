# intel-gpu-macos-driver @VER@: @MACOS@ için kurulum (Intel Arc B580 / B570)

[English](README-INSTALL.md)

@TESTED_TR@ Sürücü imzasız ve destek verilmiyor; üstelik bilgisayarınızın macOS'u nasıl açtığını değiştiriyor.
**Başlamadan önce çalışan bir OpenCore EFI'si olan bir USB bellek hazırlayın** ve anakartınızın önyükleme menüsünden o
belleği nasıl seçeceğinizi öğrenin. Yalnızca ticari olmayan kullanım içindir (`LICENSES/`).

Her şey yolunda giderse elinize şunlar geçer: masaüstü kartta Metal ile çizilir (pencere sürüklemesi monitörün yenileme
hızında, 180 Hz'e kadar denendi), masaüstü widget'ları, Metal uygulamaları ve oyunlar çalışır, Safari
GPU'da çalışır (sistem diski kopyasıyla, 3.3), H.264, HEVC, AV1 ve VP9 çözme ile H.264, HEVC ve AV1 kodlama GPU'da yapılır.
DisplayPort'tan tek monitör, VRR, uyku ve uyanma da çalışır. İsteğe bağlı olarak Minecraft Java gibi LWJGL oyunları
OpenGL'i Mesa'nın zink'iyle çalıştırabilir (`-zink-gl.zip`, aşağıda "İsteğe bağlı: zink ile OpenGL"); 12.-14. nesil hibrit
Intel işlemciler için `HybridPM.kext` var (aşağıda).

Bilinen sorunlar:

- Girişten sonraki ilk saniyeler sonrasına göre daha az akıcı (0.1.3'ten beri daha az: WindowServer derlenmiş
  shader'larını saklıyor; kurulumdan sonraki ilk giriş onları bir kez derliyor).
- Yeniden başlatmadan sonra giriş ekranında açık gri bir bant görünüyor.
- Metal'i VideoToolbox kodlamasıyla başlatan ffmpeg benzeri araçlar, ilk oturumlarında kodlayıcılarımızı görmüyor.
- Kartın 12 GB belleğinin yalnızca 1 GB'ını işlemci görebiliyor (macOS sınırı, 2. aşama 1. madde). Sürücü bu pencereyi
  ihtiyacı olanlara ayırıyor, arka plan süreçlerinin arabelleklerini sistem belleğine koyuyor; yine de GPU'yu yoğun
  kullanan çok sayıda açık uygulama pencereyi doldurabilir.

Bu sürücünün eski bir sürümünü kullanıyorsanız doğrudan [Yükseltme](#yükseltme) bölümüne geçin.

## Neler gerekiyor?

- OpenCore'un macOS açabildiği bir bilgisayar (Intel işlemci, x86_64), Arc B580, B570, Pro B60 ya da Pro B50 ve **kartın
  DisplayPort çıkışına bağlı tek monitör**. B570, Pro B60 (8086:e211) ve Pro B50 (8086:e212) destekleniyor ama bizim
  tarafımızda test edilmedi: nasıl gittiğini bildirin. Dahili GPU gerekmiyor: test makinesinde macOS yalnızca B580'le
  kuruldu.
- USB belleği hazırlamak ve EFI'yi düzenlemek için Windows, Linux ya da macOS çalışan herhangi bir bilgisayar. Mac şart
  değil.
- [Sürüm sayfasından](https://github.com/yusufklncc/intel-gpu-macos-driver/releases/tag/v@VER@) dosyalar:

  | Dosya | İçinde ne var | Nerede açılmalı |
  |---|---|---|
  | `intel-gpu-macos-driver-@VER@-efi.zip` | OpenCore parçaları: `IntelXe.kext`, `SSDT-ARCOFF.aml` ve kaynağı, `HybridPM.kext` (isteğe bağlı, [hibrit Intel işlemciler](#hibrit-intel-işlemciler-12-14-nesil-hybridpmkext)); Windows'ta 2. ve 4. aşamaları yapan betik `IntelXe-EFI.ps1` | herhangi bir sistemde |
  | `IntelXeManager-@VER@-win-x64.zip` | Windows için IntelXe Manager: 2. ve 4. aşamalar düğmelerle, bilgisayarın denetimi ve tanılama raporu ([aşağıda](#windowsta-windows-için-intelxe-manager)); `-efi.zip`'i kullanır | Windows 10 ya da 11 (x64) |
  | `IntelXe-Manager-@VER@.zip` | IntelXe Manager uygulaması: 3. ve 4. aşamalar (ve sonraki güncellemeler) Terminal yerine düğmelerle; sonraki dosyayı kendisi indirir | bilgisayardaki macOS'ta |
  | `intel-gpu-macos-driver-@VER@-@CODENAME@.zip` | geri kalan her şey, `install.sh` ve `efi.sh` ile | **bilgisayardaki macOS'ta** (3. aşama) |
  | `intel-gpu-macos-driver-@VER@-zink-gl.zip` | isteğe bağlı: LWJGL oyunları için zink ile OpenGL | bilgisayardaki macOS'ta, 4. aşamadan sonra |
  | `intel-gpu-macos-driver-@VER@-vulkan.zip` | isteğe bağlı, deneysel (sürümde varsa): Wine oyunları için Vulkan (DXVK, vkd3d-proton) | bilgisayardaki macOS'ta, 4. aşamadan sonra |

  macOS dosyalarını yalnızca macOS'ta açın. Windows ve çoğu Linux arşiv aracı, çalıştırma izinlerini ve kurulum betiğinin
  kontrol ettiği kod imzalarını bozuyor. Sürüm sayfasındaki `.sha256` dosyası sağlama toplamlarını listeliyor,
  `.sha256.sig` de projenin anahtarıyla imzası (IntelXe Manager denetliyor; elle:
  `swift release/signing.swift verify <app/Sources/Signing.swift'teki anahtar> <.sha256> <.sha256.sig>`).
- GPU dışındaki her şey için [Dortania'nın OpenCore rehberi](https://dortania.github.io/OpenCore-Install-Guide/): anakartınıza
  uygun EFI, ACPI tabloları, SMBIOS, USB haritası.
- Sisteminize uygun bir `config.plist` düzenleyicisi, örneğin [ProperTree](https://github.com/corpnewt/ProperTree) (Windows,
  Linux ve macOS'ta çalışır, Python ister).

## Aşamalar

| Aşama | Nerede yapılıyor | Ne yapılıyor | Sonrasında ekran |
|---|---|---|---|
| 1 | Windows, Linux ya da macOS, sonra kurulacak bilgisayar | macOS'u sürücüsüz, düz bir OpenCore EFI'siyle kurmak | kartın UEFI çıktısı: hızlandırma yok, yavaş |
| 2 | `config.plist`, herhangi bir sistemden | SIP, AMFI, iki quirk ve bir çıktı ayarı | aynı |
| 3 | Bilgisayardaki macOS | uygulamada **Bu paketi kur** ya da `sudo ./install.sh --apply`; **henüz yeniden başlatmadan** | aynı |
| 4 | EFI, bilgisayardaki macOS'tan ya da herhangi bir sistemden | `IntelXe.kext` ve `SSDT-ARCOFF.aml`, hibrit bir Intel işlemcide `HybridPM.kext` de (uygulamanın EFI sayfası, `efi.sh`, Windows için IntelXe Manager, `IntelXe-EFI.ps1` ya da elle), sonra yeniden başlatma | hızlandırmalı |

`IntelXe.kext` ve `SSDT-ARCOFF.aml`'yi 4. aşamadan önce EFI'ye koymayın. Bu tablo, kurulum ekranını gösteren genel ekran
sürücüsünden kartı gizliyor; 3. aşamada sürücünün kendi ekran kext'i kurulana kadar onun yerini alacak bir şey yok.

Her aşamanın sonundaki durumda test makinesi çalıştı, ama bu dört aşama temiz bir kurulumda baştan sona hiç uygulanmadı.
Takıldığınız yeri bildirirseniz rehberi düzeltiriz.

## Windows'ta: Windows için IntelXe Manager

2. ve 4. aşamalar EFI'yi değiştirir; bunu aynı bilgisayarda ya da başka bir bilgisayarda Windows'tan yapabilirsiniz.
Orada önerilen yol **Windows için IntelXe Manager** (`IntelXeManager-@VER@-win-x64.zip`); `-efi.zip`'teki
`IntelXe-EFI.ps1` aynı değişiklikleri bir betikle ya da komut satırından yapar. İkisi de aynı kuralları okur
(`efi-rules.json`, macOS'taki `efi.sh` gibi), bir değişiklikten önce EFI klasörünün tamamını yedekler ve birbirinin
değişikliklerini geri alabilir.

1. Sürüm sayfasından `IntelXeManager-@VER@-win-x64.zip` ve `intel-gpu-macos-driver-@VER@-efi.zip` dosyalarını indirin
   ve **önce uygulamanın zip'ini denetleyin**: SHA-256'sı, sürüm sayfasındaki `intel-gpu-macos-driver-@VER@.sha256`
   listesindekiyle aynı olmalı. PowerShell'de:

   ```powershell
   Get-FileHash .\IntelXeManager-@VER@-win-x64.zip -Algorithm SHA256
   ```

   Listenin kendisi projenin anahtarıyla imzalı (`.sha256.sig`); Windows'ta bu imza için bir araç yok, yukarıdaki
   `swift release/signing.swift verify` satırı onu macOS'ta denetler.
2. İki zip'i aynı klasöre açın (sağ tık > Tümünü Ayıkla) ve `IntelXeManager-@VER@-win-x64` içindeki
   `IntelXeManager.exe`'yi başlatın. Hiçbir şey kurulmaz: klasör .NET'i, Windows App SDK'yı ve EFI oluşturucunun
   Python'unu kendi içinde taşır (yaklaşık 205 MB). Açılmış `-efi.zip`'i kendi klasörünün yanında bulur; EFI sayfasındaki *-efi.zip dosyasını seç...*
   zip'in kendisini de alır.
3. **Uygulama bir kod imzalama sertifikasıyla imzalı değil** (projenin sertifikası yok). Bu yüzden Windows yayıncısını
   bilmez: SmartScreen "Windows bilgisayarınızı korudu" diyebilir (*Ek bilgi* > *Yine de çalıştır*), yönetici izni
   penceresi de "Bilinmeyen yayımcı" gösterir. Windows değiştirilmiş bir kopya için de aynısını gösterir; yani bu
   kopyanın projeninki olup olmadığını size söyleyemez, 1. adımdaki SHA-256 söyler. Sürüm sayfasından başka bir yerden
   gelen bir kopyayı çalıştırmayın.
4. **Yönetici hakları yalnızca diskler için.** Uygulama sizin haklarınızla çalışır; Genel bakış sayfası bilgisayarı
   onlarsız okur. Windows bir EFI bölümünü yalnızca yöneticilerin bağlamasına izin verir; bu yüzden uygulama bir bölümü
   her okuyuşunda, planlayışında ve değiştirişinde kendi yardımcısını (`IntelXeManager.exe --efi-helper`) yönetici
   haklarıyla başlatır: Windows her seferinde sorar (bir değişiklik iki kez sorar: planı için ve değişiklik için). USB
   bellekteki bir EFI klasörü ya da bir kopyası için hiç sorulmaz. Aynı yardımcı *EFI'yi oluştur* için bilgisayarı da
   tarar (`--efi-helper sniff`); bir USB belleğe yazmak da sorar (`--stick-helper`, `--probe-helper`). Başka bir anda
   sorarsa hayır deyin ve bir issue açın.

Uygulamanın sayfaları: **Genel bakış** (kart ve sürücüsü, işlemci ve `HybridPM.kext` gerekip gerekmediği, 1. aşamanın
firmware ayarları, BitLocker, EFI bölümleri; hiçbir şeyi değiştirmez), **EFI** (2. aşama, Safari ayarı, 4. aşama ve geri
alma: her biri önce planını gösterir, adımlarını, notlarını ve planın SHA-256'sını; yalnızca siz onaylayınca değiştirir;
açılış günlüğü de, [aşağıda](#açılmazsa)), **Rapor** (`tools/report/collect-windows.ps1`'i çalıştırır, *Açılış
günlükleri* ile EFI'deki OpenCore günlüklerini de ekler ve issue formunu açar; Xe probe stick'i de yazar ve okur,
[Sorun bildirmek](#sorun-bildirmek)). Yalnızca EFI sayfasında seçilen EFI klasörüne ya da bölümüne ve adını ve boyutunu
onayladığınız bir USB belleğe yazar, hiçbir şey yüklemez. Windows 11'de deneme EFI klasörlerinde, yönetici yardımcısıyla
birlikte denendi; **gerçek bir EFI bölümünü değiştirmek ve onun değiştirdiği bir EFI'den açmak henüz denenmedi**, bu
yüzden ilk paragraftaki USB belleği elinizde tutun. Betiği `-efi.zip`'teki `README-Windows.md` anlatır.

**Kurulum** (deneysel) uygulamanın ilk sayfası: bu bilgisayardan kurulu bir macOS'a giden yol, her biri bitti, sıradaki
ya da engelli olarak gösterilen adımlarla. Sayfaları: **BIOS ayarları** (1. aşamanın 1. maddesindeki firmware ayarları
için bu anakart üreticisinin menü adları; her biri bitti, yapılacak ya da "Windows'tan anlaşılamıyor" olarak işaretli;
hiçbir ayarı değiştirmez), **EFI'yi oluştur** (bu bilgisayar için 1. aşamanın OpenCore EFI'si; sabitlenmiş bir
commit'teki OpCore Simplify ile, sabitlenmiş indirmelerden, 2. aşama içinde ve `ocvalidate` ile denetlenmiş; tarama
yalnızca DSDT, SSDT, APIC ve DMAR tablolarını tutar, üretilen seri numaraları maskeli kalır) ve **USB kurulum belleği**
(Apple'ın sunucularından macOS kurtarma görüntüsü, Apple'ın chunklist'ine karşı denetlenir ve bir EFI ile USB belleğe
yazılır). **Bir Windows test bilgisayarında (14 Kasım) `ocvalidate`'in geçtiği bir EFI oluşturdular ve geri okumada
aynı çıkan gerçek bir kurulum belleği yazdılar; ama yazdıkları bir bellek henüz bir bilgisayarı açmadı**: denenmiş yol
aşağıdaki elle yapılan 1. aşama.

## 1. macOS'u düz bir OpenCore EFI'siyle kurmak

1. **BIOS:** Above 4G Decoding **açık**, Resizable BAR **açık**, CSM **kapalı**. Gerisini Dortania'nın platformunuz için
   anlattığı gibi ayarlayın.
2. **USB kurulum belleği:** Elinizdeki sisteme göre Dortania'yı izleyin:
   - [Windows'ta](https://dortania.github.io/OpenCore-Install-Guide/installer-guide/windows-install.html) ya da
     [Linux'ta](https://dortania.github.io/OpenCore-Install-Guide/installer-guide/linux-install.html): ikisi de macOS
     recovery'yi `macrecovery.py` ile indiriyor, macOS'un kendisi de kurulum sırasında internetten iniyor. Bu yüzden
     bilgisayarın recovery'de çalışan bir ağ bağlantısı olmalı (doğru kext'le Ethernet ya da bir Wi-Fi kext'i).
   - [macOS'ta](https://dortania.github.io/OpenCore-Install-Guide/installer-guide/mac-install.html): tam kurulum dosyasıyla.
3. **EFI:** İşlemcinize ve anakartınıza göre Dortania'nın anlattığı gibi hazırlayın. Dortania'nın en yeni masaüstü sayfası
   [Comet Lake](https://dortania.github.io/OpenCore-Install-Guide/config.plist/comet-lake.html); Alder Lake ve Raptor Lake
   (12–14. nesil) için ayrı sayfası yok. Bu işlemcilerde o sayfayla birlikte
   [test makinesinin örnek yapılandırmasını](https://github.com/yusufklncc/intel-gpu-macos-driver/tree/main/efi/sample-z790)
   (Z790, i7-14700K) kullanın. Orada seri numaraları çıkarılmış `config.plist`, SSDT kaynakları, sürümleriyle kext listesi
   ve kurulum için neyin değiştirileceği var.

   Gereken araçlar Windows'ta da çalışıyor: [SSDTTime](https://github.com/corpnewt/SSDTTime) (anakartınızın DSDT'sinden
   SSDT'ler; DSDT'yi Windows ve Linux'ta alabilir), [USBToolBox](https://github.com/USBToolBox/tool) (USB haritası, Windows'ta)
   ve [GenSMBIOS](https://github.com/corpnewt/GenSMBIOS) (seri numaraları).

   Karta özel bir şey eklemeyin: `DeviceProperties` ve WhateverGreen ayarı gerekmiyor. Kart için iki ayar şimdiden:
   `UEFI > Output > ReconnectGraphicsOnConnect` ayarını `false` bırakın (Dortania'nın örneğinde öyle) ve
   `Booter > Quirks > ResizeAppleGpuBars = 10` yapın (Dortania'da varsayılan `-1`). Test makinesi macOS'u ikisiyle
   kurdu; `-1` ve açık Resizable BAR ile B580'de kurulum denenmedi. İkisinin nedeni 2. aşamada.
4. macOS'u kurup ilk açılış sihirbazını bitirin. Ekran, macOS'un genel ekran sürücüsüyle kartın UEFI çıktısında (GOP) çalışır:
   tek çözünürlük, hızlandırma yok, her şey yavaş, uyku da henüz çalışmaz. Bu aşamada bu normal.

## 2. SIP, AMFI, quirk'ler ve konsol ayarı (`config.plist`)

EFI bölümü küçük bir FAT32 bölümü, her sistem bağlayabilir:

| Sistem | EFI bölümünü bağlamak |
|---|---|
| Windows | Yönetici olarak açılmış Komut İstemi'nde `mountvol S: /S`, ardından aynı pencereden ProperTree'yi başlatıp `S:\EFI\OC\config.plist` dosyasını açın; bitince `mountvol S: /D` |
| Linux | `lsblk -o NAME,FSTYPE,PARTTYPENAME` bölümü gösterir (`vfat`, tür "EFI System"), sonra kendi bölümünüzle `sudo mount /dev/nvme0n1p1 /mnt` |
| macOS | `diskutil list` bölümü gösterir (açılış diskinizdeki `EFI`), sonra kendi bölümünüzle `sudo diskutil mount disk0s1` |

**Windows'ta** Windows için IntelXe Manager ([yukarıda](#windowsta-windows-için-intelxe-manager)) 1-5. maddeleri
EFI sayfasında seçtiğiniz EFI'de (bir USB belleğin EFI klasöründe ya da bilgisayarın bir EFI bölümünde) sizin yerinize
yapar: *Ayarlar (2. aşama)* altında *Planı göster...*, ardından *Yedekle ve uygula*. Önce EFI klasörünün tamamını
yedekler, `config.plist`'in geri kalanını olduğu gibi bırakır; *Geri al* bölümü değişikliği geri alır. Betik yolu
`-efi.zip`'teki `IntelXe-EFI.ps1`'dir (sağ tık > PowerShell ile Çalıştır, *Settings: plan*, ardından *Settings: apply*;
ya da komut satırında `-Plan settings` / `-Apply settings`; zip'teki `README-Windows.md`'ye bakın).
**Bilgisayardaki macOS'ta** IntelXe Manager
uygulamasının EFI sayfası (*Bu ayarları değiştir*) ya da macOS paketindeki `sudo ./efi.sh --settings-apply` aynı
değişiklikleri OpenCore'un açıldığı EFI'de yapar (bkz. 3.1).

`config.plist`'te:

1. `Booter > Quirks > ResizeAppleGpuBars = 10`. macOS, kartın 16 GB'lık BAR'ını eşleyemiyor; bu ayar 1 GB'lık bir pencere
   açıyor. OpenCore'un `ocvalidate` aracı bu değer için "unstable with macOS sleep-wake" uyarısı veriyor: kart uykuda
   BAR boyutunu unutuyor ve macOS onu yeniden ayarlamıyor; Apple'ın kendi GPU sürücüleri bununla başa çıkamıyor.
   IntelXe.kext uyanışta boyutu Linux'taki gibi yeniden ayarlıyor ve test makinesinde uyku çalışıyor. Bu uyarı beklenen
   bir şey; 10'da bırakın. (9 aynı uyarıyı yarım pencereyle alır; ocvalidate'in kabul ettiği 0 da çalışır, ama 256 MB'lık
   penceresi masaüstü ve bir oyun için küçük kalır. Ayrıntılar: kaynak deposunda `docs/design/DESIGN-rebar-window.md`.)
2. `Kernel > Quirks > DisableIoMapper = true`.
3. `NVRAM > Add > 7C436110-AB2A-4BBB-A880-FE41995C9F82` altında:
   - `csr-active-config` = `03080000` (Data): imzasız kext'ler Yardımcı Kernel Koleksiyonu'ndan yüklenebilsin diye SIP
     gevşetiliyor;
   - `boot-args`'a `amfi_get_out_of_my_way=0x1` (yoksa WindowServer imzasız Metal sürücüsünü reddediyor) ve
     `intelxe_surfvram=2` (süreçler arasında VRAM'de paylaşılan IOSurface'ler) ekleyin.
4. `NVRAM > Delete > 7C436110-AB2A-4BBB-A880-FE41995C9F82` listesinde `csr-active-config` ve `boot-args` olmalı (Dortania'nın
   örnek yapılandırmasında var); yeni değerler eskilerin yerine böyle geçiyor.
5. `UEFI > Output > ReconnectGraphicsOnConnect = false`. Bu ayar `true` olursa OpenCore'un RELEASE sürümü, kartın UEFI
   ekran sürücüsü (GOP) yeniden bağlanmadan konsolu kurmaya çalışıyor. macOS konsol ekranı olmadan açılıyor, Lilu başlatmasını
   tamamlayamıyor, VirtualSMC'nin SMC aygıtı bağlanmıyor ve `watchdogd` açılışın yaklaşık 20. saniyesinde panik veriyor.
   OpenCore'un DEBUG sürümü yavaş olduğu için bu sorunu gizliyor.
6. Sistem diski kopyası için (GPU'da Safari, 3.3): `Misc > Security > SecureBootModel = Disabled`.

macOS'u yeniden başlatın. Ekran hâlâ UEFI çıktısında çalışır.

## 3. macOS tarafı (kurulacak bilgisayarda)

### 3.1 IntelXe Manager uygulamasıyla

<p align="center"><img src="https://raw.githubusercontent.com/yusufklncc/intel-gpu-macos-driver/main/docs/images/manager-overview.png" width="70%" alt="IntelXe Manager'ın sürücünün çalıştığını gösteren Genel bakış sayfası"></p>

Uygulama 3. ve 4. aşamaları bilgisayarda düğmelerle yapar ve her adımın neyi değiştireceğini parolanızı sormadan önce
gösterir. İşi paketteki `install.sh` ve `efi.sh` yapıyor; yani 3.2'deki Terminal adımlarıyla aynı şey.

1. `IntelXe-Manager-@VER@.zip`'i Terminal'den indirin ("internetten indirildi" işareti bırakmaz) ve Uygulamalar
   klasörünüze açın:

   ```sh
   cd ~/Downloads
   curl -LO https://github.com/yusufklncc/intel-gpu-macos-driver/releases/download/v@VER@/IntelXe-Manager-@VER@.zip
   mkdir -p ~/Applications && ditto -x -k IntelXe-Manager-@VER@.zip ~/Applications
   open ~/Applications/"IntelXe Manager.app"
   ```

   Uygulama bir Apple geliştirici kimliğiyle değil, ad hoc imzalı. Tarayıcıyla indirdiyseniz macOS açmayı reddeder; o
   zaman `xattr -dr com.apple.quarantine ~/Applications/"IntelXe Manager.app"` çalıştırın ya da bir kez açmayı deneyip
   Sistem Ayarları > Gizlilik ve Güvenlik'te izin verin (*Yine de Aç*).

   Uygulama ilk açılışta **kurulum yardımcısını** açar: aşağıdaki adımları sayfa sayfa yürütür, yeniden başlattıktan sonra
   uygulamayı açtığınızda kaldığı yerden devam eder ve bir GPU testi ile isteğe bağlı eklerle (GPU'da Safari, Zink, HiDPI,
   menü çubuğu öğesi) biter. İlk adımı Safari'nin ayarını da aynı değişiklikte yapabilir (*Safari'yi de GPU için
   hazırla*). Adımlar elle, uygulamanın sayfalarında:
2. **Genel bakış** sürücünün neye ihtiyaç duyduğunu (macOS, SIP, AMFI boot-arg'ı, kart) ve neyin kurulu olduğunu gösterir.
3. **Sürücü:** *v@VER@ sürümünü indir* macOS'unuza uygun paketi indirir, sürümün sağlama listesinin projenin
   anahtarıyla imzalı olduğunu denetler ve paketi listeyle karşılaştırır. EFI
   sayfası da dosyalarını bu paketten alır.
4. 2. aşamayı henüz yapmadınız mı? Kurulum ona ihtiyaç duyuyor (SIP gevşetilmeden ve AMFI boot-arg'ı olmadan çalışmayı
   reddediyor): **EFI (OpenCore)** sayfasında "Ayarlar (2. aşama)" altında *Bu ayarları değiştir*'e basın (OpenCore
   açıldığı EFI'yi kaydetmediyse önce bölümü seçin ya da *Bir EFI klasörü kullan…*), yeniden başlatın ve uygulamayı
   yeniden açın.
5. **Sürücü:** *Bu paketi kur* (`sudo ./install.sh --apply` ile aynı). **Henüz yeniden başlatmayın.**
6. **EFI (OpenCore):** "IntelXe.kext ve SSDT-ARCOFF (4. aşama)" altında *Bu değişiklikleri yap*; neyi değiştirdiği
   aşağıda 4. aşamada anlatılıyor. Her EFI değişikliği önce değişiklik listesini gösterip sorar. Sonra yeniden başlatın.

Uygulama ayrıca sürücüyü onarır, geri alır, kaldırır, isteğe bağlı zink paketini kurar, Safari için sistem diski
kopyasını kurar (3.3; Sürücü sayfasında *Safari ve WebKit*, `SecureBootModel` EFI sayfasında), HiDPI modlarını açar
(Ekran sayfası), GPU'yu test eder, tanı raporu hazırlar ve sonraki sürümleri eskisinin üzerine kurar. Bir macOS
güncellemesinden önce Genel bakış bu sürümün onu destekleyip desteklemediğini söyler; *macOS güncellemesine hazırla*, güncellemeden sonraki
ilk açılışta sürücüyü onaran tek seferlik bir denetim bırakır (EFI'ye hiç dokunmaz; henüz gerçek bir güncellemeden
geçmedi). İsteğe bağlı oyun modu (Güç ve uygulama sayfası), önde bir oyun varken GPU'nun en düşük saat hızını yükseltir.
Yalnızca GitHub'a bağlanır, veri göndermez. Sürücü sayfası test makinesinde kullanıldı; EFI sayfası da orada SSD'nin EFI'sinde kullanıldı. Gerçek bir EFI'de
*Undo* ve temiz bir macOS'tan baştan sona uygulamayla kurulum henüz denenmedi. Arayüz sistem diline göre İngilizce ya
da Türkçe; buradaki adlar Türkçe arayüzdeki adlar.

### 3.2 Terminal'de

1. Paketi Terminal'den indirip açın. `curl` ile indirilen dosyalar "internetten indirildi" işareti taşımadığı için Gatekeeper
   onlara karışmaz:

   ```sh
   cd ~/Downloads
   curl -LO https://github.com/yusufklncc/intel-gpu-macos-driver/releases/download/v@VER@/intel-gpu-macos-driver-@VER@-@CODENAME@.zip
   ditto -x -k intel-gpu-macos-driver-@VER@-@CODENAME@.zip .
   cd intel-gpu-macos-driver-@VER@-@CODENAME@
   ```

   Tarayıcıyla indirdiyseniz çift tıklayıp açın, klasörde Terminal açın ve önce `xattr -dr com.apple.quarantine .` çalıştırın.
2. Önce kontrol edin, sonra kurun:

   ```sh
   ./install.sh                # yalnızca kontrol: macOS, SIP, AMFI, kart, sağlama toplamları, imzalar
   sudo ./install.sh --apply
   ```

   Bu aşamada kontrol, `IntelXe.kext`'in yüklü olmadığını söyler; o 4. aşamada geliyor. `--apply`, değiştirdiği her şeyi
   `/Library/Application Support/intel-gpu-macos-driver/backup-<tarih>/` altına yedekler, kext'leri ve Metal sürücüsünü
   `/Library`'ye kopyalar, açılış korumasının LaunchDaemon'ını kurar (bkz. [Açılmazsa](#açılmazsa)) ve Yardımcı Kernel
   Koleksiyonu'nu yeniden oluşturur.
3. **Henüz yeniden başlatmayın:** önce 4. aşamayı yapın.

### 3.3 Safari için önerilir: sistem diski kopyası

Safari'nin GPU süreci yalnızca sistem diskinden kod yükleyebiliyor. Bu kopya olmadan Safari macOS'un yazılımsal yolunda
çalışır: daha yavaştır ve CSS renk filtresi olan bazı sayfa öğeleri çizilmez (Metal aygıtı yokken CoreGraphics'te
görülen bir sorun; Safari > Ayarlar > Özellik Bayrakları'nda *GraphicsContext Filter Rendering* kapatılınca düzelir,
bu sekme Gelişmiş > *Web geliştiricileri için özellikleri göster* açılınca görünür).
`system-volume/ssv-install.sh`, OpenCore Legacy Patcher'ın kök yamaları gibi Metal sürücüsünü oraya kopyalar ve yeni bir
açılış anlık görüntüsü oluşturur. `Misc > Security > SecureBootModel = Disabled` gerekir (2. aşama, 6. madde). Başka
hiçbir şey buna ihtiyaç duymuyor. macOS güncellemesi bu kopyayı siler (`./ssv-install.sh --check` söyler; yeniden
kurun). macOS 26.7.1'de denendi: Safari B580'de çalışıyor ve sayfaları filtreler dahil doğru çiziliyor. Safari'nin GPU
süreci sandbox yüzünden çevrilmiş shader'ları açılışlar arasında saklayamıyor; bu yüzden her açılıştan sonraki ilk
sayfalar birkaç saniye bekletir.

```sh
cd system-volume && ./ssv-install.sh          # yalnızca kontrol
sudo ./ssv-install.sh --apply
```

## 4. IntelXe.kext ve SSDT-ARCOFF (EFI)

Dosyaları `intel-gpu-macos-driver-@VER@-efi.zip`'ten alın (macOS paketinde de `EFI/OC/` altında aynıları var). EFI bölümünü
2. aşamadaki gibi bağlayın; kurulacak bilgisayarda kendi bölümünüzle `sudo diskutil mount disk0s1`.

Kurulacak bilgisayarın kendisinde, macOS paketindeki `efi.sh` 1-3. maddeleri (ve 2. aşamanın ayarlarını) sizin yerinize
yapar: OpenCore'un açıldığı EFI'yi bulur, EFI klasörünün tamamını yedekler, dosyaları kopyalar, `config.plist` kayıtlarını
açar. Kartınızın ACPI yolunu (2. madde) I/O Registry'den okur; yol paketteki tablonunkinden farklıysa `SSDT-ARCOFF.aml`'ın
bir kopyasına sizin yolunuzu yazıp onu ekler (plan bunu "adapted to this card's port" diye belirtir); düzenlenecek ya da
derlenecek bir tablo kalmaz. 12.-14. nesil hibrit bir Intel işlemcide `HybridPM.kext`'i de ekler (aşağıda, "Hibrit
Intel işlemciler"). `./efi.sh --plan` değişiklikleri gösterir,
`sudo ./efi.sh --apply` yapar, `sudo ./efi.sh --undo` geri alır; `--settings-plan` / `--settings-apply` 2. aşamayı yapar.
Bütün seçenekler için yalnızca `./efi.sh`. IntelXe Manager uygulamasının EFI sayfası da aynısını yapar.

**Windows'ta** Windows için IntelXe Manager 1. ve 3. maddeleri, EFI sayfasının *IntelXe.kext ve SSDT-ARCOFF
(4. aşama)* bölümünde seçtiğiniz EFI'de, yedek ve geri alma ile yapar; `-efi.zip`'teki `IntelXe-EFI.ps1` aynısını bir
betikle yapar (*Driver: plan*, *Driver: apply*). Windows, ACPI yolunu `efi.sh`'in macOS'ta yaptığı gibi denetleyemez;
bu yüzden ikisi de `SSDT-ARCOFF.aml`'ı yalnızca 2. maddedeki kart yolunuzu onayladığınızda ekler: uygulama, Windows
kartın takılı olduğu bilgisayarda çalışıyorsa yolu Aygıt Yöneticisi'nden doldurur ve tabloyu yalnızca macOS'un kurulu
olduğunu, 3. aşamanın bittiğini ve yolu denetlediğinizi işaretlediğinizde ekler (betik: kutusu ve `-CardAcpiPath`).
Paketteki tablonunkinden farklı bir yolda, `efi.sh` gibi tablonun bir kopyasını uyarlarlar. 12.-14. nesil hibrit bir
Intel işlemcide `HybridPM.kext`'i de eklerler; çalıştıkları bilgisayarın işlemcisine bakarlar, bu yüzden onları EFI'yi
açacak bilgisayarda çalıştırın.

1. `IntelXe.kext`'i `EFI/OC/Kexts/` klasörüne kopyalayın ve `Kernel > Add` altına ekleyin: `BundlePath IntelXe.kext`,
   `ExecutablePath Contents/MacOS/IntelXe`, `PlistPath Contents/Info.plist`, `Enabled true`. GPU sürücüsü bu (GuC/HuC yazılımları
   içinde); Lilu istemiyor.
2. `SSDT-ARCOFF.aml`, kartı macOS'un genel ekran sürücüsünden gizliyor (Tahoe'da o sürücü kartı alırsa IntelXe kext'leri
   başlayamıyor). **ACPI yolu anakarta göre değişiyor.** Paketteki tablo, yuvanın kök portunu `\_SB.PC00.PEG1`, kartın kendi PCIe
   anahtarını da onun altında `PEGP` diye adlandıran bir anakart için. `PEGP`'nin altı her B580'de aynı: `PCI(0100)`'de bir alt
   port, `PCI(0000)`'da GPU. Kendi yolunuzu şöyle bulun:

   | Sistem | Nasıl |
   |---|---|
   | Windows | Aygıt Yöneticisi → Görüntü bağdaştırıcıları → Arc kartı (Intel'in sürücüsü yoksa "Microsoft Temel Görüntü Bağdaştırıcısı") → Özellikler → Ayrıntılar → **Konum yolları**. `ACPI(...)` ile başlayan satır `ACPI(_SB_)#ACPI(PC00)#ACPI(PEG1)#ACPI(PEGP)#PCI(0100)#PCI(0000)` gibi görünür; bu `\_SB.PC00.PEG1.PEGP` demektir |
   | Linux | `lspci -nn \| grep -e e20b -e e20c -e e211 -e e212` GPU'yu verir (ör. `03:00.0`); `lspci -tv` kartın iki seviye üstündeki portu gösterir (ör. `01:00.0`); `cat /sys/bus/pci/devices/0000:01:00.0/firmware_node/path` ACPI yolunu yazar, ör. `\_SB_.PC00.PEG1.PEGP` |
   | macOS | [gfxutil](https://github.com/acidanthera/gfxutil/releases) bütün PCI aygıtlarını ACPI yollarıyla listeler: `8086:e20b` satırına bakın (B570'te `8086:e20c`, Pro B60'ta `8086:e211`, Pro B50'de `8086:e212`). IORegistryExplorer aynı adları IOService düzleminde gösterir (`PC00@0` → `PEG1@1` → `PEGP@0` → ...) |

   Yolunuz paketteki gibiyse (`\_SB.PC00.PEG1.PEGP`) `SSDT-ARCOFF.aml`'yi olduğu gibi kullanın. Değilse macOS'ta tabloyu
   `efi.sh` ya da uygulamanın EFI sayfası uyarlasın (yukarıda); yolun uzunluğu ne olursa olsun bunu yapar. Elle yapmak için
   `SSDT-ARCOFF.dsl`'de yolun geçtiği iki yere (`External` ve `Scope` satırları) kendi yolunuzu yazıp `iasl SSDT-ARCOFF.dsl`
   ile derleyin. `iasl`, ACPICA'nın parçası: Windows'ta ACPICA'nın Windows araçları (acpica.org, Downloads), Linux'ta `acpica-tools` (ya da `iasl`) paketi, macOS'ta
   `brew install acpica` ya da [MaciASL](https://github.com/acidanthera/MaciASL/releases)'in içindeki `iasl`. Hiçbir `ACPI(...)`
   parçası kartın üst portunu adlandırmıyorsa (son `ACPI(...)` parçasından sonra iki değil üç `PCI(...)` geliyorsa) tabloya bir köprü
   seviyesi daha gerekiyor: Konum yolları satırınızla bir issue açın.
3. `.aml` dosyasını `EFI/OC/ACPI/` klasörüne kopyalayın ve `ACPI > Add` altına ekleyin (`Path SSDT-ARCOFF.aml`, `Enabled true`).
4. Yeniden başlatın. Masaüstü artık hızlandırmalı olmalı: saydamlık ve bulanıklık, akıcı pencere sürükleme, `Bu Mac Hakkında`'da
   kartın adı görünür (12 GB'lık Arc B580). Terminal'de `kmutil showloaded | grep -i intelxe` `IntelXe` ve `IntelXeFB`'yi listeler, `sysctl
   debug.nmxaccelfb` de `1` yazar.

## Hibrit Intel işlemciler (12.-14. nesil): HybridPM.kext

macOS'un işlemci güç yönetimi (XCPM), performans ve verimlilik çekirdekli işlemcilerden eski. Alder Lake ve Raptor Lake'te
P çekirdeklerini sınırlıyor (bir i7-14700K 5,6 yerine 4,4 GHz'de kaldı) ve tam yüklü çekirdekleri 800 MHz'de tutabiliyor. GPU
sürücüsü her komut arabelleği için işlemcide epey iş yapıyor, bu yüzden GPU da yavaş görünüyor: test makinesinde Geekbench'in
GPU puanları `HybridPM.kext` ile yaklaşık üçte bir yüksekti, tek işlemci iş parçacığı iki kattan fazla hızlıydı. HybridPM GPU
sürücüsünden ayrı ve isteğe bağlı; `-efi.zip`'te ve macOS paketinde `EFI/OC/Kexts/` altında.

**Hangi işlemciler:** P ve E çekirdekli 12., 13. ve 14. nesil Core i3/i5/i7/i9 (Alder Lake ve Raptor Lake, masaüstü ve dizüstü;
örneğin i5-12600K, i5-13400F, i7-14700K, i7-13620H) ve Core Ultra 200S masaüstü işlemciler (Arrow Lake, örneğin Core Ultra 7
265K; bir katkıda bulunan ölçtü, test makinesinde değil). Öteki Core Ultra'lara, E çekirdeği olmayan ve daha eski
işlemcilere dokunmuyor (yalnızca nedenini günlüğe yazıyor). Model listesi depodaki `kernel/HybridPM/README.md`'de.

**Eklemek:** kurulacak bilgisayarda 4. aşama bunu yapıyor: `efi.sh --plan` / `sudo ./efi.sh --apply` ve uygulamanın EFI
sayfası, işlemcinin adı bunlardan biriyse `HybridPM.kext`'i `IntelXe.kext` ile birlikte kopyalıyor ve kaydını `Lilu.kext`'in
kaydının hemen altına ekliyor (var olan kayıt açılıyor; aynı adlı eski bir derleme değiştiriliyor). EFI Lilu yüklemiyorsa bir
notla dışarıda bırakıyor. Windows için IntelXe Manager ve `IntelXe-EFI.ps1` de Windows'ta, çalıştıkları bilgisayarın işlemcisine
bakarak aynısını yapıyor. İşlemcinin adına bakıyorlar ve bir Core Ultra'da onu henüz eklemiyorlar: Arrow Lake'te elle
ekleyin. Elle:

1. Bir Lilu eklentisi. Çoğu OpenCore EFI'sinde VirtualSMC için `Lilu.kext` zaten var; sizinkinde yoksa
   [acidanthera/Lilu](https://github.com/acidanthera/Lilu/releases)'dan ekleyin (test makinesinde 1.7.2). Bu paket Lilu
   içermiyor.
2. `HybridPM.kext`'i `EFI/OC/Kexts/` klasörüne kopyalayın ve `Kernel > Add` altına `Lilu.kext`'in **altına** ekleyin:
   `BundlePath HybridPM.kext`, `ExecutablePath Contents/MacOS/HybridPM`, `PlistPath Contents/Info.plist`, `Enabled true`.
3. Yeniden başlatın. Açılıştan 20 saniye sonra devreye giriyor.

**Kontrol etmek:**

```sh
sysctl debug.hybridpm.enabled debug.hybridpm.why debug.hybridpm.route
# debug.hybridpm.enabled: 1, debug.hybridpm.why: on (model 0xB7), debug.hybridpm.route: 1
./hpmstat 1000 3     # paket klasöründe: her çekirdeğin saati ve yükü, HWP sınırları, paket gücü
```

`enabled` 0 ise `why` nedenini söyler: `not hybrid` ya da `unsupported model` (Core Ultra ve diğerleri), ya da bir macOS
güncellemesi düzelttiği rutini değiştirdiyse `route 3`; macOS o zaman onsuzmuş gibi çalışır. `debug.hybridpm` hiç yoksa
HybridPM ya da Lilu yüklenmemiştir (`kmutil showloaded | grep -i -e lilu -e hybridpm`).

**Kapatmak:** boot-args'a `-hpmoff` ekleyin ya da `Kernel > Add` kaydını `Enabled false` yapın. Bir sonraki yeniden başlatmaya
kadar `sudo sysctl debug.hybridpm.hwp=0 debug.hybridpm.autonomous=0 debug.hybridpm.epp=-1 debug.hybridpm.eet_off=0`
macOS'un kendi ayarlarını geri verir.

## İsteğe bağlı: zink ile OpenGL

macOS'un kendi OpenGL'i Metal'in üzerinde çalışıyor ve bu sürücüde yavaş. LWJGL 3 Java oyunları (Minecraft Java 1.13 ve
sonrası) bunun yerine Mesa'nın zink'ini kullanabilir: zink OpenGL'i doğrudan sürücünün Vulkan'ına çeviriyor. Yalnızca
OpenGL'i GLFW/LWJGL ile açan uygulamalar kullanır; geri kalan her şey Apple'ın OpenGL'inde kalır. Sürücünün kurulu ve
`intelxe_surfvram=2` ayarının açık olması gerekiyor (2. aşama). IntelXe Manager'ın **OpenGL (Zink)** sayfası kurar ve
açar; ya da Terminal'de:

```sh
cd ~/Downloads
curl -LO https://github.com/yusufklncc/intel-gpu-macos-driver/releases/download/v@VER@/intel-gpu-macos-driver-@VER@-zink-gl.zip
ditto -x -k intel-gpu-macos-driver-@VER@-zink-gl.zip .
cd intel-gpu-macos-driver-@VER@-zink-gl
sudo ./install.sh                # /Library/IntelXe/GL ve Vulkan yükleyicisi; değiştirdiğini yedekler
./install.sh --test              # B580'de zink üzerinden bir GL bağlamı
sudo ./install.sh --global on    # bundan sonra açılan bütün LWJGL 3 uygulamaları zink kullanır
```

`--global off` geri alır, `--undo` kaldırır; ayrıntılar zip'teki `README.md`'de. Test makinesinde ölçüldü (macOS 26.7.1,
Minecraft 1.21.1, Sodium ve Iris ile, pencereli, aynı nokta): BSL shader paketiyle zink'te 197–205 fps, aynı bilgisayarda
Windows'ta Intel'in sürücüsüyle 214–218; shader paketi olmadan zink'te
1400–1500 fps, Windows'ta 700–1000. Deneysel: CGL/NSOpenGL kullanan yerel bir macOS uygulaması da tek tek zink'e
geçebilir: `./cglzink-app.sh add /Applications/X.app` `~/Applications/X (zink).app`'i oluşturur (zip'teki `README.md`,
"Native OpenGL applications") ya da varsayılan olarak kapalı deneysel bir anahtarla hepsi birden: `sudo ./install.sh
--global-gl on` ya da IntelXe Manager'ın Zink sayfası; o sayfa zink kullanan ya da Apple'ın OpenGL'inde kalan
uygulamaların listesini de tutuyor. macOS'un girişte kendiliğinden
yeniden açtığı bir başlatıcı, ayar yerine oturmadan başlayıp Apple'ın OpenGL'inde kalabilir: kapatıp yeniden açın
(zink açıkken Java "Picked up JAVA_TOOL_OPTIONS" yazar).

## İsteğe bağlı, deneysel: Wine'daki Windows oyunları için Vulkan

**Donanımda Wine ile henüz denenmedi.** Wine, Direct3D oyunlarını Vulkan konuşan DXVK (Direct3D 8-11) ve vkd3d-proton
(Direct3D 12) ile çalıştırır; macOS'ta bu Vulkan normalde MoltenVK'dır ve onu bir kez daha Metal'e çevirir.
`-vulkan.zip` bunun yerine onlara sürücünün kendi Vulkan'ını (ANV) verir, her seferinde tek oyun için: yalnızca
`intelxe-vk run` ile başlattığınız program kullanır; Wine, MoltenVK'sı ve sürücü olduğu gibi kalır. Sürücünün kurulu
olması ve 64 bit bir Wine öneki gerekir; Wine sürümünüzün bunu kullanıp kullanamayacağını `intelxe-vk check` söyler.

```sh
cd ~/Downloads
ditto -x -k intel-gpu-macos-driver-@VER@-vulkan.zip .
cd intel-gpu-macos-driver-@VER@-vulkan
sudo ./install.sh                   # /Library/IntelXe/Vulkan; değiştirdiğini yedekler
./install.sh --test                 # sağlayıcı üzerinden 120 karelik bir pencere: PASS
V=/Library/IntelXe/Vulkan/bin/intelxe-vk
$V check "/Applications/Wine Stable.app"   # o Wine'ın açtığı Vulkan kitaplığı; önekleriniz
$V fetch                            # DXVK ve vkd3d-proton'u indirir (sabit sürüm, sağlama toplamı denetimli)
$V prefix-install ~/.wine           # DLL'lerini öneke kurar; önekin kendi DLL'leri saklanır
WINEPREFIX=~/.wine $V run wine oyun.exe
```

`$V prefix-restore ~/.wine` öneki Wine'ın kendi Direct3D'sine döndürür, `sudo ./install.sh --undo` gerisini kaldırır.
`fetch`, vkd3d-proton'un arşivi için `zstd` ister (`brew install zstd`). Ayrıntılar ve sınırlar zip'teki `README.md`'de
(32 bit oyunlar ve HDR denenmedi; önekteki `dxvk.conf`, DXVK'nın iki tanımlayıcı özelliğini bilerek kapatır).

## İsteğe bağlı: HiDPI modları (ekran ölçeklendirme)

Windows'taki ekran ölçeklendirmesi gibi: Sistem Ayarları > Ekranlar, yazıyı ve pencereleri daha büyük ama keskin çizen
"gibi görünür" boyutlar sunar (Daha Büyük Metin ... Daha Fazla Alan). 2560x1440 bir monitörde bunlar 2048x1152,
1920x1080, 1706x960, 1600x900 ve 1280x720. Monitörün tam yenileme hızında çalışırlar (test monitöründe 180 Hz ve
Değişken): macOS iki katı boyutta çizer, GPU her kareyi panelin boyutuna küçültür. Açılmadıkça kapalıdırlar. IntelXe
Manager'ın **Ekran** sayfası açıp kapatır; ya da Terminal'de, paket klasöründe:

```sh
./hidpi.sh               # şu an ne ayarlı
sudo ./hidpi.sh --on     # NVRAM intelxefb-hidpi=1, HiDPI boyutlarıyla bir ekran ayarı, DisplayResolutionEnabled
sudo ./hidpi.sh --off    # yalnızca --on'un yazdıklarını geri alır
```

İkisinden sonra da yeniden başlatın, sonra Sistem Ayarları > Ekranlar'dan bir boyut seçin. Ekran ayarı, diğer HiDPI
araçlarının kullandığı yere yazılır (`/Library/Displays/Contents/Resources/Overrides`); monitörünüz için başka bir
aracın (one-key-hidpi vb.) yazdığı bir ayar zaten varsa `hidpi.sh` ona dokunmaz ve durur: önce onu kaldırın. Bir boyut
ekranı siyah bırakırsa SSH'tan `sudo ./hidpi.sh --off` çalıştırıp yeniden başlatın; OpenCore menüsündeki **Reset NVRAM**
de modları kapatır (anahtar NVRAM değişkenidir).

## Açılmazsa

- **Siyah ekran ya da sürekli yeniden başlama:** Çalışan EFI'nin olduğu USB bellekten açın ya da OpenCore'da macOS seçiliyken Shift'e
  basarak güvenli modda açın (Yardımcı Kernel Koleksiyonu'nu atlar). Sonra `sudo ./install.sh --undo` çalıştırın ve `IntelXe.kext` ile
  `SSDT-ARCOFF.aml`'yi EFI'den çıkarın.
- **İlk dakikada çökmeler (açılış koruması):** Hızlandırmalı ilk dakikası içinde biten tek bir açılış (panik ya da güç düğmesine
  basılı tutmak) yalnızca bir sonrakinin yeniden denemesine yol açar. Üst üste iki taneden sonra sonraki açılışlar en güvenli ayarlarla
  hızlandırır (async ve flip kapalı; `ioreg -rc IntelXeFramebuffer -d1 | grep AccelSafe` `Yes` der); bir dakika sorunsuz çalışan bir
  açılıştan sonra koruma normale döner. Üst üste dört taneden sonra hızlandırma durdurulur. 0.1.5'ten itibaren böyle bir
  açılış masaüstünü **ürün yazılımının ekran arabelleğinde** gösterir (Apple'ın kendi ekran sürücüsü, ürün yazılımının
  açtığı görüntü üzerinde; macOS kurulumu bu kartta da böyle çalışır): hızlandırma ve Metal yoktur, tek çözünürlük vardır,
  bu açılışta Mac'i uyutmayın. Giriş yapın, nedenini bulun (`/Library/Logs/DiagnosticReports` içindeki panik raporu,
  IntelXe Manager'ın Rapor sayfası), onarın ya da kaldırın, sonra korumayı sıfırlayıp yeniden başlatın: IntelXe Manager'ın
  Genel bakış sayfasında *Açılış koruması* > *Açılış korumasını sıfırla*, ya da Terminal'de (veya SSH ile):

  ```sh
  sudo nvram intelxe-accel-off=ok intelxe-guard=ok
  ```

  `sysctl debug.intelxe.bootguard` korumanın bu açılışı nasıl okuduğunu gösterir (`state=held`, `display=firmware`). İki
  değişken birden, çünkü durdurulmuş bir açılışta sayaç temiz bir yeniden başlatmaya kadar yalnızca bellekte sıfırlanır;
  `nvram -d` macOS 26'da hata vermez ama değişkeni silmez, `=ok` her sürümde çalışır. `-intelxe_noaccel` boot-arg'ı ve
  güvenli kip de ürün yazılımının ekran arabelleğini kullanır. 0.1.4'teki davranışa (hızlandırmasız IntelXeFB; macOS 26'da
  Apple logosunda kalır) dönmek için `sudo nvram intelxe-held-display=fb`; `=ok` geri alır.

  Durdurulmuş bir açılış yine de Apple logosunda kalırsa (EFI'deki `IntelXe.kext` 0.1.5'ten eski, ya da devir başarısız:
  yukarıdaki sysctl'de `handoff=`, SSH ile okunur): yeniden başlatın ve OpenCore seçim ekranında **Reset NVRAM**'i seçin
  (gizliyse Boşluk tuşu). Bu korumayı temizler; OpenCore kendi boot-args ve SIP ayarını her açılışta yeniden yazar (B580'de
  denendi). Ya da SSH ile yukarıdaki komutla temizleyip yeniden başlatın.
- **Açılışın yaklaşık 20. saniyesinde panik, `watchdogd ... exited`** (`/Library/Logs/DiagnosticReports/Kernel-*.panic` içinde `watchdogd`
  geçiyorsa): `UEFI > Output > ReconnectGraphicsOnConnect` `true` kalmıştır; `false` yapın (2. aşama, 5. madde).
- **macOS açılmıyor ve nedenini anlayamıyorsunuz: açılış günlüğü.** OpenCore'un günlüğünü, boot.efi'nin satırlarını ve
  bir sonraki açılışta kaydettiği panikleri bir rapor için EFI'nin köküne yazdırır. Çalışan açılıştan (USB belleğiniz)
  ya da Windows'tan açın: uygulamaların EFI sayfası (*Açılış günlüğü (sorun giderme için)*), `./efi.sh --bootlog-plan` sonra
  `sudo ./efi.sh --bootlog-apply`, ya da `IntelXe-EFI.ps1 -Plan bootlog` sonra `-Apply bootlog -Expect HASH`. Sorunlu
  açılışı bir kez daha deneyin, sonra günlükleri raporla toplayın ([Sorun bildirmek](#sorun-bildirmek)). Açıkken her
  açılış yavaşlar: sonra kapatın (aynı bölüm, `--bootlog-off-apply`, `-Apply bootlog-off`).
- **Her şeyi geri almak:** `sudo ./install.sh --undo`, sonra `IntelXe.kext` ve `SSDT-ARCOFF.aml` EFI'den çıkarılmış olarak yeniden başlatın.
- **`install.sh`'nin diğer komutları:** `./install.sh --status` kurulu olanı bu paketle karşılaştırır, hiçbir şeyi değiştirmez.
  `sudo ./install.sh --repair` kurulu dosyalardan paketle uyuşmayanları yeniden kopyalar, son yedeğe dokunmaz. `sudo ./install.sh --uninstall`
  kext'leri ve Metal sürücüsünü `/Library`'den siler, yedekleri bırakır; `--undo` ise son kurulumun değiştirdiklerini geri getirir.
  `--uninstall`'dan sonra, yeniden başlatmadan önce `IntelXe.kext`'i EFI'den çıkarın.

## Yükseltme

**0.1.3'ten:** `IntelXe.kext` (EFI'de), `IntelXeFB.kext`, `NMXAccel.kext` ve Metal sürücüsü yeni (lock V2, çevirici
düzeltmesi, B570'in kimliği, GPU saati ve sıcaklık okumaları, doğrudan gönderim); Mesa ve video kodekleri 0.1.3'ünkiler.
Paketi kurun (uygulamada *v@VER@ sürümünü indir*, sonra *Bu paketi kur*; ya da `sudo ./install.sh --apply`), sonra
yeniden başlatmadan önce EFI'deki `IntelXe.kext`'i değiştirin (macOS'ta uygulamanın EFI sayfası ya da
`sudo ./efi.sh --apply`, Windows'ta Windows için IntelXe Manager ya da `IntelXe-EFI.ps1`; hepsi hibrit bir Intel işlemcide `HybridPM.kext`'i de ekler;
ya da elle; `config.plist` kaydı olduğu gibi kalır). Sistem diski kopyasını kurduysanız (3.3) bu paketinkini de kurun
(Sürücü sayfasında *Safari ve WebKit* ya da `sudo ./system-volume/ssv-install.sh --apply`); Safari yeni Metal
sürücüsünü böyle alır. Uygulamalar için GPU saatini yüksek tutan GuC düşük gecikme ipucu 0.1.4'ten beri varsayılan
olarak kapalı (açıkken testlerde uzun bir boşta kalmanın ardından GPU hatası görüldü); uygulamanın *Güç ve uygulama*
sayfası (*Uygulamalar için hızlı GPU yanıtı*) ya da `sudo touch /Library/GPUBundles/NVMTLDriver.low-latency` açar.
Doğrudan gönderim 0.1.4'te kapalıydı, 0.1.5'ten itibaren açık; `-intelxe_nodirect` boot-arg'ı kapatır.

**0.1.2'den:** 0.1.3'ten olduğu gibi; ayrıca `NMXAGDC.kext`, video çözücü ve Mesa'nın Vulkan sürücüsü de yeni
(firmware'inkinden yüksek tazeleme hızları, varsayılan olarak açık Adaptive-Sync modları, monitörün tam tazeleme
hızında isteğe bağlı HiDPI modları, tam aralıklı videonun tam aralıklı çözülmesi, WindowServer'ın derlenmiş
shader'larını saklaması). Sistem diski kopyasını kurduysanız `install.sh --apply`, kopya eski Mesa'yı taşıdığı için
`/Library/GPUBundles/NVMTLDriver.use-library` dosyasını oluşturur; bu paketin kopyasını uygulamanın Sürücü sayfasından
kurmak onu siler, Terminal'de `ssv-install.sh --apply`'dan sonra `sudo rm /Library/GPUBundles/NVMTLDriver.use-library`
çalıştırın; sonra yeniden başlatın.

**0.1.0 ya da 0.1.1'den:** Bütün kext'ler, Metal sürücüsü ve video kütüphaneleri yeni, **EFI'deki `IntelXe.kext` de**
(bir güvenlik açığını kapatıyor: yetkisiz bir süreç yalnızca okuyabileceği sayfalara yazılabilir GPU eşlemesi
alabiliyordu). Paketi kurun (uygulamada *v@VER@ sürümünü indir*, sonra *Bu paketi kur*; ya da
`sudo ./install.sh --apply`), sonra yeniden başlatmadan önce EFI'deki `IntelXe.kext`'i değiştirin (macOS'ta uygulamanın
EFI sayfası ya da `sudo ./efi.sh --apply`, Windows'ta Windows için IntelXe Manager ya da `IntelXe-EFI.ps1`, ya da elle
kopyalayın; `config.plist` kaydı
olduğu gibi kalır).
`--apply` açılış korumasının LaunchDaemon'ını da kurar ve 0.1.0 ile 0.1.1'in izin listesini (0.1.1 rehberinin istediği
`-NotificationCenter` satırıyla ya da satırsız) değiştirir; böylece Safari'nin GPU süreci, Dock simgeleri ve masaüstü
widget'ları yeniden sürücüyü kullanır. Başka bir biçimde değiştirdiğiniz izin listesi korunur: paketteki
`Library/GPUBundles/nvmtl-allow.txt` ile karşılaştırın. Sistem diski kopyasını kurduysanız `--apply`, bu paketinkini
(3.3) kurup `/Library/GPUBundles/NVMTLDriver.use-library` dosyasını silene kadar bütün süreçlere `/Library` kopyasını
kullandırır. `config.plist`'te `UEFI > Output > ReconnectGraphicsOnConnect = false` olduğunu kontrol edin (2. aşama,
5. madde), özellikle OpenCore'u RELEASE sürümüne geçirmeden önce.

**0.1.0'dan önceki, B580 adlı özel derlemelerden:** Adı değişen kext'ler ve boot-arg'ların tablosu İngilizce rehberde (`README-INSTALL.md`,
"Upgrading from an earlier build").

## Paketin içindekiler

| Yol | Nereye gidiyor |
|---|---|
| `EFI/OC/Kexts/IntelXe.kext` | EFI, `Kernel > Add` (4. aşama); `-efi.zip`'tekiyle aynı dosya |
| `EFI/OC/ACPI/SSDT-ARCOFF.aml`, `.dsl` | EFI, `ACPI > Add`, yolu kontrol ettikten sonra (4. aşama) |
| `EFI/OC/Kexts/HybridPM.kext` | isteğe bağlı: EFI, `Kernel > Add`'de Lilu'nun altına; 12.-14. nesil hibrit Intel işlemciler için |
| `Library/Extensions/IntelXeFB.kext`, `NMXAccel.kext`, `NMXAGDC.kext` | `/Library/Extensions` (ekran, hızlandırıcı), `install.sh` ile |
| `Library/GPUBundles/NVMTLDriverLib.bundle`, `nvmtl/`, `nvmtl-allow.txt` | `/Library/GPUBundles` (Metal sürücüsü, Mesa, codec'ler), `install.sh` ile |
| (`install.sh` oluşturur) | `/Library/LaunchDaemons/io.github.yusufklncc.intelxe.guard.plist`: bir açılış bir dakika çalışınca açılış korumasını temizler |
| `system-volume/` | isteğe bağlı: Metal sürücüsünün sistem diskindeki kopyası (GPU'da Safari, 3.3) |
| `efi.sh` | isteğe bağlı, macOS'ta: OpenCore'un açıldığı EFI'de 2. ve 4. aşamalar, yedek ve geri alma ile |
| `hidpi.sh` | isteğe bağlı: monitör için HiDPI modları (ekran ölçeklendirme), açma ve kapama |
| `hpmstat` | isteğe bağlı: HybridPM yüklüyken çekirdek saatleri ve paket gücü |
| `SHA256SUMS`, `sigcheck` | `install.sh`'nin yaptığı kontroller |
| `SOURCES.md`, `LICENSES/` | hangi kaynağın ve betiğin hangi dosyayı ürettiği; lisans metinleri |

## Sorun bildirmek

GitHub'da hata şablonuyla bir issue açın. Depodaki `tools/report/` logları toplar ve önce seri numaralarını, UUID'leri, MAC adreslerini ve
kullanıcı adlarını çıkarır (uygulamaların Rapor sayfası onu çalıştırır). Lütfen ham sistem raporlarını paylaşmayın.
Açılış günlüğü açıksa ([yukarıda](#açılmazsa)) `collect-macos.sh --boot-logs`, `collect-windows.ps1 -BootLogs` ya da Rapor
sayfasındaki *Açılış günlükleri* EFI'deki OpenCore günlüklerini ve kaydedilen panikleri de ekler.

Sürücünün henüz desteklemediği bir Intel GPU: Linux raporuyla (`tools/report/collect-linux.sh`) bir *Support for my
Intel GPU* issue'su açın. Xe probe stick (sürümde varsa `xe-probe-stick-VERSION.img.zip`; deneysel, henüz gerçek
donanımda çalışmadı) bu raporu Linux'u olmayan bir bilgisayarda hazırlar: ağsız açılır, diskleri salt okunur tutar ve
raporu belleğe yazar (Windows için IntelXe Manager onu yazar ve okur; `tools/probe-stick/README.md`).
