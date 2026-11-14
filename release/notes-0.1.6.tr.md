Akıcı masaüstü sürümü: WindowServer artık birleştirdiği her kareyi sistem belleğine geri okumuyor (**ekrana verme
4. aşaması**: kare yalnızca bir şey onu okuduğunda kart belleğinden alınıyor) ve GPU boştayken başlatılan bir pencere,
GPU saati yükselirken artık takılmıyor (**ekran hızlandırması**). Ayrıca: Theo R.'nin katkısı iki düzeltme (165 Hz bir
panel 165 Hz'de açılıyor; 4. aşamayı başlatan geri okuma ölçümü), kurulum betikleri sistem diski kopyasının
`NVMTLDriver.use-library` işaretini kendileri yönetiyor ve Metal **mesh pipeline'ları**, object aşamasıyla da onsuz da
yeniden çalışıyor.

> [!WARNING]
> Deneysel. Tek bir makinede, tek bir kartla (Arc B580) test edildi; Arc B570, Pro B60 ve Pro B50 destekleniyor ama
> test edilmedi. Apple imzası yok; gevşetilmiş SIP/AMFI ve OpenCore gerekir. Kurmadan önce çalışan bir EFI'yi USB
> bellekte hazır tutun.

> [!NOTE]
> Yapay zekâ desteğiyle geliştirildi: kodu, her derlemeyi gerçek donanımda test eden proje sahibinin yönetiminde
> Anthropic'in Claude Opus 5.5 modeli (High çaba düzeyi) yazdı.

### Öne çıkanlar

- **Ekrana verme 4. aşaması: WindowServer'ın birleştirdiği kare artık her karede geri okunmuyor.** 0.1.5'e kadar Metal
  sürücüsü, masaüstünün birleştirilmiş her karesini karttan IOSurface'ın sistem belleğindeki sayfalarına geri
  kopyalıyordu; böylece onu işlemciden okuyan biri (ekran görüntüsü, ekran kaydı, çekirdeğin çevirmek yerine kopyaladığı
  bir kare) orada bulabiliyordu. Bu kopya PCIe üzerinden gidiyor ve her karede WindowServer'a GPU zamanına mal oluyordu.
  4. aşama kareyi yalnızca ekrana verme arabelleğine yazıyor ve bağlamayı daha yeni olarak işaretliyor; birleştirilmiş
  karenin işlemci kilidi, kopyalanan bir kare ya da bağlamanın kaldırılması en yeni kareyi gerektiğinde (kopya
  motoruyla) sayfalara alıyor. Önceden `debug.intelxe.surfvram >= 2` koşuluna bağlıydı; varsayılan hiçbir kurulum bunu
  ayarlamadığı için hiç devreye girmiyordu. Artık bir birleştirme bağlandığı her an devreye giriyor. B580'de (HiDPI
  4096x2304, 180 Hz, `p4check smooth`):

  | | fps | geç kalan kare | komut arabelleği başına WindowServer GPU zamanı |
  |---|---|---|---|
  | tam geri okuma (0.1.5) | 97,6 | 581'de 481 | 2,54 ms |
  | 4. aşama (0.1.6) | 175,9 | 1041'de 13 | 1,37 ms |

  Ekran görüntüleri ve ekran kayıtları güncel kareyi gösteriyor (sabit tutulan 12 rengin 12'si; çekirdeğin kareleri
  çevirmek yerine kopyaladığı, çevirmelerin kapalı olduğu durumda 8'in 8'i). 4. aşamayı alamayan bir bağlama (ekrana
  verme düzenine uymayan satır aralığı, düzlem konumu ya da ayrılan boyut) eskisi gibi geri okunuyor.
  `NVMTL_SCANOUT_READBACK=1` tam geri okumayı geri getiriyor, `=0` ikisini de kapatıyor (yalnızca ölçüm için).
- **Ekran hızlandırması: boşta kalmanın ardından başlayan bir pencerede takılma yok.** GPU boştayken GuC SLPC saati düşük
  tutuyor ve kendi meşguliyet örneklerine göre yükseltiyor; bu, yeni hareketlenmeye başlayan bir masaüstünü yaklaşık
  370 ms'de yakalıyordu: 180 Hz bir ekranda pencerenin ilk kareleri 90 Hz'de çalışıyordu. Artık ekran 250 ms sessiz
  kaldıktan sonra NMXAccel'in sunduğu ilk kare, `IntelXe.kext`'ten SLPC'nin en düşük saatine 400 ms'lik bir kira
  (yöneticinin izin verdiği en yüksek değer) istiyor; böylece saat, yükselme başlayacağı anda zaten yukarıda oluyor.
  B580'de boştan 12'şer başlatma: geç kalan kare 303 -> 13 (en kötü başlatma 44 -> 2), 166,8 -> 179,3 fps. Boştaki
  masaüstü ve 4K60 video hiç kira almıyor, aynı gücü çekiyor. Ayarlar: `debug.intelxe.disp_boost` (0 kapalı),
  `debug.intelxe.disp_boost_idle_ms` (kira, 400), `debug.nmxaccel_idle_boost_ms` (sessiz süre, 250), boot-arg
  `-intelxe_nodispboost`; sayaçlar `debug.intelxe.disp_boost_stats`'ta
  ([tasarım](https://github.com/yusufklncc/intel-gpu-macos-driver/blob/v0.1.6/docs/design/DESIGN-display-boost.md)).
- **Theo R.'nin (`@td512`) katkıları**, her biri 3440x1440 165 Hz bir panelde bulunup ölçüldü:
  - `IntelXeFB.kext`, açılışta geçerli mod olarak ürün yazılımının modunu bildiriyor. 1 numaralı modu bildiriyordu; o
    da en yüksek EDID hızı önce sıralandığında (ürün yazılımının 59,99 Hz'inden önce 165 Hz) bu hız oluyordu. Bu yüzden
    WindowServer'ın 1 numaralı moda geçişi hiçbir şey yapmıyor, panel başka bir moda geçip geri dönene kadar 60 Hz'de
    kalıyordu (PR #5).
  - Ekrana verme geri okumasının ölçülüp kapatılması (PR #4): 3440x1440'ta WindowServer'a kare başına 12,8 ms GPU
    zamanına mal oluyordu (onsuz 0,63 ms; 165 Hz'de 56 -> 162 fps). Kapatmak, işlemciden okuyanlara eski kareleri
    bırakıyordu; onları güncel tutan hali 4. aşama (yukarıda).
- **Sistem diski kopyasının `NVMTLDriver.use-library` işaretini kurulum betikleri yönetiyor.** `install.sh --apply`,
  sistem diski kopyası (Safari) başka bir sürümden olduğu sürece bu dosyayı oluşturuyor ve 0.1.6'dan itibaren
  oluşturduğunu kaydediyor; `system-volume/ssv-install.sh --apply` bu paketin kopyasını kurduktan sonra dosyayı
  kaldırıyor. Yükseltmeden sonraki elle `sudo rm` adımı yok (kendi oluşturduğunuz bir işaret kalıyor ve betikler bunu
  söylüyor). `./install.sh --status`, `use_library_by` (install, user, none) ve `system_volume_state` değerlerini
  gösteriyor (`ssv-install.sh --apply` ile yeniden başlatma arasında `pending`, sonra `same`); IntelXe Manager'ın Sürücü
  sayfası aynı durumları gösteriyor ve işaret bu sürümün sistem diski kopyasını gizlediğinde uyarıyor.
- **Mesh shader'lar yeniden çalışıyor.** Çeviricinin mesh dönüştürmesi her mesh threadgroup'unun çıktısını kendi geçici
  alanının başına yazıyordu (macOS 26'nın Metal derleyicisi tutamacı, dönüştürmenin beklemediği bir öznitelikle
  yazıyor) ve bu, ardından yaptığı çizimin başlığının üzerine geliyordu: object + mesh pipeline'ları GPU'yu
  kaybettiriyor, object aşaması olmayan mesh pipeline'ları yanlış çiziyordu. `mesh_lower.rs`'te (`rebase_handle`)
  düzeltildi; object + mesh pipeline'ları yeniden oluşturuluyor (0.1.5 onları reddediyordu). B580'de `mtl_mesh_object`
  4/4 (iki tanımlayıcı modelinde de). `NVMTL_MESH_OBJECT=0`, 0.1.5'teki gibi object aşamalarını reddediyor.

### 0.1.5'ten beri B580'de denetlenenler (değişiklik gerekmedi)

- **Bu sürücünün Level Zero'su üzerinden SYCL ve oneAPI** (kaynaktan derleniyor, paketlerde yok; 0.1.5'in
  *Geliştiriciler için* bölümüne bakın): `sycl-ls`, önceden derlenmiş (AOT) ve JIT kernel'ler, oneDPL, `kernel_compiler`
  (SPIR-V, OpenCL ve SYCL kaynakları) ve oneDNN (ReLU, matmul, konvolüsyon: 4/4) geçti; hiç GPU sıfırlaması olmadı.
- **`bar_revoke` düzeltmesi** (0.1.5): zorlanmış kipi, testin kendi 128 arabelleğini (512 MB) BAR penceresinin dışına
  taşıdı, bütün denetimler geçti, WindowServer etkilenmedi, GPU sıfırlaması olmadı. Özellik isteğe bağlı ve kapalı
  kalıyor.

### Değerlendirilip değiştirilmeyenler

- **Zink'te varsayılan olarak tanımlayıcı arabellekleri yerine tanımlayıcı kümeleri**: 0.1.5'in render önbelleği
  varsayılanından (`mesa/patches/0108`) önce kare başına GPU zamanında %16 daha hızlıydı; onun üzerinde yeniden
  ölçüldüğünde fark yok (%0,0 ve +%0,2), bu yüzden Zink tanımlayıcı arabelleklerinde kalıyor.

### Paketler

| Dosya | Ne için | Nerede açılmalı |
|---|---|---|
| `IntelXe-Manager-0.1.6.zip` | IntelXe Manager uygulaması (macOS 15 ve sonrası) | macOS |
| `IntelXeManager-0.1.6-win-x64.zip` | Windows için IntelXe Manager: Windows'tan 2. ve 4. aşamalar, bilgisayarın denetimi, kurulum adımları, raporlar (sürümü dışında değişmedi) | Windows 10 ya da 11 (x64) |
| `intel-gpu-macos-driver-0.1.6-tahoe.zip` | macOS 26 Tahoe | macOS |
| `intel-gpu-macos-driver-0.1.6-sequoia.zip` | macOS 15 Sequoia (aynı kaynaklar; Sequoia'da yeniden denenmedi) | macOS |
| `intel-gpu-macos-driver-0.1.6-efi.zip` | OpenCore parçaları: `IntelXe.kext`, `SSDT-ARCOFF`, `HybridPM.kext` (isteğe bağlı), `IntelXe-EFI.ps1` ve `efi-rules.json` | herhangi bir sistem |
| `intel-gpu-macos-driver-0.1.6-zink-gl.zip` | isteğe bağlı: Mesa'nın Zink'i üzerinden OpenGL (0.1.5'in ikili dosyaları) | macOS |
| `intel-gpu-macos-driver-0.1.6-vulkan.zip` | isteğe bağlı, deneysel: Wine için Vulkan (0.1.5'in ikili dosyaları) | macOS |
| `xe-probe-stick-0.1.0.img.zip` | isteğe bağlı, deneysel: sürücünün henüz desteklemediği Intel GPU'ları için Xe probe stick görüntüsü (0.1.5'teki görüntünün aynısı) | bir USB belleğe yazılır |

0.1.6 için yeniden derlenenler: `IntelXe.kext`, `IntelXeFB.kext`, `NMXAccel.kext`, Metal sürücüsü ve shader çeviricisi
(iki kopyası da), IntelXe Manager. `HybridPM.kext` 0.1.5'in kodunu yeni sürüm numarasıyla taşıyor; `NMXAGDC.kext`,
Mesa, video eklentileri ve FFmpeg 0.1.5'inki (her paketteki `SOURCES.md` bütün ikili dosyaları listeliyor).

Sağlama toplamları: `intel-gpu-macos-driver-0.1.6.sha256`, projenin anahtarıyla
`intel-gpu-macos-driver-0.1.6.sha256.sig` içinde imzalı; Windows uygulamasının zip'ini ve probe stick görüntüsünü de
listeliyor. Kurulum rehberi: her macOS paketinde `README-INSTALL.tr.md` ve
[depoda](https://github.com/yusufklncc/intel-gpu-macos-driver/blob/v0.1.6/release/README-INSTALL.tr.md) (İngilizcesi
`README-INSTALL.md`).

### 0.1.5'ten yükseltme

1. Paketi kurun (uygulamada *v0.1.6 sürümünü indir*, sonra *Bu paketi kur*; ya da `sudo ./install.sh --apply`).
2. Yeniden başlatmadan önce **EFI'deki `IntelXe.kext`'i değiştirin**: macOS'ta uygulamanın EFI sayfası ya da
   `sudo ./efi.sh --apply`; Windows'ta Windows için IntelXe Manager (EFI sayfası, *IntelXe.kext ve SSDT-ARCOFF
   (4. aşama)*) ya da `IntelXe-EFI.ps1`; ya da elle. `config.plist` kaydı olduğu gibi kalır. Kullanılıyorsa
   `HybridPM.kext`'i de aynı adım değiştirir. Ekran hızlandırması `IntelXe.kext`'in içinde: 0.1.5'inkiyle NMXAccel'in
   istekleri yalnızca başarısız diye sayılır (`debug.nmxaccel_vbl_boost_fail`).
3. Safari için sistem diski kopyasını kurduysanız **bu paketinkini de kurun** (uygulamanın Sürücü sayfası, *Safari ve
   WebKit*; ya da `sudo ./system-volume/ssv-install.sh --apply`). 0.1.6 Metal sürücüsünü değiştiriyor ama Mesa'yı
   değiştirmiyor; bu yüzden `install.sh` arada `/Library` kopyasına geçmiyor: bu adım olmadan WindowServer sistem
   diskinden 0.1.5'in Metal sürücüsünü kullanmaya devam eder. 0.1.5'in `install.sh`'i `NVMTLDriver.use-library`
   dosyasını bıraktıysa 0.1.6'nın kurulum betikleri onu kaldırır; sizin oluşturduğunuz bir dosya kalır.
4. Yeniden başlatın.

Değişikliklerden yalnızca birinde 0.1.5'in davranışına dönmek için: `/Library/GPUBundles/nvmtl/nvrm610.conf`'ta
`NVMTL_SCANOUT_READBACK=1` (tam geri okuma; WindowServer bir sonraki yeniden başlatmada alır); boot-args'ta
`-intelxe_nodispboost` ya da bir sonraki yeniden başlatmaya kadar `sudo sysctl debug.intelxe.disp_boost=0` (ekran
hızlandırması); aynı dosyada `NVMTL_MESH_OBJECT=0` (object + mesh pipeline'ları reddedilir).

0.1.4 ya da öncesinden geliyorsanız önce rehberin yükseltme bölümünü izleyin.

### Düzeltilenler

- Metal sürücüsü: GPU'yu kaybettiren object + mesh pipeline'ları ve yanlış çizen mesh pipeline'ları (yukarıda).
- `IntelXeFB.kext`: en yüksek EDID hızı ürün yazılımının modundan önce sıralanan bir panel açılıştan sonra 60 Hz'de
  kalıyordu (yukarıda).
- Ekrana verme 4. aşaması `intelxe_surfvram=2` olmadan hiç devreye girmiyordu; geri okuma kapalıyken (PR #4) birleştirilmiş
  kareyi işlemciden okuyanlar eski bir kare görebiliyordu (yukarıda).
- Kurulum betikleri: sistem diski kopyasıyla yapılan bir yükseltmeden sonra Safari ve WindowServer,
  `NVMTLDriver.use-library` elle silinene kadar `/Library` kopyasını kullanmaya devam ediyordu (yukarıda).

### Geliştiriciler için

- `docs/design/DESIGN-display-boost.md`: Linux'un yaptığı (i915, bir çevirme dikey boşluğu kaçırdığında saati
  yükseltiyor; `xe`'de hiç hızlandırma yok), bu yığında neden ekran hızlandırması olup bekleme hızlandırması olmadığı,
  tasarım ve B580 ölçümleri. Kaçırma tetikleyicisi (kareler, gelmeleri gereken dikey boşlukta hâlâ çalışırken bir kira;
  i915'in yaptığı gibi) sürümde var ama kapalı (`debug.intelxe.disp_boost_misses` 0): 4K60 videoda saati zamanın
  yarısı ya da daha fazlası boyunca yukarıda tuttu (+8-10 W) ve kaçırmaları azaltmadı. `IntelXeFB.kext`'in NMXScanout
  arayüzü 4. sürümde (op 4: dikey boşluk evresi); NMXAccel 0.1.6, bir karenin ne zaman gelmesi gerektiğini bununla
  biliyor.
- `docs/design/DESIGN-iosurface-vram.md` 12. bölüm (4. aşama neden devreye girmiyordu) ve
  `docs/design/DESIGN-mesh-object.md` (mesh dönüştürmesinin nedeni ve düzeltmesi; ANV'de yerel task ve mesh
  shader'larının ne gerektireceği).
- Testler: ana makine testleri `kernel/IntelXe/tests/dispboost_test.cpp` (84 denetim), `nmx_vbldue_test` (21),
  `user/tests/plugin/scanout_p4_unit.sh` (4. aşamanın kare başına kararı, bütün girdiler üzerinde; 2560x1440 ve
  3440x1440 gerçek IOSurface'larla), güncel Metal derleyicisinin mesh çıktısı üzerinde üç çevirici testi (mesh
  dönüştürmesi 28/28); `mtl_mesh_object`; B580 aşamaları `user/tests/wsi/night-phase4.sh` ve `night-dispboost.sh`,
  `p4check` ile (`smooth -v` her geç kalan kareyi, uygulamanın hemen öncesinde gördükleriyle listeliyor),
  `user/tests/metal/night-meshobj.sh`.
- Arayüz test ajanları `ProcessType Interactive` ile çalışıyor: bu olmadan bir launchd ajanı kısıtlanıyor (ajan olarak
  `p4check smooth` 44,9 fps, onunla 171,6).

### Bilinen sorunlar

- **Bu sürüm bir bütün olarak B580'de çalıştı** (14 Kasım, kendi paketiyle kuruldu: EFI `efi.sh` ile, `install.sh
  --apply`, `system-volume/ssv-install.sh --apply`; elle hiçbir adım gerekmedi, use-library dosyasını betikler oluşturup
  sildi). Bütün kext'ler 0.1.6 (NMXAGDC 0.1.5, kaynağı değişmedi), Metal eklentisi sistem diskinden yüklendi, regress
  21/21 (4 dakikalık uyku ve uyanmadan sonra 4/4), Geekbench 7 Metal 95.327 ve OpenCL 83.978 / 90.325 / 89.047 (üç
  koşu; 0.1.5: 94.828 ve 90.289), mesh object testi, H.264 ve HEVC 10 bit çözme doğrudan yolda, akıcılık 10 saniyede
  179,6 fps ve 1789 karede 3 geç kare, 4. aşama ve ekran hızlandırması sayaçları artıyor, hiç GPU sıfırlaması yok.
- **Oturum açıldıktan sonraki ilk dakikalarda ekran hızlandırması:** boştan başlayan 16 pencere açılışının 14'ünde 0-2
  geç kare vardı; ikisinde yaklaşık 20 (hızlandırma olmadan olduğu gibi). Pencereden hemen önce başka bir ekran
  güncellemesi (bir saat, bir bildirim) gelirse 400 ms'lik süreyi o alır ve pencerenin kendi kareleri yenisini almadan
  başlar; süre, GuC saati yükselmeden bitebilir.
- **3440x1440 bir panelde 4. aşama** (issue #7) ana makine testinde denetlendi (karenin sayfaya yuvarlanmasıyla devreye
  giriyor) ama henüz böyle bir panelde çalışmadı; B580 denemeleri 2560x1440 ve HiDPI 4096x2304'teydi. Bir ekran görüntüsü
  eski bir kare gösterirse `NVMTL_SCANOUT_READBACK=1` (yukarıda) tam geri okumayı geri getirir; lütfen bildirin.
- **Uzun bir boşta kalmanın ardından GPU hataları (nedeni açık)**, 0.1.5'teki gibi: GuC düşük gecikme ipucu varsayılan
  olarak kapalı kalıyor; bir `MEMORY_CAT_ERROR` görürseniz lütfen rapor gönderin.
- Gerisi 0.1.5'teki gibi: OpenCL, Geekbench 6'da 0.1.4'ün çekirdek sürücüsüne göre yaklaşık %3,5 daha yavaş;
  `ocvalidate`, `ResizeAppleGpuBars = 10` için uyarıyor (10'da bırakın); açılış korumasının durdurduğu bir açılış ürün
  yazılımının ekran arabelleğiyle açılmalı (henüz donanımda denenmedi); Windows için IntelXe Manager henüz gerçek bir
  EFI bölümünü değiştirmedi ve henüz hiçbir bilgisayar onun yaptığı bir EFI, USB kurulum belleği ya da probe stick'le
  açılmadı; Windows uygulamasının kod imzası yok; B570, Pro B60 ve Pro B50 test edilmedi; DisplayPort'ta tek monitör;
  Safari, Intel GPU'larda WebGPU `timestamp-query` sunmuyor.

### Teşekkürler

- 165 Hz panellerin açılış modu ve ekrana verme geri okumasının ölçümü (PR #4 ve #5, issue #7) için Theo R.'ye
  (`@td512`); her birini kendi makinesinde ölçtü.
- Her sürümde olduğu gibi: nullmoth (NullMoth Systems), metal2vulkan (Anees Iqbal), Linux `xe` ve i915, Mesa, FFmpeg,
  Emil J. Tywoniak'ın Xe2 VDEnc çalışması ve Lilu (acidanthera); README'ye bakın.
