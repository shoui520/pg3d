#ifndef PG3D_GAME_MANAGED_H
#define PG3D_GAME_MANAGED_H

/* il2cpp embedding API, resolved from libil2cpp's exports; call after relocation */
int managed_init(void);
void *managed_class(const char *ns, const char *name);
void *managed_method(void *klass, const char *name, int argc);
/* MethodInfo in obj's vtable entry at the given Il2CppClass offset (as compiled code reads it) */
void *managed_vmethod(void *obj, int vtable_offset);
/* obj's override of a virtual method (il2cpp_runtime_invoke does no dispatch) */
void *managed_override(void *obj, void *method);
/* il2cpp_runtime_invoke; params point at value-type args, reference args are the object.
 * Returns 0, or -1 with the exception logged. Value-type results come back boxed. */
int managed_invoke(void *method, void *obj, void **params, void **result);
void *managed_unbox(void *boxed);
void *managed_string(const char *s);

#endif
