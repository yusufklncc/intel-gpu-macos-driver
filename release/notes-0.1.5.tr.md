Windows sürümü: **Windows için IntelXe Manager**, OpenCore EFI'sini Windows'tan macOS uygulamasındaki planlar, yedekler
ve geri almayla hazırlıyor, bilgisayar için ilk EFI'yi oluşturuyor ve macOS USB kurulum belleğini yazıyor; **tek bir EFI
kuralları dosyası** artık bütün EFI araçlarını (macOS uygulaması, `efi.sh`, Windows uygulaması ve `IntelXe-EFI.ps1`)
yönetiyor ve onları karşılaştırırken bulunan dört düzeltme geliyor. Yeni varsayılanlar: **doğrudan gönderim**, Metal
sürücüsünde **tanımlayıcı arabelleği modeli**, H.264, HEVC, AV1 ve VP9 için **Vulkan Video üzerinden doğrudan video
çözme**, kodlayıcılarda **kopya motoruyla yükleme** ve Zink için daha hızlı bir render önbelleği ayarı. Ayrıca: çekirdek
sürücüsünün bellek ve fence yönetimi Linux `xe`'ye daha da yakın, karta yüklemeler PCIe Extended Tags ile yapılıyor;
açılmayan bir bilgisayar için isteğe bağlı **açılış günlüğü**, durdurulan bir açılış Apple logosu yerine ürün
yazılımının ekranını gösteriyor, IntelXe Manager'da bir **macOS güncelleme koruması** ve isteğe bağlı bir **oyun modu**,
**Arc Pro B60 ve B50** (denenmedi), sürücünün henüz desteklemediği Intel GPU'ları için **Xe probe stick** ve Metal
API'sinin daha fazlası.

> [!WARNING]
> Deneysel. Tek bir makinede, tek bir kartla (Arc B580) test edildi; Arc B570, Pro B60 ve Pro B50 destekleniyor ama
> test edilmedi. *Henüz B580'de çalışmadı* diye işaretlenenler ikili dosyalarda var ama yalnızca donanım dışında
> denetlendi. Apple imzası yok; gevşetilmiş SIP/AMFI ve OpenCore gerekir. Kurmadan önce çalışan bir EFI'yi USB
> bellekte hazır tutun.

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
    `SSDT-ARCOFF.aml`, gelen yoldan farklıysa ona uyarlanmış, hibrit işlemcide `HybridPM.kext`), açılış günlüğü
    (aşağıda) ve geri alma. Her değişiklik önce planını gösterir (adımlar, notlar, yedeğin nereye gideceği, planın
    SHA-256'sı), EFI klasörünün tamamını yedekler ve yalnızca EFI hâlâ o planı veriyorsa yapılır. Planlar ve sonuçlar
    Türkçe ya da İngilizce.
  - **Rapor**, Windows tanılama betiğini (`collect-windows.ps1`, uygulamayla gelir; *Açılış günlükleri* EFI'deki
    OpenCore günlüklerini de ekler) çalıştırır ve hata ya da yeni donanım issue formunu raporun kimliğiyle açar. Hiçbir
    şey yüklenmez. **Xe probe stick**'i (aşağıda) de yazar ve okur.
  - **Kurulum** (kenar çubuğunda ilk sırada, deneysel): bir Windows bilgisayardan kurulu bir macOS'a kadar adımlar tek
    listede: bu bilgisayar, BIOS ayarları, EFI'yi oluşturma, USB kurulum belleği, kurulumdan sonra yapılacaklar; her adım
    bitti, sıradaki ya da engelli olarak görünür ve açılışlar arasında saklanır. Sayfaları:
    - Anakart üreticisine göre **BIOS ayarları** (ASUS, MSI, Gigabyte, ASRock ya da genel menü adları ve tuşları):
      Secure Boot, CSM, Above 4G, Resizable BAR, tümleşik GPU, VT-d, CFG Lock ve Dortania'nın öteki Intel ayarları; her
      biri bitti, yapılacak ya da "Windows'tan anlaşılamıyor" olarak, kaynaklarıyla; telefona kaydedilecek bir sayfa da
      var. Hiçbir ayarı (BitLocker'ı da) değiştirmez. Test makinesinde (ASUS anakart) çalıştı.
    - **EFI'yi oluştur**: bu bilgisayar için 1. aşamanın OpenCore EFI'si; sabitlenmiş bir commit'teki
      [OpCore Simplify](https://github.com/lzhoang2801/OpCore-Simplify) ile, soru sormadan (her yanıt kayıtlı bir
      kural), yalnızca sabitlenmiş indirmelerden (OpenCore 1.0.8 ve sürüm kext'leri, her biri SHA-256 ile denetlenir),
      2. aşama `efi-rules.json`'dan ve aynı OpenCore sürümünün `ocvalidate`'iyle. Bilgisayarın taranması (Hardware
      Sniffer, yönetici hakları) yalnızca DSDT/SSDT/APIC/DMAR tablolarını tutar; SMBIOS seri numaraları üretilir, hiç
      günlüğe yazılmaz ve ekranda maskelenir. macOS'ta yedi örnekle denendi (test makinesinin Z790 + B580'i, herkese
      açık B660 ve Z490 raporları) ve test bilgisayarında **Windows'ta bir kez çalıştı** (14 Kasım): `ocvalidate`'ten
      geçen bir EFI oluşturdu; o çalıştırmanın bulduğu bir hata (yayımlanan uygulama `efi-rules.json`'ı bulamıyordu)
      düzeltildi. **Oluşturduğu bir EFI'yle henüz hiçbir bilgisayar açılmadı.**
    - **USB kurulum belleği**: Apple'ın sunucularından macOS 26 Tahoe ya da 15 Sequoia kurtarma görüntüsü (Apple'ın
      chunklist'ine karşı parça parça denetlenir), bir EFI ile (bu bilgisayarda oluşturulan ya da seçtiğiniz) adını ve
      boyutunu onayladığınız bir USB belleğe yazılır. Sanal bir diskte (VHDX) ve test bilgisayarında **gerçek bir USB
      bellekte** denendi (14 Kasım: Tahoe'nun kurtarma görüntüsü ve 106 EFI dosyasının hepsi geri okunduğunda aynıydı;
      önceden GPT olan bir bellekteki hata düzeltildikten sonra); **yazdığı bir bellekten henüz hiçbir bilgisayar
      açılmadı.**
  - Kendi klasöründen çalışır: kurulacak bir şey yok; .NET, Windows App SDK ve EFI oluşturucu (gömülü bir Python)
    içinde (açılmış hâli yaklaşık 205 MB, zip'i 83 MB). Windows 10 (2004 ya da sonrası) veya 11, x64.
  - **Yönetici hakları yalnızca diskler için:** uygulama sizin haklarınızla çalışır; yalnızca bir EFI bölümünü
    bağlamak, okumak ve değiştirmek, EFI oluşturucu için bilgisayarı taramak ve bir USB belleğe yazmak için kendi
    yardımcısını yönetici haklarıyla başlatır (Windows sorar). USB bellekteki bir EFI klasörü için sorulmaz.
  - **Kod imzası yok.** Projenin kod imzalama sertifikası yok; bu yüzden SmartScreen ilk açılışı durdurabilir ("Windows
    bilgisayarınızı korudu": *Ek bilgi* > *Yine de çalıştır*) ve yönetici izni penceresi "Bilinmeyen yayımcı" gösterir.
    Windows değiştirilmiş bir kopya için de aynısını gösterirdi: zip'in SHA-256'sını projenin anahtarıyla imzalı
    `intel-gpu-macos-driver-0.1.5.sha256` ile karşılaştırın
    (`Get-FileHash .\IntelXeManager-0.1.5-win-x64.zip -Algorithm SHA256`).
  - `-efi.zip`'teki `IntelXe-EFI.ps1` kalıyor: betik yolu (komut satırı ya da uygulamanın çalışamadığı durumlar). Artık
    `efi.sh` gibi Safari ayarını (`-Plan safari`, `-Plan settings-safari`) ve açılış günlüğünü (`-Plan bootlog`) de
    yapıyor. İki araç birbirinin değişikliklerini geri alabilir.
- **Tek bir EFI kuralları dosyası.** Sürücünün bir OpenCore EFI'sinde değiştirdikleri (2. aşama, Safari ayarı, 4. aşama,
  açılış günlüğü, geri almanın geri koydukları) artık tek bir dosyada, `efi-rules.json`; onu okuyan her aracın yanında
  geliyor (macOS paketlerinde ve uygulamada `efi.sh` ile, `-efi.zip`'te `IntelXe-EFI.ps1` ile, Windows uygulamasında
  içine gömülü). 44 ortak deneme vektörü `efi.sh`'e, PowerShell betiğine ve Windows uygulamasına karşı çalışıyor;
  `release.sh` her kopyanın depodakiyle aynı olduğunu denetliyor. İki uygulamayı karşılaştırmak dört düzeltme getirdi:
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
- **Açılış günlüğü (isteğe bağlı, sorun gidermek için).** macOS açılmadığında rapor edilecek bir şey yoktu. İki
  uygulamanın EFI sayfası (*Açılış günlüğü (sorun giderme için)*), `efi.sh --bootlog-plan` / `--bootlog-apply` ve `IntelXe-EFI.ps1 -Plan
  bootlog`, OpenCore'un günlük dosyasını, boot.efi'nin onun içindeki satırlarını (`AppleDebug`) ve OpenCore'un bir
  sonraki açılışta kaydettiği panik dosyalarını (`ApplePanic`) EFI'nin köküne açar; RELEASE bir OpenCore yeterli. Açıkken
  her açılış yavaşlar (OpenCore her satırdan sonra günlüğünü yeniden yazar); günlükleri topladıktan sonra kapatın
  (kendi değişikliği, `bootlog-off`). Raporlar onları toplar: uygulamaların Rapor sayfası (*Açılış günlükleri*),
  `collect-macos.sh --boot-logs`, `collect-windows.ps1 -BootLogs`; seri numaraları, MLB, ROM ve UUID'ler çıkarılır.
- **Doğrudan gönderim varsayılan olarak açık.** 0.1.4'ün hızlı iş başlatması (`debug.intelxe.direct`, orada isteğe
  bağlıydı) baştan açık; boot-args'taki `-intelxe_nodirect` onu kapatır. 0.1.4'te açıkken gözetimsiz (11-12 Kasım):
  üç tur regresyon testi, Geekbench, 30 dakika boşta bekleme, uyku ve uyanma, hata yok; Geekbench 7 Metal 97,5-98,9k,
  OpenCL 93,3-94,0k.
- **Tanımlayıcı arabelleği modeli Metal sürücüsünün varsayılanı** (Intel GPU'larında; WindowServer klasik modelde
  kalıyor). Her uygulamanın bağlamaları tanımlayıcı kümeleri yerine Vulkan tanımlayıcı arabelleklerine gidiyor: bağlama
  testlerinde çizim başına %4-29 daha az işlemci, görüntüler aynı. Model artık klasik modeldeki her şeye sahip (yığın
  bağlamaları 2-4, itilen tablo adresleri, GL-UBO, GPU'nun yazdığı dolaylı komut arabellekleri), her dispatch için yeni
  bir bölge açmıyor ve depolama arabelleği ofsetlerini varsayılan olarak push constant'lara koymuyor (Xe2'de iki fazla
  push dword'ü küçük kernel'lerin push verisini satır içi yazmaçlardan belleğe taşıyordu: kernel başına medyanda %28
  daha fazla döngü; `NVMTL_DB_BUFOFF=fit` dönüşümü hâlâ sığdığı yerde tutar). B580'de Geekbench 6 Metal (13 Kasım, sırayla
  üç tur): klasik 92,3k, arabellek 93,7k (medyanlar). ANV, Xe2'de hiçbir şeyin okumadığı tanımlayıcı kümesi adresi başına
  yüzey durumunu artık yazmıyor (`mesa/patches/0105`, yalnızca işlemci tarafı). Varsayılanı açık bu sürüm derlemesi ve
  0105 henüz B580'de çalışmadı. Her süreç için geri dönmek: `/Library/GPUBundles/nvmtl/nvrm610.conf`'a bir
  `NVMTL_DESC_MODEL=classic` satırı; tek bir süreç için ortamında `NVMTL_PROCESS_ENV=1 NVMTL_DESC_MODEL=classic`.
- **Vulkan Video üzerinden doğrudan video çözme varsayılan olarak açık** (H.264, HEVC, AV1 ve VP9): libavcodec'in
  çözücüsü ve Vulkan hwaccel'i yerine kendi parametre kümesi, başlık ve referans kodumuz (MIT) ve resim başına bir komut
  arabelleği; yolun kapsamadığı her şey (geçmeli H.264, HEVC aralık uzantıları, birden çok uzamsal katmanlı AV1, VP9
  profil 1 ve 3, ...) başta ya da onu getiren örnekten itibaren yine libavcodec'e gidiyor. B580'de (13 Kasım),
  libavcodec yoluna ve yazılım referanslarına karşı:
  - H.264: 95 uyumluluk akışının hepsi aynı, çözme iş parçacığının işlemci kullanımı %18-21 düşük.
  - HEVC: her gerçek klip (8 ve 10 bit, 1080p'den 4K'ya) aynı; doğrudan yol, girdisinin sağlam olduğu her yerde doğru,
    libavcodec yolunun çöktüğü üç akış dahil (aşağıda); %20-35 daha az işlemci.
  - VP9: 315 akışın 312'si aynı, libavcodec yolundan bir akışta daha doğru; %25-40 daha az işlemci. Doğrudan yol artık
    kare boyutu değişikliklerini de kendi oturumunda tutuyor (libavcodec yolu yeniden boyutlandırmadan sonraki kareleri
    yanlış çözüyor): **henüz B580'de çalışmadı**.
  - AV1: GPU dışında 247 libaom ve FATE vektörünün 243'ü libavcodec'le aynı, film grain'i GPU uyguluyor; **henüz B580'de
    çalışmadı**.
  Her süreç için kapatmak: `/Library/GPUBundles/nvmtl/nvrm610.conf`'a bir `INTELXEDEC_DIRECT=0` satırı (bu sürümün Metal
  sürücüsü bu anahtarı dosyadan okuyor); `h264,hevc` gibi bir liste yalnızca o kodekleri tutar.
- **libavcodec yolunda DPB yuvası düzeltmeleri.** FFmpeg 9.0.2 kendi DPB dizisindeki indisi Vulkan yuvası olarak
  veriyordu; bu yüzden 16 resimlik DPB'si olan bir HEVC akışı çözme sürecini ANV'nin içinde çökertiyordu (yığın taşması,
  `__stack_chk_fail`) ve uzun süreli referansları olan bir H.264 akışı 300 resmin 90'ını yanlış çözüyordu. İki taraf da
  düzeltildi: bir FFmpeg yaması yuva numaralarını oturumun `maxDpbSlots` değerinin altında tutuyor, bir başkası bir
  yürütmenin fence'ini yalnızca gönderildiğinde sıfırlıyor (reddedilen bir çözme çözücüyü kilitliyordu) ve ANV
  (`mesa/patches/0106`) yuva indislerini anahtar olarak alıyor, hiçbir zaman taşmıyor ve istemciyi durdurmuyor. Gelen
  `libavcodec` ve `libavutil` bu iki yamayla yeniden derlendi.
- **Kodlayıcılar varsayılan olarak kopya motoruyla yüklüyor.** Her piksel arabelleğinin IOSurface'ı bir kez içe
  aktarılıyor ve kodlayıcının girdisine işlemcinin bir ara arabelleğe kopyalaması yerine kopya motoru kopyalıyor. B580'de
  (13 Kasım; H.264, 8/10 bit HEVC ve AV1, 1080p): 16 modun hepsinde bit bit aynı akışlar, 8 koşunun 7'sinde %1,5-8,9
  daha hızlı (600 karelik AV1 %1,3 yavaş, o koşunun oynama payı içinde). `/Library/GPUBundles/nvmtl/nvrm610.conf`'taki
  `INTELXEVENC_UPLOAD=copy` işlemci kopyasına döndürür.
- **Karta daha hızlı yükleme (PCIe Extended Tags).** macOS, Linux'un bunu destekleyen her aygıtta açtığı Extended Tag
  alanını GPU'da hiç açmıyor; sistem belleğinden okumaları için yalnızca 32 etiketi olan kart, ana bellekten VRAM'e
  kopyaları (Shared arabellekler, doku yüklemeleri) bağlantının hızının çok altında yapıyordu. `IntelXe.kext` artık onu
  başlangıçta ve uyanmadan sonra açıyor (`-intelxe_noexttag` dokunmaz). Bir katkıda bulunan, Linux altında bir Core
  Ultra 7 265KF ve B580'le (PCIe 4.0 x8, 256 MB kopya) ölçtü: açıkken kopya motorunda 13,4 GB/s, render motorunda
  11,8 GB/s; kapalıyken 9,5 ve 2,4 GB/s. macOS'lu test makinesinde bit kext başlamadan önce zaten açıktı ve yükleme
  bu değişiklikle de onsuz da 11,8 GB/s ölçüldü: kazanç, bitin kapalı bulunduğu makineler için (kayıt defterinde
  `IntelXePCIeExtTag`).
- **macOS için IntelXe Manager:**
  - **macOS güncelleme koruması.** Genel bakış'taki *macOS güncellemeleri* bölümü, Yazılım Güncelleme'nin sunduğu her
    güncelleme için bu sürümün onu destekleyip desteklemediğini söyler (`install.sh --supports-macos`). *macOS güncellemesine
    hazırla*, kurulu paketin denetlenmiş bir kopyasını ve güncellemeden sonraki ilk açılışta çalışan tek seferlik bir denetimi
    hazırlar: kext'leri, Metal sürücüsünü, sistem diski kopyasını ve açılış korumasını denetler; yalnızca sürüm yeni
    macOS'u destekliyorsa yeniden kurar (`install.sh --apply`) ve bir kez yeniden başlatır, en çok üç deneme yapar,
    sonra kendini kaldırır. EFI'ye hiç dokunmaz. Hazırlanmadan yapılan bir güncelleme bir sonraki açılışta fark
    edilir; denetimin bulduklarını ve bir kurma düğmesini gösterir. **Henüz gerçek bir macOS güncellemesinden geçmedi.**
  - **Oyun modu (isteğe bağlı, her açılışta kapalı).** Önde bir oyun varken (App Store'un oyunlar kategorisi ya da menü
    çubuğundan eklediğiniz bir uygulama) GPU'nun en düşük saat hızı yükseltilir (2000 MHz, 2400 MHz ya da kartın en
    yükseği) ve oyun önden çekilince eski hâline döner. Bu sürümün kext'iyle yeni `debug.intelxe.slpc_boost`'u (aşağıda)
    kullanır: parola yok, yardımcı yok ve uygulama ölürse taban 10 saniye içinde kendiliğinden kalkar. İsterseniz oyun
    sürerken Time Machine yedeklemeleri durdurulur (bu, yönetici parolası ister). **Henüz B580'de çalışmadı:** kare
    sürelerine etkisi ölçülecek.
  - Zink sayfası Wine için Vulkan sağlayıcısını kurar, dener ve kaldırır (`-vulkan.zip`, aşağıda).
- **Arrow Lake masaüstü için HybridPM.** `HybridPM.kext` artık Core Ultra 200S masaüstü işlemcilerde de (Arrow Lake,
  model 0xC6) macOS'un işlemci güç yönetimini düzeltiyor; her çekirdek türünün HWP ölçeği bir Core Ultra 7 265KF'de
  Linux'tan ölçüldüğü gibi. O makinede onsuz tek iş parçacıklı bir döngü macOS'ta 8,4-9,0 s, Linux'ta 2,5 s sürüyordu.
  Ölçümü bir katkıda bulunan kendi makinesinde yaptı, biz değil. **EFI araçları onu bir Core Ultra'ya eklemiyor**
  (işlemcinin adına bakıyorlar): proje onu orada çalıştırmadı ve katkıda bulunanın macOS 26.7.3 üzerindeki son raporu,
  HybridPM'in yamaladığı XCPM kodunun orada da aynı biçimde olduğunu henüz doğrulamıyor. Rehberin *Hibrit Intel
  işlemciler* bölümündeki gibi elle ekleyin ve `hpmstat` ile denetleyin. Öteki Core Ultra modelleri desteklenmiyor.

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
- B580'de Geekbench 6 OpenCL bu değişiklikler boyunca 86,7k'dan 83,7k'ya indi (yaklaşık %3,5, sıfırlama yok), ama
  bu beklemeler yüzünden değil: tek tek ve hepsi birlikte kapatıldıklarında (`debug.intelxe.bind_tlb_fence`,
  `exec_tlb_dep`, `defer_jobs`, `ggtt_inval_wait`; varsayılan 1, yukarıdaki davranış) hiçbiri puanı koşudan koşuya
  oynamanın ötesinde değiştirmedi (13 Kasım, her yan 3 koşu; `defer_jobs` bir kez daha her yan 5 koşuyla: medyan
  kapalı 83,9k, açık 85,6k). Açık kalıyorlar; `debug.intelxe.stats` ne yaptıklarını sayar ("rb8 cost" satırı).
  `bind_tlb_fence=0` ilk koşusunda GPU'yu sıfırladı; artık kext `-intelxe_unsafe_bindfence` ile açılmadıkça
  reddediliyor. `debug.intelxe.tlb_inring` (EXEC TLB geçersiz kılmasını halkada bekliyor ve kuyruğu kurulu kalıyor)
  B580'de doğru çalıştı ama kazanç getirmedi; 0 kalıyor.
- **0 noktasındaki zaman çizelgesi beklemeleri, Linux'taki gibi fence'i bekliyor.** Mesa'nın her ikili syncobj
  beklemesi 0 noktasında bir zaman çizelgesi beklemesi; IntelXe böyle bir kaydı hemen hazır sayıyordu, bu yüzden bir
  wait-before-signal'dan sonra (Mesa'nın kuyruğunda bir gönderim iş parçacığı olunca) bir present, karesinin sinyali
  çekirdeğe ulaşmadan çalışabiliyordu: 14-25 kare sonra `EINVAL` ve `VK_ERROR_UNKNOWN`; B580'de Vulkan programlarında
  (Wine/DXVK'nın kalıbı) ANV'de, sağlayıcıda ve yükleyicide aynı biçimde görüldü. Çekirdeğin ana makine testleriyle
  denetlendi; düzeltilmiş kext o programı henüz B580'de çalıştırmadı.
- **Level Zero'nun sysman'i ve IntelXe Manager için GPU'nun durumu:** `debug.intelxe.sensors_raw` (sürüm 2; GuC'nin motor
  etkinliği arabelleklerinden, Linux `xe`'nin okuduğu gibi motor başına meşgul tıkları), anında değiştirilen SLPC en
  düşük ve en yüksek saat hızı (yalnızca root), aygıt kilidi altında örneklenen sensörler. B580'de okundu (13 Kasım):
  kart 36,5 W, paket 19,6 W, GPU 47 °C, VRAM 50 °C.
- **`debug.intelxe.slpc_boost`:** her kullanıcının isteyebileceği bir en düşük GPU saat hızı, 10 saniyelik bir kira
  olarak (yeniden yazınca uzar; 0 yazınca, uykuda ya da süresi dolunca biter): GPU için tek bir kira, yöneticinin
  tavanıyla (`slpc_max`, yoksa kartın en yükseği) sınırlı ve hiçbir zaman `slpc_min`'in altında değil, saniyede en çok
  10 istek (bırakma her zaman kabul edilir). IntelXe Manager'ın oyun modu onu kullanıyor. B580'de çalıştı (13 Kasım):
  kullanıcı yazabiliyor, 2850 MHz'e kırpma ve süre dolması çalışıyor; hız sınırının hemen ardından reddedilen bırakma
  o zamandan beri düzeltildi (bırakma artık her zaman kabul ediliyor). Ayrıca düzeltildi: sabitlenen bir ekran
  arabelleğinin boyut denetiminde bir taşma (`xeScanoutPin`).
- **Karesi tam sayfada bitmeyen modların ekrana verilmesi** (3440x1440, 4837,5 sayfa): böyle bir modun her karesi
  reddediliyor ve işlemci kopyası yoluna gidiyordu (165 Hz bir panelde 66 fps). Kare artık IntelXeFB'nin eşlediği gibi
  tam sayfaya yuvarlanıyor. Bir katkıda bulunanın 3440x1440 monitöründe bulundu.
- **Arc Pro B60 (8086:E211) ve Pro B50 (8086:E212)**: B580'in yongası (BMG-G21); kext'ler, `install.sh`, iki uygulama ve
  EFI araçları onları kabul ediyor, Bu Mac Hakkında adlarını gösteriyor; VRAM ve BAR karttan okunuyor. **Elimizde Pro
  kart yok, hiç çalıştırmadık.** BMG-G31 (Arc Pro B70 ve kardeşleri) desteklenmiyor; yalnızca deneyenler için
  `-intelxe_allow_g31` boot-arg'ı kext'lerin onu denemesine izin verir, günlükte uyarılarla. `Wa_14022085890` (1200 MHz
  SLPC tabanı) Linux'taki gibi platforma göre uygulanıyor (B580 ve B570'te değişiklik yok).
- **Açılış korumasının durdurduğu bir açılış artık Apple'ın ürün yazılımı ekran arabelleğini gösteriyor**, logoda
  kalmıyor: ekran `IONDRVFramebuffer`'a devrediliyor (hızlandırmasız, tek çözünürlük); böylece IntelXe Manager açılışın
  neden durdurulduğunu söyleyip korumayı sıfırlayabiliyor (Genel bakış sayfası, *Açılış koruması*; `sysctl
  debug.intelxe.bootguard`). **Henüz B580'de çalışmadı** (Bilinen sorunlar'a bakın).
- **Deneyenler için uçtan uca XeProbe.** `IntelXe.kext`'in, sürücünün sürmediği bir Intel GPU'yu yoklayan ikinci kişiliği
  (`xeprobe=0xAYGIT` boot-arg'ı, `docs/PROBE-MODE.md`) artık 3. aşamaya götürebildiği ailelerin GuC görüntülerini
  taşıyor (Tiger Lake, Alder Lake-P, DG1, DG2, Meteor Lake, Lunar Lake, Panther Lake; sabitlenmiş, toplam yaklaşık
  2,4 MB, Intel'in aygıt yazılımı lisansları `LICENSES/`'ta), `debug.xeprobe.info` ile rapor veriyor ve EFI araçları
  (`xeprobe` / `xeprobe-off` kuralları), IntelXe Manager'ın *Hazırlanan yoklama* sayfası ve `tools/report/xeprobe.sh`
  tarafından hazırlanıp kaldırılıyor. İyi görüntüleri reddeden bir 3. aşama başlık denetimi düzeltildi. Yalnızca
  B580'de çalıştı; ona dokunmuyor.
- `debug.intelxe.bind_async` arkasında eşzamansız bind'lar (varsayılan kapalı).
- **BAR penceresindeki arabelleklerin geri alınabilir işlemci eşlemeleri**, `debug.intelxe.bar_revoke` arkasında
  (**varsayılan kapalı, deneysel**): 1 GB'lık işlemcinin görebildiği VRAM penceresi daralınca bir arabelleğin işlemci
  eşlemesi yeni sistem sayfalarına taşınıyor, veri GPU ile kopyalanıyor ve sayfa tabloları yeniden yazılıyor; VRAM
  serbest bırakılmadan önce doğrulanıyor. İlk B580 çalıştırması (14 Kasım) serbest bırakılmış belleğin kullanıldığı bir
  hata buldu (eşlemenin pager'ına sahip tanımlayıcılar serbest bırakılıyordu: sonraki işlemci hataları `SIGBUS` ile
  bitiyordu, başka süreçlerde de); bu sürümde düzeltildi, **henüz B580'de yeniden çalışmadı**. 0'da bırakın.
- Lunar Lake için ilk adımlar (donanım yok; Lunar Lake başlangıçta reddedilmeye devam ediyor): platform kaydı Linux'a
  karşı denetlendi, Meteor Lake, Lunar Lake ve Panther Lake'in GSC aygıt yazılımı adları düzeltildi, platform başına bir
  bellek modeli; XeProbe tümleşik bir GPU'nun stolen belleğini ve GGTT'sini okuyor (`docs/design/DESIGN-lunar-lake.md`).

### Metal sürücüsü

- **Argüman arabelleklerinde doku dizileri:** bir argüman arabelleğindeki örneklenen `array<texture2d<T>, N>`, sürücünün
  tek bir tanımlayıcıyla (ilk öğenin dokusuyla) bildirdiği tek bir dizi değişkeniydi; öteki öğeler kendi dokularını
  okumuyordu. Artık her öğe, tek bir doku alanı gibi kendi bindless yuvasını okuyor. Çevirici test takımı: 2508 geçti.
- **Object aşamalı pipeline'lar** (object + mesh) B580'de GPU'yu kaybettiriyordu (20 s, sonra aygıt gidiyordu); artık
  uygulamanın işleyebileceği bir hatayla (`NSError` kodu 42) reddediliyorlar ve GPU çalışmaya devam ediyor. Object
  aşaması olmayan mesh pipeline'ları değişmedi. `NVMTL_MESH_OBJECT=1` onları eskisi gibi kurar.
- Çeviricinin dolaylı komut arabelleği komutları için alamadığı bir kernel artık Apple GPU'larındaki gibi, pipeline'ı
  hatayla başarısız olan bir fonksiyon (önceden `nil` dönüyordu; bir uygulama onu iletip çöktü).
- **Metal 4'ün pipeline'dan bağımsız ikili fonksiyonları** (bütün süreç için fonksiyon başına bir switch durum kimliği)
  ve **WebKit'in dolaylı komut arabelleği çizimleri** (`vsICBIndirect*`, işlemcinin hiç çizmediği yuvalara çizimler)
  aygıtın ürettiği komutlar yolunda: B580'de `mtl_gpu_icb3` 16/16 ve `mtl4_linking` 18/18 (13 Kasım).
- **Tile pipeline'ları** (`MTLTileRenderPipelineDescriptor`, `dispatchThreadsPerTile`): B580'de tile belleği yok; bu
  yüzden tile fonksiyonları ek başına düzlemler üzerinde hesaplamaya indiriliyor (MSAA katmanlar olarak) ve render geçişi
  bir tile dispatch'inin olduğu yerde bölünüyor. B580'de (14 Kasım) `mtl_tile` 7 durumun 6'sını geçti; MSAA durumu
  pipeline oluşturulurken başarısız oldu (kütüphanede onun için bir fonksiyon yoktu); bu sürümde düzeltildi, **henüz
  B580'de yeniden çalışmadı**. Apple GPU aileleri hâlâ iddia edilmiyor.
- **Metal API'sinin daha fazlası** (her biri, özelliği olduğunda Apple'ın GPU'suna karşı denetlendi; B580'de, 13-14
  Kasım: yansıma, bağlama (`mtl4_linking` 18/18) ve `MTL4Compiler` testleri geçti):
  - Apple'ınki gibi pipeline yansıması: kernel'lerin threadgroup argümanları, arabelleklerin ve dokuların bildirilen
    erişimi, AIR'ın hizalaması.
  - Görünür fonksiyon tablosu öykünmesi üzerinden ikili fonksiyonlar ve dinamik bağlama (`binaryFunctions`, ek ikili
    fonksiyonlu pipeline'lar, Metal 4'ün ikili fonksiyon ve bağlama tanımlayıcıları). Makine öğrenmesi pipeline'ları ve
    pipeline betikleri `MTLDeviceErrorNotSupported` ile reddediliyor (tile pipeline'ları artık destekleniyor, yukarıda).
  - `MTL4Compiler`: yansıma, statik bağlama, threadgroup başına gereken iş parçacıkları ve arşivler (Metal 4 giriş
    noktalarıyla birlikte; onlar `NVMTL_METAL4=1` olmadıkça kapalı).

### Deneysel ve varsayılan olarak kapalı

Bunların her biri sürümün ikili dosyalarında var, açılmadıkça hiçbir şey yapmıyor ve aksi yazmıyorsa henüz B580'de
çalıştırılmadı:

- Metal sparse dokular, tier 1 (`NVMTL_SPARSE_TEXTURES=1`), artık **yerleşimli sparse arabelleklerle**
  (`MTLBufferSparseTier1`, Metal 4 kuyruklarıyla eşlenir) ve **tier 2** ile (ek olarak `NVMTL_SPARSE_TIER2=1`: döşeme
  başına erişim sayaçları, sparse bir dokuyu örnekleyen gölgelendiricilerde atomik işlemlerle öykünülür; kapalıyken her
  çeviri bayt bayt eskisi gibi); gölgelendiricinin yazdığı dolaylı komut arabelleği durumu
  `VK_EXT_device_generated_commands` ile (`NVMTL_ICB_DGC=1`). İlk B580 çalıştırmasında hesaplama yuvaları sıfır okudu
  (C0-C2): adres indirgemesi, indisi push constant'lardan gelen bir arabelleği tanımlayıcı olarak tutuyordu, bu yüzden
  komutların verdiği adresler yok sayılıyordu. Böyle kernel'ler artık arabelleklerini adresle okuyor; bu sürümde
  düzeltildi, **henüz B580'de yeniden çalışmadı**.
- Çekirdek beklemesinden önce fence yoklaması (`NVMTL_FENCE_SPIN_US`), hizalı olmayan ana bellek işaretçilerini içe
  aktarma (`NVMTL_IMPORT_UNALIGNED=1`: B580'de Geekbench 7 OpenCL 87,7k -> 89,0-89,4k, ama altı çalıştırmada bir GPU
  hatası; gözetimsiz bir A/B'ye kadar kapalı kalıyor), user fence'lerle ANV tamamlama (`ANV_UFENCE=1`: B580'de doğrudan
  gönderimle medyan bekleme 34,9 -> 17,1 µs), kaydedilen bir ipucu olarak `VK_EXT_memory_priority`.
- BAR penceresindeki arabelleklerin geri alınabilir işlemci eşlemeleri (`debug.intelxe.bar_revoke`, yukarıda).
- İkinci ekran hattı, `-intelxefb_pipeb` (ikinci bir monitör gerekir; tek monitör için `sysctl
  debug.intelxefb_pipeb_plan` bir prova yapar) ve `nmxheads=1` ile NMXAccel'in ikinci bir ekranı hızlandırması.
- **Zink (isteğe bağlı `-zink-gl.zip`):** Zink üzerinden yerel OpenGL uygulamaları (`cglzink-app.sh`):
  `CGLSetOffScreen`, IOSurface dokularına çizim, CGL pbuffer'ları. 13 Kasım'daki B580 ölçümlerinden sonra iki varsayılan
  değişti: ANV'nin render önbelleği durum anahtarlaması (upstream'in yalnızca vkd3d için açtığı
  `anv_state_cache_perf_fix`) Zink için açık (`mesa/patches/0108`, bu sürümün `libvulkan_intel.dylib`'inde): Zink'in kare
  başına GPU süresi mip zinciriyle %10,7, yalnızca render geçişlerinde %36,3 kısa, görüntüler aynı
  (`anv_state_cache_perf_fix=false` kapatır); ve `libcglzink` IOSurface dokularını yalnızca değiştiklerinde geri yazıyor
  (`CGLZINK_SURF_WRITEBACK=dirty`; pikseller aynı, o yolda 120 -> 391 fps; `=flush` her flush'taki eski kopya).
  `ZINK_BAR_BUFFERS=sysmem|all` (isteğe bağlı, küçük BAR için) arabellekleri bir ara kopya yerine doğrudan eşler.
- **Wine'daki Windows oyunları için Vulkan** (isteğe bağlı `-vulkan.zip`): Wine, DXVK ve vkd3d-proton için ANV
  üzerinde bir Vulkan sağlayıcısı, `intelxe-vk` ile; IntelXe Manager onu Zink sayfasında kurar, dener ve kaldırır.
  Yeni: `vkscene`, ANV, sağlayıcı ya da yükleyici üzerinden geri okuma sağlamalarıyla bir Vulkan 1.3 sahnesi (MSAA,
  derinlik, bindless dokular, zaman çizelgesi semaforları), yukarıdaki 0 noktası düzeltmesinden önceki çekirdekler için
  `--no-wbs`; tek bir JVM argümanıyla Vulkan üzerinden Minecraft (`intelxe-vk minecraft-args`; Java'yı Mojang'ın
  çalışma ortamında, TLauncher'da, Prism'de, MultiMC'de ya da bir JDK'da bulur); DXVK A/B'leri için `intelxe-vk bench`
  ve `INTELXE_VK_FRAMELOG`. Metal sürücüsü devraldığı sürücü değişkenlerini temizlese de sağlayıcı kendi `INTELXE_VK_*`
  ayarlarını tutuyor.
- **Xe probe stick** (`xe-probe-stick-0.1.0.img.zip`): sürücünün henüz desteklemediği bir Intel GPU'su olanlar için
  açılabilir bir USB görüntüsü. Bilgisayarı ağsız olarak Linux 7.3 ile açar, her iç diski salt okunur tutar, Linux'un
  `xe` ya da `i915` sürücüsünün GPU'yu `mmiotrace` altında başlatmasına izin verir ve belleğe tek bir arındırılmış,
  mühürlü rapor yazar; *Intel GPU'm için destek* issue'su için. Windows için IntelXe Manager onu indirir (sürümün imzalı
  sağlama listesine karşı denetleyerek), yazar ve raporları geri okur; `tools/probe-stick/write-stick.sh` onu macOS'ta
  ve Linux'ta yazar. QEMU'da denendi, Windows'tan gerçek bir USB belleğe yazılıp geri okunduğunda aynıydı (14 Kasım);
  **henüz hiçbir bilgisayar onunla açılmadı**.
- **Intel'in hesaplama yığını** paketlerde yok (*Geliştiriciler için*'e bakın): onun için gelen, Level Zero'nun
  sysman'inin okuduğu çekirdek tarafı (`sensors_raw`, SLPC aralığı, motor etkinliği).

### Paketler

| Dosya | Ne için | Nerede açılmalı |
|---|---|---|
| `IntelXe-Manager-0.1.5.zip` | IntelXe Manager uygulaması (macOS 15 ve sonrası) | macOS |
| `IntelXeManager-0.1.5-win-x64.zip` | **yeni:** Windows için IntelXe Manager: Windows'tan 2. ve 4. aşamalar, bilgisayarın denetimi, kurulum adımları, raporlar | Windows 10 ya da 11 (x64) |
| `intel-gpu-macos-driver-0.1.5-tahoe.zip` | macOS 26 Tahoe | macOS |
| `intel-gpu-macos-driver-0.1.5-sequoia.zip` | macOS 15 Sequoia (aynı kaynaklar; Sequoia'da yeniden denenmedi) | macOS |
| `intel-gpu-macos-driver-0.1.5-efi.zip` | OpenCore parçaları: `IntelXe.kext`, `SSDT-ARCOFF`, `HybridPM.kext` (isteğe bağlı), `IntelXe-EFI.ps1` ve `efi-rules.json` | herhangi bir sistem |
| `intel-gpu-macos-driver-0.1.5-zink-gl.zip` | isteğe bağlı: Mesa'nın Zink'i üzerinden OpenGL | macOS |
| `intel-gpu-macos-driver-0.1.5-vulkan.zip` | isteğe bağlı, deneysel: Wine için Vulkan | macOS |
| `xe-probe-stick-0.1.0.img.zip` | isteğe bağlı, deneysel: sürücünün henüz desteklemediği Intel GPU'ları için Xe probe stick görüntüsü | bir USB belleğe yazılır |

Sağlama toplamları: `intel-gpu-macos-driver-0.1.5.sha256`, projenin anahtarıyla
`intel-gpu-macos-driver-0.1.5.sha256.sig` içinde imzalı; Windows uygulamasının zip'ini ve probe stick görüntüsünü de
listeliyor. Kurulum rehberi: her macOS paketinde `README-INSTALL.tr.md` ve
[depoda](https://github.com/yusufklncc/intel-gpu-macos-driver/blob/v0.1.5/release/README-INSTALL.tr.md); yeni
*Windows'ta: Windows için IntelXe Manager* bölümü Windows uygulamasını anlatıyor; `docs/WINDOWS-GUIDE.tr.md`
(İngilizcesi `WINDOWS-GUIDE.md`) bir Windows bilgisayardan kurulu bir macOS'a kadar onunla adım adım gidiyor, test
bilgisayarından ekran görüntüleriyle.

### 0.1.4'ten yükseltme

1. Paketi kurun (uygulamada *v0.1.5 sürümünü indir*, sonra *Bu paketi kur*; ya da `sudo ./install.sh --apply`).
2. Yeniden başlatmadan önce **EFI'deki `IntelXe.kext`'i değiştirin**: macOS'ta uygulamanın EFI sayfası ya da
   `sudo ./efi.sh --apply`; Windows'ta Windows için IntelXe Manager (EFI sayfası, *IntelXe.kext ve SSDT-ARCOFF
   (4. aşama)*) ya da `IntelXe-EFI.ps1`; ya da elle. `config.plist` kaydı olduğu gibi kalır. Kullanılıyorsa
   `HybridPM.kext`'i de aynı adım değiştirir.
3. Safari için sistem diski kopyasını kurduysanız bu paketinkini de kurun (uygulamanın Sürücü sayfası, *Safari ve
   WebKit*; ya da `sudo ./system-volume/ssv-install.sh --apply`).
4. Yeniden başlatın.

Yeni varsayılanlardan yalnızca birinde 0.1.4'ün davranışına dönmek için: boot-args'ta `-intelxe_nodirect` (iş
gönderimi); `/Library/GPUBundles/nvmtl/nvrm610.conf`'ta her biri bir satır olarak `NVMTL_DESC_MODEL=classic`
(tanımlayıcı modeli), `INTELXEDEC_DIRECT=0` (video çözme libavcodec üzerinden), `INTELXEVENC_UPLOAD=copy` (kodlayıcılarda
işlemci kopyası); sonradan başlayan süreçler bunları alır.

0.1.3 ya da öncesinden geliyorsanız önce rehberin yükseltme bölümünü izleyin.

### Düzeltilenler

- `IntelXe.kext`: karesi tam sayfada bitmeyen bir mod (3440x1440) işlemci kopyası yolundan çiziliyordu (yukarıda).
- Metal sürücüsü: argüman arabelleklerindeki doku dizileri, GPU'yu kaybettiren object + mesh pipeline'ları, dolaylı komut
  arabelleği kernel'leri için `nil` fonksiyon (yukarıda).
- 0.1.4'te Metal sürücüsünün paketi 0.1.2 sürümünü söylüyordu (`Info.plist`'i ikili dosyalarından eskiydi), bu yüzden
  IntelXe Manager ve hata raporları yanlış sürümü gösteriyordu; `package.sh` artık başka bir sürümün paketini reddediyor.
- Video: libavcodec yolunun DPB yuvaları (16 resimlik DPB'li HEVC akışları çözme sürecini çökertiyordu; DPB'nin 16.
  girdisinde uzun süreli referansı olan bir H.264 akışı 300 resmin 90'ını yanlış çözüyordu), FFmpeg'de ve ANV'de
  (yukarıda).
- `IntelXe.kext`: bir wait-before-signal'dan sonra Vulkan present'leri `EINVAL` ile başarısız oluyordu (0 noktasındaki
  zaman çizelgesi beklemeleri, yukarıda); hız sınırının hemen ardından GPU saat hızlandırmasının bırakılması
  reddediliyordu; ekran arabelleği sabitlemenin boyut denetiminde bir taşma.
- Metal sürücüsü: aygıtın ürettiği komutlar üzerinden hesaplama dolaylı komut arabelleği yuvaları sıfır okuyordu (isteğe
  bağlı yol); MSAA'lı bir tile pipeline'ının tile fonksiyonu yoktu (yukarıda).
- `efi.sh` ve `IntelXe-EFI.ps1`: *Tek bir EFI kuralları dosyası* altındaki dört düzeltme.
- Windows için IntelXe Manager'da gerçek bir bilgisayardaki ilk çalıştırmalarda bulunanlar (14 Kasım): *EFI'yi oluştur*
  yayımlanan uygulamada `efi-rules.json`'ı bulamıyordu; USB kurulum belleği sayfası önceden GPT olan bir bellekte
  başarısız oluyordu; Windows, geri okuma sırasında probe stick'in yeni FAT32 birimini bağlayıp ona yazıyordu (yeni bölüm
  tablosu ona artık denetimden sonra bildiriliyor); Core testleri Windows'un hâlâ açık tuttuğu ham bir diski okuyordu.
- `tools/probe-stick/write-stick.sh`, EFI taşıyan bir belleğin adını söylüyor ve üzerine yazmadan önce `ERASE EFI`
  yazılmasını (ya da `--erase-efi`) istiyor; Windows uygulamasının iki bellek yazıcısı bunu ayrı bir onayla soruyor.

### Geliştiriciler için

- **macOS'ta Intel'in hesaplama yığını** (paketlerde yok): Level Zero 1.18 ve OpenCL ICD (`compute-runtime`, gmmlib,
  yükleyici) `xemac` üzerinde macOS x86_64 için derleniyor; Intel'in grafik derleyicisi (IGC, `build-igc.sh`) ve artık
  **SYCL** de: Intel'in DPC++ v7.2.0'ı (SPIR-V ve B580 için önceden derlenmiş kodla clang `-fsycl`, `libsycl`, Unified
  Runtime'ın Level Zero bağdaştırıcıları) yedi küçük macOS yamasıyla (`compute/build-sycl.sh`,
  `docs/design/DESIGN-sycl.md`). B580'de (13 Kasım) `ze_hello`'nun her Level Zero adımı geçti, `cl_hello` IGC
  derlemesiyle çalıştı ve `sycl_hello` iki Level Zero bağdaştırıcısında ve IGC JIT'te 8/8 geçti (indirgeme adımındaki bir
  çökme, DPC++'ın clang'ı ile Apple'ınki arasındaki bir C++ çağrı kuralı farkıydı; bir yamayla düzeltildi). Level
  Zero'nun **sysman**'i de çalışıyor (NEO yamaları 0013-0014 onun sysfs okumalarını kext'ten yanıtlıyor: güç, sıcaklıklar,
  motor etkinliği; root için frekans denetimi). Bunların hiçbiri sürüm paketlerinde yok: derleme Mac'inde sabitlenmiş
  kaynaklardan derleniyor (`compute/`) ve deneysel.
- Windows uygulamasının *EFI'yi oluştur* sayfasının arkasındaki **EFI oluşturucu** (`windows/efibuild`, Python, MIT)
  tek başına da çalışıyor: `efibuild.py build --report Report.json --acpi ACPI --out DIR`
  (`docs/design/DESIGN-efi-build.md`).
- **Xe probe stick** Docker'da sabitlenmiş girdilerden, her seferinde bayt bayt aynı derleniyor
  (`tools/probe-stick/build.sh`; uçtan uca bir QEMU testi); `collect-linux.sh` v3'te arşiv ve çağıranlar için tek bir
  arındırma var (`--redact`).
- **Sürüm imzalama anahtarları kurulu uygulamaları bozmadan değiştirilebiliyor**, her adım denetlenerek:
  `release/release-keys.sh` bir halef anahtarı iki uygulamanın anahtar listesine birlikte ekliyor, sonra eskisini
  emekliye ayırıyor; yeni anahtarı getiren sürüm ikisiyle de imzalanıyor (`.sha256.sig` eski anahtarla, her kurulu
  uygulama onu okur; `.sha256.sigs` anahtar başına bir satırla; `SIGN_KEY_NEXT`). `package.sh` uygulamaların
  listelemediği imza anahtarlarını ve onları taşımayan bir uygulama derlemesini reddediyor; `release.sh` `.sig`'i bir
  önceki sürümün uygulamasına karşı ve yeni bir anahtarın `.sigs` satırı olduğunu denetliyor; `release/test-signing.sh`
  üç sürümlük değişikliğin tamamını atılacak anahtarlarla çalıştırıyor. Henüz bir halef anahtar yok
  (`docs/RELEASE-KEYS.md`). `package.sh` ayrıca başka bir sürümün Metal sürücüsü paketini reddediyor, Windows uygulamasının
  zip'ini Windows'ta derlendiği gibi alıyor (`WINAPP_ZIP`; sürümünü, dosyalarını ve SHA-256'sını denetleyerek) ve probe
  stick görüntüsünü (`PROBE_IMG`) zip'leyip imzalı listeye ekliyor.
- Windows uygulamasının Core'u (modeller, kurallar, içine derlenen `IntelXe-EFI.ps1` çekirdeği, kurulum adımları, iki
  belleğin disk kuralları, ortak vektörlerle xUnit testleri) macOS'ta ve Linux'ta da derlenip deneniyor:
  `dotnet test windows/app/IntelXe.Core.Tests`.
- `docs/design/DESIGN-rebar-window.md`: `ocvalidate`'in `ResizeAppleGpuBars = 10`'u neden uyku ve uyanmayla kararsız
  saydığı (macOS uykudan sonra Resizable BAR boyutunu geri yüklemiyor; `IntelXe.kext` yüklüyor) ve rehberin neden 10'da
  kaldığı.
- Testler ve araçlar: `bindstress` (çalışan işlerin altında bind ve unbind'lar), `unbindfence`, `cl_gbmicro` (Geekbench
  OpenCL'e benzeyen bir mikro ölçüm), `mtl_desc_paths` (iki tanımlayıcı modelinde Metal'in bağlama yolları; 29 durum,
  Apple'ın GPU'sunda hepsi geçiyor), `mtl_desc_cost`, `dbuf_model`, `mtl_sparse_tier2`, `mtl4_linking`, `vtdec --lib`,
  `tests/vtdirect-ab.sh`, `sycl_hello` ve hızlandırma kirası (48 denetim) ile halkadaki TLB beklemesi (3,8 milyon) için
  ana makine testleri. `docs/design/DESIGN-anv-descbuf-cost.md`: ANV'nin Xe2'de tanımlayıcı arabellekleri için ne
  ürettiği.
- Proje sitesinde (GitHub Pages) yola göre bir kurulum rehberi, Uygulamalar ve Sürücüler sayfaları ve bir belirti
  dizini var; `tools/site-guide.py --check` rehberi paketlerin kurulum rehberine bağlı tutuyor.

### Bilinen sorunlar

- **Windows için IntelXe Manager henüz gerçek bir EFI bölümünü değiştirmedi**: EFI sayfası Windows 11'de deneme EFI
  klasörlerinde (yönetici yardımcısı dahil) ve ortak vektörlerle denendi, bölüm bağlama yalnızca sahte bir bağlayıcıyla.
  İki Windows aracı da henüz bir bilgisayarın ardından açıldığı bir EFI hazırlamadı. Çalışan bir EFI'si olan USB belleği
  elinizde tutun ve her planı onaylamadan önce okuyun.
- **Henüz hiçbir bilgisayar, *EFI'yi oluştur*'un yaptığı bir EFI'yle ya da Windows uygulamasının yazdığı bir USB kurulum
  belleği veya probe stick'le açılmadı.** Her biri test bilgisayarında bir kez çalıştı (14 Kasım: `ocvalidate`'ten geçen
  bir EFI, gerçek bir kurulum belleği ve gerçek bir probe stick; ikisi de geri okunduğunda aynıydı); onlardan açılış
  sıradaki deneme.
- **Windows uygulamasının kod imzası yok** (yukarıda): SmartScreen ve yönetici izni penceresi bilinmeyen bir yayımcı
  gösterir.
- **Bu biçimiyle henüz B580'de çalışmayanlar:** bu sürümün Mesa ANV'si bir bütün olarak (yeni yamalarından 0103, 0104,
  0106 ve 0108 B580'de ayrı derlemelerde çalıştı, 0105 henüz değil), doğrudan çözme yolunda AV1, o yoldaki VP9 boyut
  değişiklikleri, tile pipeline'ının MSAA düzeltmesi, hesaplama dolaylı komut arabelleği düzeltmesi (isteğe bağlı),
  `bar_revoke` düzeltmesi (isteğe bağlı), 0 noktası zaman çizelgesi beklemesi düzeltmesi ve tanımlayıcı arabelleği
  modelini varsayılan yapan bu sürümün Metal sürücüsü derlemesi (modelin kendisi B580'de çalıştı; her biri için
  yukarıya bakın). Video, bir Metal uygulaması ya da bir Vulkan programı sorun çıkarırsa *0.1.4'ten yükseltme*
  altındaki anahtarlar 0.1.4'ün davranışına döndürür; lütfen bildirin.
- **Uzun bir boşta kalmanın ardından GPU hataları (nedeni açık)**, 0.1.4'teki gibi: GuC düşük gecikme ipucu varsayılan
  olarak kapalı kalıyor. Yukarıdaki çekirdek değişiklikleri öne çıkan varsayımları izliyor; bir `MEMORY_CAT_ERROR`
  görürseniz lütfen rapor gönderin.
- **OpenCL, 0.1.4'ün çekirdek sürücüsüne göre Geekbench 6'da yaklaşık %3,5 daha yavaş** (yukarıda, *Çekirdek
  sürücüsü*).
- **`ocvalidate`, `ResizeAppleGpuBars = 10` için uyarıyor** ("unstable with macOS sleep-wake"). 10'da bırakın: sürücü
  uyanmadan sonra BAR boyutunu geri yüklüyor (`docs/design/DESIGN-rebar-window.md`); bu geri yüklemenin test kartında
  gerçekten gerekip gerekmediği henüz görülmedi.
- **Açılış korumasının durdurduğu bir açılış ürün yazılımının ekran arabelleğiyle açılmalı** (hızlandırmasız, tek
  çözünürlük; `-intelxe_noaccel` ile de), böylece IntelXe Manager nedenini gösterip korumayı sıfırlayabilir. **Henüz
  donanımda denenmedi**: yine Apple logosunda kalırsa eskisi gibi OpenCore seçicisinde Reset NVRAM yapın. Durdurulmuş
  bir açılışta Mac'i uyutmayın.
- Gerisi 0.1.4'teki gibi: B570 test edilmedi (Pro B60 ve B50 de); PCIe ASPM (`debug.intelxe.aspm_l1`) bir B580'de
  çalışmadı (test makinesinin kök portu L1 bildirmiyor); DisplayPort'ta tek monitör; Safari, Intel GPU'larda WebGPU
  `timestamp-query` sunmuyor.

### Teşekkürler

- 3440x1440 modlar için ekrana verme düzeltmesi, PCIe Extended Tags ve HybridPM'in Arrow Lake desteği için `@td512`'ye;
  her birini kendi makinesinde ölçtü (issue #3).
- Windows uygulamasının EFI oluşturucusunun sabitlenmiş bir commit'te sürdüğü
  [OpCore Simplify](https://github.com/lzhoang2801/OpCore-Simplify) ve
  [Hardware Sniffer](https://github.com/lzhoang2801/Hardware-Sniffer) (lzhoang2801, BSD-3-Clause); onunla gelen
  python.org'un gömülebilir Python'u ve pywin32.
- Xe probe stick, Arch Linux'un çekirdek ve Intel GPU aygıt yazılımı paketleriyle ve Alpine Linux'un busybox'ıyla açılır;
  lisansları bellektedir.
- Her sürümde olduğu gibi: nullmoth (NullMoth Systems), metal2vulkan (Anees Iqbal), Linux `xe` ve i915, Mesa, FFmpeg,
  Emil J. Tywoniak'ın Xe2 VDEnc çalışması ve Lilu (acidanthera); README'ye bakın.
- Windows uygulaması Microsoft'un .NET'i ve Windows App SDK'sı (WinUI 3) üzerine kurulu; ikisini de klasöründe taşır.
