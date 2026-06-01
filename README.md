# Play Hub TV

Android TV app for sending a stream URL from a phone/browser to the TV and playing it with libVLC.

## Xiaomi Android TV Box Build

The current debug build is tuned for the tested Xiaomi Android TV box:

- Android version: 11
- SDK: 30
- CPU ABI: `armeabi-v7a`

Build the device-specific APK from the project root:

```powershell
.\gradlew.bat :app:assembleDebug
```

Use this APK for sideloading:

```text
app\build\outputs\apk\debug\app-armeabi-v7a-debug.apk
```

Do not use `app-debug-androidTest.apk`. That is only for Android instrumentation tests.

The app build is configured as:

```text
compileSdk: 35
targetSdk: 30
minSdk: 23
native-code: armeabi-v7a only
APK signing: v1 + v2
```

This keeps the APK smaller and more compatible with the Xiaomi box. The `armeabi-v7a` split reduces the APK size by excluding unused native libraries for other CPU architectures.

For transferring the APK, prefer a Wi-Fi transfer tool such as LocalSend. USB/pendrive transfer can corrupt or truncate large APK files on some TV boxes or file managers, which may show a generic error like:

```text
There was a problem parsing the package
```

If sideloading still fails, install with ADB to get the real error:

```powershell
adb connect TV_BOX_IP:5555
adb install -r app\build\outputs\apk\debug\app-armeabi-v7a-debug.apk
```

## Run In Android TV Emulator From PowerShell

You can build, install, and run the app without opening Android Studio.

Start the Android TV emulator:

```powershell
emulator -avd Television_720p
```

In another PowerShell window, confirm the emulator is connected:

```powershell
adb devices -l
```

Build the debug APK:

```powershell
.\gradlew.bat :app:assembleDebug
```

Install the emulator-compatible APK:

```powershell
adb install -r app\build\outputs\apk\debug\app-armeabi-v7a-debug.apk
```

Launch the app:

```powershell
adb shell monkey -p com.example.playhubtv -c android.intent.category.LEANBACK_LAUNCHER 1
```

For emulator testing, forward the app's local web server port to Windows:

```powershell
adb forward tcp:8080 tcp:8080
```

Then open this URL from the Windows browser:

```text
http://127.0.0.1:8080/
```

The emulator's internal address, usually `http://10.0.2.15:8080/`, is not reachable from your phone or Windows browser directly. Phone QR scanning should be tested on a real Android TV box connected to the same Wi-Fi/LAN.
