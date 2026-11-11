// bridge.cpp - puente nativo para Among Us (arm64)
// Se carga con System.loadLibrary("bridge") desde un parche smali.
// Fase 1: espera a que el juego arranque IL2CPP, y escribe en un txt
//         todas las clases/metodos con su RVA (sustituye al dump.cs).
// Fase 2 (despues): aqui se agregan los hooks y la logica del mod.

#include <dlfcn.h>
#include <pthread.h>
#include <unistd.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <string>
#include <sys/stat.h>

#ifdef __ANDROID__
#include <android/log.h>
#define LOG(...) __android_log_print(ANDROID_LOG_INFO, "LIBridge", __VA_ARGS__)
#else
#define LOG(...) (printf(__VA_ARGS__), printf("\n"))
#endif

// Assemblies a volcar. Agrega mas nombres si hace falta.
static const char* TARGETS[] = { "Assembly-CSharp.dll" };

// ---- firmas de la API exportada por libil2cpp.so ----
typedef void*       (*t_domain_get)();
typedef void**      (*t_domain_get_assemblies)(void*, size_t*);
typedef void*       (*t_assembly_get_image)(void*);
typedef const char* (*t_image_get_name)(void*);
typedef size_t      (*t_image_get_class_count)(void*);
typedef void*       (*t_image_get_class)(void*, size_t);
typedef const char* (*t_class_get_name)(void*);
typedef const char* (*t_class_get_namespace)(void*);
typedef void*       (*t_class_get_methods)(void*, void**);
typedef const char* (*t_method_get_name)(void*);
typedef uint32_t    (*t_method_get_param_count)(void*);
typedef void*       (*t_thread_attach)(void*);

static t_domain_get            il2cpp_domain_get;
static t_domain_get_assemblies il2cpp_domain_get_assemblies;
static t_assembly_get_image    il2cpp_assembly_get_image;
static t_image_get_name        il2cpp_image_get_name;
static t_image_get_class_count il2cpp_image_get_class_count;
static t_image_get_class       il2cpp_image_get_class;
static t_class_get_name        il2cpp_class_get_name;
static t_class_get_namespace   il2cpp_class_get_namespace;
static t_class_get_methods     il2cpp_class_get_methods;
static t_method_get_name       il2cpp_method_get_name;
static t_method_get_param_count il2cpp_method_get_param_count;
static t_thread_attach         il2cpp_thread_attach;

template <typename T>
static bool resolve(void* h, const char* name, T& out) {
    out = reinterpret_cast<T>(dlsym(h, name));
    if (!out) LOG("no se pudo resolver %s", name);
    return out != nullptr;
}

static bool resolve_all(void* h) {
    return resolve(h, "il2cpp_domain_get", il2cpp_domain_get)
        && resolve(h, "il2cpp_domain_get_assemblies", il2cpp_domain_get_assemblies)
        && resolve(h, "il2cpp_assembly_get_image", il2cpp_assembly_get_image)
        && resolve(h, "il2cpp_image_get_name", il2cpp_image_get_name)
        && resolve(h, "il2cpp_image_get_class_count", il2cpp_image_get_class_count)
        && resolve(h, "il2cpp_image_get_class", il2cpp_image_get_class)
        && resolve(h, "il2cpp_class_get_name", il2cpp_class_get_name)
        && resolve(h, "il2cpp_class_get_namespace", il2cpp_class_get_namespace)
        && resolve(h, "il2cpp_class_get_methods", il2cpp_class_get_methods)
        && resolve(h, "il2cpp_method_get_name", il2cpp_method_get_name)
        && resolve(h, "il2cpp_method_get_param_count", il2cpp_method_get_param_count)
        && resolve(h, "il2cpp_thread_attach", il2cpp_thread_attach);
}

// direccion base de libil2cpp.so en memoria (para calcular RVAs)
static uintptr_t module_base(const char* lib) {
    FILE* f = fopen("/proc/self/maps", "r");
    if (!f) return 0;
    char line[512];
    uintptr_t base = 0;
    while (fgets(line, sizeof(line), f)) {
        if (strstr(line, lib)) { base = strtoull(line, nullptr, 16); break; }
    }
    fclose(f);
    return base;
}

static std::string package_name() {
    char buf[256] = {0};
    FILE* f = fopen("/proc/self/cmdline", "r");
    if (f) { fread(buf, 1, sizeof(buf) - 1, f); fclose(f); }
    return std::string(buf);
}

static bool is_target(const char* name) {
    for (const char* t : TARGETS) if (name && strcmp(name, t) == 0) return true;
    return false;
}

static void dump_all(FILE* out, uintptr_t base) {
    size_t asm_count = 0;
    void** assemblies = il2cpp_domain_get_assemblies(il2cpp_domain_get(), &asm_count);
    LOG("assemblies cargados: %zu", asm_count);

    for (size_t a = 0; a < asm_count; a++) {
        void* image = il2cpp_assembly_get_image(assemblies[a]);
        const char* iname = il2cpp_image_get_name(image);
        if (!is_target(iname)) continue;

        fprintf(out, "// ===== %s =====\n", iname);
        size_t n = il2cpp_image_get_class_count(image);
        for (size_t c = 0; c < n; c++) {
            void* klass = il2cpp_image_get_class(image, c);
            if (!klass) continue;
            const char* ns = il2cpp_class_get_namespace(klass);
            const char* cn = il2cpp_class_get_name(klass);
            fprintf(out, "\n%s%s%s\n", ns && *ns ? ns : "", ns && *ns ? "." : "", cn);

            void* iter = nullptr;
            while (void* m = il2cpp_class_get_methods(klass, &iter)) {
                // el primer campo de MethodInfo es el puntero a la funcion
                uintptr_t ptr = (uintptr_t)(*(void**)m);
                uintptr_t rva = ptr ? ptr - base : 0;
                fprintf(out, "  [RVA 0x%lX] %s (%u params)\n",
                        (unsigned long)rva, il2cpp_method_get_name(m),
                        il2cpp_method_get_param_count(m));
            }
        }
    }
}

static void* worker(void*) {
    // 1) esperar a que el juego cargue libil2cpp.so
    void* h = nullptr;
    for (int i = 0; i < 300 && !h; i++) {
        h = dlopen("libil2cpp.so", RTLD_NOW | RTLD_NOLOAD);
        if (!h) sleep(1);
    }
    if (!h) { LOG("libil2cpp.so nunca aparecio"); return nullptr; }
    if (!resolve_all(h)) return nullptr;

    // 2) dar tiempo a que Unity inicialice IL2CPP (ajusta si crashea)
    sleep(20);

    void* domain = il2cpp_domain_get();
    if (!domain) { LOG("dominio nulo, sube el sleep"); return nullptr; }
    il2cpp_thread_attach(domain);

    uintptr_t base = module_base("libil2cpp.so");
    LOG("libil2cpp base = 0x%lX", (unsigned long)base);

    // 3) escribir el volcado en la carpeta propia de la app
    std::string pkg = package_name();
    std::string dir = "/storage/emulated/0/Android/data/" + pkg + "/files";
    mkdir(dir.c_str(), 0771);
    std::string path = dir + "/il2cpp_dump.txt";
    FILE* out = fopen(path.c_str(), "w");
    if (!out) { LOG("no pude abrir %s", path.c_str()); return nullptr; }

    dump_all(out, base);
    fclose(out);
    LOG("listo: %s", path.c_str());

    // FASE 2: aqui van los hooks (Dobby/Substrate) y la logica del mod.
    return nullptr;
}

// se ejecuta al cargar la libreria (System.loadLibrary)
__attribute__((constructor)) static void bridge_init() {
    pthread_t t;
    pthread_create(&t, nullptr, worker, nullptr);
    pthread_detach(t);
}
