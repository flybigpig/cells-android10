/*
 * Copyright (C) 2020 The Android Open Source Project
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

package com.android.server.demo;

import android.content.Context;
import android.demo.IDemoManager;
import android.os.IBinder;
import android.util.Slog;

import com.android.server.SystemService;

import java.util.HashMap;
import java.util.Map;

/**
 * Demo system service. Runs in the system_server process and exposes a simple
 * in-memory API over Binder. It does not depend on any HAL.
 */
public class DemoManagerService extends SystemService {
    private static final String TAG = "DemoManagerService";
    private static final String VERSION = "1.0";

    private int mStatus = 0;
    private final Map<String, String> mConfigs = new HashMap<>();

    private final IBinder mStub = new IDemoManager.Stub() {
        @Override
        public String getVersion() {
            return VERSION;
        }

        @Override
        public int getStatus() {
            return mStatus;
        }

        @Override
        public void setStatus(int status) {
            mStatus = status;
            Slog.i(TAG, "setStatus -> " + status);
        }

        @Override
        public void setConfig(String key, String value) {
            mConfigs.put(key, value);
        }

        @Override
        public String getConfig(String key) {
            return mConfigs.get(key);
        }

        @Override
        public int compute(int a, int b) {
            return a + b;
        }
    };

    public DemoManagerService(Context context) {
        super(context);
    }

    @Override
    public void onStart() {
        Slog.i(TAG, "publish demo service");
        publishBinderService(Context.DEMO_SERVICE, mStub);
    }
}
