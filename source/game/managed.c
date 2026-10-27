/*
 * Calling managed code from native hooks. Direct calls are fine for trivial
 * getters, but anything that can throw must go through il2cpp_runtime_invoke:
 * a managed exception unwinding into one of our frames has no handler and
 * aborts the process.
 */
#include "game/managed.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "game/gamebin.h"
#include "util/log.h"

typedef void *(*domain_get_fn)(void);
typedef void **(*domain_get_assemblies_fn)(void *domain, size_t *size);
typedef void *(*assembly_get_image_fn)(void *assembly);
typedef void *(*class_from_name_fn)(void *image, const char *ns, const char *name);
typedef void *(*class_get_method_fn)(void *klass, const char *name, int argc);
typedef void *(*runtime_invoke_fn)(void *method, void *obj, void **params, void **exc);
typedef void *(*string_new_fn)(const char *s);
typedef void *(*object_unbox_fn)(void *obj);
typedef void (*format_exception_fn)(void *ex, char *out, int size);
typedef const char *(*method_get_name_fn)(void *method);
typedef void *(*object_get_virtual_method_fn)(void *obj, void *method);

static domain_get_fn domain_get;
static domain_get_assemblies_fn domain_get_assemblies;
static assembly_get_image_fn assembly_get_image;
static class_from_name_fn class_from_name;
static class_get_method_fn class_get_method;
static runtime_invoke_fn runtime_invoke;
static string_new_fn string_new;
static object_unbox_fn object_unbox;
static format_exception_fn format_exception;
static method_get_name_fn method_get_name;
static object_get_virtual_method_fn object_get_virtual_method;

#define SYM(var, name)                                         \
  do {                                                         \
    var = (void *)so_symbol(gamebin_il2cpp, name);             \
    if (!var) {                                                \
      LOG("[managed] %s not exported", name);                  \
      ok = 0;                                                  \
    }                                                          \
  } while (0)

int managed_init(void) {
  int ok = 1;
  SYM(domain_get, "il2cpp_domain_get");
  SYM(domain_get_assemblies, "il2cpp_domain_get_assemblies");
  SYM(assembly_get_image, "il2cpp_assembly_get_image");
  SYM(class_from_name, "il2cpp_class_from_name");
  SYM(class_get_method, "il2cpp_class_get_method_from_name");
  SYM(runtime_invoke, "il2cpp_runtime_invoke");
  SYM(string_new, "il2cpp_string_new");
  SYM(object_unbox, "il2cpp_object_unbox");
  SYM(format_exception, "il2cpp_format_exception");
  SYM(method_get_name, "il2cpp_method_get_name");
  SYM(object_get_virtual_method, "il2cpp_object_get_virtual_method");
  return ok;
}

void *managed_class(const char *ns, const char *name) {
  size_t n = 0;
  void **assemblies = domain_get_assemblies(domain_get(), &n);
  for (size_t i = 0; i < n; i++) {
    void *k = class_from_name(assembly_get_image(assemblies[i]), ns, name);
    if (k)
      return k;
  }
  LOG("[managed] class %s.%s not found", ns, name);
  return NULL;
}

void *managed_method(void *klass, const char *name, int argc) {
  void *m = klass ? class_get_method(klass, name, argc) : NULL;
  if (!m)
    LOG("[managed] method %s/%d not found", name, argc);
  return m;
}

void *managed_vmethod(void *obj, int vtable_offset) {
  void *klass = *(void **)obj;
  return *(void **)((uint8_t *)klass + vtable_offset + 4); /* {methodPtr, MethodInfo*} */
}

void *managed_override(void *obj, void *method) {
  return obj && method ? object_get_virtual_method(obj, method) : NULL;
}

int managed_invoke(void *method, void *obj, void **params, void **result) {
  void *exc = NULL;
  if (result)
    *result = NULL;
  if (!method)
    return -1;
  void *r = runtime_invoke(method, obj, params, &exc);
  if (exc) {
    char msg[512];
    format_exception(exc, msg, sizeof(msg));
    LOG("[managed] %s threw: %s", method_get_name(method), msg);
    return -1;
  }
  if (result)
    *result = r;
  return 0;
}

void *managed_unbox(void *boxed) {
  return boxed ? object_unbox(boxed) : NULL;
}

void *managed_string(const char *s) {
  return string_new(s);
}
