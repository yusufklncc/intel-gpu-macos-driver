# intel-gpu-macos-driver 0.1.6: macOS 26 Tahoe için kurulum (Intel Arc B580 / B570)

[English](README-INSTALL.md)

Deneysel bir sürüm. Yalnızca tek makinede denendi: Z790 masaüstü, Arc B580, macOS 26.7.1, OpenCore. Sürücü imzasızdır, desteği yoktur ve bilgisayarınızın macOS'u açma biçimini değiştirir.
**Başlamadan önce çalışan bir OpenCore EFI'si olan bir USB bellek hazırlayın** ve anakartınızın önyükleme menüsünden o
belleği nasıl seçeceğinizi öğrenin. Yalnızca ticari olmayan kullanım içindir (`LICENSES/`).

Her şey yolunda giderse şunları elde edersiniz: masaüstü kartta Metal ile çizilir (pencere sürükleme monitörün yenileme
hızında akar, 180 Hz'e kadar denendi); masaüstü widget'ları, Metal uygulamaları ve oyunlar çalışır; Safari GPU'da
çalışır (sistem diski kopyasıyla, 3.3); H.264, HEVC, AV1 ve VP9 kod çözme ile H.264, HEVC ve AV1 kodlama GPU'da yapılır.
DisplayPort'a bağlı tek monitör, VRR, uyku ve uyanma da çalışır. İsteğe bağlı olarak Minecraft Java gibi LWJGL oyunları
OpenGL'i Mesa'nın zink'iyle çalıştırabilir (`-zink-gl.zip`, aşağıda "İsteğe bağlı: zink ile OpenGL"); 12.-14. nesil hibrit
Intel işlemciler için de `HybridPM.kext` var (aşağıda).

Bilinen sorunlar:

- Oturum açtıktan sonraki ilk saniyeler, sonrasına göre hâlâ daha az akıcı (0.1.3'ten beri bu daha hafif, çünkü
  WindowServer derlediği shader'ları saklıyor; kurulumdan sonraki ilk oturum açılışında onları bir kez derliyor).
- Yeniden başlattıktan sonra giriş ekranında açık gri bir bant görünüyor.
- Metal'i VideoToolbox kodlamasıyla başlatan ffmpeg benzeri araçlar, ilk oturumlarında kodlayıcılarımızı görmüyor.
- Kartın 12 GB belleğinin yalnızca 1 GB'ını işlemci görebiliyor (macOS sınırı, 2. aşama 1. madde). Sürücü bu pencereyi
  ihtiyacı olanlara ayırıyor, arka plan süreçlerinin arabelleklerini sistem belleğine koyuyor; yine de GPU'yu yoğun
  kullanan çok sayıda açık uygulama pencereyi doldurabilir.

Bu sürücünün eski bir sürümünü kullanıyorsanız doğrudan [Yükseltme](#yükseltme) bölümüne geçin.

## Gerekenler

- OpenCore ile macOS açabilen bir bilgisayar (Intel işlemci, x86_64); içinde bir Arc B580, B570, Pro B60 ya da Pro B50 ve
  **kartın DisplayPort çıkışına bağlı tek monitör**. B570, Pro B60 (8086:e211) ve Pro B50 (8086:e212) destekleniyor ama
  bizim tarafımızdan denenmedi; nasıl çalıştığını lütfen bildirin. Dahili GPU gerekmez: test makinesinde macOS yalnızca
  B580 takılıyken kuruldu.
- USB belleği hazırlamak ve EFI'yi düzenlemek için Windows, Linux ya da macOS çalışan herhangi bir bilgisayar. Mac şart
  değil.
- [Sürüm sayfasından](https://github.com/yusufklncc/intel-gpu-macos-driver/releases/tag/v0.1.6) şu dosyalar:

  | Dosya | İçinde ne var | Nerede açılır |
  |---|---|---|
  | `intel-gpu-macos-driver-0.1.6-efi.zip` | OpenCore parçaları: `IntelXe.kext`, `SSDT-ARCOFF.aml` ve kaynağı, `HybridPM.kext` (isteğe bağlı, [hibrit Intel işlemciler](#hibrit-intel-işlemciler-12-14-nesil-hybridpmkext)); Windows'ta 2. ve 4. aşamaları yapan betik `IntelXe-EFI.ps1` | herhangi bir sistemde |
  | `IntelXeManager-0.1.6-win-x64.zip` | Windows için IntelXe Manager: 2. ve 4. aşamalar düğmelerle, bilgisayarın denetimi ve tanılama raporu ([aşağıda](#windowsta-windows-için-intelxe-manager)); `-efi.zip`'i kullanır | Windows 10 ya da 11 (x64) |
  | `IntelXe-Manager-0.1.6.zip` | IntelXe Manager uygulaması: 3. ve 4. aşamalar (ve sonraki güncellemeler) Terminal yerine düğmelerle; bir sonraki satırdaki dosyayı kendisi indirir | bilgisayardaki macOS'ta |
  | `intel-gpu-macos-driver-0.1.6-tahoe.zip` | geri kalan her şey, `install.sh` ve `efi.sh` dahil | **bilgisayardaki macOS'ta** (3. aşama) |
  | `intel-gpu-macos-driver-0.1.6-zink-gl.zip` | isteğe bağlı: LWJGL oyunları için zink üzerinden OpenGL | bilgisayardaki macOS'ta, 4. aşamadan sonra |
  | `intel-gpu-macos-driver-0.1.6-vulkan.zip` | isteğe bağlı, deneysel (sürümde varsa): Wine oyunları için Vulkan (DXVK, vkd3d-proton) | bilgisayardaki macOS'ta, 4. aşamadan sonra |

  macOS dosyalarını yalnızca macOS'ta açın. Windows ve çoğu Linux arşiv aracı, çalıştırma izinlerini ve kurulum betiğinin
  denetlediği kod imzalarını korumaz. Sürüm sayfasındaki `.sha256` dosyası bu dosyaların sağlama toplamlarını,
  `.sha256.sig` de bu listenin projenin anahtarıyla atılmış imzasını içerir (IntelXe Manager imzayı kendisi doğrular;
  elle: `swift release/signing.swift verify <app/Sources/Signing.swift'teki anahtar> <.sha256> <.sha256.sig>`).
- GPU dışındaki her şey için [Dortania'nın OpenCore kurulum rehberi](https://dortania.github.io/OpenCore-Install-Guide/):
  anakartınıza uygun EFI, ACPI tabloları, SMBIOS, USB haritası.
- Sisteminize uygun bir `config.plist` düzenleyicisi, örneğin [ProperTree](https://github.com/corpnewt/ProperTree) (Windows,
  Linux ve macOS'ta çalışır, Python ister).

## Aşamalar

| Aşama | Nerede yapılır | Ne yapılır | Sonrasında ekran |
|---|---|---|---|
| 1 | Windows, Linux ya da macOS, sonra kurulacak bilgisayar | macOS'u sürücüsüz, sade bir OpenCore EFI'siyle kurmak | kartın UEFI çıktısı: hızlandırma yok, yavaş |
| 2 | `config.plist`, herhangi bir sistemden | SIP, AMFI, iki quirk ve bir çıktı ayarı | aynı |
| 3 | Bilgisayardaki macOS | uygulamada **Bu paketi kur** ya da `sudo ./install.sh --apply`; **henüz yeniden başlatmayın** | aynı |
| 4 | EFI, bilgisayardaki macOS'tan ya da herhangi bir sistemden | `IntelXe.kext` ve `SSDT-ARCOFF.aml`, hibrit bir Intel işlemcide `HybridPM.kext` de (uygulamanın EFI sayfası, `efi.sh`, Windows için IntelXe Manager, `IntelXe-EFI.ps1` ya da elle), sonra yeniden başlatın | hızlandırmalı |

`IntelXe.kext` ve `SSDT-ARCOFF.aml`'i 4. aşamadan önce EFI'ye koymayın. Bu tablo kartı, kurulum ekranını gösteren genel
ekran sürücüsünden gizler; 3. aşamada sürücünün kendi ekran kext'i kurulana kadar da o sürücünün yerini tutacak bir şey
yoktur.

Test makinesi her aşamanın sonundaki durumda çalıştı; ama dört aşama temiz bir kurulumda baştan sona hiç uygulanmadı.
Takıldığınız yeri bildirirseniz rehberi düzeltiriz.

## Windows'ta: Windows için IntelXe Manager

2. ve 4. aşamalar EFI'yi değiştirir; bunları aynı bilgisayarda ya da başka bir bilgisayarda Windows'tan da yapabilirsiniz.
Orada önerilen yol **Windows için IntelXe Manager** (`IntelXeManager-0.1.6-win-x64.zip`); `-efi.zip`'teki
`IntelXe-EFI.ps1` aynı değişiklikleri bir betikle ya da komut satırından yapar. İkisi de aynı kuralları okur
(`efi-rules.json`, macOS'taki `efi.sh` gibi), bir değişiklikten önce EFI klasörünün tamamını yedekler ve biri, ötekinin
yaptığı değişikliği geri alabilir.

1. Sürüm sayfasından `IntelXeManager-0.1.6-win-x64.zip` ve `intel-gpu-macos-driver-0.1.6-efi.zip` dosyalarını indirin
   ve **önce uygulamanın zip'ini denetleyin**: SHA-256'sı, sürüm sayfasındaki `intel-gpu-macos-driver-0.1.6.sha256`
   listesinde bu dosya için yazan değerle aynı olmalı. PowerShell'de:

   ```powershell
   Get-FileHash .\IntelXeManager-0.1.6-win-x64.zip -Algorithm SHA256
   ```

   Listenin kendisi projenin anahtarıyla imzalı (`.sha256.sig`); Windows'ta bu imzayı denetleyecek bir araç yok,
   yukarıdaki `swift release/signing.swift verify` satırı onu macOS'ta denetler.
2. İki zip'i de aynı klasöre çıkarın (sağ tık > Tümünü Ayıkla) ve `IntelXeManager-0.1.6-win-x64` içindeki
   `IntelXeManager.exe`'yi başlatın. Kurulacak bir şey yok: .NET, Windows App SDK ve EFI oluşturucunun Python'u
   klasörün içinde (yaklaşık 205 MB). Uygulama açılmış `-efi.zip`'i kendi klasörünün yanında bulur; EFI sayfasındaki
   *-efi.zip dosyasını seç...* ile zip'in kendisini de seçebilirsiniz.
3. **Uygulama bir kod imzalama sertifikasıyla imzalanmadı** (projenin böyle bir sertifikası yok). Bu yüzden Windows
   yayımcıyı tanımaz: SmartScreen "Windows bilgisayarınızı korudu" uyarısı verebilir (*Ek bilgi* > *Yine de çalıştır*),
   yönetici izni penceresinde de "Bilinmeyen yayımcı" yazar. Windows değiştirilmiş bir kopya için de aynısını gösterir;
   yani kopyanın projeninki olup olmadığını size Windows değil, 1. adımdaki SHA-256 söyler. Sürüm sayfası dışında bir
   yerden gelen kopyayı çalıştırmayın.
4. **Yönetici hakları yalnızca diskler için gerekir.** Uygulama sizin haklarınızla çalışır; Genel bakış sayfası
   bilgisayarı yönetici hakları olmadan okur. Windows, EFI bölümlerini yalnızca yöneticilerin bağlamasına izin verir; bu
   yüzden uygulama bir bölümü her okuduğunda, planladığında ve değiştirdiğinde kendi yardımcısını
   (`IntelXeManager.exe --efi-helper`) yönetici haklarıyla başlatır ve Windows her seferinde izin ister (bir değişiklik
   için iki kez: plan için ve değişikliğin kendisi için). USB bellekteki bir EFI klasörü ya da bir EFI kopyası için hiç
   izin istenmez. Aynı yardımcı *EFI'yi oluştur* için bilgisayarı da tarar (`--efi-helper sniff`); bir USB belleğe
   yazarken de izin istenir (`--stick-helper`, `--probe-helper`). Uygulama başka bir anda izin isterse hayır deyin ve bir
   issue açın.

Uygulamanın sayfaları: **Genel bakış** (kart ve sürücüsü, işlemci ve `HybridPM.kext`'e gerek olup olmadığı, 1. aşamanın
firmware ayarları, BitLocker, EFI bölümleri; hiçbir şeyi değiştirmez), **EFI** (2. aşama, Safari ayarı, 4. aşama ve geri
alma: her biri önce planını, yani adımlarını, notlarını ve planın SHA-256'sını gösterir ve yalnızca siz onaylayınca bir
şey değiştirir; ayrıca açılış günlüğü, [aşağıda](#açılmazsa)), **Rapor** (`tools/report/collect-windows.ps1`'i
çalıştırır, *Açılış günlükleri* seçiliyse EFI'deki OpenCore günlüklerini de ekler ve issue formunu açar; Xe tarama
belleğini de yazar ve okur, [Sorun bildirmek](#sorun-bildirmek)). Uygulama yalnızca EFI sayfasında seçilen EFI klasörüne
ya da bölümüne ve adını ve boyutunu onayladığınız bir USB belleğe yazar; hiçbir şeyi internete yüklemez. Windows 11'de,
yönetici yardımcısıyla birlikte, deneme EFI klasörlerinde denendi; **gerçek bir EFI bölümünü değiştirmek ve uygulamanın
değiştirdiği bir EFI'den açmak henüz denenmedi**, bu yüzden ilk paragraftaki USB belleği elinizin altında tutun. Betiği
`-efi.zip`'teki `README-Windows.md` anlatır.

**Kurulum** (deneysel), uygulamanın ilk sayfası: bu bilgisayardan kurulu bir macOS'a giden yolu adım adım gösterir; her
adım bitti, sıradaki ya da engelli olarak işaretlidir. Bu yolun sayfaları: **BIOS ayarları** (1. aşamanın 1. maddesindeki
firmware ayarlarını bu anakart üreticisinin menü adlarıyla listeler; her biri bitti, yapılacak ya da "Windows'tan
anlaşılamıyor" olarak işaretlidir; hiçbir ayarı değiştirmez), **EFI'yi oluştur** (bu bilgisayar için 1. aşamanın OpenCore
EFI'sini, sabitlenmiş bir commit'teki OpCore Simplify ile ve sabitlenmiş indirmelerden oluşturur; 2. aşama bu EFI'de
zaten yapılmıştır ve EFI `ocvalidate` ile denetlenir; tarama yalnızca DSDT, SSDT, APIC ve DMAR tablolarını saklar,
üretilen seri numaraları maskeli kalır) ve **USB kurulum belleği** (macOS'un recovery görüntüsünü Apple'ın
sunucularından indirir, Apple'ın chunklist'ine göre denetler ve bir EFI ile birlikte USB belleğe yazar). **Bir Windows
test bilgisayarında (14 Kasım) bu sayfalar `ocvalidate`'ten geçen bir EFI oluşturdu ve geri okumada aynı çıkan gerçek bir
kurulum belleği yazdı; ama yazdıkları bir bellekten henüz hiçbir bilgisayar açılmadı**: denenmiş yol, aşağıdaki elle
yapılan 1. aşama.

## 1. macOS'u sade bir OpenCore EFI'siyle kurmak

1. **BIOS:** Above 4G Decoding **açık**, Resizable BAR **açık**, CSM **kapalı**. Gerisini Dortania'nın platformunuz için
   anlattığı gibi ayarlayın.
2. **USB kurulum belleği:** Elinizdeki sisteme göre Dortania'yı izleyin:
   - [Windows'ta](https://dortania.github.io/OpenCore-Install-Guide/installer-guide/windows-install.html) ya da
     [Linux'ta](https://dortania.github.io/OpenCore-Install-Guide/installer-guide/linux-install.html): ikisi de macOS
     recovery'yi `macrecovery.py` ile indiriyor, macOS'un kendisi de kurulum sırasında internetten iniyor. Bu yüzden
     bilgisayarın recovery'de çalışan bir ağ bağlantısı olmalı (doğru kext'le Ethernet ya da bir Wi-Fi kext'i).
   - [macOS'ta](https://dortania.github.io/OpenCore-Install-Guide/installer-guide/mac-install.html): tam kurulum programıyla.
3. **EFI:** İşlemcinize ve anakartınıza göre Dortania'nın anlattığı gibi hazırlayın. Dortania'nın en yeni masaüstü sayfası
   [Comet Lake](https://dortania.github.io/OpenCore-Install-Guide/config.plist/comet-lake.html); Alder Lake ve Raptor Lake
   (12–14. nesil) için ayrı sayfası yok. Bu işlemcilerde o sayfayla birlikte
   [test makinesinin örnek yapılandırmasını](https://github.com/yusufklncc/intel-gpu-macos-driver/tree/main/efi/sample-z790)
   (Z790, i7-14700K) kullanın. Orada seri numaraları çıkarılmış `config.plist`, SSDT kaynakları, sürümleriyle birlikte
   kext listesi ve kurulum programı için nelerin değiştirildiği var.

   Gereken araçlar Windows'ta da çalışıyor: [SSDTTime](https://github.com/corpnewt/SSDTTime) (anakartınızın DSDT'sinden
   SSDT'leri üretir; DSDT'nin dökümünü Windows'ta da Linux'ta da alabilirsiniz),
   [USBToolBox](https://github.com/USBToolBox/tool) (USB haritası, Windows'ta) ve
   [GenSMBIOS](https://github.com/corpnewt/GenSMBIOS) (seri numaraları).

   Şimdilik karta özel bir şey eklemeyin: kart için ne `DeviceProperties` ne de WhateverGreen ayarı gerekiyor. Yalnızca
   şu iki ayarı şimdiden yapın: `UEFI > Output > ReconnectGraphicsOnConnect` ayarını `false` bırakın (Dortania'nın
   örneğinde öyle) ve `Booter > Quirks > ResizeAppleGpuBars = 10` yapın (Dortania'da varsayılan `-1`). Test makinesi
   macOS'u bu iki ayarla kurdu; `-1` ile ve Resizable BAR açıkken B580'de kurulum denenmedi. İki ayarın nedeni 2. aşamada
   anlatılıyor.
4. macOS'u kurun ve Kurulum Yardımcısı'nı bitirin. Ekran, macOS'un genel ekran sürücüsüyle kartın UEFI çıktısında (GOP)
   çalışır: tek çözünürlük, hızlandırma yok, her şey yavaş, uyku da henüz çalışmaz. Bu aşamada beklenen durum bu.

## 2. SIP, AMFI, quirk'ler ve konsol ayarı (`config.plist`)

EFI bölümü küçük bir FAT32 bölümüdür; her sistem onu bağlayabilir:

| Sistem | EFI bölümünü bağlamak |
|---|---|
| Windows | Yönetici olarak açılmış Komut İstemi'nde `mountvol S: /S`, ardından aynı pencereden ProperTree'yi başlatıp `S:\EFI\OC\config.plist` dosyasını açın; bitince `mountvol S: /D` |
| Linux | `lsblk -o NAME,FSTYPE,PARTTYPENAME` bölümü gösterir (`vfat`, tür "EFI System"); sonra bölüm adını kendinizinkiyle değiştirerek `sudo mount /dev/nvme0n1p1 /mnt` |
| macOS | `diskutil list` bölümü gösterir (açılış diskinizdeki `EFI`); sonra bölüm adını kendinizinkiyle değiştirerek `sudo diskutil mount disk0s1` |

**Windows'ta** Windows için IntelXe Manager ([yukarıda](#windowsta-windows-için-intelxe-manager)) 1-5. maddeleri
EFI sayfasında seçtiğiniz EFI'de (bir USB belleğin EFI klasöründe ya da bilgisayarın bir EFI bölümünde) sizin yerinize
yapar: *Ayarlar (2. aşama)* altında *Planı göster...*, ardından *Yedekle ve uygula*. Önce EFI klasörünün tamamını
yedekler, `config.plist`'in geri kalanını olduğu gibi bırakır; *Geri al* bölümü değişikliği geri alır. Betikle yapmak
için `-efi.zip`'teki `IntelXe-EFI.ps1`'i kullanın (sağ tık > PowerShell ile Çalıştır, *Settings: plan*, ardından
*Settings: apply*; ya da komut satırında `-Plan settings` / `-Apply settings`; ayrıntılar zip'teki `README-Windows.md`'de).
**Bilgisayardaki macOS'ta** IntelXe Manager
uygulamasının EFI sayfası (*Bu ayarları değiştir*) ya da macOS paketindeki `sudo ./efi.sh --settings-apply` aynı
değişiklikleri OpenCore'un açıldığı EFI'de yapar (bkz. 3.1).

`config.plist`'te:

1. `Booter > Quirks > ResizeAppleGpuBars = 10`. macOS, kartın 16 GB'lık BAR'ını eşleyemiyor; bu ayar 1 GB'lık bir pencere
   açıyor. OpenCore'un `ocvalidate` aracı bu değer için "unstable with macOS sleep-wake" uyarısı veriyor: kart uykuda
   BAR boyutunu unutuyor ve macOS onu yeniden ayarlamıyor; Apple'ın kendi GPU sürücüleri bununla başa çıkamıyor.
   IntelXe.kext, Linux'un yaptığı gibi, uyanırken boyutu yeniden ayarlıyor ve test makinesinde uyku çalışıyor. Bu uyarı
   beklenen bir durum; değeri 10'da bırakın. (9 da aynı uyarıyı alıyor, üstelik pencere yarıya iniyor; ocvalidate'in kabul
   ettiği 0 da çalışıyor ama 256 MB'lık penceresi masaüstü ve bir oyun için küçük kalıyor. Ayrıntılar: kaynak deposunda
   `docs/design/DESIGN-rebar-window.md`.)
2. `Kernel > Quirks > DisableIoMapper = true`.
3. `NVRAM > Add > 7C436110-AB2A-4BBB-A880-FE41995C9F82` altında:
   - `csr-active-config` = `03080000` (Data): imzasız kext'ler Yardımcı Kernel Koleksiyonu'ndan yüklenebilsin diye SIP
     gevşetiliyor;
   - `boot-args`'a `amfi_get_out_of_my_way=0x1` (yoksa WindowServer imzasız Metal sürücüsünü reddediyor) ve
     `intelxe_surfvram=2` (süreçler arasında VRAM'de paylaşılan IOSurface'ler) ekleyin.
4. `NVRAM > Delete > 7C436110-AB2A-4BBB-A880-FE41995C9F82` listesinde `csr-active-config` ve `boot-args` olmalı (Dortania'nın
   örnek yapılandırmasında var); yeni değerler ancak böyle eskilerin yerine geçer.
5. `UEFI > Output > ReconnectGraphicsOnConnect = false`. Bu ayar `true` olursa OpenCore'un RELEASE derlemesi, kartın UEFI
   ekran sürücüsü (GOP) yeniden bağlanmadan konsolu kurmaya çalışıyor. macOS konsol framebuffer'ı olmadan açılıyor, Lilu
   başlangıcını tamamlayamıyor, VirtualSMC'nin SMC aygıtı bağlanmıyor ve `watchdogd` açılışın yaklaşık 20. saniyesinde
   panik veriyor. OpenCore'un DEBUG derlemesi bu sorunu gizleyecek kadar yavaş.
6. Sistem diski kopyası için (GPU'da Safari, 3.3): `Misc > Security > SecureBootModel = Disabled`.

macOS'u yeniden başlatın. Ekran hâlâ UEFI çıktısında çalışır.

## 3. macOS tarafı (kurulacak bilgisayarda)

### 3.1 IntelXe Manager uygulamasıyla

<p align="center"><img src="https://raw.githubusercontent.com/yusufklncc/intel-gpu-macos-driver/main/docs/images/manager-overview.png" width="70%" alt="IntelXe Manager'ın sürücünün çalıştığını gösteren Genel bakış sayfası"></p>

Uygulama 3. ve 4. aşamaları bilgisayarda düğmelerle yapar ve her adımın neyi değiştireceğini parolanızı sormadan önce
gösterir. Asıl işi paketteki `install.sh` ve `efi.sh` yapar; yani sonuç 3.2'deki Terminal adımlarıyla aynıdır.

1. `IntelXe-Manager-0.1.6.zip`'i Terminal'den indirin (böylece "internetten indirildi" işareti almaz) ve Uygulamalar
   klasörünüze çıkarın:

   ```sh
   cd ~/Downloads
   curl -LO https://github.com/yusufklncc/intel-gpu-macos-driver/releases/download/v0.1.6/IntelXe-Manager-0.1.6.zip
   mkdir -p ~/Applications && ditto -x -k IntelXe-Manager-0.1.6.zip ~/Applications
   open ~/Applications/"IntelXe Manager.app"
   ```

   Uygulama bir Apple geliştirici kimliğiyle değil, ad hoc imzalanmıştır. Tarayıcıyla indirdiyseniz macOS uygulamayı
   açmaz; bu durumda `xattr -dr com.apple.quarantine ~/Applications/"IntelXe Manager.app"` komutunu çalıştırın ya da
   uygulamayı bir kez açmayı deneyip Sistem Ayarları > Gizlilik ve Güvenlik'te izin verin (*Yine de Aç*).

   Uygulama ilk açılışta **kurulum yardımcısını** başlatır: yardımcı aşağıdaki adımlarda sayfa sayfa ilerler, yeniden
   başlattıktan sonra uygulamayı açtığınızda kaldığı yerden devam eder ve bir GPU testiyle, ardından isteğe bağlı eklerle
   (GPU'da Safari, Zink, HiDPI, menü çubuğu öğesi) biter. İlk adımda Safari'nin ayarını da aynı değişikliğe katabilir
   (*Safari'yi de GPU için hazırla*). Aynı adımları uygulamanın sayfalarında elle yapmak için:
2. **Genel bakış**, sürücünün gereksinimlerini (macOS, SIP, AMFI boot-arg'ı, kart) ve nelerin kurulu olduğunu gösterir.
3. **Sürücü:** *v0.1.6 sürümünü indir* macOS'unuza uygun paketi indirir, sürümün sağlama listesinin projenin
   anahtarıyla imzalı olduğunu denetler ve paketi listeyle karşılaştırır. EFI
   sayfası da dosyalarını bu paketten alır.
4. 2. aşamayı henüz yapmadınız mı? Kurulum buna bağlı (SIP gevşetilmemişse ve AMFI boot-arg'ı yoksa kurulum başlamaz):
   **EFI (OpenCore)** sayfasında "Ayarlar (2. aşama)" altında *Bu ayarları değiştir*'e basın (OpenCore açıldığı EFI'yi
   kaydetmediyse önce bölümü seçin ya da *Bir EFI klasörü kullan…* ile bir klasör seçin), sonra yeniden başlatıp
   uygulamayı yeniden açın.
5. **Sürücü:** *Bu paketi kur* (`sudo ./install.sh --apply` ile aynı). **Henüz yeniden başlatmayın.**
6. **EFI (OpenCore):** "IntelXe.kext ve SSDT-ARCOFF (4. aşama)" altında *Bu değişiklikleri yap*'a basın; ne değiştirdiği
   aşağıda, 4. aşamada anlatılıyor. Uygulama her EFI değişikliğinden önce değişiklik listesini gösterir ve onayınızı
   ister. Sonra yeniden başlatın.

Uygulama ayrıca sürücüyü onarır, geri alır, kaldırır, isteğe bağlı zink paketini kurar, Safari için sistem diski
kopyasını kurar (3.3; Sürücü sayfasında *Safari ve WebKit*, `SecureBootModel` EFI sayfasında), HiDPI modlarını açar
(Ekran sayfası), GPU'yu test eder, tanılama raporu hazırlar ve sonraki sürümleri eskisinin üzerine kurar. Bir macOS
güncellemesinden önce Genel bakış, bu sürümün o güncellemeyi destekleyip desteklemediğini söyler; *macOS güncellemesine
hazırla*, güncellemeden sonraki ilk açılışta sürücüyü onaran tek seferlik bir denetim bırakır (EFI'ye hiç dokunmaz;
henüz gerçek bir güncellemeyle denenmedi). İsteğe bağlı oyun modu (Güç ve uygulama sayfası), önde bir oyun varken
GPU'nun en düşük saat hızını yükseltir. Uygulama yalnızca GitHub'a bağlanır, veri göndermez. Sürücü sayfası test
makinesinde, EFI sayfası da aynı makinede SSD'nin EFI'sinde kullanıldı. Gerçek bir EFI'de *Geri al* düğmesi ve temiz
bir macOS'ta uygulamayla baştan sona kurulum henüz denenmedi. Arayüz, sistem dilinize göre İngilizce ya da Türkçedir;
burada Türkçe arayüzdeki adlar kullanılıyor.

### 3.2 Terminal'de

1. Paketi Terminal'den indirip arşivden çıkarın. `curl` ile indirilen dosyalar "internetten indirildi" işareti
   taşımadığı için Gatekeeper onlara karışmaz:

   ```sh
   cd ~/Downloads
   curl -LO https://github.com/yusufklncc/intel-gpu-macos-driver/releases/download/v0.1.6/intel-gpu-macos-driver-0.1.6-tahoe.zip
   ditto -x -k intel-gpu-macos-driver-0.1.6-tahoe.zip .
   cd intel-gpu-macos-driver-0.1.6-tahoe
   ```

   Tarayıcıyla indirdiyseniz arşivi çift tıklayarak açın, klasörde bir Terminal penceresi açın ve önce
   `xattr -dr com.apple.quarantine .` komutunu çalıştırın.
2. Önce kontrol edin, sonra kurun:

   ```sh
   ./install.sh                # yalnızca kontrol: macOS, SIP, AMFI, kart, sağlama toplamları, imzalar
   sudo ./install.sh --apply
   ```

   Bu aşamada denetim `IntelXe.kext`'in yüklü olmadığını bildirir; o 4. aşamada eklenecek. `--apply`, değiştirdiği her
   şeyi `/Library/Application Support/intel-gpu-macos-driver/backup-<tarih>/` altına yedekler, kext'leri ve Metal
   sürücüsünü `/Library`'ye kopyalar, açılış korumasının LaunchDaemon'ını kurar (bkz. [Açılmazsa](#açılmazsa)) ve
   Yardımcı Kernel Koleksiyonu'nu yeniden oluşturur.
3. **Henüz yeniden başlatmayın:** önce 4. aşamayı yapın.

### 3.3 Safari için önerilir: sistem diski kopyası

Safari'nin GPU süreci yalnızca sistem diskinden kod yükleyebiliyor. Bu kopya olmadan Safari, macOS'un yazılımla çizim
yolunda çalışır: daha yavaştır ve CSS renk filtresi kullanan bazı sayfa öğeleri çizilmez (Metal aygıtı olmadığında
CoreGraphics'te görülen bir sorun; Safari > Ayarlar > Özellik Bayrakları'nda *GraphicsContext Filter Rendering*
kapatılınca düzelir; bu sekme, Gelişmiş > *Web geliştiricileri için özellikleri göster* açılınca görünür).
`system-volume/ssv-install.sh`, OpenCore Legacy Patcher'ın kök yamalarının yaptığı gibi Metal sürücüsünü sistem diskine
kopyalar ve yeni bir açılış anlık görüntüsü oluşturur. `Misc > Security > SecureBootModel = Disabled` gerekir (2. aşama,
6. madde). Bu ayar başka hiçbir şey için gerekmiyor. Bir macOS güncellemesi bu kopyayı siler (`./ssv-install.sh --check`
bunu bildirir; kopyayı yeniden kurun). macOS 26.7.1'de denendi: Safari B580'de çalışıyor ve sayfaları filtreler dahil
doğru çiziliyor. Safari'nin GPU süreci, sandbox nedeniyle çevrilmiş shader'ları Safari'nin bir çalışmasından ötekine
saklayamıyor; bu yüzden Safari her açıldığında ilk sayfalar birkaç saniye bekletir.

```sh
cd system-volume && ./ssv-install.sh          # yalnızca kontrol
sudo ./ssv-install.sh --apply
```

Daha eski bir sürümün sistem diski kopyası kuruluysa `install.sh --apply`, bu arada bütün süreçler `/Library` kopyasını
kullansın diye `/Library/GPUBundles/NVMTLDriver.use-library` dosyasını oluşturur. `ssv-install.sh --apply`, bu paketin
kopyasını kurduktan sonra dosyayı kendisi siler (yalnızca `install.sh`'in oluşturduğu dosyayı siler; dosyayı siz
oluşturduysanız yerinde kalır ve betik bunu bildirir). `./install.sh --status`, `use_library` (`use_library_by` ile) ve
`system_volume_state` değerlerini gösterir: `ssv-install.sh --apply` ile yeniden başlatma arasında `pending`, sonra
`same`.

## 4. IntelXe.kext ve SSDT-ARCOFF (EFI)

Dosyaları `intel-gpu-macos-driver-0.1.6-efi.zip`'ten alın (macOS paketinde de `EFI/OC/` altında aynıları var). EFI bölümünü
2. aşamadaki gibi bağlayın; kurulacak bilgisayarın kendisindeyseniz bölüm adını kendinizinkiyle değiştirerek
`sudo diskutil mount disk0s1` komutunu çalıştırın.

Kurulacak bilgisayarın kendisinde, macOS paketindeki `efi.sh` 1-3. maddeleri (ve 2. aşamanın ayarlarını) sizin yerinize
yapar: OpenCore'un açıldığı EFI'yi bulur, EFI klasörünün tamamını yedekler, dosyaları kopyalar ve `config.plist`'teki
kayıtlarını etkinleştirir. Kartınızın ACPI yolunu (2. madde) I/O Registry'den okur; yol paketteki tablonunkinden
farklıysa `SSDT-ARCOFF.aml`'in bir kopyasına sizin yolunuzu yazıp onu ekler (plan bunu "adapted to this card's port"
diye belirtir); böylece sizin düzenleyip derlemeniz gereken bir tablo kalmaz. 12.-14. nesil hibrit bir Intel işlemcide
`HybridPM.kext`'i de ekler (aşağıda, "Hibrit Intel işlemciler"). `./efi.sh --plan` değişiklikleri gösterir,
`sudo ./efi.sh --apply` yapar, `sudo ./efi.sh --undo` geri alır; `--settings-plan` / `--settings-apply` 2. aşamayı yapar.
Bütün seçenekleri görmek için yalnızca `./efi.sh` yazın. IntelXe Manager uygulamasının EFI sayfası da aynısını yapar.

**Windows'ta** Windows için IntelXe Manager 1. ve 3. maddeleri, EFI sayfasının *IntelXe.kext ve SSDT-ARCOFF
(4. aşama)* bölümünde seçtiğiniz EFI'de, yedek alarak ve geri alınabilir biçimde yapar; `-efi.zip`'teki `IntelXe-EFI.ps1`
aynısını bir betikle yapar (*Driver: plan*, *Driver: apply*). Windows, ACPI yolunu `efi.sh`'in macOS'ta yaptığı gibi
denetleyemez; bu yüzden ikisi de `SSDT-ARCOFF.aml`'i yalnızca 2. maddedeki kart yolunuzu onayladığınızda ekler: Windows
kartın takılı olduğu bilgisayarda çalışıyorsa uygulama yolu Aygıt Yöneticisi'nden doldurur ve tabloyu yalnızca macOS'un
kurulu olduğunu, 3. aşamanın bittiğini ve yolu denetlediğinizi işaretlediğinizde ekler (betikte: kutusu ve
`-CardAcpiPath`). Yol paketteki tablonunkinden farklıysa ikisi de `efi.sh` gibi tablonun bir kopyasını uyarlar. 12.-14.
nesil hibrit bir Intel işlemcide `HybridPM.kext`'i de eklerler; çalıştıkları bilgisayarın işlemcisine baktıkları için
onları bu EFI ile açılacak bilgisayarda çalıştırın.

1. `IntelXe.kext`'i `EFI/OC/Kexts/` klasörüne kopyalayın ve `Kernel > Add` altına ekleyin: `BundlePath IntelXe.kext`,
   `ExecutablePath Contents/MacOS/IntelXe`, `PlistPath Contents/Info.plist`, `Enabled true`. Bu, GPU sürücüsünün
   kendisidir (GuC/HuC firmware'i içindedir); Lilu gerektirmez.
2. `SSDT-ARCOFF.aml`, kartı macOS'un genel ekran sürücüsünden gizler (gizlenmezse Tahoe'da kartı o sürücü alır ve IntelXe
   kext'leri başlayamaz). **ACPI yolu anakarta göre değişir.** Paketteki tablo, yuvanın kök portunu `\_SB.PC00.PEG1`, kartın kendi PCIe
   anahtarını da onun altında `PEGP` diye adlandıran bir anakart için. `PEGP`'nin altı her B580'de aynı: `PCI(0100)`'de bir alt
   port, `PCI(0000)`'da GPU. Kendi yolunuzu şöyle bulun:

   | Sistem | Nasıl |
   |---|---|
   | Windows | Aygıt Yöneticisi → Görüntü bağdaştırıcıları → Arc kartı (Intel'in sürücüsü yoksa "Microsoft Temel Görüntü Bağdaştırıcısı") → Özellikler → Ayrıntılar → **Konum yolları**. `ACPI(...)` ile başlayan satır `ACPI(_SB_)#ACPI(PC00)#ACPI(PEG1)#ACPI(PEGP)#PCI(0100)#PCI(0000)` gibi görünür; bu `\_SB.PC00.PEG1.PEGP` demektir |
   | Linux | `lspci -nn \| grep -e e20b -e e20c -e e211 -e e212` GPU'yu verir (ör. `03:00.0`); `lspci -tv` GPU'nun iki seviye üstünde kartın üst (upstream) portunu gösterir (ör. `01:00.0`); `cat /sys/bus/pci/devices/0000:01:00.0/firmware_node/path` ACPI yolunu yazar, ör. `\_SB_.PC00.PEG1.PEGP` |
   | macOS | [gfxutil](https://github.com/acidanthera/gfxutil/releases) bütün PCI aygıtlarını ACPI yollarıyla listeler: `8086:e20b` satırına bakın (B570'te `8086:e20c`, Pro B60'ta `8086:e211`, Pro B50'de `8086:e212`). IORegistryExplorer aynı adları IOService düzleminde gösterir (`PC00@0` → `PEG1@1` → `PEGP@0` → ...) |

   Yolunuz paketteki gibiyse (`\_SB.PC00.PEG1.PEGP`) `SSDT-ARCOFF.aml`'i olduğu gibi kullanın. Değilse macOS'ta tabloyu
   `efi.sh` ya da uygulamanın EFI sayfası uyarlasın (yukarıda); ikisi de yol ne uzunlukta olursa olsun bunu yapabilir. Elle
   yapmak için `SSDT-ARCOFF.dsl`'de yolun geçtiği iki yere (`External` ve `Scope` satırları) kendi yolunuzu yazıp
   `iasl SSDT-ARCOFF.dsl` ile derleyin. `iasl` ACPICA'nın bir parçasıdır: Windows'ta ACPICA'nın Windows araçları (acpica.org, Downloads), Linux'ta `acpica-tools` (ya da `iasl`) paketi, macOS'ta
   `brew install acpica` ya da [MaciASL](https://github.com/acidanthera/MaciASL/releases)'in içindeki `iasl`. Hiçbir `ACPI(...)`
   parçası kartın üst portunu adlandırmıyorsa (son `ACPI(...)` parçasından sonra iki değil üç `PCI(...)` geliyorsa) tablonun bir köprü
   seviyesi daha içermesi gerekir: Konum yolları satırınızla bir issue açın.
3. `.aml` dosyasını `EFI/OC/ACPI/` klasörüne kopyalayın ve `ACPI > Add` altına ekleyin (`Path SSDT-ARCOFF.aml`, `Enabled true`).
4. Yeniden başlatın. Masaüstü artık hızlandırmalı çalışmalı: saydamlık ve bulanıklık, akıcı pencere sürükleme,
   `Bu Mac Hakkında`'da kartın adı görünür (12 GB'lık Arc B580). Terminal'de `kmutil showloaded | grep -i intelxe` `IntelXe` ve `IntelXeFB`'yi listeler, `sysctl
   debug.nmxaccelfb` de `1` yazar.

## Hibrit Intel işlemciler (12.-14. nesil): HybridPM.kext

macOS'un işlemci güç yönetimi (XCPM), performans ve verimlilik çekirdekleri olan işlemcilerden önce yazıldı. Alder Lake ve
Raptor Lake'te P çekirdeklerini sınırlıyor (bir i7-14700K 5,6 yerine 4,4 GHz'de kaldı) ve tam yüklü çekirdekleri 800 MHz'de
tutabiliyor. GPU sürücüsü her komut arabelleği için işlemcide epey iş yapıyor, bu yüzden GPU da yavaş görünüyor: test
makinesinde Geekbench'in GPU puanları `HybridPM.kext` ile yaklaşık üçte bir daha yüksek çıktı, tek bir işlemci iş
parçacığı da iki kattan fazla hızlandı. HybridPM GPU sürücüsünden ayrıdır ve isteğe bağlıdır; `-efi.zip`'te ve macOS
paketinin `EFI/OC/Kexts/` klasöründe bulunur.

**Hangi işlemciler:** P ve E çekirdekli 12., 13. ve 14. nesil Core i3/i5/i7/i9 (Alder Lake ve Raptor Lake, masaüstü ve dizüstü;
örneğin i5-12600K, i5-13400F, i7-14700K, i7-13620H) ve Core Ultra 200S masaüstü işlemciler (Arrow Lake, örneğin Core Ultra 7
265K; bir katkıda bulunan ölçtü, test makinesinde ölçülmedi). HybridPM öteki Core Ultra'lara, E çekirdeği olmayan
işlemcilere ve daha eski işlemcilere dokunmaz (yalnızca nedenini günlüğe yazar). Model listesi depodaki
`kernel/HybridPM/README.md` dosyasında.

**Eklemek:** kurulacak bilgisayarın kendisinde bunu 4. aşama yapar: `efi.sh --plan` / `sudo ./efi.sh --apply` ve
uygulamanın EFI sayfası, işlemcinin adı bunlardan biriyse `HybridPM.kext`'i `IntelXe.kext` ile birlikte kopyalar ve
kaydını `Lilu.kext`'in kaydının hemen altına ekler (kayıt zaten varsa etkinleştirilir; aynı adlı eski bir derleme
yenisiyle değiştirilir). EFI Lilu yüklemiyorsa HybridPM'i eklemez ve bunu bir notla belirtir. Windows için IntelXe
Manager ve `IntelXe-EFI.ps1` de Windows'ta, çalıştıkları bilgisayarın işlemcisine bakarak aynısını yapar. İşlemcinin
adına baktıkları için bir Core Ultra'da HybridPM'i henüz eklemezler: Arrow Lake'te elle ekleyin. Elle:

1. HybridPM bir Lilu eklentisidir. Çoğu OpenCore EFI'sinde VirtualSMC için `Lilu.kext` zaten var; sizinkinde yoksa
   [acidanthera/Lilu](https://github.com/acidanthera/Lilu/releases)'dan ekleyin (test makinesinde 1.7.2). Lilu bu
   pakette yok.
2. `HybridPM.kext`'i `EFI/OC/Kexts/` klasörüne kopyalayın ve `Kernel > Add` içinde `Lilu.kext`'in **altına** ekleyin:
   `BundlePath HybridPM.kext`, `ExecutablePath Contents/MacOS/HybridPM`, `PlistPath Contents/Info.plist`, `Enabled true`.
3. Yeniden başlatın. HybridPM açılıştan 20 saniye sonra devreye girer.

**Kontrol etmek:**

```sh
sysctl debug.hybridpm.enabled debug.hybridpm.why debug.hybridpm.route
# debug.hybridpm.enabled: 1, debug.hybridpm.why: on (model 0xB7), debug.hybridpm.route: 1
./hpmstat 1000 3     # paket klasöründe: her çekirdeğin saati ve yükü, HWP sınırları, paket gücü
```

`enabled` 0 ise nedeni `why`'da yazar: `not hybrid` ya da `unsupported model` (Core Ultra ve diğerleri); bir macOS
güncellemesi HybridPM'in düzelttiği rutini değiştirdiyse `route 3` görünür. Bu durumlarda macOS, HybridPM yokmuş gibi
çalışır. `debug.hybridpm` hiç yoksa HybridPM ya da Lilu yüklenmemiştir (`kmutil showloaded | grep -i -e lilu -e hybridpm`).

**Kapatmak:** boot-args'a `-hpmoff` ekleyin ya da `Kernel > Add` kaydını `Enabled false` yapın. Bir sonraki yeniden başlatmaya
kadar `sudo sysctl debug.hybridpm.hwp=0 debug.hybridpm.autonomous=0 debug.hybridpm.epp=-1 debug.hybridpm.eet_off=0`
macOS'un kendi ayarlarını geri getirir.

## İsteğe bağlı: zink ile OpenGL

macOS'un kendi OpenGL'i Metal'in üzerinde çalışıyor ve bu sürücüde yavaş. LWJGL 3 Java oyunları (Minecraft Java 1.13 ve
sonrası) bunun yerine Mesa'nın zink'ini kullanabilir: zink, OpenGL çağrılarını doğrudan sürücünün Vulkan'ına çeviriyor.
Zink'i yalnızca OpenGL'i GLFW/LWJGL ile açan uygulamalar kullanır; geri kalan her şey Apple'ın OpenGL'inde kalır.
Sürücünün kurulu olması ve `intelxe_surfvram=2` ayarının yapılmış olması gerekir (2. aşama). IntelXe Manager'ın
**OpenGL (Zink)** sayfası zink'i kurar ve açar; ya da Terminal'de:

```sh
cd ~/Downloads
curl -LO https://github.com/yusufklncc/intel-gpu-macos-driver/releases/download/v0.1.6/intel-gpu-macos-driver-0.1.6-zink-gl.zip
ditto -x -k intel-gpu-macos-driver-0.1.6-zink-gl.zip .
cd intel-gpu-macos-driver-0.1.6-zink-gl
sudo ./install.sh                # /Library/IntelXe/GL ve Vulkan yükleyicisi; değiştirdiklerini yedekler
./install.sh --test              # B580'de zink üzerinden bir GL bağlamı açar
sudo ./install.sh --global on    # bundan sonra açılan bütün LWJGL 3 uygulamaları zink'i kullanır
```

`--global off` geri alır, `--undo` kaldırır; ayrıntılar zip'teki `README.md`'de. Test makinesinde ölçüldü (macOS 26.7.1,
Minecraft 1.21.1, Sodium ve Iris ile, pencere modunda, dünyada aynı noktada): BSL shader paketiyle zink'te 197–205 fps,
aynı bilgisayarda Windows'ta Intel'in sürücüsüyle 214–218; shader paketi olmadan zink'te
1400–1500 fps, Windows'ta 700–1000. Deneysel: CGL/NSOpenGL kullanan yerel bir macOS uygulaması da zink'e geçebilir, her
seferinde bir uygulama: `./cglzink-app.sh add /Applications/X.app`, `~/Applications/X (zink).app` uygulamasını oluşturur
(zip'teki `README.md`, "Native OpenGL applications"); ya da varsayılan olarak kapalı deneysel bir anahtarla hepsi
birden: `sudo ./install.sh
--global-gl on` ya da IntelXe Manager'ın Zink sayfası; bu sayfa zink kullanan ve Apple'ın OpenGL'inde kalan
uygulamaların listesini de tutuyor. macOS'un oturum açılışında kendiliğinden
yeniden açtığı bir başlatıcı, ayar devreye girmeden başlayıp Apple'ın OpenGL'inde çalışabilir: başlatıcıdan çıkıp
yeniden açın (zink açıkken Java "Picked up JAVA_TOOL_OPTIONS" yazar).

## İsteğe bağlı, deneysel: Wine'daki Windows oyunları için Vulkan

**Donanımda Wine ile henüz denenmedi.** Wine, Direct3D oyunlarını Vulkan kullanan DXVK (Direct3D 8-11) ve vkd3d-proton
(Direct3D 12) ile çalıştırır; macOS'ta bu Vulkan normalde MoltenVK'dır ve o da Vulkan'ı bir kez daha Metal'e çevirir.
`-vulkan.zip` bunların yerine sürücünün kendi Vulkan'ını (ANV) kullandırır, her seferinde tek bir oyunda: onu yalnızca
`intelxe-vk run` ile başlattığınız program kullanır; Wine, Wine'ın MoltenVK'sı ve sürücü olduğu gibi kalır. Sürücünün
kurulu olması ve 64 bit bir Wine öneki gerekir; Wine derlemenizin bunu kullanıp kullanamayacağını `intelxe-vk check`
söyler.

```sh
cd ~/Downloads
ditto -x -k intel-gpu-macos-driver-0.1.6-vulkan.zip .
cd intel-gpu-macos-driver-0.1.6-vulkan
sudo ./install.sh                   # /Library/IntelXe/Vulkan; değiştirdiklerini yedekler
./install.sh --test                 # sağlayıcı üzerinden 120 karelik bir pencere: PASS
V=/Library/IntelXe/Vulkan/bin/intelxe-vk
$V check "/Applications/Wine Stable.app"   # o Wine'ın açtığı Vulkan kitaplığı; önekleriniz
$V fetch                            # DXVK ve vkd3d-proton'u indirir (sabit sürüm, sağlama toplamı denetimli)
$V prefix-install ~/.wine           # DLL'leri öneke kurar; önekin kendi DLL'leri saklanır
WINEPREFIX=~/.wine $V run wine oyun.exe
```

`$V prefix-restore ~/.wine` öneki yeniden Wine'ın kendi Direct3D'sine döndürür, `sudo ./install.sh --undo` da gerisini
kaldırır. `fetch`, vkd3d-proton'un arşivini açmak için `zstd` ister (`brew install zstd`). Ayrıntılar ve sınırlar zip'teki
`README.md`'de (32 bit oyunlar ve HDR denenmedi; önekteki `dxvk.conf`, DXVK'nın iki descriptor özelliğini bilerek kapatır).

## İsteğe bağlı: HiDPI modları (ekran ölçeklendirme)

Windows'taki ekran ölçeklendirmesine benzer: Sistem Ayarları > Ekranlar, yazıyı ve pencereleri daha büyük ama keskin
gösteren "gibi görünür" boyutlar sunar (Daha Büyük Metin ... Daha Fazla Alan). 2560x1440 bir monitörde bunlar 2048x1152,
1920x1080, 1706x960, 1600x900 ve 1280x720. Bu boyutlar monitörün tam yenileme hızında çalışır (test monitöründe 180 Hz
ve Değişken): macOS görüntüyü iki kat büyük çizer, GPU da her kareyi panelin çözünürlüğüne küçültür. Siz açmadıkça
kapalıdırlar. IntelXe Manager'ın **Ekran** sayfasından açıp kapatabilirsiniz; ya da Terminal'de, paket klasöründe:

```sh
./hidpi.sh               # şu an ne ayarlı
sudo ./hidpi.sh --on     # NVRAM'de intelxefb-hidpi=1, HiDPI boyutlarını içeren bir ekran ayarı, DisplayResolutionEnabled
sudo ./hidpi.sh --off    # yalnızca --on'un yazdıklarını geri alır
```

Hangisini çalıştırırsanız çalıştırın ardından yeniden başlatın, sonra Sistem Ayarları > Ekranlar'dan bir boyut seçin.
Ekran ayarı, diğer HiDPI araçlarının kullandığı yere yazılır (`/Library/Displays/Contents/Resources/Overrides`);
monitörünüz için başka bir aracın (one-key-hidpi vb.) yazdığı bir ayar zaten varsa `hidpi.sh` ona dokunmaz ve durur:
önce o ayarı kaldırın. Bir boyut ekranı siyah bırakırsa SSH üzerinden `sudo ./hidpi.sh --off` ile modları kapatıp
yeniden başlatın; OpenCore'un açılış menüsündeki **Reset NVRAM** de modları kapatır (anahtar bir NVRAM değişkenidir).

## Açılmazsa

- **Siyah ekran ya da sürekli yeniden başlama:** Çalışan EFI'nin bulunduğu USB bellekten açın ya da OpenCore'un açılış
  menüsünde macOS seçiliyken Shift'e basarak güvenli modda açın (güvenli mod Yardımcı Kernel Koleksiyonu'nu atlar). Sonra
  `sudo ./install.sh --undo` çalıştırın ve `IntelXe.kext` ile `SSDT-ARCOFF.aml`'i EFI'den çıkarın.
- **İlk dakikada çökmeler (açılış koruması):** Hızlandırmalı ilk dakikası dolmadan biten tek bir açılış (panik ya da güç
  düğmesine basılı tutarak kapatma) yalnızca bir sonraki açılışın yeniden denemesine yol açar. Üst üste iki böyle
  açılıştan sonra sonraki açılışlar hızlandırmayı en güvenli ayarlarla açar (async ve flip kapalı;
  `ioreg -rc IntelXeFramebuffer -d1 | grep AccelSafe` `Yes` yazar); bir dakika sorunsuz çalışan bir açılıştan sonra
  koruma normale döner. Üst üste dört böyle açılıştan sonra hızlandırma durdurulur. 0.1.5'ten itibaren hızlandırması
  durdurulmuş bir açılış, masaüstünü **firmware'in framebuffer'ında** gösterir: firmware'in açtığı görüntü üzerinde
  Apple'ın kendi framebuffer sürücüsü çalışır; macOS kurulum programı da bu kartta böyle çalışır. Hızlandırma ve Metal
  yoktur, tek bir çözünürlük vardır; bu açılışta Mac'i uyku moduna almayın. Oturum açın, nedenini bulun
  (`/Library/Logs/DiagnosticReports` içindeki panik raporu, IntelXe Manager'ın Rapor sayfası), sürücüyü onarın ya da
  kaldırın, sonra korumayı sıfırlayıp yeniden başlatın: IntelXe Manager'ın Genel bakış sayfasında *Açılış koruması* >
  *Açılış korumasını sıfırla*, ya da Terminal'de (ya da SSH ile):

  ```sh
  sudo nvram intelxe-accel-off=ok intelxe-guard=ok
  ```

  `sysctl debug.intelxe.bootguard` korumanın bu açılışı nasıl değerlendirdiğini gösterir (`state=held`,
  `display=firmware`). İkisi birden gerekiyor, çünkü hızlandırması durdurulmuş bir açılışta sayaç, temiz bir yeniden
  başlatmaya kadar yalnızca bellekte sıfırlanır. `nvram -d` macOS 26'da hata vermez ama değişkeni de silmez; `=ok` her
  sürümde çalışır. `-intelxe_noaccel` boot-arg'ıyla ya da güvenli modda yapılan bir açılış da firmware'in framebuffer'ını
  kullanır. 0.1.4'teki davranışa (hızlandırmasız IntelXeFB; macOS 26'da Apple logosunda kalır) dönmek için:
  `sudo nvram intelxe-held-display=fb`; `=ok` bunu geri alır.

  Hızlandırması durdurulmuş açılış yine de Apple logosunda kalırsa (ya EFI'deki `IntelXe.kext` 0.1.5'ten eski ya da
  ekranın devri başarısız olmuş: yukarıdaki sysctl'deki `handoff=`, SSH ile okunur): yeniden başlatın ve OpenCore'un
  açılış menüsünde **Reset NVRAM**'i seçin (gizliyse Boşluk tuşuyla görünür). Bu, korumayı temizler; OpenCore kendi
  boot-args ve SIP ayarını her açılışta yeniden yazar (B580'de denendi). Ya da SSH ile bağlanıp korumayı yukarıdaki
  komutla temizleyin ve yeniden başlatın.
- **Açılışın yaklaşık 20. saniyesinde panik, `watchdogd ... exited`** (`/Library/Logs/DiagnosticReports/Kernel-*.panic`
  içindeki raporda `watchdogd` geçiyorsa): `UEFI > Output > ReconnectGraphicsOnConnect` hâlâ `true`; ayarı `false` yapın
  (2. aşama, 5. madde).
- **macOS açılmıyor ve nedenini anlayamıyorsunuz: açılış günlüğü.** Açılış günlüğü açıkken OpenCore bir sonraki açılışta
  kendi günlüğünü, boot.efi'nin satırlarını ve kaydettiği panikleri, rapora eklenmek üzere EFI'nin köküne yazar. Çalışan
  bir açılıştan (USB belleğiniz) ya da Windows'tan etkinleştirin: uygulamaların EFI sayfası (*Açılış günlüğü (sorun
  giderme için)*), `./efi.sh --bootlog-plan` sonra `sudo ./efi.sh --bootlog-apply`, ya da `IntelXe-EFI.ps1 -Plan bootlog`
  sonra `-Apply bootlog -Expect HASH`. Sorunlu açılışı bir kez daha deneyin, sonra günlükleri raporla birlikte toplayın
  ([Sorun bildirmek](#sorun-bildirmek)). Açık kaldığı sürece her açılış yavaşlar: işiniz bitince kapatın (aynı bölüm,
  `--bootlog-off-apply`, `-Apply bootlog-off`).
- **Her şeyi geri almak:** `sudo ./install.sh --undo` çalıştırın, sonra `IntelXe.kext` ve `SSDT-ARCOFF.aml`'i EFI'den
  çıkarıp yeniden başlatın.
- **`install.sh`'in diğer komutları:** `./install.sh --status` kurulu olanları bu paketle karşılaştırır, hiçbir şeyi
  değiştirmez. `sudo ./install.sh --repair` kurulu dosyalardan paketle uyuşmayanları paketten yeniden kopyalar, son yedeğe
  dokunmaz. `sudo ./install.sh --uninstall` kext'leri ve Metal sürücüsünü `/Library`'den siler, yedekleri saklar;
  `--undo` ise son kurulumun değiştirdiği dosyaları eski hâline getirir.
  `--uninstall`'dan sonra, yeniden başlatmadan önce `IntelXe.kext`'i EFI'den çıkarın.

## Yükseltme

**0.1.3'ten:** `IntelXe.kext` (EFI'de), `IntelXeFB.kext`, `NMXAccel.kext` ve Metal sürücüsü yenilendi (lock V2,
çevirici düzeltmesi, B570'in kimliği, GPU saati ve sıcaklık okumaları, doğrudan gönderim); Mesa ve video kodekleri
0.1.3'tekilerle aynı. Paketi kurun (uygulamada *v0.1.6 sürümünü indir*, sonra *Bu paketi kur*; ya da
`sudo ./install.sh --apply`), ardından yeniden başlatmadan önce EFI'deki `IntelXe.kext`'i değiştirin (macOS'ta uygulamanın
EFI sayfası ya da `sudo ./efi.sh --apply`, Windows'ta Windows için IntelXe Manager ya da `IntelXe-EFI.ps1`; hepsi hibrit bir Intel işlemcide `HybridPM.kext`'i de ekler;
ya da elle; `config.plist` kaydı olduğu gibi kalır). Sistem diski kopyasını kurduysanız (3.3) bu paketin kopyasını da
kurun (Sürücü sayfasında *Safari ve WebKit* ya da `sudo ./system-volume/ssv-install.sh --apply`); Safari yeni Metal
sürücüsünü ancak böyle kullanır. Uygulamalar çalışırken GPU saatini yüksek tutan GuC düşük gecikme ipucu 0.1.4'ten beri
varsayılan olarak kapalı (açıkken, testlerde GPU uzun süre boşta kaldıktan sonra GPU hatası görüldü); uygulamanın *Güç ve
uygulama* sayfasından (*Uygulamalar için hızlı GPU yanıtı*) ya da `sudo touch /Library/GPUBundles/NVMTLDriver.low-latency`
ile açabilirsiniz. Doğrudan gönderim 0.1.4'te kapalıydı, 0.1.5'ten itibaren açık; `-intelxe_nodirect` boot-arg'ı onu
kapatır.

**0.1.2'den:** 0.1.3'tekiyle aynı adımlar; ayrıca `NMXAGDC.kext`, video çözücü ve Mesa'nın Vulkan sürücüsü de yenilendi
(firmware'in sunduğundan yüksek yenileme hızları, varsayılan olarak açık Adaptive-Sync modları, monitörün tam yenileme
hızında isteğe bağlı HiDPI modları, tam aralıklı videonun tam aralıklı çözülmesi, WindowServer'ın derlenmiş
shader'larını saklaması). Sistem diski kopyasını kurduysanız `install.sh --apply`, kopya eski Mesa'yı içerdiği için
`/Library/GPUBundles/NVMTLDriver.use-library` dosyasını oluşturur; bu paketin kopyasını kurduğunuzda (uygulamanın Sürücü
sayfasından ya da `sudo ./system-volume/ssv-install.sh --apply` ile) dosya yeniden silinir (3.3); sonra yeniden başlatın.

**0.1.0 ya da 0.1.1'den:** Bütün kext'ler, Metal sürücüsü ve video kütüphaneleri yenilendi, **EFI'deki `IntelXe.kext` de**
(yeni kext bir güvenlik açığını kapatıyor: yetkisiz bir süreç, yalnızca okuyabilmesi gereken sayfalara yazılabilir bir GPU
eşlemesi alabiliyordu). Paketi kurun (uygulamada *v0.1.6 sürümünü indir*, sonra *Bu paketi kur*; ya da
`sudo ./install.sh --apply`), sonra yeniden başlatmadan önce EFI'deki `IntelXe.kext`'i değiştirin (macOS'ta uygulamanın
EFI sayfası ya da `sudo ./efi.sh --apply`, Windows'ta Windows için IntelXe Manager ya da `IntelXe-EFI.ps1`, ya da elle
kopyalayın; `config.plist` kaydı
olduğu gibi kalır).
`--apply` açılış korumasının LaunchDaemon'ını da kurar ve 0.1.0 ile 0.1.1'in izin listesini (0.1.1 rehberinin istediği
`-NotificationCenter` satırı olsun ya da olmasın) değiştirir; böylece Safari'nin GPU süreci, Dock simgeleri ve masaüstü
widget'ları yeniden sürücüyü kullanır. İzin listesini başka bir biçimde değiştirdiyseniz liste korunur: paketteki
`Library/GPUBundles/nvmtl-allow.txt` ile karşılaştırın. Sistem diski kopyasını kurduysanız `--apply`, siz bu paketin
kopyasını (3.3) kurana kadar bütün süreçlere `/Library` kopyasını kullandırır; kopyayı kurduğunuzda
`/Library/GPUBundles/NVMTLDriver.use-library` dosyası yeniden silinir. `config.plist`'te
`UEFI > Output > ReconnectGraphicsOnConnect = false` olduğunu denetleyin (2. aşama, 5. madde); özellikle OpenCore'un
RELEASE derlemesine geçmeden önce.

**0.1.0'dan önceki, B580 adlı özel derlemelerden:** Adı değişen kext'lerin ve boot-arg'ların tablosu İngilizce rehberde (`README-INSTALL.md`,
"Upgrading from an earlier build").

## Paketin içindekiler

| Yol | Nereye gidiyor |
|---|---|
| `EFI/OC/Kexts/IntelXe.kext` | EFI, `Kernel > Add` (4. aşama); `-efi.zip`'tekiyle aynı dosya |
| `EFI/OC/ACPI/SSDT-ARCOFF.aml`, `.dsl` | EFI, `ACPI > Add`, yolu kontrol ettikten sonra (4. aşama) |
| `EFI/OC/Kexts/HybridPM.kext` | isteğe bağlı: EFI, `Kernel > Add`'de Lilu'nun altına; 12.-14. nesil hibrit Intel işlemciler için |
| `Library/Extensions/IntelXeFB.kext`, `NMXAccel.kext`, `NMXAGDC.kext` | `/Library/Extensions` (ekran, hızlandırıcı), `install.sh` ile |
| `Library/GPUBundles/NVMTLDriverLib.bundle`, `nvmtl/`, `nvmtl-allow.txt` | `/Library/GPUBundles` (Metal sürücüsü, Mesa, codec'ler), `install.sh` ile |
| (`install.sh` oluşturur) | `/Library/LaunchDaemons/io.github.yusufklncc.intelxe.guard.plist`: bir açılış bir dakikayı doldurunca açılış korumasını temizler |
| `system-volume/` | isteğe bağlı: Metal sürücüsünün sistem diskindeki kopyası (GPU'da Safari, 3.3) |
| `efi.sh` | isteğe bağlı, macOS'ta: OpenCore'un açıldığı EFI'de 2. ve 4. aşamalar, yedek ve geri alma ile |
| `hidpi.sh` | isteğe bağlı: monitör için HiDPI modlarını (ekran ölçeklendirme) açar ve kapatır |
| `hpmstat` | isteğe bağlı: HybridPM yüklüyken çekirdek saatleri ve paket gücü |
| `SHA256SUMS`, `sigcheck` | `install.sh`'in yaptığı denetimler |
| `SOURCES.md`, `LICENSES/` | her ikili dosyayı hangi kaynağın ve betiğin ürettiği; lisans metinleri |

## Sorun bildirmek

GitHub'da hata şablonuyla bir issue açın. Depodaki `tools/report/` günlükleri toplar ve önce seri numaralarını, UUID'leri,
MAC adreslerini ve kullanıcı adlarını çıkarır (uygulamaların Rapor sayfası onu çalıştırır). Lütfen ham sistem raporlarını
paylaşmayın. Açılış günlüğü açıksa ([yukarıda](#açılmazsa)) `collect-macos.sh --boot-logs`, `collect-windows.ps1 -BootLogs`
ya da Rapor sayfasındaki *Açılış günlükleri* EFI'deki OpenCore günlüklerini ve kaydedilen panikleri de ekler.

Sürücünün henüz desteklemediği bir Intel GPU için: Linux raporuyla (`tools/report/collect-linux.sh`) bir *Support for my
Intel GPU* issue'su açın. Xe tarama belleği (Xe probe stick; sürümde varsa `xe-probe-stick-VERSION.img.zip`; deneysel,
henüz gerçek donanımda çalıştırılmadı) bu raporu Linux kurulu olmayan bir bilgisayarda hazırlar: ağa bağlanmadan açılır,
diskleri salt okunur tutar ve raporu belleğe yazar (Windows için IntelXe Manager belleği yazar ve raporu okur;
`tools/probe-stick/README.md`).
