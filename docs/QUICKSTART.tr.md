# Hızlı başlangıç: IntelXe Manager ile

[English](QUICKSTART.md)

Olağan durum için tek sayfa: Arc B580 ya da B570 takılı, macOS'u OpenCore ile açan bir PC; kurulum **IntelXe
Manager** uygulamasıyla yapılıyor. Her adım, tam kılavuzda ([`release/README-INSTALL.tr.md`](../release/README-INSTALL.tr.md))
onu anlatan bölümü söylüyor; Terminal komutları ve bir şey ters giderse ne yapılacağı orada.

Deneysel ve imzasız: önce tam kılavuzun başındaki uyarıyı okuyun.

## Başlamadan önce

- **Çalışan bir OpenCore EFI'si olan bir USB bellek** ve anakartınızın açılış menüsü tuşu. Bir değişiklikten sonra PC
  açılmazsa bellekten açarsınız.
- **Kartın DisplayPort çıkışına bağlı tek monitör.**
- Sade bir OpenCore EFI'siyle kurulmuş macOS 26 Tahoe ya da 15 Sequoia ([Dortania kılavuzu](https://dortania.github.io/OpenCore-Install-Guide/);
  tam kılavuz, 1. aşama). Ekran sona kadar yavaş: macOS kartın firmware çıkışında çalışıyor.
- EFI'de henüz `IntelXe.kext` ya da `SSDT-ARCOFF.aml` olmamalı. Onlar en sonda geliyor (5. adım).

## 1. Uygulamayı indirin

[Sürümler sayfasından](https://github.com/yusufklncc/intel-gpu-macos-driver/releases) `IntelXe-Manager-<sürüm>.zip`
dosyasını İndirilenler'e indirin, sonra Terminal'de:

```sh
mkdir -p ~/Applications && ditto -x -k ~/Downloads/IntelXe-Manager-*.zip ~/Applications
xattr -dr com.apple.quarantine ~/Applications/"IntelXe Manager.app"
open ~/Applications/"IntelXe Manager.app"
```

İkinci satır gerekli, çünkü uygulama bir Apple geliştirici kimliğiyle imzalı değil (tam kılavuz, 3.1).

<p align="center"><img src="images/manager-overview.png" width="70%" alt="IntelXe Manager'ın sürücünün çalıştığını gösteren Genel bakış sayfası"></p>

## 2. Kurulum yardımcısı açılır

Uygulama ilk açılışta kurulum yardımcısını gösterir. Aşağıdaki adımları sayfa sayfa yapar, parolanızı sormadan önce
her değişikliğin ne olacağını gösterir ve yeniden başlattıktan sonra uygulamayı açtığınızda kaldığı yerden devam eder.

## 3. OpenCore'u hazırlayın (2. aşama)

Yardımcı, açtığınız EFI'deki OpenCore `config.plist` dosyasını değiştirir: gevşetilmiş SIP, AMFI boot-arg'ı, iki quirk
ve bir konsol ayarı. Safari'yi sonra kartta çalıştırmak istiyorsanız *Safari'yi de GPU için hazırla* seçeneğini
işaretleyin. Önce bütün EFI klasörünü yedekler. OpenCore hangi EFI'den açıldığını kaydetmediyse uygulama seçmeniz için
EFI bölümlerini listeler.

**Yeniden başlatın**, sonra uygulamayı yeniden açın.

EFI'yi bunun yerine Windows'tan hazırlamak için: Windows için IntelXe Manager (`IntelXeManager-SÜRÜM-win-x64.zip`,
0.1.5'ten itibaren) ya da betik olarak `-efi.zip` içindeki `IntelXe-EFI.ps1` (tam kılavuz, *Windows'ta* ve 2. aşama).

## 4. Sürücüyü kurun (3. aşama)

Yardımcı macOS sürümünüze uygun paketi indirir, sürümün imzasını ve paketin sağlama toplamını denetler ve kurar. Değiştirdiklerini
yedekler ve kurulumu geri alabilir.

**Henüz yeniden başlatmayın.**

## 5. EFI (4. aşama)

Yardımcı EFI'ye `IntelXe.kext` ve `SSDT-ARCOFF.aml` ekler. Kartınızın ACPI yolu paketteki tablonunkinden farklıysa
tabloyu ona göre yeniden yazar. E-çekirdekli 12.-14. nesil bir Intel işlemcide `HybridPM.kext`'i de `Lilu.kext`'in
altına ekler; HybridPM, bu projenin dağıtmadığı Lilu'ya ihtiyaç duyar (tam kılavuz, *Hibrit Intel işlemciler*).

**Yeniden başlatın.** Masaüstü artık monitörünüzün yenileme hızında hareket etmeli.

## 6. Denetleyin

- Uygulamanın Genel bakış sayfası sürücünün çalıştığını söyler; Bu Mac Hakkında kartın adını gösterir.
- Yardımcının son sayfası bir GPU testi çalıştırır ve ekleri sunar: Safari'yi GPU'da çalıştırma (sistem diski kopyası;
  tam kılavuz, 3.3), Minecraft Java ve diğer LWJGL oyunları için Zink üzerinden OpenGL, HiDPI modları, menü çubuğu öğesi.

## Açılmazsa

USB bellekten açın ya da OpenCore girdisi seçiliyken Shift'e basıp güvenli modda açın ve sürücüyü geri alın (paketteki
`sudo ./install.sh --undo` ya da uygulamanın Sürücü sayfası). Ayrıntılar tam kılavuzun *Açılmazsa* bölümünde; tanı
raporunun nasıl hazırlanacağı *Sorun bildirmek* bölümünde.

## Sonraki sürümler

Uygulamanın Sürücü sayfası yeni sürümü indirip eskisinin üzerine kurar; ardından EFI sayfası `IntelXe.kext`'i
değiştirir (tam kılavuz, *Yükseltme*). Bir adımın gerekip gerekmediğini sürüm notları söyler.
