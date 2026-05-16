LRESULT CALLBACK HookedWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_LBUTTONDOWN && !g_guiOpen) {
        bool shouldReach = false;
        if (g_reachCurrentTarget && g_mcInstance && g_pointedEntityField) {
            Config cfg;
            {
                LockGuard lk(g_configMutex);
                cfg = g_config;
            }
            if (cfg.reachEnabled) {
                float rval = (float)(rand() % 100);
                if (rval < cfg.reachChance) {
                    float reachSpan = cfg.reachMax - cfg.reachMin;
                    if (reachSpan < 0.0f) reachSpan = 0.0f;
                    float rfrac = (float)rand() / (float)RAND_MAX;
                    double rolledReach = (double)(cfg.reachMin + (rfrac * reachSpan));
                    if (rolledReach < 3.0) rolledReach = 3.0;

                    if (g_reachCurrentTargetDistSq <= rolledReach * rolledReach) {
                        shouldReach = true;
                    }
                }
            }
        }

        if (shouldReach) {
            JNIEnv* env = nullptr;
            bool attached = false;
            if (g_jvm && g_jvm->GetEnv((void**)&env, JNI_VERSION_1_8) != JNI_OK) {
                if (g_jvm->AttachCurrentThread((void**)&env, nullptr) == JNI_OK) {
                    attached = true;
                } else {
                    env = nullptr;
                }
            }
            if (env) {
                env->SetObjectField(g_mcInstance, g_pointedEntityField, g_reachCurrentTarget);
                if (env->ExceptionCheck()) env->ExceptionClear();

                if (g_objectMouseOverField) {
                    jobject curMop = env->GetObjectField(g_mcInstance, g_objectMouseOverField);
                    if (!env->ExceptionCheck() && curMop) {
                        if (g_entityHitField) {
                            env->SetObjectField(curMop, g_entityHitField, g_reachCurrentTarget);
                            if (!env->ExceptionCheck() && g_typeOfHitField && g_mopEntityTypeConst) {
                                env->SetObjectField(curMop, g_typeOfHitField, g_mopEntityTypeConst);
                            }
                            if (env->ExceptionCheck()) env->ExceptionClear();
                            env->SetObjectField(g_mcInstance, g_objectMouseOverField, curMop);
                            if (env->ExceptionCheck()) env->ExceptionClear();
                        }
                        env->DeleteLocalRef(curMop);
                    } else if (env->ExceptionCheck()) {
                        env->ExceptionClear();
                    }
                }
            }
            if (attached && g_jvm) g_jvm->DetachCurrentThread();
        }
    }

    switch (msg) {
    case WM_MOUSEMOVE:
        g_mouseX = LOWORD(lParam); g_mouseY = HIWORD(lParam); 
        if (g_guiOpen) return 0; // BLOCK input to game
        break;
    case WM_LBUTTONDOWN:
        g_mouseClicked = true; g_mouseDown = true;
        if (g_guiOpen) return 0;
        break;
    case WM_LBUTTONUP:
        g_mouseDown = false;
        if (g_guiOpen) return 0;
        break;
    case WM_RBUTTONDOWN:
        g_mouseRightClicked = true;
        g_mouseRightDown = true;
        if (g_guiOpen) return 0;
        break;
    case WM_RBUTTONUP:
        g_mouseRightDown = false;
        if (g_guiOpen) return 0;
        break;
    case WM_MOUSEWHEEL:
        g_scrollDelta = GET_WHEEL_DELTA_WPARAM(wParam);
        if (g_guiOpen) return 0;
        break;
    }
    if (g_origWndProc) {
        return CallWindowProcA(g_origWndProc, hwnd, msg, wParam, lParam);
    }
    return DefWindowProcA(hwnd, msg, wParam, lParam);
}
