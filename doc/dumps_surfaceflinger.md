```
PS C:\D\android_project\erp-pda> adb shell dumpsys SurfaceFlinger
Build configuration: [sf PRESENT_TIME_OFFSET=0 FORCE_HWC_FOR_RBG_TO_YUV=1 MAX_VIRT_DISPLAY_DIM=0 RUNNING_WITHOUT_SYNC_FRAMEWORK=0 NUM_FRAMEBUFFER_SURFACE_BUFFERS=2] [libui] [libgui]

Display identification data:
Display 19260618794624641 (HWC display 0): port=129 pnpId=QCM displayName=""

Wide-Color information:
Device has wide color built-in display: 1
Device uses color management: 1
DisplayColorSetting: Managed
Display 19260618794624641 color modes:
    ColorMode::NATIVE (0)
    Current color mode: ColorMode::NATIVE (0)

Sync configuration: [using: EGL_ANDROID_native_fence_sync EGL_KHR_wait_sync]

Scheduler:
+  Idle timer: off
+  Touch timer: 200 ms
+  Use content detection: off

ScreenOff: 20d18:35:36.109
60fps: 0d21:41:14.043

           app phase:   1000000 ns               SF phase:   1000000 ns
     early app phase:   1000000 ns         early SF phase:   1000000 ns
  GL early app phase:   1000000 ns      GL early SF phase:   1000000 ns
next VSYNC threshold: 9223372036854775807 ns
      present offset:         0 ns           VSYNC period:  16666666 ns

DesiredDisplayConfigSpecs (DisplayManager): default config ID: 0, primary range: [0.00 180.00], app request range: [0.00 180.00]

(config override by backdoor: no)

app: state=Idle VSyncState={displayId=19260618794624641, count=1476389}
  pending events (count=0):
  connections (count=25):
    Connection{0xb400006eb12e3930, VSyncRequest::None}
    Connection{0xb400006eb12e8550, VSyncRequest::None}
    Connection{0xb400006eb12e9dd0, VSyncRequest::None}
    Connection{0xb400006eb12ee3d0, VSyncRequest::None}
    Connection{0xb400006eb12e72f0, VSyncRequest::None}
    Connection{0xb400006eb12e9970, VSyncRequest::None}
    Connection{0xb400006eb12e8010, VSyncRequest::None}
    Connection{0xb400006eb12ebc70, VSyncRequest::None}
    Connection{0xb400006eb12f1070, VSyncRequest::None}
    Connection{0xb400006eb12ef0f0, VSyncRequest::None}
    Connection{0xb400006eb12ea150, VSyncRequest::None}
    Connection{0xb400006eb12ea770, VSyncRequest::None}
    Connection{0xb400006eb12e7210, VSyncRequest::None}
    Connection{0xb400006eb12f0a50, VSyncRequest::None}
    Connection{0xb400006eb12e6250, VSyncRequest::None}
    Connection{0xb400006eb12f00b0, VSyncRequest::None}
    Connection{0xb400006eb12edb10, VSyncRequest::None}
    Connection{0xb400006eb12ef1d0, VSyncRequest::None}
    Connection{0xb400006eb12ecdf0, VSyncRequest::None}
    Connection{0xb400006eb12ecc30, VSyncRequest::None}
    Connection{0xb400006eb12e7130, VSyncRequest::None}
    Connection{0xb400006eb12eb730, VSyncRequest::None}
    Connection{0xb400006eb12e7f30, VSyncRequest::None}
    Connection{0xb400006eb12ea3f0, VSyncRequest::None}
    Connection{0xb400006eb12e7e50, VSyncRequest::None}
VsyncReactor in use
Has 1 unfired fences
mInternalIgnoreFences=0 mExternalIgnoreFences=0
mMoreSamplesNeeded=0 mPeriodConfirmationInProgress=0
mPeriodTransitioningTo=nullptr
No Last HW vsync
CallbackRepeaters:
        SamplingThreadDispSyncListener: mPeriod=16.67 last vsync time -60921.81ms relative to now (stopped)
        app: mPeriod=16.67 last vsync time -9260.47ms relative to now (stopped)
        sf: mPeriod=16.67 last vsync time -9160.29ms relative to now (stopped)
VSyncTracker:
        mIdealPeriod=16.67
        Refresh Rate Map:
                For ideal period 16.67ms: period = 16.68ms, intercept = 99478
VSyncDispatch:
        Timer:
                DebugState: Waiting
        mTimerSlack: 0.50ms mMinVsyncDistance: 3.00ms
        mIntendedWakeupTime: 9221499256832.00ms from now
        mLastTimerCallback: 9175.80ms ago mLastTimerSchedule: 9175.71ms ago
        Callbacks:
                app:
                        mWorkDuration: 15.67ms mEarliestVsync: -9260.52ms relative to now
                        mLastDispatchTime: 9260.52ms ago
                SamplingThreadDispSyncListener:
                        mWorkDuration: 19.67ms mEarliestVsync: -60921.87ms relative to now
                        mLastDispatchTime: 60921.87ms ago
                sf:
                        mWorkDuration: 15.67ms mEarliestVsync: -9160.34ms relative to now
                        mLastDispatchTime: 9160.34ms ago

Static screen stats:
  < 1 frames: 3111.773 s (4.0%)
  < 2 frames: 3336.656 s (4.3%)
  < 3 frames: 290.400 s (0.4%)
  < 4 frames: 219.618 s (0.3%)
  < 5 frames: 242.919 s (0.3%)
  < 6 frames: 282.740 s (0.4%)
  < 7 frames: 2613.675 s (3.4%)
  7+ frames: 67792.195 s (87.0%)

Total missed frame count: 5087
HWC missed frame count: 2232
GPU missed frame count: 4142

Buffering stats:
  [Layer name] <Active time> <Two buffer> <Double buffered> <Triple buffered>
  [com.yto.customermanmagererp/com.yto.customermanmagererp.ui.activity.HomeActivity#0] 2264.99 0.337 0.543 0.457
  [NavigationBar0#0] 1767.09 0.515 0.676 0.324
  [StatusBar#0] 1232.43 0.510 0.907 0.093
  [NotificationShade#0] 486.78 0.137 0.884 0.116
  [com.android.launcher3/com.android.launcher3.uioverrides.QuickstepLauncher#0] 244.26 0.434 0.596 0.404
  [ColorFade#0] 214.63 0.004 0.034 0.966
  [com.yto.customermanmagererp/com.yto.customermanmagererp.ui.activity.BridgeWebViewActivity#0] 124.05 0.083 0.929 0.071
  [com.yto.pda.cwms/com.yto.pda.cwms.ui.activity.MainActivity#0] 115.46 0.339 0.622 0.378
  [InputMethod#0] 102.61 0.966 1.000 0.000
  [com.yto.customermanmagererp/com.yto.customermanmagererp.ui.activity.LoginActivity#0] 89.48 0.544 0.703 0.297
  [com.yto.customermanmagererp/com.yto.customermanmagererp.ui.activity.HomeActivity#1] 81.66 0.536 0.630 0.370
  [com.yto.customermanmagererp/com.huawei.hms.hmsscankit.ScanKitActivity#0] 55.33 0.056 0.737 0.263
  [com.yto.pda.cwms/com.yto.pda.cwms.ui.activity.MainActivity#1] 51.19 0.231 0.363 0.637
  [com.yto.customermanmagererp/com.yto.customermanager.ui.activity.OrderBalanceSearchActivity#0] 29.56 0.288 0.587 0.413
  [SurfaceView - com.aliyun.security.sase/com.aliyun.security.sase.MainActivity#0] 28.71 0.645 0.691 0.309
  [com.UCMobile/com.uc.browser.InnerUCMobile#1] 22.10 0.047 0.987 0.013
  [com.yto.customermanmagererp/com.yto.customermanager.ui.activity.OrderBalanceSearchActivity#1] 20.55 0.565 0.600 0.400
  [com.yto.customermanager/com.yto.customermanager.ui.activity.OrderBalanceSearchActivity#0] 19.78 0.675 0.772 0.228
  [com.aliyun.security.sase/com.aliyun.security.sase.MainActivity#0] 18.32 0.555 0.918 0.082
  [BootAnimation#0] 13.10 0.000 0.000 1.000
  [com.yto.customermanager/com.yto.customermanager.ui.activity.HomeActivity#1] 12.16 0.442 0.933 0.067
  [com.yto.pda.cwms/com.yto.pda.cwms.ui.activity.MainActivity#2] 11.34 0.221 0.385 0.615
  [com.yto.customermanager/com.yto.customermanager.ui.activity.OrderBalanceSearchActivity#1] 10.90 0.313 0.496 0.504
  [Splash Screen com.yto.customermanmagererp#0] 10.73 0.740 1.000 0.000
  [com.UCMobile/com.uc.browser.InnerUCMobile#0] 10.24 0.186 0.476 0.524
  [com.android.settings/com.android.settings.applications.InstalledAppDetails#0] 8.70 0.787 0.989 0.011
  [com.tencent.mm/com.tencent.mm.plugin.nfc_open.ui.NfcWebViewUI#0] 7.44 0.841 1.000 0.000
  [com.tencent.mm/com.tencent.mm.ui.LauncherUI#0] 6.64 0.466 0.788 0.212
  [com.yto.customermanager/com.yto.customermanager.ui.activity.HomeActivity#0] 6.47 0.622 0.891 0.109
  [com.tencent.mm/com.tencent.mm.plugin.account.ui.LoginPasswordUI#0] 5.09 0.212 0.212 0.788
  [com.yto.pda.cwms/com.yto.pda.cwms.ui.activity.LoginActivity#0] 4.39 0.684 0.853 0.127
  [com.yto.customermanmagererp/com.yto.framework.jsbridge.activity.CommonPayWebActivity#0] 3.51 0.093 0.865 0.135
  [VolumeDialogImpl#0] 3.28 0.297 0.297 0.703
  [com.yto.customermanmagererp/com.yto.customermanmagererp.ui.activity.CrashErrorActivity#1] 3.24 0.814 0.814 0.186
  [com.yto.pda.cwms/com.yto.pda.cwms.ui.activity.LoginActivity#1] 2.99 0.307 0.307 0.693
  [com.UCMobile/com.uc.module.barcode.CaptureActivity#0] 2.96 1.000 1.000 0.000
  [com.android.permissioncontroller/com.android.permissioncontroller.permission.ui.GrantPermissionsActivity#0] 2.89 0.601 0.601 0.399
  [SurfaceView - com.UCMobile/com.uc.module.barcode.CaptureActivity#0] 2.77 0.000 1.000 0.000
  [magnifier surface#0] 2.44 0.289 0.928 0.072
  [com.yto.pda.cwms/com.yto.pda.cwms.ui.activity.MainActivity#3] 2.43 0.445 0.445 0.555
  [com.yto.customermanmagererp/com.yto.customermanmagererp.ui.activity.LaunchActivity#0] 2.28 0.561 0.656 0.344
  [com.yto.customermanager/com.yto.customermanager.ui.activity.LoginActivity#0] 1.57 0.782 1.000 0.000
  [com.android.packageinstaller/com.android.packageinstaller.InstallInstalling#0] 1.24 1.000 1.000 0.000
  [com.tencent.mm/com.tencent.mm.plugin.nfc_open.ui.NfcWebViewUI#1] 0.95 0.000 1.000 0.000
  [com.android.packageinstaller/com.android.packageinstaller.UninstallerActivity#0] 0.88 0.445 0.586 0.414
  [com.android.packageinstaller/com.android.packageinstaller.InstallStaging#0] 0.87 0.000 0.000 1.000
  [com.yto.customermanager/com.yto.customermanager.ui.activity.BridgeWebViewActivity#0] 0.82 1.000 1.000 0.000
  [Splash Screen com.yto.pda.cwms#0] 0.76 0.724 1.000 0.000
  [PopupWindow:aa6a72d#0] 0.66 0.000 1.000 0.000
  [com.yto.customermanmagererp/com.yto.customermanmagererp.ui.activity.CrashErrorActivity#0] 0.62 1.000 1.000 0.000
  [PopupWindow:3b20f24#0] 0.61 1.000 1.000 0.000
  [com.yto.customermanmagererp/com.netease.nim.uikit.business.session.activity.TeamMessageActivity#0] 0.59 0.000 0.562 0.438
  [com.android.packageinstaller/com.android.packageinstaller.PackageInstallerActivity#0] 0.57 0.687 0.687 0.313
  [com.yto.pda.cwms/com.yto.pda.cwms.ui.activity.LoginActivity#3] 0.52 0.400 1.000 0.000
  [PopupWindow:6068952#0] 0.50 0.693 0.693 0.307
  [SurfaceView - com.tencent.mm/com.tencent.mm.ui.LauncherUI#0] 0.49 1.000 1.000 0.000
  [PopupWindow:a5ef502#0] 0.48 0.676 0.676 0.324
  [com.ubx.uscanner/com.ubx.uscanner.ScannerDemoActivity#0] 0.41 0.395 1.000 0.000
  [com.android.packageinstaller/com.android.packageinstaller.UninstallUninstalling#0] 0.39 1.000 1.000 0.000
  [Application Not Responding: com.yto.customermanager#0] 0.34 0.000 1.000 0.000
  [com.yto.customermanager/com.yto.customermanager.ui.activity.HomeActivity#2] 0.34 1.000 1.000 0.000
  [PopupWindow:4adbf68#0] 0.34 0.265 0.265 0.735
  [PopupWindow:e8175de#0] 0.33 0.000 0.000 1.000
  [PopupWindow:9aae3fa#0] 0.32 1.000 1.000 0.000
  [PopupWindow:aed41e4#0] 0.31 1.000 1.000 0.000
  [PopupWindow:43e2e89#0] 0.30 0.230 0.230 0.770
  [PopupWindow:c1f482c#0] 0.29 1.000 1.000 0.000
  [com.yto.pda.cwms/com.yto.pda.cwms.ui.activity.LoginActivity#2] 0.29 1.000 1.000 0.000
  [com.yto.customermanager/com.yto.customermanager.ui.activity.LoginActivity#1] 0.28 0.000 0.000 1.000
  [PopupWindow:1b5afe1#0] 0.28 0.205 0.205 0.795
  [com.tencent.mm/com.tencent.mm.ui.transmit.SendAppMessageWrapperUI#1] 0.28 1.000 1.000 0.000
  [PopupWindow:153ed80#0] 0.28 0.000 1.000 0.000
  [PopupWindow:4c795db#0] 0.25 0.000 0.000 1.000
  [PopupWindow:e4a6cff#0] 0.25 0.000 0.000 1.000
  [PopupWindow:9f8069e#0] 0.25 0.000 0.000 1.000
  [com.tencent.mm/com.tencent.mm.plugin.base.stub.WXEntryActivity#0] 0.24 1.000 1.000 0.000
  [PopupWindow:d17bf56#0] 0.24 1.000 1.000 0.000
  [PopupWindow:19534e3#0] 0.23 0.000 0.000 1.000
  [PopupWindow:805780c#0] 0.23 0.000 0.000 1.000
  [PopupWindow:f625d5b#0] 0.23 0.000 0.000 1.000
  [PopupWindow:1976da4#0] 0.23 1.000 1.000 0.000
  [PopupWindow:44aa000#0] 0.23 0.000 0.000 1.000
  [PopupWindow:9188fdd#0] 0.22 1.000 1.000 0.000
  [PopupWindow:aafc010#0] 0.22 1.000 1.000 0.000
  [PopupWindow:939e6ae#0] 0.22 1.000 1.000 0.000
  [PopupWindow:5d85e7e#0] 0.21 1.000 1.000 0.000
  [com.tencent.mm/com.tencent.mm.plugin.readerapp.ui.ReaderAppUI#0] 0.20 0.000 0.000 1.000
  [PopupWindow:beb2158#0] 0.18 1.000 1.000 0.000
  [com.yto.customermanmagererp/com.yto.customermanager.ui.activity.MaterielRechargeActivity#0] 0.14 1.000 1.000 0.000
  [#0] 0.13 1.000 1.000 0.000
  [com.yto.customermanmagererp/com.yto.customermanager.ui.activity.MaterielRechargeActivity#1] 0.10 1.000 1.000 0.000
  [com.yto.customermanager/com.yto.customermanager.ui.activity.BridgeWebViewActivity#1] 0.08 0.000 1.000 0.000

Visible layers (count = 64)
GraphicBufferProducers: 8, max 4096
Composition layers
* Layer 0xb400006fb12f07a0 (com.android.systemui.ImageWallpaper#0)
      isSecure=false geomUsesSourceCrop=true geomBufferUsesDisplayInverseTransform=false geomLayerTransform 0x00000000 (ROT_0 ) 0x05 (SCALE TRANSLATE )
    1.9800  0.0000  -36.0000
    0.0000  1.9800  -72.0000
    0.0000  0.0000  1.0000

      geomBufferSize=[0 0 480 800] geomContentCrop=[0 0 480 800] geomCrop=[0 0 -1 -1] geomBufferTransform=0
        Region transparentRegionHint (this=0xb400006f012f6520, count=1)
    [  0,   0,   0,   0]
      geomLayerBounds=[18.181818 36.363636 381.818176 763.636353]       shadowRadius=0.000000
      blend=NONE (1) alpha=1.000000 backgroundBlurRadius=0
      type=2013 appId=10131 composition type=DEVICE (2)
      buffer: slot=0 buffer=0xb400006ec131a7b0
      sideband stream=0x0
      color=[0.000000 0.000000 0.000000]
      isOpaque=true hasProtectedContent=false isColorspaceAgnostic=false dataspace=UNKNOWN (0) hdr metadata types=0 colorTransform=[[1.000,0.000,0.000,0.000][0.000,1.000,0.000,0.000][0.000,0.000,1.000,0.000][0.000,0.000,0.000,1.000]]
* Layer 0xb400006fa1324f50 (Task=1#0)
      isSecure=false geomUsesSourceCrop=false geomBufferUsesDisplayInverseTransform=false geomLayerTransform 0x00000000 (ROT_0 ) 0x00 (IDENTITY )
    1.0000  0.0000  0.0000
    0.0000  1.0000  0.0000
    0.0000  0.0000  1.0000

      geomBufferSize=[0 0 -1 -1] geomContentCrop=[0 0 -1 -1] geomCrop=[0 0 -1 -1] geomBufferTransform=0
        Region transparentRegionHint (this=0xb400006f012f7b00, count=1)
    [  0,   0,   0,   0]
      geomLayerBounds=[0.000000 0.000000 720.000000 1440.000000]       shadowRadius=0.000000
      blend=NONE (1) alpha=1.000000 backgroundBlurRadius=0
      type=0 appId=0 composition type=INVALID (0)
      buffer: slot=-1 buffer=0x0
      sideband stream=0x0
      color=[0.000000 0.000000 0.000000]
      isOpaque=true hasProtectedContent=false isColorspaceAgnostic=false dataspace=UNKNOWN (0) hdr metadata types=0 colorTransform=[[1.000,0.000,0.000,0.000][0.000,1.000,0.000,0.000][0.000,0.000,1.000,0.000][0.000,0.000,0.000,1.000]]
* Layer 0xb400006fa1366740 (Task=4393#0)
      isSecure=false geomUsesSourceCrop=false geomBufferUsesDisplayInverseTransform=false geomLayerTransform 0x00000000 (ROT_0 ) 0x00 (IDENTITY )
    1.0000  0.0000  0.0000
    0.0000  1.0000  0.0000
    0.0000  0.0000  1.0000

      geomBufferSize=[0 0 -1 -1] geomContentCrop=[0 0 -1 -1] geomCrop=[0 0 -1 -1] geomBufferTransform=0
        Region transparentRegionHint (this=0xb400006f012f4f40, count=1)
    [  0,   0,   0,   0]
      geomLayerBounds=[0.000000 0.000000 720.000000 1440.000000]       shadowRadius=0.000000
      blend=NONE (1) alpha=1.000000 backgroundBlurRadius=0
      type=0 appId=0 composition type=INVALID (0)
      buffer: slot=-1 buffer=0x0
      sideband stream=0x0
      color=[0.000000 0.000000 0.000000]
      isOpaque=true hasProtectedContent=false isColorspaceAgnostic=false dataspace=UNKNOWN (0) hdr metadata types=0 colorTransform=[[1.000,0.000,0.000,0.000][0.000,1.000,0.000,0.000][0.000,0.000,1.000,0.000][0.000,0.000,0.000,1.000]]
* Layer 0xb400006fa1358370 (Task=4#0)
      isSecure=false geomUsesSourceCrop=false geomBufferUsesDisplayInverseTransform=false geomLayerTransform 0x00000000 (ROT_0 ) 0x00 (IDENTITY )
    1.0000  0.0000  0.0000
    0.0000  1.0000  0.0000
    0.0000  0.0000  1.0000

      geomBufferSize=[0 0 -1 -1] geomContentCrop=[0 0 -1 -1] geomCrop=[0 0 -1 -1] geomBufferTransform=0
        Region transparentRegionHint (this=0xb400006f012f77e0, count=1)
    [  0,   0,   0,   0]
      geomLayerBounds=[0.000000 0.000000 720.000000 1440.000000]       shadowRadius=0.000000
      blend=NONE (1) alpha=1.000000 backgroundBlurRadius=0
      type=0 appId=0 composition type=INVALID (0)
      buffer: slot=-1 buffer=0x0
      sideband stream=0x0
      color=[0.000000 0.000000 0.000000]
      isOpaque=true hasProtectedContent=false isColorspaceAgnostic=false dataspace=UNKNOWN (0) hdr metadata types=0 colorTransform=[[1.000,0.000,0.000,0.000][0.000,1.000,0.000,0.000][0.000,0.000,1.000,0.000][0.000,0.000,0.000,1.000]]
* Layer 0xb400006fa1360c20 (Secondary Divider Dim#0)
      isSecure=false geomUsesSourceCrop=false geomBufferUsesDisplayInverseTransform=false geomLayerTransform 0x00000000 (ROT_0 ) 0x00 (IDENTITY )
    1.0000  0.0000  0.0000
    0.0000  1.0000  0.0000
    0.0000  0.0000  1.0000

      geomBufferSize=[0 0 -1 -1] geomContentCrop=[0 0 -1 -1] geomCrop=[0 0 -1 -1] geomBufferTransform=0
        Region transparentRegionHint (this=0xb400006f012f1a20, count=1)
    [  0,   0,   0,   0]
      geomLayerBounds=[0.000000 0.000000 720.000000 1440.000000]       shadowRadius=0.000000
      blend=NONE (1) alpha=1.000000 backgroundBlurRadius=0
      type=0 appId=0 composition type=INVALID (0)
      buffer: slot=-1 buffer=0x0
      sideband stream=0x0
      color=[0.000000 0.000000 0.000000]
      isOpaque=true hasProtectedContent=false isColorspaceAgnostic=false dataspace=UNKNOWN (0) hdr metadata types=0 colorTransform=[[1.000,0.000,0.000,0.000][0.000,1.000,0.000,0.000][0.000,0.000,1.000,0.000][0.000,0.000,0.000,1.000]]
* Layer 0xb400006fa13555e0 (Task=3#0)
      isSecure=false geomUsesSourceCrop=false geomBufferUsesDisplayInverseTransform=false geomLayerTransform 0x00000000 (ROT_0 ) 0x00 (IDENTITY )
    1.0000  0.0000  0.0000
    0.0000  1.0000  0.0000
    0.0000  0.0000  1.0000

      geomBufferSize=[0 0 -1 -1] geomContentCrop=[0 0 -1 -1] geomCrop=[0 0 -1 -1] geomBufferTransform=0
        Region transparentRegionHint (this=0xb400006f012f26a0, count=1)
    [  0,   0,   0,   0]
      geomLayerBounds=[0.000000 0.000000 720.000000 1440.000000]       shadowRadius=0.000000
      blend=NONE (1) alpha=1.000000 backgroundBlurRadius=0
      type=0 appId=0 composition type=INVALID (0)
      buffer: slot=-1 buffer=0x0
      sideband stream=0x0
      color=[0.000000 0.000000 0.000000]
      isOpaque=true hasProtectedContent=false isColorspaceAgnostic=false dataspace=UNKNOWN (0) hdr metadata types=0 colorTransform=[[1.000,0.000,0.000,0.000][0.000,1.000,0.000,0.000][0.000,0.000,1.000,0.000][0.000,0.000,0.000,1.000]]
* Layer 0xb400006fa135b100 (Primary Divider Dim#0)
      isSecure=false geomUsesSourceCrop=false geomBufferUsesDisplayInverseTransform=false geomLayerTransform 0x00000000 (ROT_0 ) 0x00 (IDENTITY )
    1.0000  0.0000  0.0000
    0.0000  1.0000  0.0000
    0.0000  0.0000  1.0000

      geomBufferSize=[0 0 -1 -1] geomContentCrop=[0 0 -1 -1] geomCrop=[0 0 -1 -1] geomBufferTransform=0
        Region transparentRegionHint (this=0xb400006f012f6200, count=1)
    [  0,   0,   0,   0]
      geomLayerBounds=[0.000000 0.000000 720.000000 1440.000000]       shadowRadius=0.000000
      blend=NONE (1) alpha=1.000000 backgroundBlurRadius=0
      type=0 appId=0 composition type=INVALID (0)
      buffer: slot=-1 buffer=0x0
      sideband stream=0x0
      color=[0.000000 0.000000 0.000000]
      isOpaque=true hasProtectedContent=false isColorspaceAgnostic=false dataspace=UNKNOWN (0) hdr metadata types=0 colorTransform=[[1.000,0.000,0.000,0.000][0.000,1.000,0.000,0.000][0.000,0.000,1.000,0.000][0.000,0.000,0.000,1.000]]
* Layer 0xb400006fa1463e50 (Task=4624#0)
      isSecure=false geomUsesSourceCrop=false geomBufferUsesDisplayInverseTransform=false geomLayerTransform 0x00000000 (ROT_0 ) 0x00 (IDENTITY )
    1.0000  0.0000  0.0000
    0.0000  1.0000  0.0000
    0.0000  0.0000  1.0000

      geomBufferSize=[0 0 -1 -1] geomContentCrop=[0 0 -1 -1] geomCrop=[0 0 -1 -1] geomBufferTransform=0
        Region transparentRegionHint (this=0xb400006f012fa080, count=1)
    [  0,   0,   0,   0]
      geomLayerBounds=[0.000000 0.000000 720.000000 1440.000000]       shadowRadius=0.000000
      blend=NONE (1) alpha=1.000000 backgroundBlurRadius=0
      type=0 appId=0 composition type=INVALID (0)
      buffer: slot=-1 buffer=0x0
      sideband stream=0x0
      color=[0.000000 0.000000 0.000000]
      isOpaque=true hasProtectedContent=false isColorspaceAgnostic=false dataspace=UNKNOWN (0) hdr metadata types=0 colorTransform=[[1.000,0.000,0.000,0.000][0.000,1.000,0.000,0.000][0.000,0.000,1.000,0.000][0.000,0.000,0.000,1.000]]
* Layer 0xb400006fa147aad0 (Task=4645#0)
      isSecure=false geomUsesSourceCrop=false geomBufferUsesDisplayInverseTransform=false geomLayerTransform 0x00000000 (ROT_0 ) 0x00 (IDENTITY )
    1.0000  0.0000  0.0000
    0.0000  1.0000  0.0000
    0.0000  0.0000  1.0000

      geomBufferSize=[0 0 -1 -1] geomContentCrop=[0 0 -1 -1] geomCrop=[0 0 -1 -1] geomBufferTransform=0
        Region transparentRegionHint (this=0xb400006f013055c0, count=1)
    [  0,   0,   0,   0]
      geomLayerBounds=[0.000000 0.000000 720.000000 1440.000000]       shadowRadius=0.000000
      blend=NONE (1) alpha=1.000000 backgroundBlurRadius=0
      type=0 appId=0 composition type=INVALID (0)
      buffer: slot=-1 buffer=0x0
      sideband stream=0x0
      color=[0.000000 0.000000 0.000000]
      isOpaque=true hasProtectedContent=false isColorspaceAgnostic=false dataspace=UNKNOWN (0) hdr metadata types=0 colorTransform=[[1.000,0.000,0.000,0.000][0.000,1.000,0.000,0.000][0.000,0.000,1.000,0.000][0.000,0.000,0.000,1.000]]
* Layer 0xb400006fb12f76c0 (com.yto.customermanmagererp/com.yto.customermanmagererp.ui.activity.HomeActivity#0)
      isSecure=false geomUsesSourceCrop=true geomBufferUsesDisplayInverseTransform=false geomLayerTransform 0x00000000 (ROT_0 ) 0x00 (IDENTITY )
    1.0000  0.0000  0.0000
    0.0000  1.0000  0.0000
    0.0000  0.0000  1.0000

      geomBufferSize=[0 0 720 1440] geomContentCrop=[0 0 720 1440] geomCrop=[0 0 720 1440] geomBufferTransform=0
        Region transparentRegionHint (this=0xb400006f013058e0, count=1)
    [  0,   0,   0,   0]
      geomLayerBounds=[0.000000 0.000000 720.000000 1440.000000]       shadowRadius=0.000000
      blend=NONE (1) alpha=1.000000 backgroundBlurRadius=0
      type=1 appId=10291 composition type=DEVICE (2)
      buffer: slot=2 buffer=0xb400006ec13156b0
      sideband stream=0x0
      color=[0.000000 0.000000 0.000000]
      isOpaque=true hasProtectedContent=false isColorspaceAgnostic=false dataspace=UNKNOWN (0) hdr metadata types=0 colorTransform=[[1.000,0.000,0.000,0.000][0.000,1.000,0.000,0.000][0.000,0.000,1.000,0.000][0.000,0.000,0.000,1.000]]
* Layer 0xb400006fa13a51a0 (Surface(name=f7e7973 InputMethod)/@0x9c77a2e - animation-leash#0)
      isSecure=false geomUsesSourceCrop=false geomBufferUsesDisplayInverseTransform=false geomLayerTransform 0x00000000 (ROT_0 ) 0x01 (TRANSLATE )
    1.0000  0.0000  0.0000
    0.0000  1.0000  48.0000
    0.0000  0.0000  1.0000

      geomBufferSize=[0 0 -1 -1] geomContentCrop=[0 0 -1 -1] geomCrop=[0 0 -1 -1] geomBufferTransform=0
        Region transparentRegionHint (this=0xb400006f01303360, count=1)
    [  0,   0,   0,   0]
      geomLayerBounds=[0.000000 -48.000000 720.000000 1392.000000]       shadowRadius=0.000000
      blend=NONE (1) alpha=1.000000 backgroundBlurRadius=0
      type=0 appId=0 composition type=INVALID (0)
      buffer: slot=-1 buffer=0x0
      sideband stream=0x0
      color=[0.000000 0.000000 0.000000]
      isOpaque=true hasProtectedContent=false isColorspaceAgnostic=false dataspace=UNKNOWN (0) hdr metadata types=0 colorTransform=[[1.000,0.000,0.000,0.000][0.000,1.000,0.000,0.000][0.000,0.000,1.000,0.000][0.000,0.000,0.000,1.000]]
* Layer 0xb400006fa144a440 (Surface(name=7cb2b10 StatusBar)/@0x89fb8f5 - animation-leash#0)
      isSecure=false geomUsesSourceCrop=false geomBufferUsesDisplayInverseTransform=false geomLayerTransform 0x00000000 (ROT_0 ) 0x00 (IDENTITY )
    1.0000  0.0000  0.0000
    0.0000  1.0000  0.0000
    0.0000  0.0000  1.0000

      geomBufferSize=[0 0 -1 -1] geomContentCrop=[0 0 -1 -1] geomCrop=[0 0 -1 -1] geomBufferTransform=0
        Region transparentRegionHint (this=0xb400006f012fe540, count=1)
    [  0,   0,   0,   0]
      geomLayerBounds=[0.000000 0.000000 720.000000 1440.000000]       shadowRadius=0.000000
      blend=NONE (1) alpha=1.000000 backgroundBlurRadius=0
      type=0 appId=0 composition type=INVALID (0)
      buffer: slot=-1 buffer=0x0
      sideband stream=0x0
      color=[0.000000 0.000000 0.000000]
      isOpaque=true hasProtectedContent=false isColorspaceAgnostic=false dataspace=UNKNOWN (0) hdr metadata types=0 colorTransform=[[1.000,0.000,0.000,0.000][0.000,1.000,0.000,0.000][0.000,0.000,1.000,0.000][0.000,0.000,0.000,1.000]]
* Layer 0xb400006fb1305500 (StatusBar#0)
      isSecure=false geomUsesSourceCrop=true geomBufferUsesDisplayInverseTransform=false geomLayerTransform 0x00000000 (ROT_0 ) 0x00 (IDENTITY )
    1.0000  0.0000  0.0000
    0.0000  1.0000  0.0000
    0.0000  0.0000  1.0000

      geomBufferSize=[0 0 720 48] geomContentCrop=[0 0 720 48] geomCrop=[0 0 720 48] geomBufferTransform=0
        Region transparentRegionHint (this=0xb400006f012f13e0, count=1)
    [  0,   0,   0,   0]
      geomLayerBounds=[0.000000 0.000000 720.000000 48.000000]       shadowRadius=0.000000
      blend=PREMULTIPLIED (2) alpha=1.000000 backgroundBlurRadius=0
      type=2000 appId=10131 composition type=DEVICE (2)
      buffer: slot=2 buffer=0xb400006ec13105b0
      sideband stream=0x0
      color=[0.000000 0.000000 0.000000]
      isOpaque=false hasProtectedContent=false isColorspaceAgnostic=true dataspace=UNKNOWN (0) hdr metadata types=0 colorTransform=[[1.000,0.000,0.000,0.000][0.000,1.000,0.000,0.000][0.000,0.000,1.000,0.000][0.000,0.000,0.000,1.000]]
* Layer 0xb400006fa140e770 (Surface(name=e4a84e5 NavigationBar0)/@0x8da968a - animation-leash#0)
      isSecure=false geomUsesSourceCrop=false geomBufferUsesDisplayInverseTransform=false geomLayerTransform 0x00000000 (ROT_0 ) 0x01 (TRANSLATE )
    1.0000  0.0000  0.0000
    0.0000  1.0000  1344.0000
    0.0000  0.0000  1.0000

      geomBufferSize=[0 0 -1 -1] geomContentCrop=[0 0 -1 -1] geomCrop=[0 0 -1 -1] geomBufferTransform=0
        Region transparentRegionHint (this=0xb400006f012f9d60, count=1)
    [  0,   0,   0,   0]
      geomLayerBounds=[0.000000 -1344.000000 720.000000 96.000000]       shadowRadius=0.000000
      blend=NONE (1) alpha=1.000000 backgroundBlurRadius=0
      type=0 appId=0 composition type=INVALID (0)
      buffer: slot=-1 buffer=0x0
      sideband stream=0x0
      color=[0.000000 0.000000 0.000000]
      isOpaque=true hasProtectedContent=false isColorspaceAgnostic=false dataspace=UNKNOWN (0) hdr metadata types=0 colorTransform=[[1.000,0.000,0.000,0.000][0.000,1.000,0.000,0.000][0.000,0.000,1.000,0.000][0.000,0.000,0.000,1.000]]
* Layer 0xb400006fb12f3f30 (NavigationBar0#0)
      isSecure=false geomUsesSourceCrop=true geomBufferUsesDisplayInverseTransform=false geomLayerTransform 0x00000000 (ROT_0 ) 0x01 (TRANSLATE )
    1.0000  0.0000  0.0000
    0.0000  1.0000  1344.0000
    0.0000  0.0000  1.0000

      geomBufferSize=[0 0 720 96] geomContentCrop=[0 0 720 96] geomCrop=[0 0 720 96] geomBufferTransform=0
        Region transparentRegionHint (this=0xb400006f012f3640, count=1)
    [  0,   0,   0,   0]
      geomLayerBounds=[0.000000 0.000000 720.000000 96.000000]       shadowRadius=0.000000
      blend=PREMULTIPLIED (2) alpha=1.000000 backgroundBlurRadius=0
      type=2019 appId=10131 composition type=DEVICE (2)
      buffer: slot=1 buffer=0xb400006ec1314ed0
      sideband stream=0x0
      color=[0.000000 0.000000 0.000000]
      isOpaque=false hasProtectedContent=false isColorspaceAgnostic=true dataspace=UNKNOWN (0) hdr metadata types=0 colorTransform=[[1.000,0.000,0.000,0.000][0.000,1.000,0.000,0.000][0.000,0.000,1.000,0.000][0.000,0.000,0.000,1.000]]
Displays (1 entries)
+ DisplayDevice{19260618794624641, internal, primary, ""}
   powerMode=On (2), activeConfig=0,    Composition Display State: [""]
   isVirtual=false hwcId=19260618794624641
   isEnabled=true isSecure=true usesClientComposition=true usesDeviceComposition=false flipClientTarget=false reusedClientComposition=false layerStack=0 layerStackInternal=true
   transform 0x00000000 (ROT_0 ) 0x00 (IDENTITY )
    1.0000  0.0000  -0.0000
    0.0000  1.0000  -0.0000
    0.0000  0.0000  1.0000

   bounds=[0 0 720 1440] frame=[0 0 720 1440] viewport=[0 0 720 1440] sourceClip=[0 0 720 1440] destinationClip=[0 0 720 1440] needsFiltering=false
   colorMode=NATIVE (0) renderIntent=COLORIMETRIC (0) dataspace=UNKNOWN (0) colorTransformMatrix=[[1.000,0.000,0.000,0.000][0.000,1.000,0.000,0.000][0.000,0.000,1.000,0.000][0.000,0.000,0.000,1.000]]target dataspace=UNKNOWN (0)
   Composition Display Color State:
   HWC Support: wideColorGamut=false hdr10plus=false hdr10=false hlg=false dv=false metadata=3
   Composition RenderSurface State:
   size=[720 1440] ANativeWindow=0xb400006f912ec020 (format 1) flips=514944
  FramebufferSurface: dataspace: Default(0)
   mAbandoned=0
   - BufferQueue mMaxAcquiredBufferCount=1 mMaxDequeuedBufferCount=1
     mDequeueBufferCannotBlock=0 mAsyncMode=0
     mQueueBufferCanDrop=0 mLegacyBufferDrop=1
     default-size=[720x1440] default-format=1      transform-hint=00 frame-counter=75555
     mTransformHintInUse=00 mAutoPrerotation=0
   FIFO(0):
   (mConsumerName=FramebufferSurface, mConnectedApi=1, mConsumerUsageBits=6656, mId=35800000000, producer=[807:???], consumer=[856:/system/bin/surfaceflinger])
   Slots:
    >[00:0xb400006ec1310370] state=ACQUIRED 0xb400006e812e6d30 frame=75555 [ 720x1440: 768,  1]
     [01:0xb400006ec1313cd0] state=FREE     0xb400006e812fdd10 frame=75554 [ 720x1440: 768,  1]

   3 Layers
  - Output Layer 0xb400006f3131bc20(com.yto.customermanmagererp/com.yto.customermanmagererp.ui.activity.HomeActivity#0)
        Region visibleRegion (this=0xb400006f3131bc38, count=1)
    [  0,   0, 720, 1440]
        Region visibleNonTransparentRegion (this=0xb400006f3131bca0, count=1)
    [  0,   0, 720, 1440]
        Region coveredRegion (this=0xb400006f3131bd08, count=2)
    [  0,   0, 720,  48]
    [  0, 1344, 720, 1440]
        Region output visibleRegion (this=0xb400006f3131bd70, count=1)
    [  0,   0, 720, 1440]
        Region shadowRegion (this=0xb400006f3131bdd8, count=1)
    [  0,   0,   0,   0]
      forceClientComposition=false clearClientTarget=false displayFrame=[0 0 720 1440] sourceCrop=[0.000000 0.000000 720.000000 1440.000000] bufferTransform=0 (0) dataspace=UNKNOWN (0) z-index=0
      hwc: layer=0x08186e composition=CLIENT (1)
  - Output Layer 0xb400006f31310800(StatusBar#0)
        Region visibleRegion (this=0xb400006f31310818, count=1)
    [  0,   0, 720,  48]
        Region visibleNonTransparentRegion (this=0xb400006f31310880, count=1)
    [  0,   0, 720,  48]
        Region coveredRegion (this=0xb400006f313108e8, count=1)
    [  0,   0,   0,   0]
        Region output visibleRegion (this=0xb400006f31310950, count=1)
    [  0,   0, 720,  48]
        Region shadowRegion (this=0xb400006f313109b8, count=1)
    [  0,   0,   0,   0]
      forceClientComposition=false clearClientTarget=false displayFrame=[0 0 720 48] sourceCrop=[0.000000 0.000000 720.000000 48.000000] bufferTransform=0 (0) dataspace=UNKNOWN (0) z-index=1
      hwc: layer=0x0817f6 composition=CLIENT (1)
  - Output Layer 0xb400006f3130c680(NavigationBar0#0)
        Region visibleRegion (this=0xb400006f3130c698, count=1)
    [  0, 1344, 720, 1440]
        Region visibleNonTransparentRegion (this=0xb400006f3130c700, count=1)
    [  0, 1344, 720, 1440]
        Region coveredRegion (this=0xb400006f3130c768, count=1)
    [  0,   0,   0,   0]
        Region output visibleRegion (this=0xb400006f3130c7d0, count=1)
    [  0, 1344, 720, 1440]
        Region shadowRegion (this=0xb400006f3130c838, count=1)
    [  0,   0,   0,   0]
      forceClientComposition=false clearClientTarget=false displayFrame=[0 1344 720 1440] sourceCrop=[0.000000 0.000000 720.000000 96.000000] bufferTransform=0 (0) dataspace=UNKNOWN (0) z-index=2
      hwc: layer=0x08186d composition=CLIENT (1)

SurfaceFlinger global state:
EGL implementation : 1.5
EGL_KHR_image EGL_KHR_image_base EGL_QCOM_create_image EGL_KHR_lock_surface EGL_KHR_lock_surface2 EGL_KHR_lock_surface3 EGL_KHR_gl_texture_2D_image EGL_KHR_gl_texture_cubemap_image EGL_KHR_gl_texture_3D_image EGL_KHR_gl_renderbuffer_image EGL_ANDROID_blob_cache EGL_KHR_create_context EGL_KHR_surfaceless_context
 EGL_KHR_create_context_no_error EGL_KHR_get_all_proc_addresses EGL_QCOM_lock_image2 EGL_EXT_protected_content EGL_KHR_no_config_context EGL_EXT_surface_SMPTE2086_metadata EGL_ANDROID_recordable EGL_ANDROID_native_fence_sync EGL_ANDROID_image_native_buffer EGL_ANDROID_framebuffer_target EGL_EXT_create_context_r
obustness EGL_EXT_pixel_format_float EGL_EXT_yuv_surface EGL_IMG_context_priority EGL_IMG_image_plane_attribs EGL_KHR_cl_event EGL_KHR_cl_event2 EGL_KHR_fence_sync EGL_KHR_gl_colorspace EGL_EXT_image_gl_colorspace EGL_KHR_mutable_render_buffer EGL_KHR_partial_update EGL_KHR_reusable_sync EGL_KHR_wait_sync EGL_QCOM_gpu_perf
GLES: Qualcomm, Adreno (TM) 610, OpenGL ES 3.2 V@0502.0 (GIT@bbbe64c9d2, I6ff57b37e0, 1627303968) (Date:07/26/21)
GL_OES_EGL_image GL_OES_EGL_image_external GL_OES_EGL_sync GL_OES_vertex_half_float GL_OES_framebuffer_object GL_OES_rgb8_rgba8 GL_OES_compressed_ETC1_RGB8_texture GL_AMD_compressed_ATC_texture GL_KHR_texture_compression_astc_ldr GL_OES_texture_npot GL_EXT_texture_filter_anisotropic GL_EXT_texture_format_BGRA88
88 GL_EXT_read_format_bgra GL_OES_texture_3D GL_EXT_color_buffer_float GL_EXT_color_buffer_half_float GL_QCOM_alpha_test GL_OES_depth24 GL_OES_packed_depth_stencil GL_OES_depth_texture GL_OES_depth_texture_cube_map GL_EXT_sRGB GL_OES_texture_float GL_OES_texture_float_linear GL_OES_texture_half_float GL_OES_tex
ture_half_float_linear GL_EXT_texture_type_2_10_10_10_REV GL_EXT_texture_sRGB_decode GL_EXT_texture_format_sRGB_override GL_OES_element_index_uint GL_EXT_copy_image GL_EXT_geometry_shader GL_EXT_tessellation_shader GL_OES_texture_stencil8 GL_EXT_shader_io_blocks GL_OES_shader_image_atomic GL_OES_sample_variable
s GL_EXT_texture_border_clamp GL_EXT_EGL_image_external_wrap_modes GL_EXT_multisampled_render_to_texture GL_EXT_multisampled_render_to_texture2 GL_OES_shader_multisample_interpolation GL_EXT_texture_cube_map_array GL_EXT_draw_buffers_indexed GL_EXT_gpu_shader5 GL_EXT_robustness GL_EXT_texture_buffer GL_EXT_shad
er_framebuffer_fetch GL_ARM_shader_framebuffer_fetch_depth_stencil GL_OES_texture_storage_multisample_2d_array GL_OES_sample_shading GL_OES_get_program_binary GL_EXT_debug_label GL_KHR_blend_equation_advanced GL_KHR_blend_equation_advanced_coherent GL_QCOM_tiled_rendering GL_ANDROID_extension_pack_es31a GL_EXT_
primitive_bounding_box GL_OES_standard_derivatives GL_OES_vertex_array_object GL_EXT_disjoint_timer_query GL_KHR_debug GL_EXT_YUV_target GL_EXT_sRGB_write_control GL_EXT_texture_norm16 GL_EXT_discard_framebuffer GL_OES_surfaceless_context GL_OVR_multiview GL_OVR_multiview2 GL_EXT_texture_sRGB_R8 GL_KHR_no_error
 GL_EXT_debug_marker GL_OES_EGL_image_external_essl3 GL_OVR_multiview_multisampled_render_to_texture GL_EXT_buffer_storage GL_EXT_external_buffer GL_EXT_blit_framebuffer_params GL_EXT_clip_cull_distance GL_EXT_protected_textures GL_EXT_shader_non_constant_global_initializers GL_QCOM_texture_foveated GL_QCOM_tex
ture_foveated_subsampled_layout GL_QCOM_shader_framebuffer_fetch_noncoherent GL_QCOM_shader_framebuffer_fetch_rate GL_EXT_memory_object GL_EXT_memory_object_fd GL_EXT_EGL_image_array GL_NV_shader_noperspective_interpolation GL_KHR_robust_buffer_access_behavior GL_EXT_EGL_image_storage GL_EXT_blend_func_extended GL_EXT_clip_control GL_OES_texture_view GL_EXT_fragment_invocation_density GL_QCOM_YUV_texture_gather
RenderEngine supports protected context: 1
RenderEngine is in protected context: 0
RenderEngine program cache size for unprotected context: 64
RenderEngine program cache size for protected context: 0
RenderEngine last dataspace conversion: (Default) to (Default)
RenderEngine image cache size: 12
Dumping buffer ids...
0x35800092f83
0x35800092f84
0x35800092f85
0x35800092e60
0x35800092c45
0x3580000b4d3
0x3580000b424
0x35800039c79
0x3580000b407
0x3580000b406
0x3580000b3f4
0x3580000b3e9
RenderEngine framebuffer image cache size: 2
Dumping buffer ids...
0x35800000003
0x358000000eb
  Region undefinedRegion (this=0xb400006f012e3d68, count=1)
    [  0,   0,   0,   0]
  orientation=ROTATION_0, isPoweredOn=1
  transaction-flags         : 00000000
  gpu_to_cpu_unsupported    : 0
  refresh-rate              : 60.000002 fps
  x-dpi                     : 281.352997
  y-dpi                     : 281.352997
  transaction time: 0.000000 us
Tracing state: disabled
  number of entries: 0 (0.00MB / 0.00MB)

Display 19260618794624641 HWC layers:
-----------------------------------------------------------------------------------------------------------------------------------------------
 Layer name
           Z |  Window Type |  Layer Class |  Comp Type |  Transform |   Disp Frame (LTRB) |          Source Crop (LTRB) |     Frame Rate (Explicit) [Focused]
-----------------------------------------------------------------------------------------------------------------------------------------------
 com.yto.customermanmagererp/com.yto.[...]nmagererp.ui.activity.HomeActivity#0
  rel      0 |            1 |            0 |     CLIENT |          0 |    0    0  720 1440 |    0.0    0.0  720.0 1440.0 |                              [*]
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
 StatusBar#0
  rel      0 |         2000 |            0 |     CLIENT |          0 |    0    0  720   48 |    0.0    0.0  720.0   48.0 |                              [ ]
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
 NavigationBar0#0
  rel      0 |         2019 |            0 |     CLIENT |          0 |    0 1344  720 1440 |    0.0    0.0  720.0   96.0 |                              [ ]
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

h/w composer state:
  h/w composer enabled

------------HWC----------------
HWC2 display_id: 0
layer: 6254 z: 0 composition: Client/Client alpha: 255 format:         RGBA_8888_UBWC dataspace:0x00000000 transform: 0/0/0 buffer_id: 0xb4000071ecaff310 secure: 0
layer: 6134 z: 1 composition: Client/Client alpha: 255 format:         RGBA_8888_UBWC dataspace:0x00000000 transform: 0/0/0 buffer_id: 0xb4000071ecb006c0 secure: 0
layer: 6253 z: 2 composition: Client/Client alpha: 255 format:         RGBA_8888_UBWC dataspace:0x00000000 transform: 0/0/0 buffer_id: 0xb4000071ecaffd30 secure: 0

---------client target---------
format: RGBA_8888_UBWC dataspace:0x08810000  buffer_id: 0xb4000071ecb01560 secure: 0

----------Color Modes---------
color modes supported:
mode: 0 RIs { 0 dynamic_range [ 0 ] }
current mode: 0
current render_intent: 0
current dynamic_range: SDR
current transform:
  1.00   0.00   0.00   0.00
  0.00   1.00   0.00   0.00
  0.00   0.00   1.00   0.00
  0.00   0.00   0.00   1.00

------------SDM----------------
device type:0
state: 1 vsync on: 0 max. mixer stages: 4
num configs: 1 active config index: 0
Display Attributes:
 Mode:Video Primary:true DynFPS:false
 HDR Panel:false QSync:false DynBitclk:false
 Left Split:720 Right Split:0
 PartialUpdate:false
 FPS min:60 max:60 cur:60 TransferTime: 0us MaxBrightness:255
 Display WxH: 720x1440 MixerWxH: 720x1440 DPI: 281.354x281.354 LM_Split: false
 vsync_period 16666666 v_back_porch: 10 v_front_porch: 16 v_pulse_width: 5
 v_total: 1471 h_total: 820 clk: 72373 Topology: 1 Qsync mode: 0
Current Color Mode: hal_native
Available Color Modes:

ROI(LTRB)#0 LEFT(0 0 720 1440)

|-----|---------------|-----------|------|-------------|--------------------------|---------------------|---------------------|----|------------|-----------|----|-----|----|
| Idx |   Comp Type   |   Split   | Pipe |    W x H    |          Format          |  Src Rect (L T R B) |  Dst Rect (L T R B) |  Z | Pipe Flags | Deci(HxV) | CS | Rng | Tr |
|-----|---------------|-----------|------|-------------|--------------------------|---------------------|---------------------|----|------------|-----------|----|-----|----|
|   3 |    GPU_TARGET |    Pipe-1 |   62 |  768 x 1440 |           RGBA_8888_UBWC |    0    0  720 1440 |    0    0  720 1440 |  0 | 0x00000001 |   0 x   0 |  1 |   1 |  1 |
|-----|---------------|-----------|------|-------------|--------------------------|---------------------|---------------------|----|------------|-----------|----|-----|----|

Color Sampling, dark (0.0) to light (1.0): sampled frames: 0
        no color statistics collected

------------Active Fences Info---------
---------------------------------------
GraphicBufferAllocator buffers:
0xb400006e812e6ca0:    0.25 KiB |    1 (  64) x    1 |    1 |        1 | 0x300 | placeholder
0xb400006e812e6d30: 4320.00 KiB |  720 ( 768) x 1440 |    1 |        1 | 0x1a00 | FramebufferSurface
0xb400006e812f8550:  144.00 KiB |  720 ( 768) x   48 |    1 |        1 | 0x10000900 | StatusBar#0
0xb400006e812fdd10: 4320.00 KiB |  720 ( 768) x 1440 |    1 |        1 | 0x1a00 | FramebufferSurface
0xb400006e813021b0:  288.00 KiB |  720 ( 768) x   96 |    1 |        1 | 0x10000900 | NavigationBar0#0
0xb400006e81304640:  144.00 KiB |  720 ( 768) x   48 |    1 |        1 | 0x10000900 | StatusBar#0
0xb400006e813054e0:  288.00 KiB |  720 ( 768) x   96 |    1 |        1 | 0x10000900 | NavigationBar0#0
0xb400006e81306410:  288.00 KiB |  720 ( 768) x   96 |    1 |        1 | 0x10000900 | NavigationBar0#0
0xb400006e813066e0:  144.00 KiB |  720 ( 768) x   48 |    1 |        1 | 0x10000900 | StatusBar#0
0xb400006e81315b00: 1600.00 KiB |  480 ( 512) x  800 |    1 |        2 | 0x10000900 | com.android.systemui.ImageWallpaper#0
0xb400006e8132ada0: 4320.00 KiB |  720 ( 768) x 1440 |    1 |        1 | 0x10000900 | ColorFade#0
0xb400006e813367d0: 4320.00 KiB |  720 ( 768) x 1440 |    1 |        1 | 0x10000900 | com.yto.customermanmagererp/com.yto.customermanmagererp.ui.activity.HomeActivity#0
0xb400006e81338b40: 4320.00 KiB |  720 ( 768) x 1440 |    1 |        1 | 0x10000900 | com.yto.customermanmagererp/com.yto.customermanmagererp.ui.activity.HomeActivity#0
0xb400006e8133a6d0: 4320.00 KiB |  720 ( 768) x 1440 |    1 |        1 | 0x10000900 | com.yto.customermanmagererp/com.yto.customermanmagererp.ui.activity.HomeActivity#0
Total allocated by GraphicBufferAllocator (estimate): 28816.25 KB
Imported gralloc buffers:
+ name:com.yto.customermanmagererp/com.yto.customermanmagererp.ui.activity.HomeActivity#0, id:20517, size:4.3e+03KiB, w/h:720x1440, usage: 0x10000900, req fmt:1, fourcc/mod:875708993/0, compressed: true
        planes: R/G/B/A:         w/h:720x1440, stride:3072 bytes, size:4448256
+ name:com.yto.customermanmagererp/com.yto.customermanmagererp.ui.activity.HomeActivity#0, id:20516, size:4.3e+03KiB, w/h:720x1440, usage: 0x10000900, req fmt:1, fourcc/mod:875708993/0, compressed: true
        planes: R/G/B/A:         w/h:720x1440, stride:3072 bytes, size:4448256
+ name:com.yto.customermanmagererp/com.yto.customermanmagererp.ui.activity.HomeActivity#0, id:20515, size:4.3e+03KiB, w/h:720x1440, usage: 0x10000900, req fmt:1, fourcc/mod:875708993/0, compressed: true
        planes: R/G/B/A:         w/h:720x1440, stride:3072 bytes, size:4448256
+ name:screenshot, id:20505, size:4.3e+03KiB, w/h:720x1440, usage: 0x333, req fmt:1, fourcc/mod:875708993/0, compressed: false
        planes: R/G/B/A:         w/h:720x1440, stride:3072 bytes, size:4423680
+ name:screenshot, id:20484, size:4.3e+03KiB, w/h:720x1440, usage: 0x333, req fmt:1, fourcc/mod:875708993/0, compressed: false
        planes: R/G/B/A:         w/h:720x1440, stride:3072 bytes, size:4423680
+ name:ColorFade#0, id:18384, size:4.3e+03KiB, w/h:720x1440, usage: 0x10000900, req fmt:1, fourcc/mod:875708993/0, compressed: true
        planes: R/G/B/A:         w/h:720x1440, stride:3072 bytes, size:4448256
+ name:com.android.systemui.ImageWallpaper#0, id:8200, size:1.6e+03KiB, w/h:480x800, usage: 0x10000900, req fmt:2, fourcc/mod:875709016/360287970189639681, compressed: true
        planes: R/G/B:   w/h:480x800, stride:2048 bytes, size:1654784
+ name:StatusBar#0, id:1640, size:1.5e+02KiB, w/h:720x48, usage: 0x10000900, req fmt:1, fourcc/mod:875708993/0, compressed: true
        planes: R/G/B/A:         w/h:720x48, stride:3072 bytes, size:151552
+ name:NavigationBar0#0, id:1637, size:2.9e+02KiB, w/h:720x96, usage: 0x10000900, req fmt:1, fourcc/mod:875708993/0, compressed: true
        planes: R/G/B/A:         w/h:720x96, stride:3072 bytes, size:299008
+ name:NavigationBar0#0, id:1649, size:2.9e+02KiB, w/h:720x96, usage: 0x10000900, req fmt:1, fourcc/mod:875708993/0, compressed: true
        planes: R/G/B/A:         w/h:720x96, stride:3072 bytes, size:299008
+ name:StatusBar#0, id:1636, size:1.5e+02KiB, w/h:720x48, usage: 0x10000900, req fmt:1, fourcc/mod:875708993/0, compressed: true
        planes: R/G/B/A:         w/h:720x48, stride:3072 bytes, size:151552
+ name:StatusBar#0, id:1635, size:1.5e+02KiB, w/h:720x48, usage: 0x10000900, req fmt:1, fourcc/mod:875708993/0, compressed: true
        planes: R/G/B/A:         w/h:720x48, stride:3072 bytes, size:151552
+ name:NavigationBar0#0, id:1634, size:2.9e+02KiB, w/h:720x96, usage: 0x10000900, req fmt:1, fourcc/mod:875708993/0, compressed: true
        planes: R/G/B/A:         w/h:720x96, stride:3072 bytes, size:299008
+ name:FramebufferSurface, id:21, size:4.3e+03KiB, w/h:720x1440, usage: 0x1a00, req fmt:1, fourcc/mod:875708993/0, compressed: true
        planes: R/G/B/A:         w/h:720x1440, stride:3072 bytes, size:4448256
+ name:FramebufferSurface, id:4, size:4.3e+03KiB, w/h:720x1440, usage: 0x1a00, req fmt:1, fourcc/mod:875708993/0, compressed: true
        planes: R/G/B/A:         w/h:720x1440, stride:3072 bytes, size:4448256
+ name:placeholder, id:1, size:4KiB, w/h:1x1, usage: 0x300, req fmt:1, fourcc/mod:875708993/0, compressed: false
        planes: R/G/B/A:         w/h:1x1, stride:256 bytes, size:4096
Total imported by gralloc: 3.8e+04KiB
TimeStats miniDump:
Number of layers currently being tracked is 0
Number of layers in the stats pool is 0

+ ContainerLayer (Root#0)
  Region TransparentRegion (this=0 count=0)
  Region VisibleRegion (this=0 count=0)
  Region SurfaceDamageRegion (this=0 count=0)
      layerStack=   0, z=        0, pos=(0,0), size=(   0,   0), crop=[  0,   0,  -1,  -1], cornerRadius=0.000000, isProtected=0, isTrustedOverlay=0, isOpaque=0, invalidate=1, dataspace=Default, defaultPixelFormat=Unknown/None, backgroundBlurRadius=0, color=(0.000,0.000,0.000,1.000), flags=0x00000002, tr=[0.00, 0.00][0.00, 0.00]
      parent=none
      zOrderRelativeOf=none
      activeBuffer=[   0x   0:   0,Unknown/None], tr=[0.00, 0.00][0.00, 0.00] queued-frames=0, mRefreshPending=0, metadata={5:4bytes}, cornerRadiusCrop=[0.00, 0.00, 0.00, 0.00],  shadowRadius=0.000,
+ ContainerLayer (mWindowContainers#0)
  Region TransparentRegion (this=0 count=0)
  Region VisibleRegion (this=0 count=0)
  Region SurfaceDamageRegion (this=0 count=0)
      layerStack=   0, z=        0, pos=(0,0), size=(   0,   0), crop=[  0,   0,  -1,  -1], cornerRadius=0.000000, isProtected=0, isTrustedOverlay=0, isOpaque=0, invalidate=1, dataspace=Default, defaultPixelFormat=Unknown/None, backgroundBlurRadius=0, color=(0.000,0.000,0.000,1.000), flags=0x00000000, tr=[0.00, 0.00][0.00, 0.00]
      parent=Root#0
      zOrderRelativeOf=none
      activeBuffer=[   0x   0:   0,Unknown/None], tr=[0.00, 0.00][0.00, 0.00] queued-frames=0, mRefreshPending=0, metadata={5:4bytes}, cornerRadiusCrop=[0.00, 0.00, 0.00, 0.00],  shadowRadius=0.000,
+ ContainerLayer (DisplayArea.Root#0)
  Region TransparentRegion (this=0 count=0)
  Region VisibleRegion (this=0 count=0)
  Region SurfaceDamageRegion (this=0 count=0)
      layerStack=   0, z=        0, pos=(0,0), size=(   0,   0), crop=[  0,   0,  -1,  -1], cornerRadius=0.000000, isProtected=0, isTrustedOverlay=0, isOpaque=0, invalidate=1, dataspace=Default, defaultPixelFormat=Unknown/None, backgroundBlurRadius=0, color=(0.000,0.000,0.000,1.000), flags=0x00000000, tr=[0.00, 0.00][0.00, 0.00]
      parent=mWindowContainers#0
      zOrderRelativeOf=none
      activeBuffer=[   0x   0:   0,Unknown/None], tr=[0.00, 0.00][0.00, 0.00] queued-frames=0, mRefreshPending=0, metadata={5:4bytes}, cornerRadiusCrop=[0.00, 0.00, 0.00, 0.00],  shadowRadius=0.000,
+ ContainerLayer (Leaf:0:1#0)
  Region TransparentRegion (this=0 count=0)
  Region VisibleRegion (this=0 count=0)
  Region SurfaceDamageRegion (this=0 count=0)
      layerStack=   0, z=        0, pos=(0,0), size=(   0,   0), crop=[  0,   0,  -1,  -1], cornerRadius=0.000000, isProtected=0, isTrustedOverlay=0, isOpaque=0, invalidate=1, dataspace=Default, defaultPixelFormat=Unknown/None, backgroundBlurRadius=0, color=(0.000,0.000,0.000,1.000), flags=0x00000000, tr=[0.00, 0.00][0.00, 0.00]
      parent=DisplayArea.Root#0
      zOrderRelativeOf=none
      activeBuffer=[   0x   0:   0,Unknown/None], tr=[0.00, 0.00][0.00, 0.00] queued-frames=0, mRefreshPending=0, metadata={5:4bytes}, cornerRadiusCrop=[0.00, 0.00, 0.00, 0.00],  shadowRadius=0.000,
+ ContainerLayer (WallpaperWindowToken{3e4ce52 token=android.os.Binder@46452dd}#0)
  Region TransparentRegion (this=0 count=0)
  Region VisibleRegion (this=0 count=0)
  Region SurfaceDamageRegion (this=0 count=0)
      layerStack=   0, z=        0, pos=(0,0), size=(   0,   0), crop=[  0,   0,  -1,  -1], cornerRadius=0.000000, isProtected=0, isTrustedOverlay=0, isOpaque=0, invalidate=1, dataspace=Default, defaultPixelFormat=Unknown/None, backgroundBlurRadius=0, color=(0.000,0.000,0.000,1.000), flags=0x00000000, tr=[0.00, 0.00][0.00, 0.00]
      parent=Leaf:0:1#0
      zOrderRelativeOf=none
      activeBuffer=[   0x   0:   0,Unknown/None], tr=[0.00, 0.00][0.00, 0.00] queued-frames=0, mRefreshPending=0, metadata={5:4bytes}, cornerRadiusCrop=[0.00, 0.00, 0.00, 0.00],  shadowRadius=0.000,
+ ContainerLayer (d569c96 com.android.systemui.ImageWallpaper#0)
  Region TransparentRegion (this=0 count=0)
  Region VisibleRegion (this=0 count=0)
  Region SurfaceDamageRegion (this=0 count=0)
      layerStack=   0, z=        0, pos=(0,0), size=(   0,   0), crop=[  0,   0,  -1,  -1], cornerRadius=0.000000, isProtected=0, isTrustedOverlay=0, isOpaque=0, invalidate=1, dataspace=Default, defaultPixelFormat=Unknown/None, backgroundBlurRadius=0, color=(0.000,0.000,0.000,1.000), flags=0x00000000, tr=[0.00, 0.00][0.00, 0.00]
      parent=WallpaperWindowToken{3e4ce52 token=android.os.Binder@46452dd}#0
      zOrderRelativeOf=none
      activeBuffer=[   0x   0:   0,Unknown/None], tr=[0.00, 0.00][0.00, 0.00] queued-frames=0, mRefreshPending=0, metadata={5:4bytes}, cornerRadiusCrop=[0.00, 0.00, 0.00, 0.00],  shadowRadius=0.000,
+ BufferQueueLayer (com.android.systemui.ImageWallpaper#0)
  Region TransparentRegion (this=0 count=0)
  Region VisibleRegion (this=0 count=0)
  Region SurfaceDamageRegion (this=0 count=0)
      layerStack=   0, z=        0, pos=(-36,-72), size=( 480, 800), crop=[  0,   0,  -1,  -1], cornerRadius=0.000000, isProtected=0, isTrustedOverlay=0, isOpaque=1, invalidate=0, dataspace=Default, defaultPixelFormat=RGBx_8888, backgroundBlurRadius=0, color=(0.000,0.000,0.000,1.000), flags=0x00000003, tr=[1.98, 0.00][0.00, 1.98]
      parent=d569c96 com.android.systemui.ImageWallpaper#0
      zOrderRelativeOf=none
      activeBuffer=[ 480x 800: 512,RGBx_8888], tr=[0.00, 0.00][0.00, 0.00] queued-frames=0, mRefreshPending=0, metadata={5:4bytes, windowType:2013, ownerUID:10131}, cornerRadiusCrop=[0.00, 0.00, 0.00, 0.00],  shadowRadius=0.000,
+ ContainerLayer (Leaf:2:2#0)
  Region TransparentRegion (this=0 count=0)
  Region VisibleRegion (this=0 count=0)
  Region SurfaceDamageRegion (this=0 count=0)
      layerStack=   0, z=        1, pos=(0,0), size=(   0,   0), crop=[  0,   0,  -1,  -1], cornerRadius=0.000000, isProtected=0, isTrustedOverlay=0, isOpaque=0, invalidate=1, dataspace=Default, defaultPixelFormat=Unknown/None, backgroundBlurRadius=0, color=(0.000,0.000,0.000,1.000), flags=0x00000000, tr=[0.00, 0.00][0.00, 0.00]
      parent=DisplayArea.Root#0
      zOrderRelativeOf=none
      activeBuffer=[   0x   0:   0,Unknown/None], tr=[0.00, 0.00][0.00, 0.00] queued-frames=0, mRefreshPending=0, metadata={5:4bytes}, cornerRadiusCrop=[0.00, 0.00, 0.00, 0.00],  shadowRadius=0.000,
+ ContainerLayer (DefaultTaskDisplayArea#0)
  Region TransparentRegion (this=0 count=0)
  Region VisibleRegion (this=0 count=0)
  Region SurfaceDamageRegion (this=0 count=0)
      layerStack=   0, z=        2, pos=(0,0), size=(   0,   0), crop=[  0,   0,  -1,  -1], cornerRadius=0.000000, isProtected=0, isTrustedOverlay=0, isOpaque=0, invalidate=1, dataspace=Default, defaultPixelFormat=Unknown/None, backgroundBlurRadius=0, color=(0.000,0.000,0.000,1.000), flags=0x00000000, tr=[0.00, 0.00][0.00, 0.00]
      parent=DisplayArea.Root#0
      zOrderRelativeOf=none
      activeBuffer=[   0x   0:   0,Unknown/None], tr=[0.00, 0.00][0.00, 0.00] queued-frames=0, mRefreshPending=0, metadata={5:4bytes}, cornerRadiusCrop=[0.00, 0.00, 0.00, 0.00],  shadowRadius=0.000,
+ EffectLayer (Task=1#0)
  Region TransparentRegion (this=0 count=0)
  Region VisibleRegion (this=0 count=0)
  Region SurfaceDamageRegion (this=0 count=0)
      layerStack=   0, z=        0, pos=(0,0), size=(   0,   0), crop=[  0,   0, 720, 1440], cornerRadius=0.000000, isProtected=0, isTrustedOverlay=0, isOpaque=1, invalidate=0, dataspace=Default, defaultPixelFormat=Unknown/None, backgroundBlurRadius=0, color=(-1.000,-1.000,-1.000,1.000), flags=0x00000000, tr=[0.00, 0.00][0.00, 0.00]
      parent=DefaultTaskDisplayArea#0
      zOrderRelativeOf=none
      activeBuffer=[   0x   0:   0,Unknown/None], tr=[0.00, 0.00][0.00, 0.00] queued-frames=0, mRefreshPending=0, metadata={5:4bytes, taskId:1}, cornerRadiusCrop=[0.00, 0.00, 0.00, 0.00],  shadowRadius=0.000,
+ EffectLayer (Task=4393#0)
  Region TransparentRegion (this=0 count=0)
  Region VisibleRegion (this=0 count=0)
  Region SurfaceDamageRegion (this=0 count=0)
      layerStack=   0, z=        0, pos=(0,0), size=(   0,   0), crop=[  0,   0,  -1,  -1], cornerRadius=0.000000, isProtected=0, isTrustedOverlay=0, isOpaque=1, invalidate=0, dataspace=Default, defaultPixelFormat=Unknown/None, backgroundBlurRadius=0, color=(-1.000,-1.000,-1.000,1.000), flags=0x00000000, tr=[0.00, 0.00][0.00, 0.00]
      parent=Task=1#0
      zOrderRelativeOf=none
      activeBuffer=[   0x   0:   0,Unknown/None], tr=[0.00, 0.00][0.00, 0.00] queued-frames=0, mRefreshPending=0, metadata={5:4bytes, taskId:4393}, cornerRadiusCrop=[0.00, 0.00, 0.00, 0.00],  shadowRadius=0.000,
+ ContainerLayer (d3c6366 ActivityRecordInputSink com.android.launcher3/.uioverrides.QuickstepLauncher#0)
  Region TransparentRegion (this=0 count=0)
  Region VisibleRegion (this=0 count=0)
  Region SurfaceDamageRegion (this=0 count=0)
      layerStack=   0, z=-2147483648, pos=(0,0), size=(   0,   0), crop=[  0,   0,  -1,  -1], cornerRadius=0.000000, isProtected=0, isTrustedOverlay=0, isOpaque=0, invalidate=1, dataspace=Default, defaultPixelFormat=Unknown/None, backgroundBlurRadius=0, color=(0.000,0.000,0.000,1.000), flags=0x00000000, tr=[0.00, 0.00][0.00, 0.00]
      parent=ActivityRecord{fe396a7 u0 com.android.launcher3/.uioverrides.QuickstepLauncher#0
      zOrderRelativeOf=none
      activeBuffer=[   0x   0:   0,Unknown/None], tr=[0.00, 0.00][0.00, 0.00] queued-frames=0, mRefreshPending=0, metadata={5:4bytes}, cornerRadiusCrop=[0.00, 0.00, 0.00, 0.00],  shadowRadius=0.000,
+ ContainerLayer (ActivityRecord{fe396a7 u0 com.android.launcher3/.uioverrides.QuickstepLauncher#0)
  Region TransparentRegion (this=0 count=0)
  Region VisibleRegion (this=0 count=0)
  Region SurfaceDamageRegion (this=0 count=0)
      layerStack=   0, z=        0, pos=(0,0), size=(   0,   0), crop=[  0,   0,  -1,  -1], cornerRadius=0.000000, isProtected=0, isTrustedOverlay=0, isOpaque=0, invalidate=1, dataspace=Default, defaultPixelFormat=Unknown/None, backgroundBlurRadius=0, color=(0.000,0.000,0.000,1.000), flags=0x00000001, tr=[0.00, 0.00][0.00, 0.00]
      parent=Task=4393#0
      zOrderRelativeOf=none
      activeBuffer=[   0x   0:   0,Unknown/None], tr=[0.00, 0.00][0.00, 0.00] queued-frames=0, mRefreshPending=0, metadata={5:4bytes}, cornerRadiusCrop=[0.00, 0.00, 0.00, 0.00],  shadowRadius=0.000,
+ ContainerLayer (62f7f2a com.android.launcher3/com.android.launcher3.uioverrides.QuickstepLauncher#0)
  Region TransparentRegion (this=0 count=0)
  Region VisibleRegion (this=0 count=0)
  Region SurfaceDamageRegion (this=0 count=0)
      layerStack=   0, z=        0, pos=(0,0), size=(   0,   0), crop=[  0,   0,  -1,  -1], cornerRadius=0.000000, isProtected=0, isTrustedOverlay=0, isOpaque=0, invalidate=1, dataspace=Default, defaultPixelFormat=Unknown/None, backgroundBlurRadius=0, color=(0.000,0.000,0.000,1.000), flags=0x00000000, tr=[0.00, 0.00][0.00, 0.00]
      parent=ActivityRecord{fe396a7 u0 com.android.launcher3/.uioverrides.QuickstepLauncher#0
      zOrderRelativeOf=none
      activeBuffer=[   0x   0:   0,Unknown/None], tr=[0.00, 0.00][0.00, 0.00] queued-frames=0, mRefreshPending=0, metadata={5:4bytes}, cornerRadiusCrop=[0.00, 0.00, 0.00, 0.00],  shadowRadius=0.000,
+ ContainerLayer (homeAnimationLayer#0)
  Region TransparentRegion (this=0 count=0)
  Region VisibleRegion (this=0 count=0)
  Region SurfaceDamageRegion (this=0 count=0)
      layerStack=   0, z=        1, pos=(0,0), size=(   0,   0), crop=[  0,   0,  -1,  -1], cornerRadius=0.000000, isProtected=0, isTrustedOverlay=0, isOpaque=0, invalidate=1, dataspace=Default, defaultPixelFormat=Unknown/None, backgroundBlurRadius=0, color=(0.000,0.000,0.000,1.000), flags=0x00000000, tr=[0.00, 0.00][0.00, 0.00]
      parent=DefaultTaskDisplayArea#0
      zOrderRelativeOf=none
      activeBuffer=[   0x   0:   0,Unknown/None], tr=[0.00, 0.00][0.00, 0.00] queued-frames=0, mRefreshPending=0, metadata={5:4bytes}, cornerRadiusCrop=[0.00, 0.00, 0.00, 0.00],  shadowRadius=0.000,
+ ContainerLayer (animationLayer#0)
  Region TransparentRegion (this=0 count=0)
  Region VisibleRegion (this=0 count=0)
  Region SurfaceDamageRegion (this=0 count=0)
      layerStack=   0, z=        3, pos=(0,0), size=(   0,   0), crop=[  0,   0,  -1,  -1], cornerRadius=0.000000, isProtected=0, isTrustedOverlay=0, isOpaque=0, invalidate=1, dataspace=Default, defaultPixelFormat=Unknown/None, backgroundBlurRadius=0, color=(0.000,0.000,0.000,1.000), flags=0x00000000, tr=[0.00, 0.00][0.00, 0.00]
      parent=DefaultTaskDisplayArea#0
      zOrderRelativeOf=none
      activeBuffer=[   0x   0:   0,Unknown/None], tr=[0.00, 0.00][0.00, 0.00] queued-frames=0, mRefreshPending=0, metadata={5:4bytes}, cornerRadiusCrop=[0.00, 0.00, 0.00, 0.00],  shadowRadius=0.000,
+ EffectLayer (Task=4#0)
  Region TransparentRegion (this=0 count=0)
  Region VisibleRegion (this=0 count=0)
  Region SurfaceDamageRegion (this=0 count=0)
      layerStack=   0, z=        4, pos=(0,0), size=(   0,   0), crop=[  0,   0, 720, 1440], cornerRadius=0.000000, isProtected=0, isTrustedOverlay=0, isOpaque=1, invalidate=0, dataspace=Default, defaultPixelFormat=Unknown/None, backgroundBlurRadius=0, color=(-1.000,-1.000,-1.000,1.000), flags=0x00000000, tr=[0.00, 0.00][0.00, 0.00]
      parent=DefaultTaskDisplayArea#0
      zOrderRelativeOf=none
      activeBuffer=[   0x   0:   0,Unknown/None], tr=[0.00, 0.00][0.00, 0.00] queued-frames=0, mRefreshPending=0, metadata={5:4bytes, taskId:4}, cornerRadiusCrop=[0.00, 0.00, 0.00, 0.00],  shadowRadius=0.000,
+ EffectLayer (Secondary Divider Dim#0)
  Region TransparentRegion (this=0 count=0)
  Region VisibleRegion (this=0 count=0)
  Region SurfaceDamageRegion (this=0 count=0)
      layerStack=   0, z=2147483647, pos=(0,0), size=(   0,   0), crop=[  0,   0,  -1,  -1], cornerRadius=0.000000, isProtected=0, isTrustedOverlay=0, isOpaque=1, invalidate=0, dataspace=Default, defaultPixelFormat=Unknown/None, backgroundBlurRadius=0, color=(0.000,0.000,0.000,1.000), flags=0x00000001, tr=[0.00, 0.00][0.00, 0.00]
      parent=Task=4#0
      zOrderRelativeOf=none
      activeBuffer=[   0x   0:   0,Unknown/None], tr=[0.00, 0.00][0.00, 0.00] queued-frames=0, mRefreshPending=0, metadata={5:4bytes}, cornerRadiusCrop=[0.00, 0.00, 0.00, 0.00],  shadowRadius=0.000,
+ EffectLayer (Task=3#0)
  Region TransparentRegion (this=0 count=0)
  Region VisibleRegion (this=0 count=0)
  Region SurfaceDamageRegion (this=0 count=0)
      layerStack=   0, z=        6, pos=(0,0), size=(   0,   0), crop=[  0,   0, 720, 1440], cornerRadius=0.000000, isProtected=0, isTrustedOverlay=0, isOpaque=1, invalidate=0, dataspace=Default, defaultPixelFormat=Unknown/None, backgroundBlurRadius=0, color=(-1.000,-1.000,-1.000,1.000), flags=0x00000000, tr=[0.00, 0.00][0.00, 0.00]
      parent=DefaultTaskDisplayArea#0
      zOrderRelativeOf=none
      activeBuffer=[   0x   0:   0,Unknown/None], tr=[0.00, 0.00][0.00, 0.00] queued-frames=0, mRefreshPending=0, metadata={5:4bytes, taskId:3}, cornerRadiusCrop=[0.00, 0.00, 0.00, 0.00],  shadowRadius=0.000,
+ EffectLayer (Primary Divider Dim#0)
  Region TransparentRegion (this=0 count=0)
  Region VisibleRegion (this=0 count=0)
  Region SurfaceDamageRegion (this=0 count=0)
      layerStack=   0, z=2147483647, pos=(0,0), size=(   0,   0), crop=[  0,   0,  -1,  -1], cornerRadius=0.000000, isProtected=0, isTrustedOverlay=0, isOpaque=1, invalidate=0, dataspace=Default, defaultPixelFormat=Unknown/None, backgroundBlurRadius=0, color=(0.000,0.000,0.000,1.000), flags=0x00000001, tr=[0.00, 0.00][0.00, 0.00]
      parent=Task=3#0
      zOrderRelativeOf=none
      activeBuffer=[   0x   0:   0,Unknown/None], tr=[0.00, 0.00][0.00, 0.00] queued-frames=0, mRefreshPending=0, metadata={5:4bytes}, cornerRadiusCrop=[0.00, 0.00, 0.00, 0.00],  shadowRadius=0.000,
+ ContainerLayer (splitScreenDividerAnchor#0)
  Region TransparentRegion (this=0 count=0)
  Region VisibleRegion (this=0 count=0)
  Region SurfaceDamageRegion (this=0 count=0)
      layerStack=   0, z=        7, pos=(0,0), size=(   0,   0), crop=[  0,   0,  -1,  -1], cornerRadius=0.000000, isProtected=0, isTrustedOverlay=0, isOpaque=0, invalidate=1, dataspace=Default, defaultPixelFormat=Unknown/None, backgroundBlurRadius=0, color=(0.000,0.000,0.000,1.000), flags=0x00000000, tr=[0.00, 0.00][0.00, 0.00]
      parent=DefaultTaskDisplayArea#0
      zOrderRelativeOf=none
      activeBuffer=[   0x   0:   0,Unknown/None], tr=[0.00, 0.00][0.00, 0.00] queued-frames=0, mRefreshPending=0, metadata={5:4bytes}, cornerRadiusCrop=[0.00, 0.00, 0.00, 0.00],  shadowRadius=0.000,
+ EffectLayer (Task=4624#0)
  Region TransparentRegion (this=0 count=0)
  Region VisibleRegion (this=0 count=0)
  Region SurfaceDamageRegion (this=0 count=0)
      layerStack=   0, z=        8, pos=(0,0), size=(   0,   0), crop=[  0,   0, 720, 1440], cornerRadius=0.000000, isProtected=0, isTrustedOverlay=0, isOpaque=1, invalidate=0, dataspace=Default, defaultPixelFormat=Unknown/None, backgroundBlurRadius=0, color=(-1.000,-1.000,-1.000,1.000), flags=0x00000000, tr=[0.00, 0.00][0.00, 0.00]
      parent=DefaultTaskDisplayArea#0
      zOrderRelativeOf=none
      activeBuffer=[   0x   0:   0,Unknown/None], tr=[0.00, 0.00][0.00, 0.00] queued-frames=0, mRefreshPending=0, metadata={5:4bytes, taskId:4624}, cornerRadiusCrop=[0.00, 0.00, 0.00, 0.00],  shadowRadius=0.000,
+ ContainerLayer (e231e21 ActivityRecordInputSink com.aliyun.security.sase/.MainActivity#0)
  Region TransparentRegion (this=0 count=0)
  Region VisibleRegion (this=0 count=0)
  Region SurfaceDamageRegion (this=0 count=0)
      layerStack=   0, z=-2147483648, pos=(0,0), size=(   0,   0), crop=[  0,   0,  -1,  -1], cornerRadius=0.000000, isProtected=0, isTrustedOverlay=0, isOpaque=0, invalidate=1, dataspace=Default, defaultPixelFormat=Unknown/None, backgroundBlurRadius=0, color=(0.000,0.000,0.000,1.000), flags=0x00000000, tr=[0.00, 0.00][0.00, 0.00]
      parent=ActivityRecord{6a81246 u0 com.aliyun.security.sase/.MainActivity#0
      zOrderRelativeOf=none
      activeBuffer=[   0x   0:   0,Unknown/None], tr=[0.00, 0.00][0.00, 0.00] queued-frames=0, mRefreshPending=0, metadata={5:4bytes}, cornerRadiusCrop=[0.00, 0.00, 0.00, 0.00],  shadowRadius=0.000,
+ ContainerLayer (ActivityRecord{6a81246 u0 com.aliyun.security.sase/.MainActivity#0)
  Region TransparentRegion (this=0 count=0)
  Region VisibleRegion (this=0 count=0)
  Region SurfaceDamageRegion (this=0 count=0)
      layerStack=   0, z=        0, pos=(0,0), size=(   0,   0), crop=[  0,   0,  -1,  -1], cornerRadius=0.000000, isProtected=0, isTrustedOverlay=0, isOpaque=0, invalidate=1, dataspace=Default, defaultPixelFormat=Unknown/None, backgroundBlurRadius=0, color=(0.000,0.000,0.000,1.000), flags=0x00000001, tr=[0.00, 0.00][0.00, 0.00]
      parent=Task=4624#0
      zOrderRelativeOf=none
      activeBuffer=[   0x   0:   0,Unknown/None], tr=[0.00, 0.00][0.00, 0.00] queued-frames=0, mRefreshPending=0, metadata={5:4bytes}, cornerRadiusCrop=[0.00, 0.00, 0.00, 0.00],  shadowRadius=0.000,
+ ContainerLayer (f6265f5 com.aliyun.security.sase/com.aliyun.security.sase.MainActivity#0)
  Region TransparentRegion (this=0 count=0)
  Region VisibleRegion (this=0 count=0)
  Region SurfaceDamageRegion (this=0 count=0)
      layerStack=   0, z=        0, pos=(0,0), size=(   0,   0), crop=[  0,   0,  -1,  -1], cornerRadius=0.000000, isProtected=0, isTrustedOverlay=0, isOpaque=0, invalidate=1, dataspace=Default, defaultPixelFormat=Unknown/None, backgroundBlurRadius=0, color=(0.000,0.000,0.000,1.000), flags=0x00000000, tr=[0.00, 0.00][0.00, 0.00]
      parent=ActivityRecord{6a81246 u0 com.aliyun.security.sase/.MainActivity#0
      zOrderRelativeOf=none
      activeBuffer=[   0x   0:   0,Unknown/None], tr=[0.00, 0.00][0.00, 0.00] queued-frames=0, mRefreshPending=0, metadata={5:4bytes}, cornerRadiusCrop=[0.00, 0.00, 0.00, 0.00],  shadowRadius=0.000, 
+ EffectLayer (Task=4645#0)
  Region TransparentRegion (this=0 count=0)
  Region VisibleRegion (this=0 count=0)
  Region SurfaceDamageRegion (this=0 count=0)
      layerStack=   0, z=        9, pos=(0,0), size=(   0,   0), crop=[  0,   0, 720, 1440], cornerRadius=0.000000, isProtected=0, isTrustedOverlay=0, isOpaque=1, invalidate=0, dataspace=Default, defaultPixelFormat=Unknown/None, backgroundBlurRadius=0, color=(-1.000,-1.000,-1.000,1.000), flags=0x00000000, tr=[0.00, 0.00][0.00, 0.00]
      parent=DefaultTaskDisplayArea#0
      zOrderRelativeOf=none
      activeBuffer=[   0x   0:   0,Unknown/None], tr=[0.00, 0.00][0.00, 0.00] queued-frames=0, mRefreshPending=0, metadata={5:4bytes, taskId:4645}, cornerRadiusCrop=[0.00, 0.00, 0.00, 0.00],  shadowRadius=0.000,
+ ContainerLayer (7d8ecab ActivityRecordInputSink com.yto.customermanmagererp/.ui.activity.HomeActivity#0)
  Region TransparentRegion (this=0 count=0)
  Region VisibleRegion (this=0 count=0)
  Region SurfaceDamageRegion (this=0 count=0)
      layerStack=   0, z=-2147483648, pos=(0,0), size=(   0,   0), crop=[  0,   0,  -1,  -1], cornerRadius=0.000000, isProtected=0, isTrustedOverlay=0, isOpaque=0, invalidate=1, dataspace=Default, defaultPixelFormat=Unknown/None, backgroundBlurRadius=0, color=(0.000,0.000,0.000,1.000), flags=0x00000000, tr=[0.00, 0.00][0.00, 0.00]
      parent=ActivityRecord{6bb0f08 u0 com.yto.customermanmagererp/.ui.activity.HomeActivity#0
      zOrderRelativeOf=none
      activeBuffer=[   0x   0:   0,Unknown/None], tr=[0.00, 0.00][0.00, 0.00] queued-frames=0, mRefreshPending=0, metadata={5:4bytes}, cornerRadiusCrop=[0.00, 0.00, 0.00, 0.00],  shadowRadius=0.000,
+ ContainerLayer (ActivityRecord{6bb0f08 u0 com.yto.customermanmagererp/.ui.activity.HomeActivity#0)
  Region TransparentRegion (this=0 count=0)
  Region VisibleRegion (this=0 count=0)
  Region SurfaceDamageRegion (this=0 count=0)
      layerStack=   0, z=        0, pos=(0,0), size=(   0,   0), crop=[  0,   0,  -1,  -1], cornerRadius=0.000000, isProtected=0, isTrustedOverlay=0, isOpaque=0, invalidate=1, dataspace=Default, defaultPixelFormat=Unknown/None, backgroundBlurRadius=0, color=(0.000,0.000,0.000,1.000), flags=0x00000000, tr=[0.00, 0.00][0.00, 0.00]
      parent=Task=4645#0
      zOrderRelativeOf=none
      activeBuffer=[   0x   0:   0,Unknown/None], tr=[0.00, 0.00][0.00, 0.00] queued-frames=0, mRefreshPending=0, metadata={5:4bytes}, cornerRadiusCrop=[0.00, 0.00, 0.00, 0.00],  shadowRadius=0.000,
+ ContainerLayer (e043776 com.yto.customermanmagererp/com.yto.customermanmagererp.ui.activity.HomeActivity#0)
  Region TransparentRegion (this=0 count=0)
  Region VisibleRegion (this=0 count=0)
  Region SurfaceDamageRegion (this=0 count=0)
      layerStack=   0, z=        0, pos=(0,0), size=(   0,   0), crop=[  0,   0,  -1,  -1], cornerRadius=0.000000, isProtected=0, isTrustedOverlay=0, isOpaque=0, invalidate=1, dataspace=Default, defaultPixelFormat=Unknown/None, backgroundBlurRadius=0, color=(0.000,0.000,0.000,1.000), flags=0x00000000, tr=[0.00, 0.00][0.00, 0.00]
      parent=ActivityRecord{6bb0f08 u0 com.yto.customermanmagererp/.ui.activity.HomeActivity#0
      zOrderRelativeOf=none
      activeBuffer=[   0x   0:   0,Unknown/None], tr=[0.00, 0.00][0.00, 0.00] queued-frames=0, mRefreshPending=0, metadata={5:4bytes}, cornerRadiusCrop=[0.00, 0.00, 0.00, 0.00],  shadowRadius=0.000,
+ BufferQueueLayer (com.yto.customermanmagererp/com.yto.customermanmagererp.ui.activity.HomeActivity#0)
  Region TransparentRegion (this=0 count=0)
  Region VisibleRegion (this=0 count=1)
    [  0,   0, 720, 1440]
  Region SurfaceDamageRegion (this=0 count=0)
      layerStack=   0, z=        0, pos=(0,0), size=( 720,1440), crop=[  0,   0, 720, 1440], cornerRadius=0.000000, isProtected=0, isTrustedOverlay=0, isOpaque=1, invalidate=0, dataspace=Default, defaultPixelFormat=RGBA_8888, backgroundBlurRadius=0, color=(0.000,0.000,0.000,1.000), flags=0x00000002, tr=[0.00, 0.00][0.00, 0.00]
      parent=e043776 com.yto.customermanmagererp/com.yto.customermanmagererp.ui.activity.HomeActivity#0
      zOrderRelativeOf=none
      activeBuffer=[ 720x1440: 768,RGBA_8888], tr=[0.00, 0.00][0.00, 0.00] queued-frames=0, mRefreshPending=0, metadata={5:4bytes, windowType:1, ownerUID:10291}, cornerRadiusCrop=[0.00, 0.00, 0.00, 0.00],  shadowRadius=0.000,
+ ContainerLayer (ImeContainer#0)
  Region TransparentRegion (this=0 count=0)
  Region VisibleRegion (this=0 count=0)
  Region SurfaceDamageRegion (this=0 count=0)
      layerStack=   0, z=        1, pos=(0,0), size=(   0,   0), crop=[  0,   0,  -1,  -1], cornerRadius=0.000000, isProtected=0, isTrustedOverlay=0, isOpaque=0, invalidate=1, dataspace=Default, defaultPixelFormat=Unknown/None, backgroundBlurRadius=0, color=(0.000,0.000,0.000,1.000), flags=0x00000000, tr=[0.00, 0.00][0.00, 0.00]
      parent=ActivityRecord{6bb0f08 u0 com.yto.customermanmagererp/.ui.activity.HomeActivity#0
      zOrderRelativeOf=e043776 com.yto.customermanmagererp/com.yto.customermanmagererp.ui.activity.HomeActivity#0
      activeBuffer=[   0x   0:   0,Unknown/None], tr=[0.00, 0.00][0.00, 0.00] queued-frames=0, mRefreshPending=0, metadata={5:4bytes}, cornerRadiusCrop=[0.00, 0.00, 0.00, 0.00],  shadowRadius=0.000,
+ ContainerLayer (WindowToken{325dea8 android.os.Binder@534b6cb}#0)
  Region TransparentRegion (this=0 count=0)
  Region VisibleRegion (this=0 count=0)
  Region SurfaceDamageRegion (this=0 count=0)
      layerStack=   0, z=        0, pos=(0,0), size=(   0,   0), crop=[  0,   0,  -1,  -1], cornerRadius=0.000000, isProtected=0, isTrustedOverlay=0, isOpaque=0, invalidate=1, dataspace=Default, defaultPixelFormat=Unknown/None, backgroundBlurRadius=0, color=(0.000,0.000,0.000,1.000), flags=0x00000000, tr=[0.00, 0.00][0.00, 0.00]
      parent=ImeContainer#0
      zOrderRelativeOf=none
      activeBuffer=[   0x   0:   0,Unknown/None], tr=[0.00, 0.00][0.00, 0.00] queued-frames=0, mRefreshPending=0, metadata={5:4bytes}, cornerRadiusCrop=[0.00, 0.00, 0.00, 0.00],  shadowRadius=0.000,
+ EffectLayer (Surface(name=f7e7973 InputMethod)/@0x9c77a2e - animation-leash#0)
  Region TransparentRegion (this=0 count=0)
  Region VisibleRegion (this=0 count=0)
  Region SurfaceDamageRegion (this=0 count=0)
      layerStack=   0, z=        0, pos=(0,48), size=(   0,   0), crop=[  0,   0,   0,   0], cornerRadius=0.000000, isProtected=0, isTrustedOverlay=0, isOpaque=1, invalidate=0, dataspace=Default, defaultPixelFormat=Unknown/None, backgroundBlurRadius=0, color=(-1.000,-1.000,-1.000,1.000), flags=0x00000001, tr=[0.00, 0.00][0.00, 0.00]
      parent=WindowToken{325dea8 android.os.Binder@534b6cb}#0
      zOrderRelativeOf=none
      activeBuffer=[   0x   0:   0,Unknown/None], tr=[0.00, 0.00][0.00, 0.00] queued-frames=0, mRefreshPending=0, metadata={5:4bytes}, cornerRadiusCrop=[0.00, 0.00, 0.00, 0.00],  shadowRadius=0.000,
+ ContainerLayer (f7e7973 InputMethod#0)
  Region TransparentRegion (this=0 count=0)
  Region VisibleRegion (this=0 count=0)
  Region SurfaceDamageRegion (this=0 count=0)
      layerStack=   0, z=        0, pos=(0,48), size=(   0,   0), crop=[  0,   0,  -1,  -1], cornerRadius=0.000000, isProtected=0, isTrustedOverlay=0, isOpaque=0, invalidate=1, dataspace=Default, defaultPixelFormat=Unknown/None, backgroundBlurRadius=0, color=(0.000,0.000,0.000,1.000), flags=0x00000000, tr=[0.00, 0.00][0.00, 0.00]
      parent=Surface(name=f7e7973 InputMethod)/@0x9c77a2e - animation-leash#0
      zOrderRelativeOf=none
      activeBuffer=[   0x   0:   0,Unknown/None], tr=[0.00, 0.00][0.00, 0.00] queued-frames=0, mRefreshPending=0, metadata={5:4bytes}, cornerRadiusCrop=[0.00, 0.00, 0.00, 0.00],  shadowRadius=0.000,
+ ContainerLayer (boostedAnimationLayer#0)
  Region TransparentRegion (this=0 count=0)
  Region VisibleRegion (this=0 count=0)
  Region SurfaceDamageRegion (this=0 count=0)
      layerStack=   0, z=       10, pos=(0,0), size=(   0,   0), crop=[  0,   0,  -1,  -1], cornerRadius=0.000000, isProtected=0, isTrustedOverlay=0, isOpaque=0, invalidate=1, dataspace=Default, defaultPixelFormat=Unknown/None, backgroundBlurRadius=0, color=(0.000,0.000,0.000,1.000), flags=0x00000000, tr=[0.00, 0.00][0.00, 0.00]
      parent=DefaultTaskDisplayArea#0
      zOrderRelativeOf=none
      activeBuffer=[   0x   0:   0,Unknown/None], tr=[0.00, 0.00][0.00, 0.00] queued-frames=0, mRefreshPending=0, metadata={5:4bytes}, cornerRadiusCrop=[0.00, 0.00, 0.00, 0.00],  shadowRadius=0.000,
+ ContainerLayer (Leaf:3:14#0)
  Region TransparentRegion (this=0 count=0)
  Region VisibleRegion (this=0 count=0)
  Region SurfaceDamageRegion (this=0 count=0)
      layerStack=   0, z=        3, pos=(0,0), size=(   0,   0), crop=[  0,   0,  -1,  -1], cornerRadius=0.000000, isProtected=0, isTrustedOverlay=0, isOpaque=0, invalidate=1, dataspace=Default, defaultPixelFormat=Unknown/None, backgroundBlurRadius=0, color=(0.000,0.000,0.000,1.000), flags=0x00000000, tr=[0.00, 0.00][0.00, 0.00]
      parent=DisplayArea.Root#0
      zOrderRelativeOf=none
      activeBuffer=[   0x   0:   0,Unknown/None], tr=[0.00, 0.00][0.00, 0.00] queued-frames=0, mRefreshPending=0, metadata={5:4bytes}, cornerRadiusCrop=[0.00, 0.00, 0.00, 0.00],  shadowRadius=0.000,
+ ContainerLayer (WindowToken{c7b56c7 android.os.BinderProxy@a33fae1}#0)
  Region TransparentRegion (this=0 count=0)
  Region VisibleRegion (this=0 count=0)
  Region SurfaceDamageRegion (this=0 count=0)
      layerStack=   0, z=        0, pos=(0,0), size=(   0,   0), crop=[  0,   0,  -1,  -1], cornerRadius=0.000000, isProtected=0, isTrustedOverlay=0, isOpaque=0, invalidate=1, dataspace=Default, defaultPixelFormat=Unknown/None, backgroundBlurRadius=0, color=(0.000,0.000,0.000,1.000), flags=0x00000000, tr=[0.00, 0.00][0.00, 0.00]
      parent=Leaf:3:14#0
      zOrderRelativeOf=none
      activeBuffer=[   0x   0:   0,Unknown/None], tr=[0.00, 0.00][0.00, 0.00] queued-frames=0, mRefreshPending=0, metadata={5:4bytes}, cornerRadiusCrop=[0.00, 0.00, 0.00, 0.00],  shadowRadius=0.000,
+ ContainerLayer (9dec4f4 AssistPreviewPanel#0)
  Region TransparentRegion (this=0 count=0)
  Region VisibleRegion (this=0 count=0)
  Region SurfaceDamageRegion (this=0 count=0)
      layerStack=   0, z=        0, pos=(0,1440), size=(   0,   0), crop=[  0,   0,  -1,  -1], cornerRadius=0.000000, isProtected=0, isTrustedOverlay=0, isOpaque=0, invalidate=1, dataspace=Default, defaultPixelFormat=Unknown/None, backgroundBlurRadius=0, color=(0.000,0.000,0.000,1.000), flags=0x00000000, tr=[0.00, 0.00][0.00, 0.00]
      parent=WindowToken{c7b56c7 android.os.BinderProxy@a33fae1}#0
      zOrderRelativeOf=none
      activeBuffer=[   0x   0:   0,Unknown/None], tr=[0.00, 0.00][0.00, 0.00] queued-frames=0, mRefreshPending=0, metadata={5:4bytes}, cornerRadiusCrop=[0.00, 0.00, 0.00, 0.00],  shadowRadius=0.000,
+ ContainerLayer (Leaf:17:34#0)
  Region TransparentRegion (this=0 count=0)
  Region VisibleRegion (this=0 count=0)
  Region SurfaceDamageRegion (this=0 count=0)
      layerStack=   0, z=        5, pos=(0,0), size=(   0,   0), crop=[  0,   0,  -1,  -1], cornerRadius=0.000000, isProtected=0, isTrustedOverlay=0, isOpaque=0, invalidate=1, dataspace=Default, defaultPixelFormat=Unknown/None, backgroundBlurRadius=0, color=(0.000,0.000,0.000,1.000), flags=0x00000000, tr=[0.00, 0.00][0.00, 0.00]
      parent=DisplayArea.Root#0
      zOrderRelativeOf=none
      activeBuffer=[   0x   0:   0,Unknown/None], tr=[0.00, 0.00][0.00, 0.00] queued-frames=0, mRefreshPending=0, metadata={5:4bytes}, cornerRadiusCrop=[0.00, 0.00, 0.00, 0.00],  shadowRadius=0.000,
+ ContainerLayer (WindowToken{eb753d3 android.os.BinderProxy@a65760d}#0)
  Region TransparentRegion (this=0 count=0)
  Region VisibleRegion (this=0 count=0)
  Region SurfaceDamageRegion (this=0 count=0)
      layerStack=   0, z=        0, pos=(0,0), size=(   0,   0), crop=[  0,   0,  -1,  -1], cornerRadius=0.000000, isProtected=0, isTrustedOverlay=0, isOpaque=0, invalidate=1, dataspace=Default, defaultPixelFormat=Unknown/None, backgroundBlurRadius=0, color=(0.000,0.000,0.000,1.000), flags=0x00000000, tr=[0.00, 0.00][0.00, 0.00]
      parent=Leaf:17:34#0
      zOrderRelativeOf=none
      activeBuffer=[   0x   0:   0,Unknown/None], tr=[0.00, 0.00][0.00, 0.00] queued-frames=0, mRefreshPending=0, metadata={5:4bytes}, cornerRadiusCrop=[0.00, 0.00, 0.00, 0.00],  shadowRadius=0.000,
+ EffectLayer (Surface(name=7cb2b10 StatusBar)/@0x89fb8f5 - animation-leash#0)
  Region TransparentRegion (this=0 count=0)
  Region VisibleRegion (this=0 count=0)
  Region SurfaceDamageRegion (this=0 count=0)
      layerStack=   0, z=        0, pos=(0,0), size=(   0,   0), crop=[  0,   0,   0,   0], cornerRadius=0.000000, isProtected=0, isTrustedOverlay=0, isOpaque=1, invalidate=0, dataspace=Default, defaultPixelFormat=Unknown/None, backgroundBlurRadius=0, color=(-1.000,-1.000,-1.000,1.000), flags=0x00000000, tr=[0.00, 0.00][0.00, 0.00]
      parent=WindowToken{eb753d3 android.os.BinderProxy@a65760d}#0
      zOrderRelativeOf=none
      activeBuffer=[   0x   0:   0,Unknown/None], tr=[0.00, 0.00][0.00, 0.00] queued-frames=0, mRefreshPending=0, metadata={5:4bytes}, cornerRadiusCrop=[0.00, 0.00, 0.00, 0.00],  shadowRadius=0.000,
+ ContainerLayer (7cb2b10 StatusBar#0)
  Region TransparentRegion (this=0 count=0)
  Region VisibleRegion (this=0 count=0)
  Region SurfaceDamageRegion (this=0 count=0)
      layerStack=   0, z=        0, pos=(0,0), size=(   0,   0), crop=[  0,   0,  -1,  -1], cornerRadius=0.000000, isProtected=0, isTrustedOverlay=0, isOpaque=0, invalidate=1, dataspace=Default, defaultPixelFormat=Unknown/None, backgroundBlurRadius=0, color=(0.000,0.000,0.000,1.000), flags=0x00000000, tr=[0.00, 0.00][0.00, 0.00]
      parent=Surface(name=7cb2b10 StatusBar)/@0x89fb8f5 - animation-leash#0
      zOrderRelativeOf=none
      activeBuffer=[   0x   0:   0,Unknown/None], tr=[0.00, 0.00][0.00, 0.00] queued-frames=0, mRefreshPending=0, metadata={5:4bytes}, cornerRadiusCrop=[0.00, 0.00, 0.00, 0.00],  shadowRadius=0.000,
+ BufferQueueLayer (StatusBar#0)
  Region TransparentRegion (this=0 count=0)
  Region VisibleRegion (this=0 count=1)
    [  0,   0, 720,  48]
  Region SurfaceDamageRegion (this=0 count=0)
      layerStack=   0, z=        0, pos=(0,0), size=( 720,  48), crop=[  0,   0, 720,  48], cornerRadius=0.000000, isProtected=0, isTrustedOverlay=0, isOpaque=0, invalidate=0, dataspace=Default, defaultPixelFormat=RGBA_8888, backgroundBlurRadius=0, color=(0.000,0.000,0.000,1.000), flags=0x00000000, tr=[0.00, 0.00][0.00, 0.00]
      parent=7cb2b10 StatusBar#0
      zOrderRelativeOf=none
      activeBuffer=[ 720x  48: 768,RGBA_8888], tr=[0.00, 0.00][0.00, 0.00] queued-frames=0, mRefreshPending=0, metadata={5:4bytes, windowType:2000, ownerUID:10131}, cornerRadiusCrop=[0.00, 0.00, 0.00, 0.00],  shadowRadius=0.000,
+ ContainerLayer (WindowToken{3f7155 android.os.BinderProxy@acb233f}#0)
  Region TransparentRegion (this=0 count=0)
  Region VisibleRegion (this=0 count=0)
  Region SurfaceDamageRegion (this=0 count=0)
      layerStack=   0, z=        1, pos=(0,0), size=(   0,   0), crop=[  0,   0,  -1,  -1], cornerRadius=0.000000, isProtected=0, isTrustedOverlay=0, isOpaque=0, invalidate=1, dataspace=Default, defaultPixelFormat=Unknown/None, backgroundBlurRadius=0, color=(0.000,0.000,0.000,1.000), flags=0x00000000, tr=[0.00, 0.00][0.00, 0.00]
      parent=Leaf:17:34#0
      zOrderRelativeOf=none
      activeBuffer=[   0x   0:   0,Unknown/None], tr=[0.00, 0.00][0.00, 0.00] queued-frames=0, mRefreshPending=0, metadata={5:4bytes}, cornerRadiusCrop=[0.00, 0.00, 0.00, 0.00],  shadowRadius=0.000,
+ ContainerLayer (c49576a NotificationShade#0)
  Region TransparentRegion (this=0 count=0)
  Region VisibleRegion (this=0 count=0)
  Region SurfaceDamageRegion (this=0 count=0)
      layerStack=   0, z=        0, pos=(0,0), size=(   0,   0), crop=[  0,   0,  -1,  -1], cornerRadius=0.000000, isProtected=0, isTrustedOverlay=0, isOpaque=0, invalidate=1, dataspace=Default, defaultPixelFormat=Unknown/None, backgroundBlurRadius=0, color=(0.000,0.000,0.000,1.000), flags=0x00000000, tr=[0.00, 0.00][0.00, 0.00]
      parent=WindowToken{3f7155 android.os.BinderProxy@acb233f}#0
      zOrderRelativeOf=none
      activeBuffer=[   0x   0:   0,Unknown/None], tr=[0.00, 0.00][0.00, 0.00] queued-frames=0, mRefreshPending=0, metadata={5:4bytes}, cornerRadiusCrop=[0.00, 0.00, 0.00, 0.00],  shadowRadius=0.000,
+ ContainerLayer (WindowToken{551884f android.os.BinderProxy@89794b0}#0)
  Region TransparentRegion (this=0 count=0)
  Region VisibleRegion (this=0 count=0)
  Region SurfaceDamageRegion (this=0 count=0)
      layerStack=   0, z=        2, pos=(0,0), size=(   0,   0), crop=[  0,   0,  -1,  -1], cornerRadius=0.000000, isProtected=0, isTrustedOverlay=0, isOpaque=0, invalidate=1, dataspace=Default, defaultPixelFormat=Unknown/None, backgroundBlurRadius=0, color=(0.000,0.000,0.000,1.000), flags=0x00000000, tr=[0.00, 0.00][0.00, 0.00]
      parent=Leaf:17:34#0
      zOrderRelativeOf=none
      activeBuffer=[   0x   0:   0,Unknown/None], tr=[0.00, 0.00][0.00, 0.00] queued-frames=0, mRefreshPending=0, metadata={5:4bytes}, cornerRadiusCrop=[0.00, 0.00, 0.00, 0.00],  shadowRadius=0.000,
+ EffectLayer (Surface(name=e4a84e5 NavigationBar0)/@0x8da968a - animation-leash#0)
  Region TransparentRegion (this=0 count=0)
  Region VisibleRegion (this=0 count=0)
  Region SurfaceDamageRegion (this=0 count=0)
      layerStack=   0, z=        0, pos=(0,1344), size=(   0,   0), crop=[  0,   0,   0,   0], cornerRadius=0.000000, isProtected=0, isTrustedOverlay=0, isOpaque=1, invalidate=0, dataspace=Default, defaultPixelFormat=Unknown/None, backgroundBlurRadius=0, color=(-1.000,-1.000,-1.000,1.000), flags=0x00000000, tr=[0.00, 0.00][0.00, 0.00]
      parent=WindowToken{551884f android.os.BinderProxy@89794b0}#0
      zOrderRelativeOf=none
      activeBuffer=[   0x   0:   0,Unknown/None], tr=[0.00, 0.00][0.00, 0.00] queued-frames=0, mRefreshPending=0, metadata={5:4bytes}, cornerRadiusCrop=[0.00, 0.00, 0.00, 0.00],  shadowRadius=0.000,
+ ContainerLayer (e4a84e5 NavigationBar0#0)
  Region TransparentRegion (this=0 count=0)
  Region VisibleRegion (this=0 count=0)
  Region SurfaceDamageRegion (this=0 count=0)
      layerStack=   0, z=        0, pos=(0,1344), size=(   0,   0), crop=[  0,   0,  -1,  -1], cornerRadius=0.000000, isProtected=0, isTrustedOverlay=0, isOpaque=0, invalidate=1, dataspace=Default, defaultPixelFormat=Unknown/None, backgroundBlurRadius=0, color=(0.000,0.000,0.000,1.000), flags=0x00000000, tr=[0.00, 0.00][0.00, 0.00]
      parent=Surface(name=e4a84e5 NavigationBar0)/@0x8da968a - animation-leash#0
      zOrderRelativeOf=none
      activeBuffer=[   0x   0:   0,Unknown/None], tr=[0.00, 0.00][0.00, 0.00] queued-frames=0, mRefreshPending=0, metadata={5:4bytes}, cornerRadiusCrop=[0.00, 0.00, 0.00, 0.00],  shadowRadius=0.000,
+ BufferQueueLayer (NavigationBar0#0)
  Region TransparentRegion (this=0 count=0)
  Region VisibleRegion (this=0 count=1)
    [  0, 1344, 720, 1440]
  Region SurfaceDamageRegion (this=0 count=0)
      layerStack=   0, z=        0, pos=(0,1344), size=( 720,  96), crop=[  0,   0, 720,  96], cornerRadius=0.000000, isProtected=0, isTrustedOverlay=0, isOpaque=0, invalidate=0, dataspace=Default, defaultPixelFormat=RGBA_8888, backgroundBlurRadius=0, color=(0.000,0.000,0.000,1.000), flags=0x00000000, tr=[0.00, 0.00][0.00, 0.00]
      parent=e4a84e5 NavigationBar0#0
      zOrderRelativeOf=none
      activeBuffer=[ 720x  96: 768,RGBA_8888], tr=[0.00, 0.00][0.00, 0.00] queued-frames=0, mRefreshPending=0, metadata={5:4bytes, windowType:2019, ownerUID:10131}, cornerRadiusCrop=[0.00, 0.00, 0.00, 0.00],  shadowRadius=0.000,
+ ContainerLayer (WindowToken{a52de3e android.os.BinderProxy@ffa3ef9}#0)
  Region TransparentRegion (this=0 count=0)
  Region VisibleRegion (this=0 count=0)
  Region SurfaceDamageRegion (this=0 count=0)
      layerStack=   0, z=        3, pos=(0,0), size=(   0,   0), crop=[  0,   0,  -1,  -1], cornerRadius=0.000000, isProtected=0, isTrustedOverlay=0, isOpaque=0, invalidate=1, dataspace=Default, defaultPixelFormat=Unknown/None, backgroundBlurRadius=0, color=(0.000,0.000,0.000,1.000), flags=0x00000000, tr=[0.00, 0.00][0.00, 0.00]
      parent=Leaf:17:34#0
      zOrderRelativeOf=none
      activeBuffer=[   0x   0:   0,Unknown/None], tr=[0.00, 0.00][0.00, 0.00] queued-frames=0, mRefreshPending=0, metadata={5:4bytes}, cornerRadiusCrop=[0.00, 0.00, 0.00, 0.00],  shadowRadius=0.000,
+ ContainerLayer (e8fbbb pip-dismiss-overlay#0)
  Region TransparentRegion (this=0 count=0)
  Region VisibleRegion (this=0 count=0)
  Region SurfaceDamageRegion (this=0 count=0)
      layerStack=   0, z=        0, pos=(0,940), size=(   0,   0), crop=[  0,   0,  -1,  -1], cornerRadius=0.000000, isProtected=0, isTrustedOverlay=0, isOpaque=0, invalidate=1, dataspace=Default, defaultPixelFormat=Unknown/None, backgroundBlurRadius=0, color=(0.000,0.000,0.000,1.000), flags=0x00000000, tr=[0.00, 0.00][0.00, 0.00]
      parent=WindowToken{a52de3e android.os.BinderProxy@ffa3ef9}#0
      zOrderRelativeOf=none
      activeBuffer=[   0x   0:   0,Unknown/None], tr=[0.00, 0.00][0.00, 0.00] queued-frames=0, mRefreshPending=0, metadata={5:4bytes}, cornerRadiusCrop=[0.00, 0.00, 0.00, 0.00],  shadowRadius=0.000,
+ ContainerLayer (mOverlayContainers#0)
  Region TransparentRegion (this=0 count=0)
  Region VisibleRegion (this=0 count=0)
  Region SurfaceDamageRegion (this=0 count=0)
      layerStack=   0, z=        1, pos=(0,0), size=(   0,   0), crop=[  0,   0,  -1,  -1], cornerRadius=0.000000, isProtected=0, isTrustedOverlay=0, isOpaque=0, invalidate=1, dataspace=Default, defaultPixelFormat=Unknown/None, backgroundBlurRadius=0, color=(0.000,0.000,0.000,1.000), flags=0x00000000, tr=[0.00, 0.00][0.00, 0.00]
      parent=Root#0
      zOrderRelativeOf=none
      activeBuffer=[   0x   0:   0,Unknown/None], tr=[0.00, 0.00][0.00, 0.00] queued-frames=0, mRefreshPending=0, metadata={5:4bytes}, cornerRadiusCrop=[0.00, 0.00, 0.00, 0.00],  shadowRadius=0.000,
+ ContainerLayer (Input Consumer recents_animation_input_consumer#0)
  Region TransparentRegion (this=0 count=0)
  Region VisibleRegion (this=0 count=0)
  Region SurfaceDamageRegion (this=0 count=0)
      layerStack=   0, z=        0, pos=(0,0), size=(   0,   0), crop=[  0,   0, 720, 1440], cornerRadius=0.000000, isProtected=0, isTrustedOverlay=0, isOpaque=0, invalidate=1, dataspace=Default, defaultPixelFormat=Unknown/None, backgroundBlurRadius=0, color=(0.000,0.000,0.000,1.000), flags=0x00000001, tr=[0.00, 0.00][0.00, 0.00]
      parent=none
      zOrderRelativeOf=none
      activeBuffer=[   0x   0:   0,Unknown/None], tr=[0.00, 0.00][0.00, 0.00] queued-frames=0, mRefreshPending=0, metadata={5:4bytes}, cornerRadiusCrop=[0.00, 0.00, 0.00, 0.00],  shadowRadius=0.000,

Offscreen Layers:
Layer Surface(name=e4a84e5 NavigationBar0)/@0x8da968a - animation-leash#1 (EffectLayer) pid:1357 uid:1000
Layer Surface(name=7cb2b10 StatusBar)/@0x89fb8f5 - animation-leash#1 (EffectLayer) pid:1357 uid:1000
Layer Surface(name=7cb2b10 StatusBar)/@0x89fb8f5 - animation-leash#0 (EffectLayer) pid:1357 uid:1000
Layer Surface(name=e4a84e5 NavigationBar0)/@0x8da968a - animation-leash#0 (EffectLayer) pid:1357 uid:1000
Layer SnapshotStartingWindow for taskId=4645#0 (BufferQueueLayer) pid:1357 uid:1000
Layer SnapshotStartingWindow for taskId=4645#0 (BufferQueueLayer) pid:1357 uid:1000
Layer Surface(name=7cb2b10 StatusBar)/@0x89fb8f5 - animation-leash#1 (EffectLayer) pid:1357 uid:1000
Layer Surface(name=e4a84e5 NavigationBar0)/@0x8da968a - animation-leash#1 (EffectLayer) pid:1357 uid:1000
Layer Surface(name=Task=1)/@0x29f6e15 - animation-leash#0 (EffectLayer) pid:1357 uid:1000
Layer Surface(name=Task=4645)/@0x1f45cc7 - animation-leash#0 (EffectLayer) pid:1357 uid:1000
Layer Surface(name=f7e7973 InputMethod)/@0x9c77a2e - animation-leash#1 (EffectLayer) pid:1357 uid:1000

```