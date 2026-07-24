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

package android.demo;

import android.os.RemoteException;

/**
 * Demo system service manager.
 * @hide
 */
public class DemoManager {
    private final IDemoManager mService;

    /** @hide */
    public DemoManager(IDemoManager service) {
        mService = service;
    }

    /** @hide */
    public String getVersion() {
        try {
            return mService.getVersion();
        } catch (RemoteException e) {
            throw e.rethrowFromSystemServer();
        }
    }

    /** @hide */
    public int getStatus() {
        try {
            return mService.getStatus();
        } catch (RemoteException e) {
            throw e.rethrowFromSystemServer();
        }
    }

    /** @hide */
    public void setStatus(int status) {
        try {
            mService.setStatus(status);
        } catch (RemoteException e) {
            throw e.rethrowFromSystemServer();
        }
    }

    /** @hide */
    public void setConfig(String key, String value) {
        try {
            mService.setConfig(key, value);
        } catch (RemoteException e) {
            throw e.rethrowFromSystemServer();
        }
    }

    /** @hide */
    public String getConfig(String key) {
        try {
            return mService.getConfig(key);
        } catch (RemoteException e) {
            throw e.rethrowFromSystemServer();
        }
    }

    /** @hide */
    public int compute(int a, int b) {
        try {
            return mService.compute(a, b);
        } catch (RemoteException e) {
            throw e.rethrowFromSystemServer();
        }
    }
}
