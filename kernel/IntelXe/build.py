#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Build IntelXe.kext (kernel/IntelXe) and IntelXeFB.kext (kernel/IntelXeFB): plain IOKit kexts, no Lilu, with the MacKernelSDK.

Usage: build.py [--debug] [--release] [--output DIR] [--sdk DIR] [--firmware DIR] [--only IntelXe|IntelXeFB]
       (default output: <repo>/build)

IntelXe.kext is built -O2 -g by default; --debug builds it -O0 with INTELXE_DEBUG=1 (the destructive bring-up steps and the
batch dump sysctls). IntelXeFB.kext is -O2 -g as well (-O0 with --debug); --release is kept for old command lines.

XeProbe (xe_probe.cpp, docs/PROBE-MODE.md) is a second personality of IntelXe.kext: it attaches only to the device the
boot-arg xeprobe=0xDEVICE names. GuC images for the families it can probe are built in when present in the firmware
directory as <stem>_guc_70.bin (stems tgl, adlp, dg1, dg2, mtl, lnl, ptl; linux-firmware i915/ and xe/;
firmware/fetch.sh --probe fetches the ones firmware/probe-guc.sha256 pins, about 2.4 MB in all); Battlemage uses the
image below. A missing image only makes stage 3 report it.

MacKernelSDK (https://github.com/acidanthera/MacKernelSDK, APSL-2.0) is a build dependency and is not vendored: pass
--sdk, set MACKERNELSDK, or clone it to <repo>/external/MacKernelSDK.

The GuC firmware is built in from bmg_guc_70.bin (linux-firmware xe/bmg_guc_70.bin) and the HuC firmware from
bmg_huc.bin (xe/bmg_huc.bin, HuC 8.2.10, GSC CPD layout), both checked by sha256. They are Intel's redistributable
binaries under LICENSE.xe and are not in git: run firmware/fetch.sh first (or pass --firmware DIR).
"""
import argparse,hashlib,os,plistlib,subprocess,sys
from pathlib import Path
HERE=Path(__file__).resolve().parent          # kernel/IntelXe
REPO=HERE.parent.parent
FBDIR=HERE.parent/'IntelXeFB'                    # kernel/IntelXeFB
VER=(REPO/'VERSION').read_text().strip()       # the release that last rebuilt the kext
GUC='bmg_guc_70.bin'
GUC_SHA='de81c75f46a127c33cd59f604d800e9ffc7ed3495967ba0d8767cd6985ab398b'  # GuC 70.72.1
HUC='bmg_huc.bin'
HUC_SHA='747452aa8c4ed7760c68a80f3d913eafde9304d6f4db481e3f2aebb0a818bf15'  # HuC 8.2.10
# BMG-G31 (Arc Pro B70; xe_platform.cpp kIdsBMGG31): not supported, so not in the IOPCIMatch of the supported cards. Both
# kexts match it in a personality of its own ("<kext>-G31"); IntelXe::start refuses it unless the tester boot-arg
# -intelxe_allow_g31 is set (the entry's testerBootArg), and IntelXeFB attaches only after IntelXe brought the card up
# (IntelXeReady). tools/parity/devices.py checks these ids against the kernel's G31 entry.
G31_TESTER_MATCH='0xe2208086 0xe2218086 0xe2228086 0xe2238086'
def run(cmd): subprocess.run([str(x) for x in cmd],check=True,cwd=HERE)
def main():
 p=argparse.ArgumentParser(description=__doc__,formatter_class=argparse.RawDescriptionHelpFormatter)
 p.add_argument('--release',action='store_true',help='no effect: both kexts are -O2 unless --debug')
 p.add_argument('--no-probe-guc',action='store_true',help='with --release: build without the XeProbe GuC images of other families (firmware/fetch.sh --probe)')
 p.add_argument('--debug',action='store_true',help='IntelXe at -O0 with INTELXE_DEBUG=1, IntelXeFB at -O0')
 p.add_argument('--output',type=Path,default=REPO/'build')
 p.add_argument('--sdk',type=Path,default=Path(os.environ.get('MACKERNELSDK',REPO/'external/MacKernelSDK')),
  help='MacKernelSDK checkout (default: $MACKERNELSDK or <repo>/external/MacKernelSDK)')
 p.add_argument('--firmware',type=Path,default=REPO/'firmware',help='directory with bmg_guc_70.bin and bmg_huc.bin')
 p.add_argument('--only',choices=['IntelXe','IntelXeFB'],help='build one kext only')
 p.add_argument('--surfvram',type=int,choices=range(0,4),help='IntelXeSurfVramDefault in the personality: debug.intelxe.surfvram at boot (boot-arg intelxe_surfvram=N wins)')
 a=p.parse_args();out=a.output.resolve()
 sdk=a.sdk.resolve()
 if not (sdk/'Headers').is_dir() or not (sdk/'Library/x86_64/libkmod.a').is_file():
  sys.exit(f'MacKernelSDK not found at {sdk}: pass --sdk, set MACKERNELSDK, or clone it to {REPO/"external/MacKernelSDK"}')
 objdir=out/'obj';objdir.mkdir(parents=True,exist_ok=True)
 # Same kernel C++ flags as HybridPM.
 base=['xcrun','clang++','-arch','x86_64','-std=c++17','-g','-fapple-kext','-fno-exceptions','-fno-rtti',
  '-fno-stack-protector','-fno-builtin','-fno-asynchronous-unwind-tables','-mno-red-zone','-nostdinc','-DKERNEL','-DKERNEL_PRIVATE',
  '-D__APPLE__','-D__MACH__','-I'+str(sdk/'Headers'),'-mmacosx-version-min=10.6','-DAPPLE_KEXT_ASSERTIONS=1',
  '-DDRIVER_PRIVATE','-DAPPLE','-DNeXT','-fno-common','-Wall','-Wextra','-Wno-unused-parameter','-Wno-deprecated-declarations',
  '-fdebug-compilation-dir=.']
 if a.only in (None,'IntelXe'):
  for fwname,sha,name,inc in ((GUC,GUC_SHA,'kGucFw','guc_fw.inc'),(HUC,HUC_SHA,'kHucFw','huc_fw.inc')):
   src=a.firmware.resolve()/fwname
   if not src.is_file(): sys.exit(f'{src} is missing: run firmware/fetch.sh first')
   fw=src.read_bytes()
   assert hashlib.sha256(fw).hexdigest()==sha, f'{src}: unexpected sha256'
   (objdir/inc).write_text(f'static const uint8_t {name}[] __attribute__((aligned(16))) = {{\n'+
    '\n'.join(','.join(str(b) for b in fw[i:i+64])+',' for i in range(0,len(fw),64))+f'\n}};\nstatic const uint32_t {name}Size = {len(fw)};\n'+
    f'static const char {name}Name[] = "{fwname}";\n')   # IntelXe::start: the platform descriptor must name this file
  # XeProbe: whatever other GuC images are in the firmware directory. firmware/probe-guc.sha256 pins one per family
  # (firmware/fetch.sh --probe fetches them); another linux-firmware release's image is built in too, and said so:
  # stage 3 prints the version it loads, and debug.xeprobe.info lists every image built in.
  pins={}
  for line in (REPO/'firmware/probe-guc.sha256').read_text().splitlines():
   w=line.split()
   if len(w)==3 and not line.startswith('#'): pins[Path(w[1]).name]=(w[0],w[2])
  probe=[]
  for stem in ('tgl','adlp','dg1','dg2','mtl','lnl','ptl'):
   f=a.firmware.resolve()/f'{stem}_guc_70.bin'
   if f.exists():
    fw=f.read_bytes();probe.append((stem,fw));h=hashlib.sha256(fw).hexdigest();pin=pins.get(f.name)
    how=f'the pinned GuC {pin[1]}' if pin and pin[0]==h else f'NOT the pinned image (sha256 {h})' if pin else f'sha256 {h}'
    print(f'XeProbe GuC image {f.name}: {len(fw)} bytes, {how}')
  missing=[s for s in ('tgl','adlp','dg1','dg2','mtl','lnl','ptl') if s not in dict(probe)]
  if missing: print(f'XeProbe: no GuC image for {", ".join(missing)} (stage 3 says so there; firmware/fetch.sh --probe)')
  if missing and a.release and not a.no_probe_guc:
   sys.exit(f'--release: XeProbe GuC image(s) missing for {", ".join(missing)}: run firmware/fetch.sh --probe (into the --firmware directory), or pass --no-probe-guc')
  probe_stems=' '.join(s for s,_ in probe)
  inc='struct XpFw { const char *name; const uint8_t *data; uint32_t size; };\n'
  for stem,fw in probe:
   inc+=f'static const uint8_t kXpFw_{stem}[] __attribute__((aligned(16))) = {{\n'+'\n'.join(','.join(str(b) for b in fw[i:i+64])+',' for i in range(0,len(fw),64))+'\n};\n'
  inc+='static const XpFw kXpFw[] = {'+''.join(f'{{"{s}", kXpFw_{s}, {len(fw)}}},' for s,fw in probe)+'{nullptr, nullptr, 0}};\n'
  inc+=f'static const uint32_t kXpFwCount = {len(probe)};\n'
  (objdir/'xe_probe_fw.inc').write_text(inc)
 base.append('-I'+str(objdir))
 def kext(name,ident,srcs,libs,personality,extra={},ioclass=None,opt=[],more={}):
  # kmod_info: what Xcode generates for a kext (KMOD_EXPLICIT_DECL plus the C++ runtime hooks libkmod.a calls).
  bundle=out/f'{name}.kext/Contents';(bundle/'MacOS').mkdir(parents=True,exist_ok=True)
  info_c=objdir/f'{name}_kmod_info.c'
  info_c.write_text('#include <mach/kmod.h>\n'
   'extern kern_return_t _start(kmod_info_t *, void *);\nextern kern_return_t _stop(kmod_info_t *, void *);\n'
   f'extern kern_return_t {name}_start(kmod_info_t *, void *);\nextern kern_return_t {name}_stop(kmod_info_t *, void *);\n'
   f'__attribute__((visibility("default"))) KMOD_EXPLICIT_DECL({ident}, "{VER}", _start, _stop)\n'
   f'__private_extern__ kmod_start_func_t *_realmain = {name}_start;\n__private_extern__ kmod_stop_func_t *_antimain = {name}_stop;\n'
   '__private_extern__ int _kext_apple_cc = __APPLE_CC__;\n')
  objects=[]
  for src in srcs:
   objects.append(objdir/(Path(src).stem+'.o'));run(base+opt+['-c',HERE/src,'-o',objects[-1]])
  objects.append(objdir/f'{name}_kmod_info.o');run(cflags+opt+['-c',info_c,'-o',objects[-1]])
  exe=bundle/f'MacOS/{name}'
  run(['xcrun','ld','-arch','x86_64','-kext','-undefined','dynamic_lookup','-no_fixup_chains','-macos_version_min','10.6','-o',exe,*objects,sdk/'Library/x86_64/libkmod.a'])
  run(['xcrun','strip','-S',exe])
  info=dict(CFBundleIdentifier=ident,CFBundleExecutable=name,CFBundleName=name,CFBundlePackageType='KEXT',CFBundleVersion=VER,
   CFBundleShortVersionString=VER,OSBundleCompatibleVersion='0.1.0',OSBundleLibraries=libs,
   IOKitPersonalities={name:dict(CFBundleIdentifier=ident,IOClass=ioclass or name,IOProviderClass='IOPCIDevice',IOPCIMatch='0xe20b8086 0xe20c8086 0xe2118086 0xe2128086',**personality),
    f'{name}-G31':dict(CFBundleIdentifier=ident,IOClass=ioclass or name,IOProviderClass='IOPCIDevice',IOPCIMatch=G31_TESTER_MATCH,**personality),
    **{k:dict(CFBundleIdentifier=ident,**v) for k,v in more.items()}},**extra)
  (bundle/'Info.plist').write_bytes(plistlib.dumps(info))
  syms=subprocess.check_output(['xcrun','nm','-u',str(exe)],text=True)
  assert not any(x in syms for x in ['chkstk_darwin','___memcpy_chk']), 'userspace ABI imported'
  print(out/f'{name}.kext')
 cflags=['xcrun','clang','-std=c11']+[x for x in base[2:] if x not in('-std=c++17','-fapple-kext','-fno-rtti','-fno-exceptions')]
 libs={x:'10.0.0' for x in ['com.apple.kpi.bsd','com.apple.kpi.iokit','com.apple.kpi.libkern','com.apple.kpi.mach','com.apple.kpi.unsupported']}
 libs['com.apple.iokit.IOPCIFamily']='1.1'
 intelxeopt=['-O0','-DINTELXE_DEBUG=1'] if a.debug else ['-O2','-DINTELXE_DEBUG=0','-Wframe-larger-than=3072']
 if a.only in (None,'IntelXe'):
  intelxepers=dict(IOProbeScore=5000)
  if a.surfvram is not None: intelxepers['IntelXeSurfVramDefault']=a.surfvram
  # XeProbe: offered every Intel PCI device, in its own match category (beside any other driver); XeProbe::probe declines
  # all but the display function the boot-arg xeprobe=0xDEVICE names, so without that boot-arg it attaches nowhere.
  xeprobe=dict(IOClass='XeProbe',IOProviderClass='IOPCIDevice',IOPCIMatch='0x00008086&0x0000ffff',IOMatchCategory='XeProbe',IOProbeScore=0,
   XeProbeGuC=probe_stems)   # the families whose GuC image is built in (release/package.sh checks all seven)
  kext('IntelXe','io.github.yusufklncc.IntelXe',['IntelXe.cpp','xe_core.cpp','xe_vram.cpp','IntelXeClient.cpp','xe_probe.cpp','xe_fd.cpp',
   'xe_platform.cpp','xe_uapi_args.cpp','xe_aspm.cpp'],libs,intelxepers,
   dict(OSBundleRequired='Root'),opt=intelxeopt,more={'XeProbe':xeprobe})
 # IntelXeFB links IOGraphicsFamily, so it goes into the AuxKC (/Library/Extensions + kmutil), not OpenCore.
 # IOMatchCategory IOFramebuffer lets it sit on the GPU beside IntelXe and NMXAccel. It matches at boot from the AuxKC
 # (2026-10-29, "boot1": no more IOPropertyMatch intelxe-fb-armed / LaunchDaemon load); off with -intelxe_nofb or in safe mode.
 fblibs={x:'10.0.0' for x in ['com.apple.kpi.bsd','com.apple.kpi.iokit','com.apple.kpi.libkern','com.apple.kpi.mach']}
 fblibs.update({'com.apple.iokit.IOPCIFamily':'1.1','com.apple.iokit.IOGraphicsFamily':'1.0.0'})
 if a.only in (None,'IntelXeFB'):
  # IOPropertyMatch IntelXeReady: on macOS 26 the card can drop off the bus in the first seconds of boot; IntelXe.kext sets
  # IntelXeReady on the PCI device and re-registers it once bring-up succeeded, so the framebuffer matches only then.
  kext('IntelXeFB','io.github.yusufklncc.IntelXeFB',[FBDIR/'IntelXeFB.cpp',FBDIR/'IntelXeLink.cpp',FBDIR/'IntelXeEdid.cpp'],fblibs,
   dict(IOProbeScore=60000,IOMatchCategory='IOFramebuffer',IOPropertyMatch=dict(IntelXeReady=True)),ioclass='IntelXeFramebuffer',
   opt=['-O0'] if a.debug else ['-O2','-Wframe-larger-than=3072'])
  # /Library/Extensions: install.sh refuses an unsigned binary, so the kext is signed ad hoc here (IntelXe.kext stays
  # unsigned: OpenCore injects it)
  run(['codesign','-f','-s','-',out/'IntelXeFB.kext'])
if __name__=='__main__':main()
