// DBZ3 Android - APK 0.1 hardware probe.
// Checks the host features the ReXGlue runtime needs on Android:
//  - page size / allocation granularity
//  - the 4.5 GB guest address-space layout (shared-memory views at 1<<n)
//  - SIGSEGV-based write watches (used by the MMIO handler / GPU memory tracking)
//  - Vulkan device, version and the features the Xenos->Vulkan backend uses
#include <jni.h>
#include <android/sharedmem.h>
#include <dlfcn.h>
#include <errno.h>
#include <setjmp.h>
#include <signal.h>
#include <string.h>
#include <sys/auxv.h>
#include <sys/mman.h>
#include <unistd.h>
#include <vulkan/vulkan.h>

#include <cstdarg>
#include <cstdio>
#include <string>
#include <vector>

#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000
#endif
#ifndef HWCAP_ATOMICS
#define HWCAP_ATOMICS (1 << 8)
#endif
#ifndef HWCAP_ASIMDDP
#define HWCAP_ASIMDDP (1 << 20)
#endif
#ifndef HWCAP_FPHP
#define HWCAP_FPHP (1 << 9)
#endif
#ifndef HWCAP2_BF16
#define HWCAP2_BF16 (1 << 14)
#endif

static std::string g_out;
static int g_ok = 0, g_warn = 0, g_fail = 0;

static void line(const char* fmt, ...) {
  char buf[1024];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  g_out += buf;
  g_out += "\n";
}
static void ok(const char* what) { g_ok++; line("  [OK]   %s", what); }
static void warn(const char* what) { g_warn++; line("  [AVISO] %s", what); }
static void fail(const char* what) { g_fail++; line("  [FALLO] %s", what); }

// ---------------------------------------------------------------- CPU / pages
static void probe_cpu() {
  line("== CPU y memoria ==");
  long page = sysconf(_SC_PAGESIZE);
  line("  Tamano de pagina: %ld bytes", page);
  if (page == 4096) ok("Paginas de 4 KB (igual que la Xbox 360 / PC)");
  else if (page == 16384) warn("Paginas de 16 KB: el runtime ya contempla granularidad > 4 KB, pero es terreno menos probado");
  else fail("Tamano de pagina inesperado");

  line("  Nucleos: %ld", sysconf(_SC_NPROCESSORS_CONF));
  unsigned long hw = getauxval(AT_HWCAP), hw2 = getauxval(AT_HWCAP2);
  line("  HWCAP=0x%lx HWCAP2=0x%lx", hw, hw2);
  (hw & HWCAP_ATOMICS) ? ok("Atomicas LSE (ARMv8.1+)") : warn("Sin atomicas LSE");
  (hw & HWCAP_FPHP) ? ok("FP16 (ARMv8.2)") : warn("Sin FP16");
  (hw & HWCAP_ASIMDDP) ? ok("DotProd (ARMv8.2)") : warn("Sin DotProd");
}

// ----------------------------------------------------- guest address space
// Same table as rex::system Memory::map_info (xmemory.cpp, SDK 0.10).
struct MapInfo { uint64_t start, end, target; };
static const MapInfo kMap[] = {
    {0x00000000, 0x3FFFFFFF, 0x0000000000000000ull}, {0x40000000, 0x7EFFFFFF, 0x0000000040000000ull},
    {0x7F000000, 0x7FFFFFFF, 0x0000000100000000ull}, {0x80000000, 0x8FFFFFFF, 0x0000000080000000ull},
    {0x90000000, 0x9FFFFFFF, 0x0000000080000000ull}, {0xA0000000, 0xBFFFFFFF, 0x0000000100000000ull},
    {0xC0000000, 0xDFFFFFFF, 0x0000000100000000ull}, {0xE0000000, 0xFFFFFFFF, 0x0000000100001000ull},
    {0x100000000, 0x11FFFFFFF, 0x0000000100000000ull},
};

static uint8_t* g_views[sizeof(kMap) / sizeof(kMap[0])];

static void unmap_views() {
  for (size_t i = 0; i < sizeof(kMap) / sizeof(kMap[0]); i++) {
    if (g_views[i]) munmap(g_views[i], kMap[i].end - kMap[i].start + 1);
    g_views[i] = nullptr;
  }
}

static bool map_views(int fd, uint8_t* base, uint64_t gran_mask, int* err_view, int* err_no) {
  for (size_t i = 0; i < sizeof(kMap) / sizeof(kMap[0]); i++) {
    size_t sz = kMap[i].end - kMap[i].start + 1;
    void* want = base + kMap[i].start;
    void* r = mmap(want, sz, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED_NOREPLACE, fd,
                   (off_t)(kMap[i].target & gran_mask));
    if (r == MAP_FAILED || r != want) {
      *err_view = (int)i;
      *err_no = (r == MAP_FAILED) ? errno : EEXIST;
      if (r != MAP_FAILED) munmap(r, sz);
      unmap_views();
      return false;
    }
    g_views[i] = (uint8_t*)r;
  }
  return true;
}

static uint8_t* g_base = nullptr;

static void probe_address_space() {
  line("== Espacio de direcciones del invitado (4,5 GB, como xmemory.cpp) ==");
  long page = sysconf(_SC_PAGESIZE);
  size_t gran = page > 65536 ? page : 65536;
  size_t size = ((0x120000000ull + gran) + gran - 1) / gran * gran;
  int fd = ASharedMemory_create("dbz3_probe", size);
  if (fd < 0) {
    line("  ASharedMemory_create(%zu MB) errno=%d (%s)", size >> 20, errno, strerror(errno));
    fail("No se pudo crear la memoria compartida de 4,5 GB");
    return;
  }
  ok("ASharedMemory de 4,5 GB creada (reservada, sin consumir RAM)");
  uint64_t gran_mask = ~uint64_t(page - 1);
  int last_view = -1, last_err = 0;
  for (int n = 32; n < 48; n++) {
    uint8_t* base = (uint8_t*)(1ull << n);
    if (map_views(fd, base, gran_mask, &last_view, &last_err)) {
      g_base = base;
      line("  Vistas mapeadas en base 0x%llx (1<<%d)", (unsigned long long)base, n);
      break;
    }
    line("  base 1<<%d: fallo en vista %d errno=%d (%s)", n, last_view, last_err, strerror(last_err));
  }
  if (!g_base) {
    fail("No se encontro hueco contiguo para las 9 vistas del invitado");
    close(fd);
    return;
  }
  ok("Las 9 vistas del invitado se mapean (paso critico del runtime)");

  // Aliasing: physical memory must be visible through 0xA0000000 and 0xC0000000 views.
  volatile uint32_t* a = (uint32_t*)(g_base + 0xA0001000);
  volatile uint32_t* c = (uint32_t*)(g_base + 0xC0001000);
  volatile uint32_t* p = (uint32_t*)(g_base + 0x100001000ull);
  *a = 0xD8A3B0CAu;
  if (*c == 0xD8A3B0CAu && *p == 0xD8A3B0CAu) ok("Alias de memoria fisica correctos (A0/C0/fisica)");
  else fail("Los alias de memoria fisica no coinciden");
  volatile uint32_t* x80 = (uint32_t*)(g_base + 0x80002000);
  volatile uint32_t* x90 = (uint32_t*)(g_base + 0x90002000);
  *x80 = 0x12345678u;
  (*x90 == 0x12345678u) ? ok("Alias de la zona XEX (80/90) correctos") : fail("Alias XEX incorrectos");
  close(fd);
}

// ------------------------------------------------------- SIGSEGV write watch
static volatile sig_atomic_t g_hits = 0;
static uint8_t* g_watch = nullptr;
static size_t g_watch_size = 0;
static struct sigaction g_old;

static void segv_handler(int sig, siginfo_t* si, void* uctx) {
  uint8_t* addr = (uint8_t*)si->si_addr;
  if (g_watch && addr >= g_watch && addr < g_watch + g_watch_size) {
    g_hits++;
    mprotect(g_watch, g_watch_size, PROT_READ | PROT_WRITE);
    return;  // retry the faulting instruction
  }
  sigaction(SIGSEGV, &g_old, nullptr);
  raise(sig);
}

static void probe_write_watch() {
  line("== Vigilancia de escritura por SIGSEGV (MMIO / cache de GPU) ==");
  g_watch_size = sysconf(_SC_PAGESIZE);
  uint8_t* mem = g_base ? g_base + 0xA0100000 : nullptr;
  if (!mem) {
    mem = (uint8_t*)mmap(nullptr, g_watch_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  }
  g_watch = mem;
  struct sigaction sa = {};
  sa.sa_sigaction = segv_handler;
  sa.sa_flags = SA_SIGINFO;
  sigemptyset(&sa.sa_mask);
  sigaction(SIGSEGV, &sa, &g_old);
  mprotect(mem, g_watch_size, PROT_READ);
  ((volatile uint8_t*)mem)[16] = 0x5A;
  sigaction(SIGSEGV, &g_old, nullptr);
  g_watch = nullptr;
  if (g_hits == 1 && mem[16] == 0x5A) ok("Fallo de pagina capturado y escritura reanudada");
  else fail("El manejador de SIGSEGV no funciono como espera el runtime");
  if (g_base) unmap_views();
}

// -------------------------------------------------------------------- Vulkan
static void probe_vulkan() {
  line("== Vulkan ==");
  VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO};
  app.pApplicationName = "dbz3_probe";
  app.apiVersion = VK_API_VERSION_1_1;
  VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
  ici.pApplicationInfo = &app;
  VkInstance inst;
  VkResult r = vkCreateInstance(&ici, nullptr, &inst);
  if (r != VK_SUCCESS) {
    line("  vkCreateInstance = %d", r);
    fail("No se pudo crear la instancia Vulkan");
    return;
  }
  uint32_t n = 0;
  vkEnumeratePhysicalDevices(inst, &n, nullptr);
  std::vector<VkPhysicalDevice> devs(n);
  vkEnumeratePhysicalDevices(inst, &n, devs.data());
  if (!n) { fail("Ninguna GPU Vulkan"); vkDestroyInstance(inst, nullptr); return; }
  VkPhysicalDevice pd = devs[0];
  VkPhysicalDeviceProperties p;
  vkGetPhysicalDeviceProperties(pd, &p);
  line("  GPU: %s", p.deviceName);
  line("  Vulkan %u.%u.%u  driver 0x%x  vendor 0x%x  device 0x%x", VK_API_VERSION_MAJOR(p.apiVersion),
       VK_API_VERSION_MINOR(p.apiVersion), VK_API_VERSION_PATCH(p.apiVersion), p.driverVersion,
       p.vendorID, p.deviceID);
  (VK_API_VERSION_MINOR(p.apiVersion) >= 3 || VK_API_VERSION_MAJOR(p.apiVersion) > 1)
      ? ok("Vulkan 1.3 o superior") : warn("Vulkan inferior a 1.3");
  line("  maxImageDimension2D=%u maxStorageBufferRange=%u maxComputeSharedMemory=%u",
       p.limits.maxImageDimension2D, p.limits.maxStorageBufferRange, p.limits.maxComputeSharedMemorySize);

  VkPhysicalDeviceFeatures f;
  vkGetPhysicalDeviceFeatures(pd, &f);
  struct { const char* name; VkBool32 v; bool critical; } feats[] = {
      {"fragmentStoresAndAtomics (EDRAM por fragment shader)", f.fragmentStoresAndAtomics, true},
      {"geometryShader (rect lists / point sprites)", f.geometryShader, false},
      {"vertexPipelineStoresAndAtomics", f.vertexPipelineStoresAndAtomics, false},
      {"shaderClipDistance", f.shaderClipDistance, false},
      {"depthClamp", f.depthClamp, false},
      {"independentBlend", f.independentBlend, false},
      {"samplerAnisotropy", f.samplerAnisotropy, false},
      {"fillModeNonSolid", f.fillModeNonSolid, false},
      {"textureCompressionBC (DXT nativo)", f.textureCompressionBC, false},
      {"dualSrcBlend", f.dualSrcBlend, false},
      {"shaderStorageImageWriteWithoutFormat", f.shaderStorageImageWriteWithoutFormat, false},
      {"fullDrawIndexUint32", f.fullDrawIndexUint32, false},
  };
  for (auto& e : feats) {
    if (e.v) ok(e.name);
    else if (e.critical) fail(e.name);
    else warn(e.name);
  }

  uint32_t ne = 0;
  vkEnumerateDeviceExtensionProperties(pd, nullptr, &ne, nullptr);
  std::vector<VkExtensionProperties> ext(ne);
  vkEnumerateDeviceExtensionProperties(pd, nullptr, &ne, ext.data());
  const char* wanted[] = {"VK_EXT_fragment_shader_interlock", "VK_EXT_shader_stencil_export",
                          "VK_KHR_shader_float_controls", "VK_EXT_memory_budget",
                          "VK_KHR_swapchain", "VK_EXT_custom_border_color",
                          "VK_KHR_portability_subset", "VK_EXT_shader_demote_to_helper_invocation"};
  line("  Extensiones de dispositivo: %u", ne);
  for (auto w : wanted) {
    bool has = false;
    for (auto& e : ext) if (!strcmp(e.extensionName, w)) has = true;
    line("  %s %s", has ? "[si]" : "[no]", w);
  }

  VkPhysicalDeviceMemoryProperties mp;
  vkGetPhysicalDeviceMemoryProperties(pd, &mp);
  for (uint32_t i = 0; i < mp.memoryHeapCount; i++)
    line("  Heap %u: %llu MB%s", i, (unsigned long long)(mp.memoryHeaps[i].size >> 20),
         (mp.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) ? " (device local)" : "");
  vkDestroyInstance(inst, nullptr);
}

extern "C" JNIEXPORT jstring JNICALL Java_com_dbz3_android_MainActivity_runProbe(JNIEnv* env, jclass) {
  g_out.clear();
  g_ok = g_warn = g_fail = 0;
  probe_cpu();
  probe_address_space();
  probe_write_watch();
  probe_vulkan();
  line("");
  line("Resumen: %d OK, %d avisos, %d fallos", g_ok, g_warn, g_fail);
  if (g_fail == 0) line("Veredicto: el dispositivo cumple lo que necesita el runtime de ReXGlue.");
  else line("Veredicto: hay fallos que bloquean el port en este dispositivo (ver arriba).");
  return env->NewStringUTF(g_out.c_str());
}
