/*
 * Copyright 2017 Andrei Pangin
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <string.h>
#include "lockTracer.h"
#include "profiler.h"
#include "vmStructs.h"


jlong LockTracer::_start_time = 0;
jclass LockTracer::_LockSupport = NULL;
jmethodID LockTracer::_getBlocker = NULL;
UnsafeParkFunc LockTracer::_original_Unsafe_Park = NULL;
bool LockTracer::_supports_lock_names = false;

Error LockTracer::start(Arguments& args) {
    // PermGen in JDK 7 makes difficult to get symbol name from jclass.
    // Also some JVMs do not support VMStructs at all.
    // Let's just record stack traces without lock names in these cases.
    _supports_lock_names = VMStructs::available() && !VMStructs::hasPermGen();

    // Enable Java Monitor events
    jvmtiEnv* jvmti = VM::jvmti();
    jvmti->SetEventNotificationMode(JVMTI_ENABLE, JVMTI_EVENT_MONITOR_CONTENDED_ENTER, NULL);
    jvmti->SetEventNotificationMode(JVMTI_ENABLE, JVMTI_EVENT_MONITOR_CONTENDED_ENTERED, NULL);
    jvmti->GetTime(&_start_time);

    if (_getBlocker == NULL) {
        JNIEnv* env = VM::jni();
        _LockSupport = (jclass)env->NewGlobalRef(env->FindClass("java/util/concurrent/locks/LockSupport"));
        _getBlocker = env->GetStaticMethodID(_LockSupport, "getBlocker", "(Ljava/lang/Thread;)Ljava/lang/Object;");
    }

    if (_original_Unsafe_Park == NULL) {
        NativeCodeCache* libjvm = Profiler::_instance.jvmLibrary();
        _original_Unsafe_Park = (UnsafeParkFunc)libjvm->findSymbol("Unsafe_Park");
        if (_original_Unsafe_Park == NULL) {
            // In some macOS builds of JDK 11 Unsafe_Park appears to have a C++ decorated name
            _original_Unsafe_Park = (UnsafeParkFunc)libjvm->findSymbol("_ZL11Unsafe_ParkP7JNIEnv_P8_jobjecthl");
        }
    }

    // Intercent Unsafe.park() for tracing contended ReentrantLocks
    if (_original_Unsafe_Park != NULL) {
        bindUnsafePark(UnsafeParkTrap);
    }

    return Error::OK;
}

void LockTracer::stop() {
    // Disable Java Monitor events
    jvmtiEnv* jvmti = VM::jvmti();
    jvmti->SetEventNotificationMode(JVMTI_DISABLE, JVMTI_EVENT_MONITOR_CONTENDED_ENTER, NULL);
    jvmti->SetEventNotificationMode(JVMTI_DISABLE, JVMTI_EVENT_MONITOR_CONTENDED_ENTERED, NULL);

    // Reset Unsafe.park() trap
    if (_original_Unsafe_Park != NULL) {
        bindUnsafePark(_original_Unsafe_Park);
    }
}

void JNICALL LockTracer::MonitorContendedEnter(jvmtiEnv* jvmti, JNIEnv* env, jthread thread, jobject object) {
    jlong enter_time;
    jvmti->GetTime(&enter_time);
    jvmti->SetTag(thread, enter_time);
}

void JNICALL LockTracer::MonitorContendedEntered(jvmtiEnv* jvmti, JNIEnv* env, jthread thread, jobject object) {
    jlong enter_time, entered_time;
    jvmti->GetTime(&entered_time);
    jvmti->GetTag(thread, &enter_time);

    // Time is meaningless if lock attempt has started before profiling
    if (enter_time >= _start_time) {
        recordContendedLock(env->GetObjectClass(object), entered_time - enter_time);
    }
}

void JNICALL LockTracer::UnsafeParkTrap(JNIEnv* env, jobject instance, jboolean isAbsolute, jlong time) {
    jvmtiEnv* jvmti = VM::jvmti();
    jclass lock_class = getParkBlockerClass(jvmti, env);
    jlong park_start_time, park_end_time;

    if (lock_class != NULL) {
        jvmti->GetTime(&park_start_time);
    }
    
    _original_Unsafe_Park(env, instance, isAbsolute, time);

    if (lock_class != NULL) {
        jvmti->GetTime(&park_end_time);
        recordContendedLock(lock_class, park_end_time - park_start_time);
    }
}

jclass LockTracer::getParkBlockerClass(jvmtiEnv* jvmti, JNIEnv* env) {
    jthread thread;
    if (jvmti->GetCurrentThread(&thread) != 0) {
        return NULL;
    }

    // Call LockSupport.getBlocker(Thread.currentThread())
    jobject park_blocker = env->CallStaticObjectMethod(_LockSupport, _getBlocker, thread);
    if (park_blocker == NULL) {
        return NULL;
    }

    jclass lock_class = env->GetObjectClass(park_blocker);
    char* class_name;
    if (jvmti->GetClassSignature(lock_class, &class_name, NULL) != 0) {
        return NULL;
    }

    // Do not count synchronizers other than ReentrantLock, ReentrantReadWriteLock and Semaphore
    if (strncmp(class_name, "Ljava/util/concurrent/locks/ReentrantLock", 41) != 0 &&
        strncmp(class_name, "Ljava/util/concurrent/locks/ReentrantReadWriteLock", 50) != 0 &&
        strncmp(class_name, "Ljava/util/concurrent/Semaphore", 31) != 0) {
        lock_class = NULL;
    }

    jvmti->Deallocate((unsigned char*)class_name);
    return lock_class;
}

void LockTracer::recordContendedLock(jclass lock_class, jlong time) {
    if (_supports_lock_names) {
        VMSymbol* lock_name = (*(java_lang_Class**)lock_class)->klass()->name();
        Profiler::_instance.recordSample(NULL, time, BCI_SYMBOL, (jmethodID)lock_name);
    } else {
        Profiler::_instance.recordSample(NULL, time, BCI_SYMBOL, NULL);
    }
}

void LockTracer::bindUnsafePark(UnsafeParkFunc entry) {
    JNIEnv* env = VM::jni();

    // Try JDK 9+ package first, then fallback to JDK 8 package
    jclass unsafe = env->FindClass("jdk/internal/misc/Unsafe");
    if (unsafe == NULL) unsafe = env->FindClass("sun/misc/Unsafe");

    if (unsafe != NULL) {
        const JNINativeMethod unsafe_park = {(char*)"park", (char*)"(ZJ)V", (void*)entry};
        env->RegisterNatives(unsafe, &unsafe_park, 1);
    }

    env->ExceptionClear();
}
