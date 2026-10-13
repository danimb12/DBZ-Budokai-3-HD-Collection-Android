# Overlay Android del ReXGlue SDK

Segunda capa de parches, **encima** de `patches/rexglue-sdk/` (la de NovaPowers),
sobre un checkout limpio del tag `v0.10.0`. Son copias completas de archivo
(mismo formato que el overlay original): `android/setup_sdk.sh` las aplica en orden.

| Archivo | Cambio |
|---|---|
| `src/core/fiber_android.cpp`, `include/rex/thread/fiber.h`, `src/core/fiber_posix.cpp`, `src/core/CMakeLists.txt` | Bionic no tiene `getcontext/makecontext`: fibras con cambio de contexto AArch64 propio |
| `include/rex/main_android.h`, `src/core/memory_posix.cpp`, `src/ui/windowed_app_main_sdl.cpp` | Init Android (`ASharedMemory`, `pthread_getname_np`) antes de crear memoria/hilos; `SDL_main` + `chdir` a la raíz de datos |
| `src/core/threading_posix.cpp` | Bionic no tiene mutex robustos |
| `include/rex/platform.h` | `rex::GetAndroidApiLevel()` |
| `include/rex/string/numeric.h`, `include/rex/chrono/chrono.h` | libc++ del NDK: sin `from_chars` de float ni `clock_time_conversion` (mismo apaño que macOS) |
| `src/core/filesystem_posix.cpp` | `GetExecutableFolder()` = `REX_ANDROID_ROOT` (carpeta de datos del juego) |
| `src/system/gpu_plugin_loader.cpp` | El plugin de GPU se carga por soname desde `nativeLibraryDir` |
| `include/rex/ui/surface_android.h`, `src/ui/window_sdl.cpp`, `src/ui/CMakeLists.txt` | Superficie `ANativeWindow` desde SDL3 para Vulkan; sin X11/Wayland |
| `include/rex/ui/windowed_app.h` | Una app por librería también en Android (entrada SDL) |
| `include/rex/ui/window_sdl.h`, `src/ui/window_sdl.cpp`, `src/ui/windowed_app_context_sdl.cpp` | Al volver de segundo plano Android crea un `ANativeWindow` nuevo: se suelta y se recrea la superficie Vulkan (antes, pantalla negra) |
| `src/graphics/vulkan/pipeline_cache.cpp` | Bionic abre `"a+"` al final del fichero: rebobinar antes de leer la caché de shaders (si no, se truncaba en cada arranque) |
| `src/ui/presenter.cpp`, `src/graphics/vulkan/command_processor.cpp` | `dbz3_perf_logging` fuera de Windows + carga de CPU por hilo en Android (`perf cpu`) |
