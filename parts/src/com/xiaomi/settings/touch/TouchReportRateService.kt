/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

package com.xiaomi.settings.touch

import android.app.Service
import android.content.BroadcastReceiver
import android.content.Context
import android.content.ContentResolver
import android.content.Intent
import android.content.IntentFilter
import android.database.ContentObserver
import android.os.Handler
import android.os.IBinder
import android.os.Looper
import android.os.UserHandle
import android.provider.Settings
import android.util.Log
import java.util.concurrent.Executors

class TouchReportRateService : Service() {

    private val handler = Handler(Looper.getMainLooper())

    private val settingObserver =
        object : ContentObserver(handler) {
            override fun onChange(selfChange: Boolean) {
                if (DEBUG) Log.d(TAG, "SettingObserver: onChange")
                applyReportRate(this@TouchReportRateService)
            }
        }

    private val screenStateReceiver =
        object : BroadcastReceiver() {
            override fun onReceive(context: Context, intent: Intent) {
                if (DEBUG) Log.d(TAG, "onReceive: ${intent.action}")
                if (intent.action == Intent.ACTION_SCREEN_ON) {
                    if (DEBUG) Log.d(TAG, "Screen on, restoring touch report rate")
                    applyReportRate(this@TouchReportRateService)
                }
            }
        }

    override fun onCreate() {
        super.onCreate()
        if (DEBUG) Log.d(TAG, "onCreate")
        contentResolver.registerContentObserver(
            Settings.System.getUriFor(SETTING_KEY),
            false,
            settingObserver,
            UserHandle.USER_CURRENT,
        )
        registerReceiver(screenStateReceiver, IntentFilter(Intent.ACTION_SCREEN_ON))
        applyReportRate(this)
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        if (DEBUG) Log.d(TAG, "onStartCommand")
        return START_STICKY
    }

    override fun onDestroy() {
        if (DEBUG) Log.d(TAG, "onDestroy")
        contentResolver.unregisterContentObserver(settingObserver)
        unregisterReceiver(screenStateReceiver)
        super.onDestroy()
    }

    override fun onBind(intent: Intent?): IBinder? = null

    companion object {
        private const val TAG = "TouchReportRateService"
        private val DEBUG = Log.isLoggable(TAG, Log.DEBUG)

        const val SETTING_KEY = "touch_high_sampling_rate"
        private const val DEFAULT_VALUE = 0
        private const val TOUCH_ID = 0

        private const val MODE_GAME_MODE = 0
        private const val MODE_ACTIVE_MODE = 202
        private val worker = Executors.newSingleThreadExecutor()
        private val main = Handler(Looper.getMainLooper())

        private fun <T> execute(callback: (Result<T>) -> Unit, operation: () -> T) {
            worker.execute {
                val result = runCatching(operation)
                result.exceptionOrNull()?.let { Log.e(TAG, "Touch report-rate operation failed", it) }
                main.post { callback(result) }
            }
        }

        fun readReportRate(callback: (Result<Boolean>) -> Unit) = execute(callback) {
            TouchFeatureWrapper.getReportRate()
        }

        fun setReportRate(resolver: ContentResolver, value: Int, callback: (Result<Unit>) -> Unit) =
            execute(callback) {
                applyHardware(value)
                check(Settings.System.putIntForUser(resolver, SETTING_KEY, value,
                    UserHandle.USER_CURRENT)) { "Touch mode applied, but preference persistence failed" }
            }

        fun applyReportRate(context: Context) {
            val resolver = context.applicationContext.contentResolver
            execute({}) {
                // Read inside the same executor as UI writes, so queued events cannot
                // replay a stale preference after a newer successful user operation.
                val value = Settings.System.getIntForUser(resolver, SETTING_KEY,
                    DEFAULT_VALUE, UserHandle.USER_CURRENT)
                applyHardware(value)
            }
        }

        private fun applyHardware(value: Int) {
            require(value == 0 || value == 1) { "Invalid touch report-rate setting: $value" }
            if (value == 1) {
                TouchFeatureWrapper.setModeValue(TOUCH_ID, MODE_GAME_MODE, 1)
                TouchFeatureWrapper.setModeValue(TOUCH_ID, MODE_ACTIVE_MODE, 1)
            } else {
                TouchFeatureWrapper.setModeValue(TOUCH_ID, MODE_ACTIVE_MODE, 0)
                TouchFeatureWrapper.setModeValue(TOUCH_ID, MODE_GAME_MODE, 0)
            }
        }

        fun startService(context: Context) {
            context.startServiceAsUser(
                Intent(context, TouchReportRateService::class.java),
                UserHandle.CURRENT,
            )
        }
    }
}
