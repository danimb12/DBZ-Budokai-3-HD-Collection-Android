# DBZ Budokai 3 HD — port Android (experimental)

Port Android del proyecto recompilado. **No contiene nada del juego**: cada persona
genera su propia APK a partir de **su** `.xex` (EU/PAL por ahora). La APK resultante
lleva código derivado de ese `.xex`: es para uso personal, **no se distribuye**.

## Requisitos de compilación (Linux x86-64)

- CMake ≥ 3.25, Ninja, clang ≥ 19 (para la herramienta `rexglue` del host), Python 3, JDK 17+
- Android SDK: `platforms;android-35`, `build-tools;35.0.0`
- Android NDK `29.0.14206865`

## Pasos

```bash
# 1) SDK ReXGlue v0.10.0 + overlay NovaPowers + overlay Android
android/setup_sdk.sh ../rexglue-sdk

# 2) Herramienta de codegen (host) y código recompilado desde TU xex EU
cmake -G Ninja -S ../rexglue-sdk -B ../rexglue-host -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER=clang-20 -DCMAKE_CXX_COMPILER=clang++-20 -DREXGLUE_ENABLE_TRACY=OFF \
  -DCMAKE_C_FLAGS=-march=x86-64-v2 -DCMAKE_CXX_FLAGS=-march=x86-64-v2
ninja -C ../rexglue-host rexglue
cp /ruta/a/tu/yae3_xenon.xex yae3_xenon_eu.xex          # SHA-256 EU en baserom.md
../rexglue-sdk/out/linux-amd64/rexglue -f codegen dbz3_manifest_eu.toml
python3 tools/prefix_eu_codegen.py generated_eu --keep-image-config

# 3) Compilación Android (arm64-v8a)
cmake -G Ninja -S android -B build-android \
  -DCMAKE_TOOLCHAIN_FILE=$ANDROID_NDK/build/cmake/android.toolchain.cmake \
  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-29 -DANDROID_STL=c++_shared \
  -DCMAKE_BUILD_TYPE=Release -DREXGLUE_SDK_DIR=$(realpath ../rexglue-sdk) \
  -DDBZ3_GENERATED_DIR=generated_eu \
  -DCMAKE_C_FLAGS=-march=armv8.2-a -DCMAKE_CXX_FLAGS=-march=armv8.2-a
ninja -C build-android dbz3 rexgpu-xenos

# 4) Empaquetar
android/build_apk.sh build-android ../rexglue-sdk DBZ3-Android.apk
```

## En el móvil

Copia `yae3_xenon.xex` y la carpeta `eu/` a `Almacenamiento interno/DBZ3/`, instala la
APK, concede "acceso a todos los archivos" y pulsa **Arrancar juego**. Los logs quedan
en `DBZ3/logs/` (botón "Compartir último log del juego").

## Estado

| Versión | Qué hace |
|---|---|
| 0.1 | Comprobación del dispositivo (memoria de 4,5 GB, SIGSEGV, Vulkan) y de los archivos |
| 0.2 | Primer arranque del núcleo recompilado EU con Vulkan (sin launcher; Tag Team, personajes nuevos y online desactivados: son solo USA) |

Detalles técnicos del overlay del SDK en `patches/rexglue-sdk-android/README.md`.
