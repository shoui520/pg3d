/* Device facts exposed through Java APIs: language and a stable device id. */
#include <stdio.h>
#include <string.h>
#include <psp2/apputil.h>
#include <psp2/system_param.h>

#include "jni/java.h"

const char *java_language(void) {
  static const char *lang;
  if (lang)
    return lang;
  int id = SCE_SYSTEM_PARAM_LANG_ENGLISH_US;
  sceAppUtilSystemParamGetInt(SCE_SYSTEM_PARAM_ID_LANG, &id);
  switch (id) {
  case SCE_SYSTEM_PARAM_LANG_JAPANESE: lang = "ja"; break;
  case SCE_SYSTEM_PARAM_LANG_FRENCH: lang = "fr"; break;
  case SCE_SYSTEM_PARAM_LANG_SPANISH: lang = "es"; break;
  case SCE_SYSTEM_PARAM_LANG_GERMAN: lang = "de"; break;
  case SCE_SYSTEM_PARAM_LANG_ITALIAN: lang = "it"; break;
  case SCE_SYSTEM_PARAM_LANG_DUTCH: lang = "nl"; break;
  case SCE_SYSTEM_PARAM_LANG_PORTUGUESE_PT:
  case SCE_SYSTEM_PARAM_LANG_PORTUGUESE_BR: lang = "pt"; break;
  case SCE_SYSTEM_PARAM_LANG_RUSSIAN: lang = "ru"; break;
  case SCE_SYSTEM_PARAM_LANG_KOREAN: lang = "ko"; break;
  case SCE_SYSTEM_PARAM_LANG_CHINESE_T:
  case SCE_SYSTEM_PARAM_LANG_CHINESE_S: lang = "zh"; break;
  case SCE_SYSTEM_PARAM_LANG_POLISH: lang = "pl"; break;
  case SCE_SYSTEM_PARAM_LANG_TURKISH: lang = "tr"; break;
  default: lang = "en"; break;
  }
  return lang;
}

/* A per-console id is not needed by an offline game; a fixed one keeps the
 * game's save keys stable across runs. */
const char *java_device_id(void) {
  return "5f1c0de5a7a91d00";
}
