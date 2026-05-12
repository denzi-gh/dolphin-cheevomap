// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <jni.h>

#include <memory>
#include <mutex>
#include <vector>

#include "Common/HookableEvent.h"
#include "Core/CheevoMap/CheevoMapEntry.h"
#include "Core/CheevoMap/CheevoMapManager.h"
#include "jni/AndroidCommon/AndroidCommon.h"
#include "jni/AndroidCommon/IDCache.h"

namespace
{
struct ListenerState
{
  std::mutex mutex;
  jobject global_ref = nullptr;
  Common::EventHook hook;
};

ListenerState& Listener()
{
  static ListenerState s;
  return s;
}

jobject MakeEntry(JNIEnv* env, const CheevoMap::LiveValue& v)
{
  jstring j_id = env->NewStringUTF(v.id.c_str());
  jstring j_label = env->NewStringUTF(v.label.c_str());
  jstring j_group = env->NewStringUTF(v.group.c_str());
  jstring j_value = env->NewStringUTF(v.value_str.c_str());
  jstring j_icon = v.icon_path.empty() ? nullptr : env->NewStringUTF(v.icon_path.c_str());

  jobjectArray j_slots = nullptr;
  if (!v.icon_slots.empty())
  {
    j_slots = env->NewObjectArray(static_cast<jsize>(v.icon_slots.size()),
                                  IDCache::GetStringClass(), nullptr);
    for (size_t i = 0; i < v.icon_slots.size(); ++i)
    {
      jstring slot = env->NewStringUTF(v.icon_slots[i].c_str());
      env->SetObjectArrayElement(j_slots, static_cast<jsize>(i), slot);
      env->DeleteLocalRef(slot);
    }
  }

  jobject obj = env->NewObject(IDCache::GetCheevoMapEntryClass(),
                               IDCache::GetCheevoMapEntryConstructor(), j_id, j_label, j_group,
                               j_value, v.visible ? JNI_TRUE : JNI_FALSE, j_icon, j_slots);

  env->DeleteLocalRef(j_id);
  env->DeleteLocalRef(j_label);
  env->DeleteLocalRef(j_group);
  env->DeleteLocalRef(j_value);
  if (j_icon) env->DeleteLocalRef(j_icon);
  if (j_slots) env->DeleteLocalRef(j_slots);
  return obj;
}
}  // namespace

extern "C" {

JNIEXPORT jboolean JNICALL
Java_org_dolphinemu_dolphinemu_features_cheevomap_CheevoMapModel_isLoaded(JNIEnv*, jclass)
{
  return CheevoMap::Manager::GetInstance().IsLoaded() ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jstring JNICALL
Java_org_dolphinemu_dolphinemu_features_cheevomap_CheevoMapModel_getCurrentTitle(JNIEnv* env,
                                                                                  jclass)
{
  const std::string title = CheevoMap::Manager::GetInstance().GetCurrentTitle();
  return env->NewStringUTF(title.c_str());
}

JNIEXPORT jobjectArray JNICALL
Java_org_dolphinemu_dolphinemu_features_cheevomap_CheevoMapModel_getEntries(JNIEnv* env, jclass)
{
  const auto snapshot = CheevoMap::Manager::GetInstance().GetSnapshot();
  jobjectArray arr = env->NewObjectArray(static_cast<jsize>(snapshot.size()),
                                         IDCache::GetCheevoMapEntryClass(), nullptr);
  for (size_t i = 0; i < snapshot.size(); ++i)
  {
    jobject obj = MakeEntry(env, snapshot[i]);
    env->SetObjectArrayElement(arr, static_cast<jsize>(i), obj);
    env->DeleteLocalRef(obj);
  }
  return arr;
}

JNIEXPORT void JNICALL
Java_org_dolphinemu_dolphinemu_features_cheevomap_CheevoMapModel_registerListener(JNIEnv* env,
                                                                                   jclass,
                                                                                   jobject listener)
{
  auto& s = Listener();
  std::lock_guard lg(s.mutex);
  if (s.global_ref)
  {
    env->DeleteGlobalRef(s.global_ref);
    s.global_ref = nullptr;
  }
  s.hook.reset();
  if (!listener)
    return;
  s.global_ref = env->NewGlobalRef(listener);
  s.hook = CheevoMap::Manager::GetInstance().RegisterUpdatedCallback([] {
    auto& state = Listener();
    std::lock_guard lg2(state.mutex);
    if (!state.global_ref)
      return;
    JNIEnv* env_thread = IDCache::GetEnvForThread();
    env_thread->CallVoidMethod(state.global_ref, IDCache::GetCheevoMapListenerOnChanged());
  });
}

JNIEXPORT void JNICALL
Java_org_dolphinemu_dolphinemu_features_cheevomap_CheevoMapModel_unregisterListener(JNIEnv* env,
                                                                                     jclass)
{
  auto& s = Listener();
  std::lock_guard lg(s.mutex);
  s.hook.reset();
  if (s.global_ref)
  {
    env->DeleteGlobalRef(s.global_ref);
    s.global_ref = nullptr;
  }
}

}  // extern "C"
