<!-- 0.1.5 sürüm notlarının taslağı (draft), 12 Kasım 2026'da git log v0.1.4..main'den yazıldı. Sürümden önce: her
     maddeyi paketlerin gerçekten taşıdıklarıyla (hangi ikili dosyalar yeniden derlendi) karşılaştırın, sürüm
     derlemesinin ölçümlerini ekleyin, gelmeyenleri çıkarın, bu açıklamayı silin ve dosyayı notes-0.1.5.tr.md yapın. -->
Windows sürümü: **Windows için IntelXe Manager**, OpenCore EFI'sini Windows'tan macOS uygulamasındaki planlar, yedekler
ve geri almayla hazırlıyor; **tek bir EFI kuralları dosyası** artık bütün EFI araçlarını (macOS uygulaması, `efi.sh`,
Windows uygulaması ve `IntelXe-EFI.ps1`) yönetiyor ve onları karşılaştırırken bulunan dört düzeltme geliyor. Ayrıca:
doğrudan gönderim varsayılan olarak açık ve çekirdek sürücüsünün bellek ve fence yönetimi Linux `xe`'ye daha da yakın.

> [!WARNING]
> Deneysel. Tek bir makinede, tek bir kartla (Arc B580) test edildi; Arc B570 destekleniyor ama test edilmedi.
> Apple imzası yok; gevşetilmiş SIP/AMFI ve OpenCore gerekir. Kurmadan önce çalışan bir EFI'yi USB bellekte hazır tutun.

> [!NOTE]
> Yapay zekâ desteğiyle geliştirildi: kodu, her derlemeyi gerçek donanımda test eden proje sahibinin yönetiminde
> Anthropic'in Claude Opus 5.5 modeli (High çaba düzeyi) yazdı.

### Öne çıkanlar

- **Windows için IntelXe Manager** (`IntelXeManager-0.1.5-win-x64.zip`, yeni). macOS uygulamasının görünüşü ve
  diliyle yerel bir Windows uygulaması (WinUI 3); 2. ve 4. aşamaları Windows'tan yapmanın artık önerilen yolu:
  - **Genel bakış** bilgisayarı hiçbir şeyi değiştirmeden okur: Arc kart ve Windows sürücüsü, işlemci ve
    `HybridPM.kext` gerekip gerekmediği, anakart, UEFI ve Secure Boot, Resizable BAR ve Above 4G decoding, BitLocker ve
    EFI bölümleri (Windows'un hangisinden açıldığı, OpenCore'un hangisinde olduğu).
  - **EFI (OpenCore)**: bilgisayarın bir EFI bölümünde ya da bir EFI klasöründe (bir USB belleğinkinde) 2. aşama,
    Safari ayarı (yalnız ya da 2. aşamayla birlikte), 4. aşama (`IntelXe.kext`, onayladığınız ACPI yolu için
    `SSDT-ARCOFF.aml`, gelen yoldan farklıysa ona uyarlanmış, hibrit işlemcide `HybridPM.kext`) ve geri alma. Her
    değişiklik önce planını gösterir (adımlar, notlar, yedeğin nereye gideceği, planın SHA-256'sı), EFI klasörünün
    tamamını yedekler ve yalnızca EFI hâlâ o planı veriyorsa yapılır. Planlar ve sonuçlar Türkçe ya da İngilizce.
  - **Rapor**, Windows tanılama betiğini (`collect-windows.ps1`, uygulamayla gelir) çalıştırır ve hata ya da yeni
    donanım issue formunu raporun kimliğiyle açar. Hiçbir şey yüklenmez.
  - Kendi klasöründen çalışır: kurulacak bir şey yok, .NET ve Windows App SDK içinde (açılmış hâli yaklaşık 170 MB,
    zip'i 66 MB). Windows 10 (2004 ya da sonrası) veya 11, x64.
  - **Yönetici hakları yalnızca EFI bölümü için:** uygulama sizin haklarınızla çalışır; yalnızca bir EFI bölümünü
    bağlamak, okumak ve değiştirmek için kendi yardımcısını yönetici haklarıyla başlatır (Windows sorar). USB bellekteki
    bir EFI klasörü için sorulmaz.
  - **Kod imzası yok.** Projenin kod imzalama sertifikası yok; bu yüzden SmartScreen ilk açılışı durdurabilir ("Windows
    bilgisayarınızı korudu": *Ek bilgi* > *Yine de çalıştır*) ve yönetici izni penceresi "Bilinmeyen yayımcı" gösterir.
    Windows değiştirilmiş bir kopya için de aynısını gösterirdi: zip'in SHA-256'sını projenin anahtarıyla imzalı
    `intel-gpu-macos-driver-0.1.5.sha256` ile karşılaştırın
    (`Get-FileHash .\IntelXeManager-0.1.5-win-x64.zip -Algorithm SHA256`).
  - `-efi.zip`'teki `IntelXe-EFI.ps1` kalıyor: betik yolu (komut satırı ya da uygulamanın çalışamadığı durumlar). Artık
    `efi.sh` gibi Safari ayarını da yapıyor (`-Plan safari`, `-Plan settings-safari`). İki araç birbirinin
    değişikliklerini geri alabilir.
- **Tek bir EFI kuralları dosyası.** Sürücünün bir OpenCore EFI'sinde değiştirdikleri (2. aşama, Safari ayarı, 4. aşama,
  geri almanın geri koydukları) artık tek bir dosyada, `efi-rules.json`; onu okuyan her aracın yanında geliyor (macOS
  paketlerinde ve uygulamada `efi.sh` ile, `-efi.zip`'te `IntelXe-EFI.ps1` ile, Windows uygulamasında içine gömülü).
  32 ortak deneme vektörü `efi.sh`'e, PowerShell betiğine ve Windows uygulamasına karşı çalışıyor; `release.sh` her
  kopyanın depodakiyle aynı olduğunu denetliyor. İki uygulamayı karşılaştırmak dört düzeltme getirdi:
  - `efi.sh`: `config.plist`'te `NVRAM > Delete` yoksa 2. aşama onu ekliyor (`IntelXe-EFI.ps1`'in yaptığı gibi).
    Önceden böyle bir `config.plist`'te 2. aşama yedeğini alıp duruyordu; EFI değişmiyor ama yedek değişiklik
    listesinden yoksun kalıyordu.
  - `efi.sh`: yedeğin değişiklik listesi adım adım, her adımdan önce yazılıyor; geri alma yarıda kalmış bir değişikliği
    de geri koyuyor ve listesi olmayan bir yedek artık ondan önceki değişikliklerin geri alınmasını engellemiyor.
  - `efi.sh`: paketteki kext'lerin ve tablonun her dosyası, bir şey kopyalanmadan önce `SHA256SUMS`'ta listelenmiş ve
    eşleşiyor olmalı (paketlemeden sonra bir kext'e eklenen dosya denetlenmeden kopyalanıyordu); paketin
    `SHA256SUMS`'ı yoksa plan bunu söylüyor.
  - `IntelXe-EFI.ps1`: düzenlenen `config.plist` her değer yazıldığı anda denetleniyor. Hibrit bir işlemcide Lilu'nun
    altına eklenen `HybridPM.kext` kaydı `IntelXe.kext`'in kaydını kaydırıyor, sonradan yapılan denetim yanlış öğeyi
    okuyordu ve araç `efi.sh`'in yaptığı bir değişikliği reddediyordu.
- **Doğrudan gönderim varsayılan olarak açık.** 0.1.4'ün hızlı iş başlatması (`debug.intelxe.direct`, orada isteğe
  bağlıydı) baştan açık; boot-args'taki `-intelxe_nodirect` onu kapatır. 0.1.4'te açıkken gözetimsiz (11-12 Kasım):
  üç tur regresyon testi, Geekbench, 30 dakika boşta bekleme, uyku ve uyanma, hata yok; Geekbench 7 Metal 97,5-98,9k,
  OpenCL 93,3-94,0k.

### Çekirdek sürücüsü (IntelXe.kext)

Bellek ve eşzamanlama Linux `xe`'ye daha yakın; amaç uzun bir boşta kalmanın ardından gelen GPU hatası (hâlâ açık,
Bilinen sorunlar'a bakın):

- Serbest bırakılan bellek ve sayfa tabloları kendi VM'lerinin işlerini bekliyor; bölünen büyük bir sayfa tüm aralığını
  geçersiz kılıyor; yeni sayfa tabloları bağlanmadan önce yazılıyor.
- Bir unbind'ın çıkış fence'i ancak TLB geçersiz kılmasından sonra sinyal veriyor; GGTT geçersiz kılmaları GuC'den
  geçiyor ve bekleniyor (iki GT'de de); askıya alma, sürdürme, yüzey kopyaları ve BO taşımalarının kopya pencereleri her
  unbind gibi bekleniyor.
- Bir GuC bağlam sıfırlamasından sonra hiç başlamayan bir iş GT'yi 10 s sonra değil hemen sıfırlıyor; birden çok fence'i
  olan bir zaman çizelgesi noktasına ancak hepsi sinyal verince ulaşılıyor; TLB fence zaman aşımı Linux'unki.
- Eşzamansız bind user fence'leri, bind kuyruğu başına çıkış fence'leri, user fence'i bekleyen bir aralığın unbind'ına
  `EBUSY`, dosyası kapandıktan sonra da tutulup yazılan user fence'ler, BO belleğindeki user fence'ler.
- Uzun süre çalışan VM'ler (`DRM_XE_VM_CREATE_FLAG_LR_MODE`): Linux'taki gibi kabul ediliyor, iş zaman aşımı yok,
  bellekleri preemption ile korunuyor (aşağıdaki Intel hesaplama çalışma zamanı için).
- `debug.intelxe.bind_async` arkasında eşzamansız bind'lar (varsayılan kapalı).

### Deneysel ve varsayılan olarak kapalı

Bunların her biri sürümün ikili dosyalarında var, açılmadıkça hiçbir şey yapmıyor ve aksi yazmıyorsa henüz B580'de
çalıştırılmadı:

- İkinci ekran hattı, `-intelxefb_pipeb` (ikinci bir monitör gerekir; tek monitör için `sysctl
  debug.intelxefb_pipeb_plan` bir prova yapar) ve `nmxheads=1` ile NMXAccel'in ikinci bir ekranı hızlandırması.
- Metal sparse dokular, tier 1 (`NVMTL_SPARSE_TEXTURES=1`) ve gölgelendiricinin yazdığı dolaylı komut arabelleği
  durumu `VK_EXT_device_generated_commands` ile (`NVMTL_ICB_DGC=1`).
- Çekirdek beklemesinden önce fence yoklaması (`NVMTL_FENCE_SPIN_US`), hizalı olmayan ana bellek işaretçilerini içe
  aktarma (`NVMTL_IMPORT_UNALIGNED=1`: B580'de Geekbench 7 OpenCL 87,7k -> 89,0-89,4k), user fence'lerle ANV tamamlama
  (`ANV_UFENCE=1`), kaydedilen bir ipucu olarak `VK_EXT_memory_priority`.
- VideoToolbox kodlayıcısının yüklemesi önbellekli içe aktarma ve kopya motoruyla (`INTELXEVENC_UPLOAD=import`).
- Zink üzerinden yerel OpenGL uygulamaları (`cglzink-app.sh`): `CGLSetOffScreen`, IOSurface dokularına çizim, CGL
  pbuffer'ları.
- **Wine'daki Windows oyunları için Vulkan** (sürümde varsa isteğe bağlı `-vulkan.zip`): Wine, DXVK ve vkd3d-proton
  için ANV üzerinde bir Vulkan sağlayıcısı, `intelxe-vk` ile; IntelXe Manager onu Zink sayfasında kurar, dener ve
  kaldırır.

### Paketler

| Dosya | Ne için | Nerede açılmalı |
|---|---|---|
| `IntelXe-Manager-0.1.5.zip` | IntelXe Manager uygulaması (macOS 15 ve sonrası) | macOS |
| `IntelXeManager-0.1.5-win-x64.zip` | **yeni:** Windows için IntelXe Manager: Windows'tan 2. ve 4. aşamalar, bilgisayarın denetimi, raporlar | Windows 10 ya da 11 (x64) |
| `intel-gpu-macos-driver-0.1.5-tahoe.zip` | macOS 26 Tahoe | macOS |
| `intel-gpu-macos-driver-0.1.5-sequoia.zip` | macOS 15 Sequoia (aynı kaynaklar; Sequoia'da yeniden denenmedi) | macOS |
| `intel-gpu-macos-driver-0.1.5-efi.zip` | OpenCore parçaları: `IntelXe.kext`, `SSDT-ARCOFF`, `HybridPM.kext` (isteğe bağlı), `IntelXe-EFI.ps1` ve `efi-rules.json` | herhangi bir sistem |
| `intel-gpu-macos-driver-0.1.5-zink-gl.zip` | isteğe bağlı: Mesa'nın Zink'i üzerinden OpenGL | macOS |
| `intel-gpu-macos-driver-0.1.5-vulkan.zip` | isteğe bağlı, deneysel (varsa): Wine için Vulkan | macOS |

Sağlama toplamları: `intel-gpu-macos-driver-0.1.5.sha256`, projenin anahtarıyla
`intel-gpu-macos-driver-0.1.5.sha256.sig` içinde imzalı; Windows uygulamasının zip'ini de listeliyor. Kurulum rehberi:
her macOS paketinde `README-INSTALL.tr.md` ve
[depoda](https://github.com/yusufklncc/intel-gpu-macos-driver/blob/v0.1.5/release/README-INSTALL.tr.md); yeni
*Windows'ta: Windows için IntelXe Manager* bölümü Windows uygulamasını anlatıyor.

### 0.1.4'ten yükseltme

1. Paketi kurun (uygulamada *v0.1.5 sürümünü indir*, sonra *Bu paketi kur*; ya da `sudo ./install.sh --apply`).
2. Yeniden başlatmadan önce **EFI'deki `IntelXe.kext`'i değiştirin**: macOS'ta uygulamanın EFI sayfası ya da
   `sudo ./efi.sh --apply`; Windows'ta Windows için IntelXe Manager (EFI sayfası, *IntelXe.kext ve SSDT-ARCOFF
   (4. aşama)*) ya da `IntelXe-EFI.ps1`; ya da elle. `config.plist` kaydı olduğu gibi kalır.
3. Safari için sistem diski kopyasını kurduysanız bu paketinkini de kurun (uygulamanın Sürücü sayfası, *Safari ve
   WebKit*; ya da `sudo ./system-volume/ssv-install.sh --apply`).
4. Yeniden başlatın. Yalnızca iş gönderiminde 0.1.4'ün davranışına dönmek için boot-args'a `-intelxe_nodirect` ekleyin.

0.1.3 ya da öncesinden geliyorsanız önce rehberin yükseltme bölümünü izleyin.

### Geliştiriciler için

- **macOS'ta Intel'in hesaplama çalışma zamanı** (paketlerde yok): Level Zero 1.18 ve OpenCL ICD (`compute-runtime`,
  gmmlib, yükleyici) `xemac` üzerinde macOS x86_64 için derleniyor; Intel'in grafik derleyicisi (IGC) de macOS için
  derleniyor (`build-igc.sh`); `ze_hello` ve `cl_hello` (`docs/design/DESIGN-level-zero.md`).
- Sürüm imzalama anahtarları kurulu uygulamaları bozmadan değişebiliyor (`.sha256.sigs`, `SIGN_KEY_NEXT`,
  `docs/RELEASE-KEYS.md`); `package.sh` başka bir sürümün Metal sürücüsü paketini reddediyor ve Windows uygulamasının
  zip'ini Windows'ta derlendiği gibi alıyor (`WINAPP_ZIP`), sürümünü, dosyalarını ve SHA-256'sını denetleyerek.
- Windows uygulamasının Core'u (modeller, kurallar, içine derlenen `IntelXe-EFI.ps1` çekirdeği, ortak vektörlerle
  xUnit testleri) macOS'ta ve Linux'ta da derlenip deneniyor: `dotnet test windows/app/IntelXe.Core.Tests`.
- Proje sitesinde (GitHub Pages) yola göre bir kurulum rehberi, Uygulamalar ve Sürücüler sayfaları ve bir belirti
  dizini var; `tools/site-guide.py --check` rehberi paketlerin kurulum rehberine bağlı tutuyor.

### Bilinen sorunlar

- **Windows için IntelXe Manager henüz gerçek bir EFI bölümünü değiştirmedi**: EFI sayfası Windows 11'de deneme EFI
  klasörlerinde (yönetici yardımcısı dahil) ve ortak vektörlerle denendi, bölüm bağlama yalnızca sahte bir bağlayıcıyla.
  İki Windows aracı da henüz bir bilgisayarın ardından açıldığı bir EFI hazırlamadı. Çalışan bir EFI'si olan USB belleği
  elinizde tutun ve her planı onaylamadan önce okuyun.
- **Windows uygulamasının kod imzası yok** (yukarıda): SmartScreen ve yönetici izni penceresi bilinmeyen bir yayımcı
  gösterir.
- **Uzun bir boşta kalmanın ardından GPU hataları (nedeni açık)**, 0.1.4'teki gibi: GuC düşük gecikme ipucu varsayılan
  olarak kapalı kalıyor. Yukarıdaki çekirdek değişiklikleri öne çıkan varsayımları izliyor; bir `MEMORY_CAT_ERROR`
  görürseniz lütfen rapor gönderin.
- Gerisi 0.1.4'teki gibi: B570 test edilmedi; PCIe ASPM (`debug.intelxe.aspm_l1`) bir B580'de çalışmadı (test
  makinesinin kök portu L1 bildirmiyor); DisplayPort'ta tek monitör; tutulan bir açılış Apple logosunda kalır (OpenCore
  seçicisinde Reset NVRAM); Safari, Intel GPU'larda WebGPU `timestamp-query` sunmuyor.

### Teşekkürler

- Her sürümde olduğu gibi: nullmoth (NullMoth Systems), metal2vulkan (Anees Iqbal), Linux `xe` ve i915, Mesa, FFmpeg,
  Emil J. Tywoniak'ın Xe2 VDEnc çalışması ve Lilu (acidanthera); README'ye bakın.
- Windows uygulaması Microsoft'un .NET'i ve Windows App SDK'sı (WinUI 3) üzerine kurulu; ikisini de klasöründe taşır.
