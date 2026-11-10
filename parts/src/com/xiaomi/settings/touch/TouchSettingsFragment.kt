/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

package com.xiaomi.settings.touch

import android.os.Bundle
import androidx.appcompat.app.AlertDialog
import android.os.UserHandle
import android.provider.Settings
import android.util.Log
import androidx.preference.Preference
import com.android.settingslib.widget.MainSwitchPreference
import com.android.settingslib.widget.SettingsBasePreferenceFragment
import com.xiaomi.settings.R

class TouchSettingsFragment :
    SettingsBasePreferenceFragment(), Preference.OnPreferenceChangeListener {

    companion object {
        private const val TAG = "TouchSettingsFragment"
        private val DEBUG = Log.isLoggable(TAG, Log.DEBUG)
    }

    private val switchBar by lazy {
        findPreference<MainSwitchPreference>(TouchReportRateService.SETTING_KEY)!!
    }

    override fun onCreatePreferences(savedInstanceState: Bundle?, rootKey: String?) {
        if (DEBUG) Log.d(TAG, "onCreatePreferences")
        setPreferencesFromResource(R.xml.settings_touch, rootKey)

        switchBar.isEnabled = false
        TouchReportRateService.readReportRate { result ->
            if (!isAdded) return@readReportRate
            switchBar.isEnabled = result.isSuccess
            result.fold({ switchBar.isChecked = it; switchBar.summary = null }, {
                switchBar.summary = it.message
                showError(it)
            })
        }
        switchBar.onPreferenceChangeListener = this
    }

    override fun onPreferenceChange(preference: Preference, newValue: Any?): Boolean {
        if (preference == switchBar) {
            val isChecked = newValue as Boolean
            if (DEBUG) Log.d(TAG, "onPreferenceChange: $isChecked")
            val resolver = requireContext().applicationContext.contentResolver
            switchBar.isEnabled = false
            TouchReportRateService.setReportRate(resolver, if (isChecked) 1 else 0) { result ->
                if (!isAdded) return@setReportRate
                switchBar.isEnabled = true
                result.fold({ switchBar.isChecked = isChecked }, { showError(it) })
            }
            return false
        }
        return true
    }

    private fun showError(error: Throwable) {
        AlertDialog.Builder(requireContext())
            .setTitle(android.R.string.dialog_alert_title)
            .setMessage(error.message ?: error.javaClass.simpleName)
            .setPositiveButton(android.R.string.ok, null).show()
    }
}
